# Couchbase Deno Client

I've forked the couchnode repo with plans to make it work with Deno.
Since the library has been remade in TypeScript, this should just amount
to removing any Node specific features and replacing them with the Deno
equivalent. At face value, this seems like it won't be a big undertaking,
but I'm unsure just how deep the Node goes in this SDK. Luckily couchnode
is primarily a wrapper around the c SDK.

What follows is the remainder of the original Node.js SDK readme; Leaving it for now.

## Useful Links

Source - [https://github.com/couchbase/couchnode](https://github.com/couchbase/couchnode)

Bug Tracker - [https://www.couchbase.com/issues/browse/JSCBC](https://www.couchbase.com/issues/browse/JSCBC)

Couchbase Developer Portal - [https://docs.couchbase.com/](https://docs.couchbase.com/nodejs-sdk/3.0/hello-world/start-using-sdk.html)

Release Notes - [https://docs.couchbase.com/nodejs-sdk/3.0/project-docs/sdk-release-notes.html](https://docs.couchbase.com/nodejs-sdk/3.0/project-docs/sdk-release-notes.html)

## Installing

To install the lastest release using npm, run:

```bash
npm install couchbase
```

To install the development version directly from github, run:

```bash
npm install "git+https://github.com/couchbase/couchnode.git#master"
```

## Introduction

Connecting to a Couchbase bucket is as simple as creating a new `Cluster`
instance to represent the `Cluster` you are using, and then using the
`bucket` and `collection` commands against this to open a connection to
open your specific bucket and collection. You are able to execute most
operations immediately, and they will be queued until the connection is
successfully established.

Here is a simple example of instantiating a connection, adding a new document
into the bucket and then retrieving its contents:

```javascript
var couchbase = require('couchbase')

couchbase.connect(
  'couchbase://127.0.0.1',
  {
    username: 'username',
    password: 'password',
  },
  (err, cluster) => {
    var bucket = cluster.bucket('default')
    var coll = bucket.defaultCollection()

    coll.upsert('testdoc', { name: 'Frank' }, (err, res) => {
      if (err) throw err

      coll.get('testdoc', (err, res) => {
        if (err) throw err

        console.log(res.value)
        // {name: Frank}
      })
    })
  }
)
```

## Documentation

An extensive documentation is available on the Couchbase website - [https://docs.couchbase.com/nodejs-sdk/3.0/hello-world/start-using-sdk.html](https://docs.couchbase.com/nodejs-sdk/3.0/hello-world/start-using-sdk.html) -
including numerous examples and code samples.

Visit our [Couchbase Node.js SDK forum](https://forums.couchbase.com/c/node-js-sdk) for help.
Or get involved in the [Couchbase Community](https://couchbase.com/community) on the [Couchbase](https://couchbase.com) website.

## Source Control

The source code is available at
[https://github.com/couchbase/couchnode](https://github.com/couchbase/couchnode).
Once you have cloned the repository, you may contribute changes through our
gerrit server. For more details see
[CONTRIBUTING.md](https://github.com/couchbase/couchnode/blob/master/CONTRIBUTING.md).

To execute our test suite, run `make test` from the root directory.

To execute our code coverage, run `make cover` from the root directory.

In addition to the full test suite and full code coverage, you may additionally
execute a subset of the tests which excludes slow-running tests for quick
verifications. These can be run through `make fasttest` and `make fastcover`
respectively.

Finally, to build the API reference for the project, run `make docs` from the
root directory, and a docs folder will be created with the api reference.

## License

Copyright 2013 Couchbase Inc.

Licensed under the Apache License, Version 2.0.

See
[LICENSE](https://github.com/couchbase/couchnode/blob/master/LICENSE)
for further details.
