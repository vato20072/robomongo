# Downloads the official standalone mongosh distribution at configure time so
# the install/package steps can bundle it next to the Robo 3T executable.
# This replaces (for MongoDB 6.x+) the old model of statically linking the
# MongoDB 4.2 shell internals - see docs/Mongo6Modernization.md.
#
# Knobs:
#   ROBO_BUNDLE_MONGOSH  - ON by default. OFF skips the download and bundling
#                          (e.g. for offline builds).
#   ROBO_MONGOSH_VERSION - mongosh version to fetch and bundle. Seeded from the
#                          ROBO_MONGOSH_VERSION environment variable if set,
#                          otherwise defaults below. Cached; override with
#                          -DROBO_MONGOSH_VERSION=x.y.z or bin/clean first.
#
# Defines on success:
#   MONGOSH_DIST_BIN_DIR - directory with the mongosh executable (and its
#                          mongosh_crypt_v1 companion library)
#
# Artifacts come from the official distribution server, e.g.
#   https://downloads.mongodb.com/compass/mongosh-2.9.2-darwin-arm64.zip

option(ROBO_BUNDLE_MONGOSH "Download and bundle mongosh into install/package output" ON)

set(_robo_mongosh_default "2.9.2")
if(DEFINED ENV{ROBO_MONGOSH_VERSION})
    set(_robo_mongosh_default "$ENV{ROBO_MONGOSH_VERSION}")
endif()
set(ROBO_MONGOSH_VERSION "${_robo_mongosh_default}" CACHE STRING "mongosh version to bundle")

# Let a changed environment variable win over the cached value, so
# `ROBO_MONGOSH_VERSION=x.y.z bin/configure` always takes effect
if(DEFINED ENV{ROBO_MONGOSH_VERSION} AND NOT "$ENV{ROBO_MONGOSH_VERSION}" STREQUAL "${ROBO_MONGOSH_VERSION}")
    set(ROBO_MONGOSH_VERSION "$ENV{ROBO_MONGOSH_VERSION}" CACHE STRING "mongosh version to bundle" FORCE)
endif()

if(ROBO_BUNDLE_MONGOSH)
    # Map platform/arch to mongosh distribution names
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(_mongosh_arch "arm64")
    else()
        set(_mongosh_arch "x64")
    endif()

    if(SYSTEM_MACOSX)
        set(_mongosh_platform "darwin-${_mongosh_arch}")
        set(_mongosh_ext "zip")
    elseif(SYSTEM_WINDOWS)
        set(_mongosh_platform "win32-x64")
        set(_mongosh_ext "zip")
    else()
        set(_mongosh_platform "linux-${_mongosh_arch}")
        set(_mongosh_ext "tgz")
    endif()

    set(_mongosh_dist "mongosh-${ROBO_MONGOSH_VERSION}-${_mongosh_platform}")
    set(_mongosh_url "https://downloads.mongodb.com/compass/${_mongosh_dist}.${_mongosh_ext}")
    set(_mongosh_dir "${CMAKE_BINARY_DIR}/mongosh")
    set(_mongosh_root "${_mongosh_dir}/${_mongosh_dist}")
    set(_mongosh_archive "${_mongosh_dir}/${_mongosh_dist}.${_mongosh_ext}")

    if(NOT EXISTS "${_mongosh_root}/bin")
        message(STATUS "Downloading mongosh ${ROBO_MONGOSH_VERSION} (${_mongosh_platform}) ...")
        file(DOWNLOAD "${_mongosh_url}" "${_mongosh_archive}"
             SHOW_PROGRESS STATUS _mongosh_status TLS_VERIFY ON)
        list(GET _mongosh_status 0 _mongosh_code)
        if(NOT _mongosh_code EQUAL 0)
            list(GET _mongosh_status 1 _mongosh_error)
            file(REMOVE "${_mongosh_archive}")
            message(WARNING
                "Failed to download mongosh ${ROBO_MONGOSH_VERSION} from ${_mongosh_url}: "
                "${_mongosh_error}. Building without bundled mongosh "
                "(pass -DROBO_BUNDLE_MONGOSH=OFF to silence this warning).")
            set(ROBO_BUNDLE_MONGOSH OFF)
        else()
            # cmake -E tar handles both .zip and .tgz on all platforms
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar xf "${_mongosh_archive}"
                WORKING_DIRECTORY "${_mongosh_dir}"
                RESULT_VARIABLE _mongosh_untar_code)
            file(REMOVE "${_mongosh_archive}")
            if(NOT _mongosh_untar_code EQUAL 0 OR NOT EXISTS "${_mongosh_root}/bin")
                message(WARNING "Failed to extract mongosh archive. Building without bundled mongosh.")
                set(ROBO_BUNDLE_MONGOSH OFF)
            endif()
        endif()
    endif()

    if(ROBO_BUNDLE_MONGOSH)
        set(MONGOSH_DIST_BIN_DIR "${_mongosh_root}/bin")
        message(STATUS "Bundling mongosh ${ROBO_MONGOSH_VERSION} from ${MONGOSH_DIST_BIN_DIR}")
    endif()
endif()
