Modernizing Robo 3T for MongoDB 6.x+
====================================

This document analyzes how the MongoDB shell is currently embedded in
Robo 3T, why the same approach cannot be repeated verbatim for MongoDB 6.x,
and lays out a recommended path.

1. How the 4.2 shell is embedded today
--------------------------------------

Robo 3T does not shell out to a `mongo` binary. It statically links the
guts of a **patched MongoDB 4.2 source tree** directly into the GUI
executable:

* The fork lives at `paralect/robomongo-shell`, branch `roboshell-v4.2`,
  and is built with SCons (`scons mongo`).
* `cmake/FindMongoDB.cmake` locates that tree via
  `ROBOMONGO_CMAKE_PREFIX_PATH` and links **thousands of individual object
  files** listed in `cmake/mongodb/<platform>-<buildtype>.objects` — the
  entire server-side JavaScript stack (SpiderMonkey/`mozjs-60`, BSON, the
  DBClient networking layer, ICU, boost, etc.).
* `src/robomongo/core/engine/ScriptEngine.cpp` drives the embedded engine
  through MongoDB internals: `mongo::ScriptEngine`, `mongo::Scope`
  (MozJS proxy scope), `mongo::shell_utils::dbConnect`, and
  `mongo::DBClientBase`. A vendored copy of the shell bootstrap lives in
  `src/robomongo/shell/` (`dbshell.cpp`, `mongo.js` glue).
* User-typed script in a query tab is executed by the *same* C++/
  SpiderMonkey engine as the real `mongo` 4.2 shell, and results come back
  as BSON objects rendered by Robo's own widgets.
* Installers (`install/`, CPack/NSIS/dmg) bundle nothing shell-related —
  the shell *is* the executable.

2. Why "the same thing for 6.x" is not a drop-in upgrade
--------------------------------------------------------

1. **The legacy `mongo` shell no longer exists.** It was deprecated in
   MongoDB 5.0 and removed in 6.0. Its replacement, **`mongosh`**, is a
   Node.js application (TypeScript on top of the official Node driver with
   its own embedded JS runtime). There is no C++ shell library in the 6.x
   server tree to fork and patch the way `roboshell-v4.2` was.
2. **The 4.2 engine partially works against modern servers** (the wire
   protocol is backward compatible and commands go over `OP_MSG`), which
   is why basic CRUD in Robo 3T still functions against 5.x/6.x servers.
   What breaks or is missing: new shell helpers and syntax, newer
   auth mechanisms (OIDC, MONGODB-AWS), Queryable Encryption, newer TLS
   stacks (the fork requires OpenSSL 1.1, EOL), API-versioned clients, and
   any server response the old BSON/DBClient layer does not understand.
3. **Re-forking the server at v6.0** (option A below) would mean porting
   the Studio3T patches onto a much larger codebase, a new SpiderMonkey
   (ESR 91), a C++20 toolchain, regenerating the per-platform object
   lists, and signing up to repeat all of that for 7.x/8.x. MongoDB has
   also been actively removing the in-server shell scaffolding, so each
   rebase gets harder. This is the path of maximum, recurring pain.

3. Options considered
---------------------

**A. Fork MongoDB 6.0 sources and rebuild the embedded engine.**
Keeps the current architecture. Enormous one-off effort, brittle object-
file linking, and a dead end: every future server release repeats the
cost, and MongoDB keeps stripping shell code out of the server tree.

**B. Integrate `mongosh` as the execution engine (recommended).**
Replace the embedded C++ engine with the official shell, bundled with the
application:

* Bundle the standalone `mongosh` binary (MongoDB ships self-contained
  ~50 MB binaries for win/mac/linux, x64 + arm64) inside the installer,
  exactly the way the app previously "contained" the 4.2 shell.
* Robo spawns a `mongosh` sidecar per connection/tab (`QProcess`) and
  drives it programmatically — evaluating user script and receiving
  results as EJSON (relaxed/canonical extended JSON), which maps cleanly
  onto Robo's existing document model. mongosh's `--eval`/`--json` modes
  and its published npm packages (`@mongosh/shell-api`,
  `@mongosh/service-provider-*`, used the same way by MongoDB Compass)
  are designed for embedding.
* Benefits: maintained by MongoDB, automatically tracks new server
  features and auth mechanisms, native Apple Silicon support for free,
  removes the fork + SCons + OpenSSL 1.1 constraint entirely, and shrinks
  this repo's build to a plain Qt application.
* Cost: a process boundary (async instead of in-process calls), shipping
  a ~50 MB binary in installers, and porting autocomplete (currently fed
  by the embedded engine) to mongosh's autocomplete API.

**C. Hybrid.** Keep the 4.2 engine for old servers and add the mongosh
sidecar for 6.x+. Maximum compatibility, but you pay both maintenance
bills; only worth it as a transition state (one or two releases).

4. The pivot - IMPLEMENTED (branch mongosh-pivot)
-------------------------------------------------

The 4.2 interim was skipped entirely (decision: only MongoDB 6.0+
matters). The embedded engine and driver are gone; the architecture is:

* **robobson** (`src/robomongo/bson/`): a compact mongo-API-compatible
  document model backed by an order-preserving EJSON DOM. Provides the
  `mongo::` API surface the GUI widgets use (BSONObj/BSONElement/
  builders/iterators, OID/Date_t/Timestamp/Decimal128, MongoURI,
  base64/hex/escape, LogSeverity, HostAndPort). Parses Extended JSON v2
  (canonical/relaxed - what mongosh emits), legacy v1, and mongo shell
  notation (ObjectId(), ISODate(), unquoted keys) for the document
  editors. Legacy `<mongo/...>` includes resolve through shim headers in
  `src/robomongo/compat/`.
* **MongoshExecutor** (`src/robomongo/core/engine/`): runs the bundled
  mongosh per evaluation (`--quiet --norc --json=canonical --eval`),
  building the connection string and TLS/auth flags from
  ConnectionSettings (incl. replica sets and SSH-tunnel overrides).
* **ScriptEngine**: same public interface as before, now backed by the
  executor. A JS prelude patches find/aggregate to capture paging
  metadata and emits it (plus final db/server) as a `__ROBO_META__`
  EJSON line on stderr at exit; stdout carries print output + the
  structured result.
* **MongoClient/MongoWorker**: all data-layer operations (tree loading,
  CRUD, indexes, users, functions) are mongosh evaluations returning
  EJSON.
* **Packaging**: `cmake/RobomongoMongosh.cmake` downloads the platform's
  official mongosh at configure time (`ROBO_MONGOSH_VERSION` env or
  -D flag; `-DROBO_BUNDLE_MONGOSH=OFF` for offline) and installers ship
  it next to the executable.

Wins: native Apple Silicon builds, minutes-long CI (no fork compile),
no OpenSSL 1.1 constraint for the app itself, and server compatibility
tracks mongosh (6.0/7.0/8.0+).

5. Known follow-ups
-------------------

* Sessions are per-evaluation: shell variables do not persist between
  Ctrl+Enter runs of a tab (each execution is a fresh mongosh). Robo's
  own `use`-tracking preserves the current database.
* Autocomplete is a static vocabulary + cached collection names
  (mongosh's autocomplete API could be integrated later).
* Unit tests (`src/robomongo-unit-tests`) still reference the old fork
  and are disabled pending a port to robobson.
* macOS codesigning should include the bundled mongosh binary.
* OpenSSL is only needed by libssh2 (SSH tunnels) now; moving to
  OpenSSL 3 requires bumping libssh2.
