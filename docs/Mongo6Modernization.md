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

4. Recommended roadmap
----------------------

* **Phase 0 — repo modernization (done in this change):** reproducible
  devbox environment (`devbox.json`, [DevboxEnvironment.md](DevboxEnvironment.md)),
  QtWebEngine made optional so the app builds with Nix-provided Qt.
* **Phase 1 — execution seam:** extract an interface from
  `Robomongo::ScriptEngine` (init / exec / interrupt / autocomplete) so
  the embedded 4.2 engine becomes one pluggable implementation. Touch
  points: `ScriptEngine`, `MongoWorker`, `MongoShell`.
* **Phase 2 — mongosh executor:** implement the interface by driving a
  bundled `mongosh` process; parse EJSON output into Robo's
  `MongoDocument` model; wire cursor paging (`it`/`DBQuery.shellBatchSize`
  equivalents) and shell timeouts; port autocomplete.
* **Phase 3 — packaging (groundwork done):** `cmake/RobomongoMongosh.cmake`
  downloads the platform's official standalone mongosh at configure time
  (version selected via `ROBO_MONGOSH_VERSION`, disable with
  `-DROBO_BUNDLE_MONGOSH=OFF`) and the install/package steps bundle it
  next to the executable — this replaces "embed it in the installer"
  from the 4.2 era. Remaining: ship mongosh as the default engine once
  Phase 2 lands, keep 4.2 as fallback behind a setting; include the
  bundled binary in macOS codesigning.
* **Phase 4 — retire the fork:** drop `FindMongoDB.cmake`, the object
  lists and the OpenSSL 1.1 pin; move minimum Qt to 5.15/Qt 6; produce
  native arm64 macOS builds.

5. Compatibility notes for the interim
--------------------------------------

Until Phase 2 lands, Robo 3T built from this repo still embeds the 4.2
engine. Against MongoDB 6.x servers: basic CRUD, aggregation and most
administrative commands work; SCRAM-SHA-1/256 auth works; anything relying
on post-4.2 shell helpers, newer auth mechanisms, or post-4.2 server
features will fail in the shell layer, not the connection layer.
