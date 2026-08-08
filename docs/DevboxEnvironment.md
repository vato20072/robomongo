Devbox Development Environment
==============================

This repository ships a reproducible development environment based on
[Devbox](https://www.jetify.com/devbox) (Nix-backed). It pins every tool
the build needs - CMake, Qt 5.15, OpenSSL, Boost - so a new contributor
can build with three commands. Since the mongosh pivot
([Mongo6Modernization.md](Mongo6Modernization.md)) there is no MongoDB
fork to compile: Robo 3T is a plain Qt application.

Quick start
-----------

```sh
curl -fsSL https://get.jetify.com/devbox | bash   # one time
devbox run configure
devbox run build
devbox run run
```

The first run downloads the pinned toolchain into `.devbox/` (git-ignored).
The init hook exports `ROBOMONGO_CMAKE_PREFIX_PATH` automatically,
pointing at the devbox Nix profile.

Packaging:

```sh
devbox run install   # self-contained bundle in build/release/install
devbox run pack      # .dmg / .tar.gz / installer in build/release/package
```

Bundled mongosh
---------------

The build downloads the official standalone `mongosh` distribution at
configure time and bundles it next to the Robo 3T executable during
install/pack (`cmake/RobomongoMongosh.cmake`). mongosh 2.x supports
MongoDB Server 6.0 and newer.

Pick the version per build - env var wins over the CMake cache:

```sh
ROBO_MONGOSH_VERSION=2.8.3 devbox run configure
bin/configure release -DROBO_MONGOSH_VERSION=2.9.2
bin/configure release -DROBO_BUNDLE_MONGOSH=OFF   # offline builds
```

Downloads are cached under `build/<type>/mongosh/` per version. The
devbox shell also provides a `mongosh` CLI (from nixpkgs, pinned in
`devbox.json`) for interactive testing - independent of the bundled
version. At runtime a development build without the bundled binary
falls back to `ROBO_MONGOSH_PATH` and then `PATH`.

What is pinned
--------------

| Tool        | Version  | Notes                                          |
|-------------|----------|------------------------------------------------|
| CMake       | 3.31.7   | pinned below 4.x (bundled googletest 1.8.1 needs pre-4.0 CMake) |
| Qt          | 5.15.x   | `qt5.full` on Linux; `qtbase` + `qtmacextras` (non-framework dylibs) on macOS |
| OpenSSL     | 1.1.1w   | only used by libssh2 (SSH tunnels); OpenSSL 3 pending a libssh2 bump |
| Boost       | headers  | date/time formatting, scoped_ptr               |
| mongosh     | latest   | CLI for testing in the dev shell               |

Platform notes
--------------

* **Apple Silicon builds natively** - the old 4.2 fork was x86_64-only;
  robobson + mongosh have no architecture constraints.
* QtWebEngine is optional (`ROBO_WEBENGINE`): when absent (Nix does not
  package it for Qt 5 on macOS), the plain-widgets Welcome tab is used.
* `.devbox/` and `.venv/` are git-ignored; delete to reclaim space.
