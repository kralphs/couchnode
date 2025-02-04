/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011-2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "internal.h"
#include "collections.h"
#include "trace.h"
#include "mctx-helper.h"
#include "sllist-inl.h"

#include "capi/cmd_observe.hh"

struct ObserveCtx : lcb::MultiCmdContext {
    void clear_requests()
    {
        requests.clear();
        num_requests.clear();
    }
    explicit ObserveCtx(lcb_INSTANCE *instance_);

    // Overrides
    lcb_STATUS MCTX_add_observe(const lcb_CMDOBSERVE *cmd) override;
    lcb_STATUS MCTX_add_endure(const lcb_CMDENDURE *cmd) override;
    lcb_STATUS MCTX_done(void *) override;
    void MCTX_fail() override;
    void MCTX_setspan(lcbtrace_SPAN *span) override;

    lcb_INSTANCE *instance;
    size_t remaining;
    unsigned oflags;

    typedef std::vector<uint8_t> ServerBuf;
    /* requests array contains one buffer per server. nrequest essentially
     * says how many elements (and thus how many servers) */
    std::vector<ServerBuf> requests;
    std::vector<size_t> num_requests;
    lcbtrace_SPAN *span;
};

struct OperationCtx : mc_REQDATAEX {
    OperationCtx(ObserveCtx *parent_, size_t remaining_);

    ObserveCtx *parent;
    size_t remaining;
};

typedef enum { F_DURABILITY = 0x01, F_DESTROY = 0x02, F_SCHEDFAILED = 0x04 } obs_flags;

// TODO: Move this to a common file
template <typename ContainerType, typename ValueType>
void add_to_buf(ContainerType &c, ValueType v)
{
    auto *p = reinterpret_cast<typename ContainerType::value_type *>(&v);
    c.insert(c.end(), p, p + sizeof(ValueType));
}

static void handle_observe_callback(mc_PIPELINE *pl, mc_PACKET *pkt, lcb_CALLBACK_TYPE cbtype, lcb_STATUS err,
                                    const void *arg)
{
    auto *opc = static_cast<OperationCtx *>(pkt->u_rdata.exdata);
    ObserveCtx *oc = opc->parent;
    auto *resp = reinterpret_cast<lcb_RESPOBSERVE *>(const_cast<void *>(arg));
    lcb_INSTANCE *instance = oc->instance;

    if (resp == nullptr) {
        int nfailed = 0;
        /** We need to fail the request manually.. */
        const char *ptr = SPAN_BUFFER(&pkt->u_value.single);
        const char *end = ptr + pkt->u_value.single.size;
        while (ptr < end) {
            lcb_uint16_t nkey;
            lcb_RESPOBSERVE cur{};
            cur.rflags = LCB_RESP_F_CLIENTGEN;

            ptr += 2;
            memcpy(&nkey, ptr, sizeof(nkey));
            nkey = ntohs(nkey);
            ptr += 2;

            cur.ctx.key.assign(ptr, nkey);
            cur.cookie = opc->cookie;
            cur.ctx.rc = err;
            handle_observe_callback(nullptr, pkt, cbtype, err, &cur);
            ptr += nkey;
            nfailed++;
        }
        lcb_assert(nfailed);
        return;
    }

    resp->cookie = (void *)opc->cookie;
    resp->ctx.rc = err;
    oc->remaining--;
    if (oc->remaining == 0 && oc->span) {
        lcbtrace_span_finish(oc->span, LCBTRACE_NOW);
        oc->span = nullptr;
    }
    if (oc->oflags & F_DURABILITY) {
        resp->ttp = pl ? pl->index : -1;
    } else if ((oc->oflags & F_SCHEDFAILED) == 0) {
        lcb_RESPCALLBACK callback = lcb_find_callback(instance, LCB_CALLBACK_OBSERVE);
        callback(instance, LCB_CALLBACK_OBSERVE, (lcb_RESPBASE *)resp);
    }

    if (oc->oflags & F_DESTROY) {
        return;
    }
    if (oc->remaining == 0) {
        lcb_RESPOBSERVE resp2{};
        resp2.ctx.rc = err;
        resp2.rflags = LCB_RESP_F_CLIENTGEN | LCB_RESP_F_FINAL;
        oc->oflags |= F_DESTROY;
        handle_observe_callback(nullptr, pkt, cbtype, err, &resp2);
        delete oc;
    }
    opc->remaining--;
    if (opc->remaining == 0) {
        TRACE_OBSERVE_END(instance, pkt);
        LCBTRACE_KV_FINISH(pl, pkt, resp, 0);
        delete opc;
    }
}

static void handle_schedfail(mc_PACKET *pkt)
{
    OperationCtx *opc = static_cast<OperationCtx *>(pkt->u_rdata.exdata);
    ObserveCtx *oc = opc->parent;
    oc->oflags |= F_SCHEDFAILED;
    handle_observe_callback(nullptr, pkt, LCB_CALLBACK_OBSERVE, LCB_ERR_SHEDULE_FAILURE, nullptr);
}

void ObserveCtx::MCTX_setspan(lcbtrace_SPAN *span_)
{
    span = span_;
}

lcb_STATUS ObserveCtx::MCTX_add_observe(const lcb_CMDOBSERVE *cmd)
{
    int vbid, srvix_dummy;
    unsigned ii;
    mc_CMDQUEUE *cq = &instance->cmdq;
    lcb_U16 servers_s[4];
    const lcb_U16 *servers;
    size_t nservers;

    if (LCB_KEYBUF_IS_EMPTY(&cmd->key)) {
        return LCB_ERR_EMPTY_KEY;
    }

    if (cq->config == nullptr) {
        return LCB_ERR_NO_CONFIGURATION;
    }

    if (LCBVB_DISTTYPE(LCBT_VBCONFIG(instance)) != LCBVB_DIST_VBUCKET) {
        return LCB_ERR_UNSUPPORTED_OPERATION;
    }

    mcreq_map_key(cq, &cmd->key, 24, &vbid, &srvix_dummy);

    if (cmd->servers_) {
        servers = cmd->servers_;
        nservers = cmd->nservers_;
    } else {
        nservers = 0;
        servers = servers_s;
        /* Replicas are always < 4 */
        for (ii = 0; ii < LCBVB_NREPLICAS(cq->config) + 1; ii++) {
            int ix = lcbvb_vbserver(cq->config, vbid, ii);
            if (ix < 0) {
                if (ii == 0) {
                    return LCB_ERR_NO_MATCHING_SERVER;
                } else {
                    continue;
                }
            }
            servers_s[nservers++] = ix;
            if (cmd->cmdflags & LCB_CMDOBSERVE_F_MASTER_ONLY) {
                break; /* Only a single server! */
            }
        }
    }

    if (nservers == 0) {
        return LCB_ERR_NO_MATCHING_SERVER;
    }

    uint32_t cid = 0;
    uint8_t ncid = 0;
    uint8_t ecid[5] = {0}; /* encoded */

    if (LCBT_SETTING(instance, use_collections)) {
        std::string path = collcache_build_spec(cmd->scope, cmd->nscope, cmd->collection, cmd->ncollection);
        instance->collcache->get(path, &cid);
        ncid = leb128_encode(cid, ecid);
    }

    for (ii = 0; ii < nservers; ii++) {
        lcb_U16 ix = servers[ii];

        lcb_assert(ix < requests.size());

        ServerBuf &rr = requests[ix];
        add_to_buf(rr, uint16_t(htons(vbid)));
        add_to_buf(rr, uint16_t(htons(ncid + cmd->key.contig.nbytes)));
        if (ncid) {
            rr.insert(rr.end(), ecid, ecid + ncid);
        }
        rr.insert(rr.end(), reinterpret_cast<const uint8_t *>(cmd->key.contig.bytes),
                  reinterpret_cast<const uint8_t *>(cmd->key.contig.bytes) + cmd->key.contig.nbytes);
        remaining++;
        num_requests[ix]++;
    }
    return LCB_SUCCESS;
}

lcb_STATUS ObserveCtx::MCTX_add_endure(const lcb_CMDENDURE *)
{
    return LCB_ERR_UNSUPPORTED_OPERATION;
}

static mc_REQDATAPROCS obs_procs = {handle_observe_callback, handle_schedfail};

lcb_STATUS ObserveCtx::MCTX_done(void *cookie_)
{
    unsigned ii;
    mc_CMDQUEUE *cq = &instance->cmdq;

    for (ii = 0; ii < requests.size(); ii++) {
        protocol_binary_request_header hdr;
        mc_PACKET *pkt;
        mc_PIPELINE *pipeline;
        ServerBuf &rr = requests[ii];
        pipeline = cq->pipelines[ii];

        if (rr.empty()) {
            continue;
        }

        pkt = mcreq_allocate_packet(pipeline);
        lcb_assert(pkt);

        mcreq_reserve_header(pipeline, pkt, MCREQ_PKT_BASESIZE);
        mcreq_reserve_value2(pipeline, pkt, rr.size());

        hdr.request.magic = PROTOCOL_BINARY_REQ;
        hdr.request.opcode = PROTOCOL_BINARY_CMD_OBSERVE;
        hdr.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        hdr.request.keylen = 0;
        hdr.request.cas = 0;
        hdr.request.vbucket = 0;
        hdr.request.extlen = 0;
        hdr.request.opaque = pkt->opaque;
        hdr.request.bodylen = htonl((lcb_uint32_t)rr.size());

        memcpy(SPAN_BUFFER(&pkt->kh_span), hdr.bytes, sizeof(hdr.bytes));
        memcpy(SPAN_BUFFER(&pkt->u_value.single), &rr[0], rr.size());

        OperationCtx *ctx = new OperationCtx(this, this->num_requests[ii]);
        ctx->start = gethrtime();
        ctx->deadline = ctx->start + LCB_US2NS(LCBT_SETTING(instance, operation_timeout));
        ctx->cookie = cookie_;

        pkt->flags |= MCREQ_F_REQEXT;
        pkt->u_rdata.exdata = ctx;
        if (instance->settings->tracer) {
            lcbtrace_REF ref;
            char opid[20] = {};
            snprintf(opid, sizeof(opid), "0x%x", (int)pkt->opaque);
            ref.type = LCBTRACE_REF_CHILD_OF;
            ref.span = span;
            MCREQ_PKT_RDATA(pkt)->span =
                lcbtrace_span_start(instance->settings->tracer, LCBTRACE_OP_OBSERVE_CAS, LCBTRACE_NOW, &ref);
            lcbtrace_span_add_tag_str(MCREQ_PKT_RDATA(pkt)->span, LCBTRACE_TAG_OPERATION_ID, opid);
            lcbtrace_span_add_system_tags(MCREQ_PKT_RDATA(pkt)->span, instance->settings, LCBTRACE_THRESHOLD_KV);
        }

        mcreq_sched_add(pipeline, pkt);
        TRACE_OBSERVE_BEGIN(instance, &hdr, SPAN_BUFFER(&pkt->u_value.single));
    }

    if (requests.size() == 0 || remaining == 0) {
        delete this;
        return LCB_ERR_INVALID_ARGUMENT;
    } else {
        MAYBE_SCHEDLEAVE(instance);
        return LCB_SUCCESS;
    }
}

void ObserveCtx::MCTX_fail()
{
    if (span) {
        lcbtrace_span_finish(span, LCBTRACE_NOW);
        span = nullptr;
    }
    delete this;
}

ObserveCtx::ObserveCtx(lcb_INSTANCE *instance_) : instance(instance_), remaining(0), oflags(0), span(nullptr)
{
    requests.resize(LCBT_NSERVERS(instance));
    num_requests.resize(requests.size(), 0);
}

OperationCtx::OperationCtx(ObserveCtx *parent_, size_t remaining_)
    : mc_REQDATAEX(nullptr, obs_procs, 0), parent(parent_), remaining(remaining_)
{
}

LIBCOUCHBASE_API
lcb_MULTICMD_CTX *lcb_observe3_ctxnew(lcb_INSTANCE *instance)
{
    return new ObserveCtx(instance);
}
