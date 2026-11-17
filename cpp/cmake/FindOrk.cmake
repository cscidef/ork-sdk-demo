# FindOrk.cmake — Locate the pre-built Ork SDK
#
# The Ork SDK ships in exactly one supported form:
#
#   cpp/lib/<triplet>/libork_static.a   (Windows: ork_static.lib)
#       Single MERGED archive: Ork transport core + fmt + kcp + libsodium +
#       OpenSSL (ssl/crypto).  Consumers link this one file and nothing else
#       from Ork.
#   cpp/lib/<triplet>/manifest.json     build provenance + plaintext flag
#   cpp/include/ork/sdk/c/              the 3 public C headers:
#                                         ork_abi.h, ork_client_abi.h,
#                                         ork_export.h
#
# There is intentionally **no** split-archive fallback (libork_core.a +
# libork_platform.a + per-dependency archives).  That layout belonged to the
# old development export and is gone together with the C++ SDK sources: the C
# headers are the only integration surface, so a "half-unpacked" tree must fail
# here with one clear message instead of at link time.
#
# Result targets
# --------------
#   Ork::Core        IMPORTED STATIC library — the merged archive plus every
#                    include directory, compile definition and system library a
#                    consumer needs.  Just do:
#                        target_link_libraries(my_app PRIVATE Ork::Core)
#   Ork::Thirdparty  INTERFACE target with the vendored header-only deps
#                    (asio / fmt / ikcp.h / concurrentqueue).  Only needed if a
#                    consumer compiles C++ sources that include them; pure C
#                    consumers can ignore it.
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
# System dependencies:
#   * A C++ runtime (the merged archive is C++ inside; C consumers must set
#     LINKER_LANGUAGE CXX — see demos/*/CMakeLists.txt).
#   * OpenSSL >= 3 headers **only if** a consumer includes <asio/ssl.hpp>.
#     The implementation is already inside the archive and is never linked
#     again; the probe here is optional and never fatal.
#
# Cache variables honoured:
#   ORK_PLATFORM (override the triplet), OPENSSL_ROOT_DIR

if(ORK_FOUND)
    return()
endif()

# ---------------------------------------------------------------------------
# 1. Locate the SDK payload (headers + merged archive) relative to this file
# ---------------------------------------------------------------------------
get_filename_component(ORK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ORK_INCLUDE_DIR "${ORK_ROOT}/include")
set(ORK_THIRDPARTY_INCLUDE_DIR "${ORK_ROOT}/thirdparty/include")

if(NOT EXISTS "${ORK_INCLUDE_DIR}/ork/sdk/c/ork_abi.h")
    set(ORK_FOUND FALSE)
    if(Ork_FIND_REQUIRED)
        message(FATAL_ERROR
            "Ork SDK headers not found (expected ${ORK_INCLUDE_DIR}/ork/sdk/c/ork_abi.h).\n"
            "This repository consumes the formal Ork delivery package; copy\n"
            "  include/ork/sdk/c/  (3 C headers)\n"
            "  lib/<triplet>/      (libork_static.a + manifest.json)\n"
            "out of dist/<platform>/ produced by scripts/package/<platform>/pack_static.")
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
            "Re-pack the SDK for that platform (see cpp/README.md).")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# 3. The merged archive — the only supported payload form
# ---------------------------------------------------------------------------
if(WIN32)
    set(_ork_merged_candidates
        "${ORK_LIBRARY_DIR}/ork_static.lib"
        "${ORK_LIBRARY_DIR}/libork_static.a")
else()
    set(_ork_merged_candidates
        "${ORK_LIBRARY_DIR}/libork_static.a"
        "${ORK_LIBRARY_DIR}/ork_static.lib")
endif()

set(ORK_MERGED_ARCHIVE "")
foreach(_candidate IN LISTS _ork_merged_candidates)
    if(EXISTS "${_candidate}")
        set(ORK_MERGED_ARCHIVE "${_candidate}")
        break()
    endif()
endforeach()

if(NOT ORK_MERGED_ARCHIVE)
    set(ORK_FOUND FALSE)
    if(Ork_FIND_REQUIRED)
        message(FATAL_ERROR
            "Missing merged Ork archive in:\n  ${ORK_LIBRARY_DIR}\n"
            "Looked for:\n  ${_ork_merged_candidates}\n"
            "Only the single merged archive (libork_static.a / ork_static.lib)\n"
            "is supported; the old split layout (libork_core.a + libork_platform.a)\n"
            "has been removed from the SDK.")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# 4. Threads (the internal network pump thread, asio io_context)
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 5. OpenSSL headers — OPTIONAL.
#    The merged archive already contains the OpenSSL implementation, so it is
#    never linked again.  Headers are only needed by C++ consumers that include
#    <asio/ssl.hpp>; pure C consumers need nothing.  Non-fatal by design.
# ---------------------------------------------------------------------------
if(NOT OPENSSL_ROOT_DIR AND DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
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
find_package(OpenSSL QUIET)

# ---------------------------------------------------------------------------
# 6. Platform link extras.
#    macOS: CoreFoundation + libresolv (asio / OpenSSL internals).
#    Windows: Winsock + mswsock (asio), crypt32 (OpenSSL certificate stores).
#    Linux: dl + rt.
# ---------------------------------------------------------------------------
set(ORK_SYSTEM_LIBRARIES "")
if(APPLE)
    list(APPEND ORK_SYSTEM_LIBRARIES "-framework CoreFoundation" resolv)
elseif(WIN32)
    list(APPEND ORK_SYSTEM_LIBRARIES ws2_32 mswsock crypt32)
elseif(UNIX)
    list(APPEND ORK_SYSTEM_LIBRARIES dl rt)
endif()

# ---------------------------------------------------------------------------
# 7. ABI version advertised by the shipped header (diagnostics only)
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
# 8. Delivery metadata + plaintext-bootstrap flag
#
#    ORK_ALLOW_PLAINTEXT_BOOTSTRAP changes behaviour inside the archive, so a
#    consumer built with a mismatched macro silently gets the wrong path.
#    Priority:
#      1. -DORK_PLAINTEXT_BOOTSTRAP=ON|OFF       (explicit wins)
#      2. manifest.json next to the payload      (authoritative)
#      3. default OFF                            (formal delivery default)
# ---------------------------------------------------------------------------
set(ORK_MANIFEST_FILE "")
foreach(_mf "${ORK_LIBRARY_DIR}/manifest.json" "${ORK_ROOT}/manifest.json")
    if(EXISTS "${_mf}")
        set(ORK_MANIFEST_FILE "${_mf}")
        break()
    endif()
endforeach()

set(ORK_MANIFEST_SDK_ID "")
set(ORK_MANIFEST_GIT_SHA "")
set(_ork_manifest_plaintext "")
if(ORK_MANIFEST_FILE)
    file(READ "${ORK_MANIFEST_FILE}" _ork_manifest_json)
    string(JSON ORK_MANIFEST_SDK_ID ERROR_VARIABLE _ork_e1
           GET "${_ork_manifest_json}" package sdk_id)
    string(JSON ORK_MANIFEST_GIT_SHA ERROR_VARIABLE _ork_e2
           GET "${_ork_manifest_json}" source git_sha)
    string(JSON _ork_manifest_plaintext ERROR_VARIABLE _ork_e3
           GET "${_ork_manifest_json}" security plaintext_bootstrap_compiled)
    if(_ork_e1 OR _ork_e2 OR _ork_e3)
        set(_ork_manifest_plaintext "")
    endif()
endif()

if(NOT DEFINED ORK_PLAINTEXT_BOOTSTRAP)
    # string(JSON) 读布尔返回的是 "ON"/"OFF" 之类的文本，不是 True/False，
    # 先归一化成大写再判，避免拿错构建配置导致行为不一致。
    set(_ork_mp "")
    if(_ork_manifest_plaintext)
        string(TOUPPER "${_ork_manifest_plaintext}" _ork_mp)
    endif()
    if(_ork_mp MATCHES "^(TRUE|ON|1|YES)$")
        set(ORK_PLAINTEXT_BOOTSTRAP ON)
    else()
        set(ORK_PLAINTEXT_BOOTSTRAP OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# 9. Imported targets
# ---------------------------------------------------------------------------
if(NOT TARGET Ork::Thirdparty)
    add_library(Ork::Thirdparty INTERFACE IMPORTED)
    set_target_properties(Ork::Thirdparty PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ORK_THIRDPARTY_INCLUDE_DIR}")
endif()

# Compile definitions that must match how the archive was built.
#   ORK_STATIC=1  — on Windows ork_export.h would otherwise expand OrkExport to
#                   __declspec(dllimport) and every call fails to link.
#   ORK_ALLOW_PLAINTEXT_BOOTSTRAP — see section 8.
set(ORK_COMPILE_DEFINITIONS ORK_STATIC=1)
if(ORK_PLAINTEXT_BOOTSTRAP)
    list(APPEND ORK_COMPILE_DEFINITIONS ORK_ALLOW_PLAINTEXT_BOOTSTRAP=1)
endif()

set(_ork_include_dirs "${ORK_INCLUDE_DIR}")
if(OPENSSL_FOUND AND OPENSSL_INCLUDE_DIR)
    list(APPEND _ork_include_dirs "${OPENSSL_INCLUDE_DIR}")
endif()

if(NOT TARGET Ork::Core)
    add_library(Ork::Core STATIC IMPORTED GLOBAL)
    set_target_properties(Ork::Core PROPERTIES
        IMPORTED_LOCATION "${ORK_MERGED_ARCHIVE}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ork_include_dirs}"
        INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}"
        INTERFACE_LINK_LIBRARIES
            "Ork::Thirdparty;\
Threads::Threads;\
${ORK_SYSTEM_LIBRARIES}")
endif()

set(ORK_FOUND TRUE)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ork
    REQUIRED_VARS ORK_MERGED_ARCHIVE ORK_INCLUDE_DIR
    VERSION_VAR ORK_ABI_VERSION
    FAIL_MESSAGE "Could not find the pre-built Ork SDK")

if(NOT Ork_FIND_QUIETLY)
    message(STATUS "Ork SDK — C ABI version ${ORK_ABI_VERSION}, platform ${ORK_PLATFORM}")
    message(STATUS "  headers : ${ORK_INCLUDE_DIR}")
    message(STATUS "  vendored: ${ORK_THIRDPARTY_INCLUDE_DIR}")
    message(STATUS "  libs    : ${ORK_LIBRARY_DIR}")
    message(STATUS "  payload : merged archive (${ORK_MERGED_ARCHIVE})")
    if(ORK_MANIFEST_SDK_ID)
        message(STATUS "  package : ${ORK_MANIFEST_SDK_ID} — built from "
                       "${ORK_MANIFEST_GIT_SHA}")
    else()
        message(STATUS "  package : no manifest.json next to the payload "
                       "(provenance unknown)")
    endif()
    message(STATUS "  plaintext bootstrap: ${ORK_PLAINTEXT_BOOTSTRAP} "
                   "(must match the archive; override with -DORK_PLAINTEXT_BOOTSTRAP=ON|OFF)")
endif()
