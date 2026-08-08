#!/usr/bin/env bash
# Builds the Robo 3T Shell (MongoDB 4.2 fork) with scons, invoked directly
# instead of via the fork's bin/build (which mis-quotes CCFLAGS on macOS and
# cannot derive MONGO_VERSION from a shallow clone).
set -euo pipefail

: "${ROBOSHELL_DIR:?ROBOSHELL_DIR not set - run inside 'devbox shell'}"
cd "$ROBOSHELL_DIR"

pip install --quiet -r etc/pip/compile-requirements.txt

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
COMMON_ARGS=(
    mongo
    --ssl
    -j"$JOBS"
    MONGO_VERSION=4.2.0
    --disable-minimum-compiler-version-enforcement
    --disable-warnings-as-errors
)

if [ "$(uname -s)" = "Darwin" ]; then
    exec scons "${COMMON_ARGS[@]}" \
        "CCFLAGS=-mmacosx-version-min=10.13 -Wno-unused-function" \
        "LINKFLAGS=-mmacosx-version-min=10.13"
else
    # Linux: point the build at OpenSSL from the devbox profile
    PROFILE="${DEVBOX_PACKAGES_DIR:-$PWD/.devbox/nix/profile/default}"
    exec scons "${COMMON_ARGS[@]}" \
        CPPPATH="$PROFILE/include" \
        LIBPATH="$PROFILE/lib"
fi
