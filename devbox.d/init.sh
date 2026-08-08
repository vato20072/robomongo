# Devbox shell initialization for Robo 3T development.
#
# Composes ROBOMONGO_CMAKE_PREFIX_PATH (semicolon-separated, consumed by
# bin/configure and the CMake Find* modules) out of:
#   1. The devbox Nix profile (provides Qt 5 and OpenSSL 1.1)
#   2. The Robo 3T Shell (patched MongoDB fork) source/build tree
#
# Override ROBOSHELL_DIR before entering the shell if your robomongo-shell
# checkout lives somewhere other than ../robomongo-shell.

ROBO_PROJECT_ROOT="${DEVBOX_PROJECT_ROOT:-$PWD}"
ROBO_DEVBOX_PROFILE="$ROBO_PROJECT_ROOT/.devbox/nix/profile/default"

export ROBOSHELL_DIR="${ROBOSHELL_DIR:-$(dirname "$ROBO_PROJECT_ROOT")/robomongo-shell}"

if [ -z "${ROBOMONGO_CMAKE_PREFIX_PATH:-}" ]; then
    export ROBOMONGO_CMAKE_PREFIX_PATH="$ROBO_DEVBOX_PROFILE;$ROBOSHELL_DIR"
fi

# Activate the devbox-managed Python venv and make sure SCons 3.1.2 is
# available in it (the version required by the MongoDB 4.2 scons build;
# installed via pip because the nixpkgs scons wrapper clashes with the
# profile's python).
if [ -n "${VENV_DIR:-}" ] && [ -f "$VENV_DIR/bin/activate" ]; then
    . "$VENV_DIR/bin/activate"
    command -v scons >/dev/null 2>&1 || pip install --quiet scons==3.1.2
fi

if [ ! -d "$ROBOSHELL_DIR" ]; then
    echo "robo: robomongo-shell not found at $ROBOSHELL_DIR"
    echo "robo: run 'devbox run shell-clone && devbox run shell-build' to set it up"
fi
