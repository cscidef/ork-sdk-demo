# FindOrk.cmake — Locate the pre-built Ork SDK (static libraries + public headers)
#
# This module is intentionally self-contained: the Ork SDK ships as pre-built
# static libraries under  cpp/lib/<triplet>/  plus public headers under
# cpp/include/  and vendored third-party headers under cpp/thirdparty/include/.
# Nothing has to be compiled from source and the harmonia_ork repository is not
# required at build time.
#
# Result targets
# --------------
#   Ork::Core        IMPORTED STATIC library (libork_core.a) carrying every
#                    include directory, compile definition and link dependency
#                    a consumer needs.  Just do:
#                        target_link_libraries(my_app PRIVATE Ork::Core)
#   Ork::Platform    libork_platform.a alone (rarely needed directly)
#   Ork::Thirdparty  INTERFACE target with the vendored header-only deps
#                    (asio / fmt / ikcp.h / concurrentqueue)
#
# Result variables
# ----------------
#   ORK_FOUND                   TRUE when everything was located
#   ORK_PLATFORM                triplet, e.g. "macos-arm64"
#   ORK_ROOT                    cpp/ directory of this demo repository
#   ORK_INCLUDE_DIR             cpp/include            (Ork public headers)
#   ORK_THIRDPARTY_INCLUDE_DIR  cpp/thirdparty/include (asio, fmt, ikcp.h, ...)
#   ORK_LIBRARY_DIR             cpp/lib/<triplet>
#   ORK_ABI_VERSION             value of ORK_ABI_VERSION in the shipped header
#
# System dependencies that are NOT vendored (must be present on the machine):
#   * OpenSSL >= 3   (headers are needed by <asio/ssl.hpp>; TLS 1.3 bootstrap)
#   * libsodium      (ChaCha20-Poly1305 AEAD cipher)
# Both are discovered through pkg-config first and fall back to
# find_package(OpenSSL) / find_library() with Homebrew hints on macOS.
#
# Cache variables honoured:
#   OPENSSL_ROOT_DIR, SODIUM_ROOT, ORK_PLATFORM (override the triplet)

if(ORK_FOUND)
    return()
endif()

# ---------------------------------------------------------------------------
# 1. Locate the SDK payload (headers + static libs) relative to this file
# ---------------------------------------------------------------------------
get_filename_component(ORK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ORK_INCLUDE_DIR "${ORK_ROOT}/include")
set(ORK_THIRDPARTY_INCLUDE_DIR "${ORK_ROOT}/thirdparty/include")

if(NOT EXISTS "${ORK_INCLUDE_DIR}/ork/sdk/c/ork_abi.h")
    set(ORK_FOUND FALSE)
    if(Ork_FIND_REQUIRED)
        message(FATAL_ERROR
            "Ork SDK headers not found (expected ${ORK_INCLUDE_DIR}/ork/sdk/c/ork_abi.h).")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# 2. Resolve the target triplet
# ---------------------------------------------------------------------------
if(NOT ORK_PLATFORM)
    if(APPLE)
        set(_ork_os "macos")
    elseif(WIN32)
        set(_ork_os "windows")
    elseif(UNIX)
        set(_ork_os "linux")
    else()
        set(_ork_os "unknown")
    endif()

    if(CMAKE_OSX_ARCHITECTURES)
        string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _ork_arch)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_ork_arch "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(_ork_arch "x64")
    else()
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ork_arch)
    endif()

    set(ORK_PLATFORM "${_ork_os}-${_ork_arch}")
endif()

set(ORK_LIBRARY_DIR "${ORK_ROOT}/lib/${ORK_PLATFORM}")

if(NOT IS_DIRECTORY "${ORK_LIBRARY_DIR}")
    set(ORK_FOUND FALSE)
    if(Ork_FIND_REQUIRED)
        message(FATAL_ERROR
            "No pre-built Ork SDK for platform '${ORK_PLATFORM}'.\n"
            "Looked in: ${ORK_ROOT}/lib\n"
            "See cpp/README.md — 'Porting to another platform'.")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# 3. Existence check for every archive the imported targets will reference.
#    ork_core      — crypto/connection/transport/codec/sdk/client aggregate
#    ork_platform  — config / logging / time / secure random (leaf library)
#    kcp, fmt, yaml-cpp — third-party pieces compiled as static archives
#    (asio, spdlog and concurrentqueue are header-only → nothing to link)
#
#    This loop only validates; it collects nothing. The actual link line is
#    assembled on Ork::Core / Ork::Platform in section 9, so a half-unpacked
#    SDK fails here with one clear message instead of at link time with a wall
#    of undefined symbols.
#
#    Only GNU-style "lib<name>.a" is recognised — currently macOS/Linux only.
# ---------------------------------------------------------------------------
set(ORK_LIB_NAMES ork_core ork_platform kcp fmt yaml-cpp)
foreach(_name IN LISTS ORK_LIB_NAMES)
    set(_path "${ORK_LIBRARY_DIR}/lib${_name}.a")
    if(NOT EXISTS "${_path}")
        set(ORK_FOUND FALSE)
        if(Ork_FIND_REQUIRED)
            message(FATAL_ERROR
                "Missing Ork static library: ${_path}\n"
                "Re-pack the SDK for '${ORK_PLATFORM}' (see cpp/README.md).\n"
                "Only GNU-style .a archives are supported (currently macOS/Linux only).")
        endif()
        return()
    endif()
endforeach()

set(ORK_CORE_LIBRARY "${ORK_LIBRARY_DIR}/libork_core.a")
set(ORK_PLATFORM_LIBRARY "${ORK_LIBRARY_DIR}/libork_platform.a")

# ---------------------------------------------------------------------------
# 4. Threads (asio io_context, the internal network pump thread, OrkClient)
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 5. OpenSSL >= 3 — headers required by <asio/ssl.hpp>, libs by the TLS path.
#    Homebrew keeps openssl@3 keg-only, so help find_package() with a hint
#    derived from pkg-config or from the usual opt/ locations.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)

if(NOT OPENSSL_ROOT_DIR AND DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
endif()

if(NOT OPENSSL_ROOT_DIR AND PKG_CONFIG_FOUND)
    pkg_check_modules(_ORK_PKG_OPENSSL QUIET openssl)
    if(_ORK_PKG_OPENSSL_FOUND AND _ORK_PKG_OPENSSL_PREFIX)
        set(OPENSSL_ROOT_DIR "${_ORK_PKG_OPENSSL_PREFIX}")
    endif()
endif()

if(NOT OPENSSL_ROOT_DIR AND APPLE)
    foreach(_ork_cand
            /opt/homebrew/opt/openssl@3
            /usr/local/opt/openssl@3
            /opt/homebrew/opt/openssl
            /usr/local/opt/openssl)
        if(EXISTS "${_ork_cand}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${_ork_cand}")
            break()
        endif()
    endforeach()
endif()

find_package(OpenSSL REQUIRED)

if(OPENSSL_VERSION AND OPENSSL_VERSION VERSION_LESS 3)
    message(WARNING
        "Ork was built against OpenSSL >= 3 (TLS 1.3 bootstrap); found ${OPENSSL_VERSION}. "
        "Linking may fail or the TLS path may misbehave.")
endif()

# ---------------------------------------------------------------------------
# 6. libsodium — ChaCha20-Poly1305 AEAD. Only the library is strictly needed:
#    no Ork public header includes <sodium.h> (it is a PRIVATE dep upstream).
# ---------------------------------------------------------------------------
set(_ork_sodium_hints "")
if(DEFINED SODIUM_ROOT)
    list(APPEND _ork_sodium_hints "${SODIUM_ROOT}/include" "${SODIUM_ROOT}/lib")
elseif(DEFINED ENV{SODIUM_ROOT})
    list(APPEND _ork_sodium_hints "$ENV{SODIUM_ROOT}/include" "$ENV{SODIUM_ROOT}/lib")
endif()
if(APPLE)
    list(APPEND _ork_sodium_hints
        /opt/homebrew/opt/libsodium/include /opt/homebrew/opt/libsodium/lib
        /usr/local/opt/libsodium/include /usr/local/opt/libsodium/lib)
endif()

find_path(SODIUM_INCLUDE_DIR
    NAMES sodium.h
    HINTS ${_ork_sodium_hints} ${SODIUM_INCLUDE_DIRS}
)
find_library(SODIUM_LIBRARY
    NAMES sodium libsodium
    HINTS ${_ork_sodium_hints} ${SODIUM_LIBRARY_DIRS}
)

if(NOT SODIUM_LIBRARY AND PKG_CONFIG_FOUND)
    pkg_check_modules(_ORK_PKG_SODIUM QUIET libsodium)
    if(_ORK_PKG_SODIUM_FOUND)
        set(SODIUM_LIBRARY "${_ORK_PKG_SODIUM_LINK_LIBRARIES}")
        if(_ORK_PKG_SODIUM_INCLUDE_DIRS AND NOT SODIUM_INCLUDE_DIR)
            set(SODIUM_INCLUDE_DIR "${_ORK_PKG_SODIUM_INCLUDE_DIRS}")
        endif()
    endif()
endif()

if(NOT SODIUM_LIBRARY)
    set(ORK_FOUND FALSE)
    if(Ork_FIND_REQUIRED)
        message(FATAL_ERROR
            "libsodium not found. Install it (macOS: 'brew install libsodium', "
            "Debian/Ubuntu: 'apt install libsodium-dev') or pass -DSODIUM_ROOT=<prefix>.")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# 7. Platform link extras.
#    macOS: CoreFoundation + libresolv (asio / OpenSSL internals). The C++
#           runtime comes from the linker driver — consumers compiling C
#           sources must set LINKER_LANGUAGE CXX (see demos/*/CMakeLists.txt).
#    Linux: dl + rt for asio timers.
# ---------------------------------------------------------------------------
set(ORK_SYSTEM_LIBRARIES "")
if(APPLE)
    list(APPEND ORK_SYSTEM_LIBRARIES "-framework CoreFoundation" resolv)
elseif(UNIX)
    list(APPEND ORK_SYSTEM_LIBRARIES dl rt)
endif()

# ---------------------------------------------------------------------------
# 8. ABI version advertised by the shipped header (diagnostics only)
# ---------------------------------------------------------------------------
file(STRINGS "${ORK_INCLUDE_DIR}/ork/sdk/c/ork_abi.h" _ork_abi_line
     REGEX "^#define[ \t]+ORK_ABI_VERSION[ \t]+[0-9]+u?")
if(_ork_abi_line)
    string(REGEX REPLACE "^#define[ \t]+ORK_ABI_VERSION[ \t]+([0-9]+)u?.*" "\\1"
           ORK_ABI_VERSION "${_ork_abi_line}")
else()
    set(ORK_ABI_VERSION "unknown")
endif()

# ---------------------------------------------------------------------------
# 9. Imported targets
# ---------------------------------------------------------------------------
if(NOT TARGET Ork::Thirdparty)
    add_library(Ork::Thirdparty INTERFACE IMPORTED)
    set_target_properties(Ork::Thirdparty PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ORK_THIRDPARTY_INCLUDE_DIR}")
endif()

# Compile definitions that MUST match the configuration the shipped archives
# were built with, otherwise header-only constants (kTlsBootstrapCompiledIn /
# kPlaintextBootstrapCompiledIn) and the conditional members of OrkClient
# (#if defined(ORK_HAVE_TLS)) disagree with the library → ODR / ABI breakage.
set(ORK_COMPILE_DEFINITIONS "")
if(NOT WIN32)
    list(APPEND ORK_COMPILE_DEFINITIONS ORK_HAVE_TLS=1)
endif()
# The pre-built demo SDK is produced with -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON,
# which enables the loopback-only development bootstrap. Code that must never
# use plaintext simply never selects BootstrapSecurityMode::PlaintextDevelopment
# (and OrkBootstrapSecurity_PlaintextDev on the C ABI).
list(APPEND ORK_COMPILE_DEFINITIONS ORK_ALLOW_PLAINTEXT_BOOTSTRAP=1)

if(NOT TARGET Ork::Platform)
    add_library(Ork::Platform STATIC IMPORTED GLOBAL)
    set_target_properties(Ork::Platform PROPERTIES
        IMPORTED_LOCATION "${ORK_PLATFORM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ORK_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}"
        INTERFACE_LINK_LIBRARIES
            "${ORK_LIBRARY_DIR}/libfmt.a;${ORK_LIBRARY_DIR}/libyaml-cpp.a;${SODIUM_LIBRARY}")
endif()

if(NOT TARGET Ork::Core)
    add_library(Ork::Core STATIC IMPORTED GLOBAL)
    set_target_properties(Ork::Core PROPERTIES
        IMPORTED_LOCATION "${ORK_CORE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ORK_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}"
        # Order matters for static archives: dependants first, dependencies last.
        INTERFACE_LINK_LIBRARIES
            "Ork::Platform;\
${ORK_LIBRARY_DIR}/libkcp.a;\
Ork::Thirdparty;\
OpenSSL::SSL;\
OpenSSL::Crypto;\
${SODIUM_LIBRARY};\
Threads::Threads;\
${ORK_SYSTEM_LIBRARIES}")
endif()

# Sodium headers are optional (no public Ork header needs them) — publish the
# include dir on Ork::Core only when it was actually found.
if(SODIUM_INCLUDE_DIR AND EXISTS "${SODIUM_INCLUDE_DIR}/sodium.h")
    set_property(TARGET Ork::Core APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}")
endif()

set(ORK_FOUND TRUE)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ork
    REQUIRED_VARS ORK_CORE_LIBRARY ORK_PLATFORM_LIBRARY ORK_INCLUDE_DIR SODIUM_LIBRARY
    VERSION_VAR ORK_ABI_VERSION
    FAIL_MESSAGE "Could not find the pre-built Ork SDK")

if(NOT Ork_FIND_QUIETLY)
    message(STATUS "Ork SDK — C ABI version ${ORK_ABI_VERSION}, platform ${ORK_PLATFORM}")
    message(STATUS "  headers : ${ORK_INCLUDE_DIR}")
    message(STATUS "  vendored: ${ORK_THIRDPARTY_INCLUDE_DIR}")
    message(STATUS "  libs    : ${ORK_LIBRARY_DIR}")
    message(STATUS "  OpenSSL : ${OPENSSL_VERSION} (${OPENSSL_INCLUDE_DIR})")
    message(STATUS "  sodium  : ${SODIUM_LIBRARY}")
endif()
