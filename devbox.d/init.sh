# Devbox shell initialization for Robo 3T development.
#
# Composes ROBOMONGO_CMAKE_PREFIX_PATH (semicolon-separated, consumed by
# bin/configure and the CMake Find* modules) from the devbox Nix profile,
# which provides Qt 5, OpenSSL and Boost.
#
# Since the mongosh pivot no MongoDB fork is needed: mongosh is fetched
# and bundled by CMake (cmake/RobomongoMongosh.cmake), and a mongosh CLI
# is available in this shell for testing.

ROBO_PROJECT_ROOT="${DEVBOX_PROJECT_ROOT:-$PWD}"
ROBO_DEVBOX_PROFILE="$ROBO_PROJECT_ROOT/.devbox/nix/profile/default"

if [ -z "${ROBOMONGO_CMAKE_PREFIX_PATH:-}" ]; then
    export ROBOMONGO_CMAKE_PREFIX_PATH="$ROBO_DEVBOX_PROFILE"
fi
