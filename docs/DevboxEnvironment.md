Devbox Development Environment
==============================

This repository ships a reproducible development environment based on
[Devbox](https://www.jetify.com/devbox) (Nix-backed). It pins every tool the
build needs — CMake, Qt 5, OpenSSL 1.1, SCons 3.1.2, Python — so a new
contributor does not have to follow the manual toolchain instructions in
[BuildRobo3TOnMacAndLinux.md](BuildRobo3TOnMacAndLinux.md).

Quick start
-----------

1. Install Devbox (one time):

   ```sh
   curl -fsSL https://get.jetify.com/devbox | bash
   ```

2. Enter the environment from the repository root:

   ```sh
   devbox shell
   ```

   The first run downloads the pinned toolchain into `.devbox/` (git-ignored).
   The init hook exports `ROBOMONGO_CMAKE_PREFIX_PATH` automatically, pointing
   at the devbox Nix profile (Qt + OpenSSL) and at the Robo 3T Shell checkout.

3. Fetch and build the Robo 3T Shell (patched MongoDB 4.2 fork) once:

   ```sh
   devbox run shell-clone    # clones ../robomongo-shell (roboshell-v4.2)
   devbox run shell-build    # scons build, takes a while
   ```

   If your `robomongo-shell` checkout lives elsewhere, export `ROBOSHELL_DIR`
   before entering the shell.

4. Configure, build and run Robo 3T:

   ```sh
   devbox run configure
   devbox run build
   devbox run run
   ```

What is pinned
--------------

| Tool        | Version  | Notes                                            |
|-------------|----------|--------------------------------------------------|
| CMake       | 3.31.7   | pinned below 4.x (bundled googletest 1.8.1 needs pre-4.0 CMake) |
| Qt          | 5.15.x   | `qt5.full` on Linux; `qtbase` + `qtmacextras` on macOS |
| OpenSSL     | 1.1.1w   | required by the MongoDB 4.2 fork (no OpenSSL 3 support) |
| SCons       | 3.1.2    | installed via pip into the devbox venv (version required by the MongoDB 4.2 scons build) |
| Python      | 3.9      | newest Python that still runs SCons 3.1.2        |

Platform notes
--------------

* **Linux (x86_64/aarch64)** — fully supported; `qt5.full` includes
  QtWebEngine, but the Linux build never used it (plain-widgets Welcome tab).
* **macOS** — Nix does not package QtWebEngine for Qt 5 on all Darwin
  platforms, so the build system now treats it as optional: if
  `Qt5WebEngineWidgets` is not found, the app is built with the plain-widgets
  Welcome tab (the same one Linux uses). Pass `-DROBO_WEBENGINE=OFF` to force
  this off explicitly.
* **Apple Silicon (arm64)** — the current MongoDB 4.2 fork only ships
  SpiderMonkey (`mozjs-60`) platform headers for `x86_64`, so the embedded
  shell cannot be compiled natively for arm64. Building on Apple Silicon
  requires an x86_64 (Rosetta 2) toolchain, or waiting for the planned shell
  modernization — see [Mongo6Modernization.md](Mongo6Modernization.md).

Bundled mongosh
---------------

As part of the [MongoDB 6.x+ modernization](Mongo6Modernization.md), the build
downloads the official standalone `mongosh` distribution at configure time and
bundles it next to the Robo 3T executable during `bin/install` / `bin/pack`
(`cmake/RobomongoMongosh.cmake`).

Pick the mongosh version per build — either via environment variable or CMake
cache flag:

```sh
ROBO_MONGOSH_VERSION=2.8.3 devbox run configure   # env var wins over cache
bin/configure release -DROBO_MONGOSH_VERSION=2.9.2
bin/configure release -DROBO_BUNDLE_MONGOSH=OFF   # offline builds
```

Downloads are cached under `build/<type>/mongosh/` per version, so switching
back and forth is cheap. The devbox shell also provides its own `mongosh` CLI
(from nixpkgs, pinned in `devbox.json`) for interactive testing against
servers — independent of the version being bundled.

Housekeeping
------------

* `devbox run clean` — remove Robo 3T build files.
* `.devbox/` is git-ignored; delete it to reclaim disk space (it will be
  re-created on the next `devbox shell`).
* OpenSSL 1.1.1 is end-of-life upstream; it is allow-listed in `devbox.json`
  (`allow_insecure`) because the embedded MongoDB 4.2 code cannot build
  against OpenSSL 3. This goes away with the shell modernization.
