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
#   Ork::Core        IMPORTED STATIC library carrying every include directory,
#                    compile definition and link dependency a consumer needs.
#                    The delivery payload is the merged archive
#                    (libork_static.a / ork_static.lib); the split layout
#                    (libork_core.a + ...) is only a fallback.  Just do:
#                        target_link_libraries(my_app PRIVATE Ork::Core)
#   Ork::Platform    libork_platform.a alone (split fallback only, rarely needed)
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
# System dependencies:
#   * OpenSSL >= 3   HEADERS only — <asio/ssl.hpp> is included by the public C++
#                    headers.  In the merged delivery layout the implementation
#                    is already inside the archive and is not linked again.
#   * libsodium      split fallback layout only (ChaCha20-Poly1305 AEAD); the
#                    merged archive already contains it.
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
# 3. Resolve every archive the imported targets will reference.
#    ork_core      — crypto/connection/transport/codec/sdk/client aggregate
#    ork_platform  — config / logging / time / secure random (leaf library)
#    kcp, fmt, yaml-cpp — third-party pieces compiled as static archives
#    (asio, spdlog and concurrentqueue are header-only → nothing to link)
#
#    Naming is platform-dependent, so each library is probed in both forms and
#    the platform-preferred one wins:
#        MSVC / Windows  → <name>.lib        (ork_core.lib, yaml-cpp.lib, ...)
#        macOS / Linux   → lib<name>.a       (libork_core.a, libyaml-cpp.a, ...)
#    Probing both means one lib/<triplet>/ directory can hold either flavour,
#    and a MinGW build on Windows still finds the GNU archives.
#
#    This loop resolves AND validates. The actual link line is assembled on
#    Ork::Core / Ork::Platform in section 9, so a half-unpacked SDK fails here
#    with one clear message instead of at link time with a wall of undefined
#    symbols.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 3a. Payload form — merged archive (delivery) vs split archives (development).
#
#     The packaged SDK (dist/macos/lib/libork_static.a,
#     dist/windows/lib/ork_static.lib) is a SINGLE merged archive holding the
#     Ork core plus every vendored static dependency.  It is built from 8 input
#     archives — ork_static, ork_platform, fmt, yaml-cpp, kcp, libsodium,
#     openssl_ssl, openssl_crypto — so only the number of files differs from the
#     five-archive split layout below.
#
#     The merged form is therefore probed first. When it is present we link
#     that one archive and skip split probing entirely, so lib/<triplet>/ can
#     hold an unpacked delivery package — which is the whole point of testing
#     what actually ships, rather than a locally exported build tree.
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

if(ORK_MERGED_ARCHIVE)
    set(ORK_PAYLOAD_FORM "merged")
else()
    set(ORK_PAYLOAD_FORM "split")
endif()

# ---------------------------------------------------------------------------
# 3b. Split archives (development layout) — skipped when 3a matched.
#
#     ork_core      — crypto/connection/transport/codec/sdk/client aggregate
#     ork_platform  — config / logging / time / secure random (leaf library)
#     kcp, fmt, yaml-cpp — third-party pieces compiled as static archives
#     (asio, spdlog and concurrentqueue are header-only → nothing to link)
#
#     Naming is platform-dependent, so each library is probed in both forms and
#     the platform-preferred one wins:
#         MSVC / Windows  → <name>.lib        (ork_core.lib, yaml-cpp.lib, ...)
#         macOS / Linux   → lib<name>.a       (libork_core.a, libyaml-cpp.a, ...)
#     Probing both means one lib/<triplet>/ directory can hold either flavour,
#     and a MinGW build on Windows still finds the GNU archives.
#
#     This loop resolves AND validates. The actual link line is assembled on
#     Ork::Core / Ork::Platform in section 9, so a half-unpacked SDK fails here
#     with one clear message instead of at link time with a wall of undefined
#     symbols.
# ---------------------------------------------------------------------------
if(NOT ORK_MERGED_ARCHIVE)
    set(ORK_LIB_NAMES ork_core ork_platform kcp fmt yaml-cpp)

    foreach(_name IN LISTS ORK_LIB_NAMES)
        if(WIN32)
            set(_ork_candidates
                "${ORK_LIBRARY_DIR}/${_name}.lib"
                "${ORK_LIBRARY_DIR}/lib${_name}.a")
        else()
            set(_ork_candidates
                "${ORK_LIBRARY_DIR}/lib${_name}.a"
                "${ORK_LIBRARY_DIR}/${_name}.lib")
        endif()

        set(_ork_resolved "")
        foreach(_candidate IN LISTS _ork_candidates)
            if(EXISTS "${_candidate}")
                set(_ork_resolved "${_candidate}")
                break()
            endif()
        endforeach()

        if(_ork_resolved STREQUAL "")
            set(ORK_FOUND FALSE)
            if(Ork_FIND_REQUIRED)
                message(FATAL_ERROR
                    "Missing Ork static library '${_name}' in:\n"
                    "  ${ORK_LIBRARY_DIR}\n"
                    "Looked for any of:\n"
                    "  ${_ork_candidates}\n"
                    "Nothing named libork_static.a / ork_static.lib was found either, "
                    "so this directory\nholds neither a delivery package nor a split "
                    "development layout.\n"
                    "Re-pack the SDK for '${ORK_PLATFORM}' (see cpp/README.md).")
            endif()
            return()
        endif()

        # yaml-cpp → ORK_LIB_YAML_CPP_PATH
        string(TOUPPER "${_name}" _ork_upper)
        string(REPLACE "-" "_" _ork_upper "${_ork_upper}")
        set(ORK_LIB_${_ork_upper}_PATH "${_ork_resolved}")
    endforeach()

    set(ORK_CORE_LIBRARY     "${ORK_LIB_ORK_CORE_PATH}")
    set(ORK_PLATFORM_LIBRARY "${ORK_LIB_ORK_PLATFORM_PATH}")
endif()

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
# 6. libsodium — ChaCha20-Poly1305 AEAD.  Split fallback layout only: the merged
#    delivery archive already contains it and its probe results are discarded.
#    Only the library is strictly needed:
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
# libsodium is not vendored, and on Windows there is no pkg-config and no
# conventional install prefix to fall back on — a bare 'sodium.lib' sitting next
# to the Ork archives is the most common layout. Adding the triplet directory as
# a hint makes that work without -DSODIUM_ROOT; on macOS/Linux it is simply an
# extra (usually empty of sodium) probe path and changes nothing.
list(APPEND _ork_sodium_hints "${ORK_LIBRARY_DIR}")

find_path(SODIUM_INCLUDE_DIR
    NAMES sodium.h
    HINTS ${_ork_sodium_hints} ${SODIUM_INCLUDE_DIRS}
)
find_library(SODIUM_LIBRARY
    NAMES sodium libsodium
    HINTS ${_ork_sodium_hints} ${SODIUM_LIBRARY_DIRS}
)

# 合并归档形态下 libsodium 已经在归档里，上面这两个探测结果一个字节都不会被
# 链接。留在变量里只会让 configure summary 打出一个外部 libsodium（Windows 上
# 常见命中 vcpkg 的 debug 版），看着像引了错的库。这里直接清空，摘要改由
# ORK_MERGED_ARCHIVE 分支说明「inside the merged archive」。
if(ORK_MERGED_ARCHIVE)
    set(SODIUM_LIBRARY "")
    set(SODIUM_INCLUDE_DIR "")
endif()

if(NOT ORK_MERGED_ARCHIVE AND NOT SODIUM_LIBRARY AND PKG_CONFIG_FOUND)
    pkg_check_modules(_ORK_PKG_SODIUM QUIET libsodium)
    if(_ORK_PKG_SODIUM_FOUND)
        set(SODIUM_LIBRARY "${_ORK_PKG_SODIUM_LINK_LIBRARIES}")
        if(_ORK_PKG_SODIUM_INCLUDE_DIRS AND NOT SODIUM_INCLUDE_DIR)
            set(SODIUM_INCLUDE_DIR "${_ORK_PKG_SODIUM_INCLUDE_DIRS}")
        endif()
    endif()
endif()

# In the merged payload libsodium is already inside the archive, so a missing
# standalone libsodium is not an error there — only the split layout needs it.
if(NOT ORK_MERGED_ARCHIVE AND NOT SODIUM_LIBRARY)
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
#    Windows: Winsock + mswsock (asio), crypt32 (OpenSSL certificate stores).
#             MSVC links the C++ runtime automatically from /MD or /MDd, so no
#             explicit runtime library is appended here — but the Ork archives
#             are built with /MD, and mixing in /MT fails at link time with
#             LNK2038 (RuntimeLibrary mismatch). Keep CMAKE_MSVC_RUNTIME_LIBRARY
#             at its default "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL".
#    Linux: dl + rt for asio timers.
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
# 8b. Delivery metadata + plaintext-bootstrap flag
#
#     ORK_ALLOW_PLAINTEXT_BOOTSTRAP changes the layout of OrkClient and the value
#     of the header-only constants, so it MUST match how the archives were built
#     — a consumer built with ON linking an OFF library is an ODR break, not a
#     link error. Priority:
#
#       1. -DORK_PLAINTEXT_BOOTSTRAP=ON|OFF        (explicit wins)
#       2. manifest.json inside the payload          (authoritative)
#       3. payload form heuristic: merged → OFF, split → ON
#
#     (3) exists because a delivery package is a formal build (plaintext off),
#     while a split layout is exported from a development tree, where the demo's
#     loopback tests need plaintext on.
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
    # string(JSON) 读 JSON 布尔并不返回 "True"/"False"，而是 ON/OFF 之类的文本
    # （CMake 4.1 实测为 "ON"）。旧写法因此永远不匹配，静默走到下面的形态推断
    # 给出 OFF —— 拿 ON 包时会编出与归档不一致的 OrkClient 布局，是 ODR 破坏
    # 而不是链接错误。先归一化成大写再判。
    set(_ork_mp "")
    if(_ork_manifest_plaintext)
        string(TOUPPER "${_ork_manifest_plaintext}" _ork_mp)
    endif()
    if(_ork_mp MATCHES "^(TRUE|ON|1|YES)$")
        set(ORK_PLAINTEXT_BOOTSTRAP ON)
    elseif(_ork_mp MATCHES "^(FALSE|OFF|0|NO)$")
        set(ORK_PLAINTEXT_BOOTSTRAP OFF)
    elseif(ORK_MERGED_ARCHIVE)
        set(ORK_PLAINTEXT_BOOTSTRAP OFF)
    else()
        set(ORK_PLAINTEXT_BOOTSTRAP ON)
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

# Compile definitions that MUST match the configuration the shipped archives
# were built with, otherwise header-only constants (kTlsBootstrapCompiledIn /
# kPlaintextBootstrapCompiledIn) and the conditional members of OrkClient
# (#if defined(ORK_HAVE_TLS)) disagree with the library → ODR / ABI breakage.
set(ORK_COMPILE_DEFINITIONS "")
if(NOT WIN32)
    list(APPEND ORK_COMPILE_DEFINITIONS ORK_HAVE_TLS=1)
endif()
# Static consumption: on Windows the C ABI headers expand OrkExport to
# __declspec(dllimport) unless ORK_STATIC is defined, and dllimport symbols
# cannot be satisfied from a .lib archive → LNK2019 / LNK4217 on every call.
# Defining it unconditionally is harmless on macOS/Linux (ork_export.h ignores
# ORK_STATIC outside _WIN32; visibility stays default).
list(APPEND ORK_COMPILE_DEFINITIONS ORK_STATIC=1)
# The plaintext development bootstrap is a build-time switch of the archives,
# not of the consumer — see 8b for how ORK_PLAINTEXT_BOOTSTRAP is decided.
if(ORK_PLAINTEXT_BOOTSTRAP)
    list(APPEND ORK_COMPILE_DEFINITIONS ORK_ALLOW_PLAINTEXT_BOOTSTRAP=1)
endif()

if(ORK_MERGED_ARCHIVE)
    # Merged delivery payload: one archive, OpenSSL/libsodium/kcp/fmt/yaml-cpp
    # are already inside it. Only system libraries plus the C++ runtime remain.
    # OpenSSL is still located above for its headers (<asio/ssl.hpp>), but the
    # implementation must NOT be linked a second time.
    if(NOT TARGET Ork::Platform)
        add_library(Ork::Platform INTERFACE IMPORTED GLOBAL)
        set_target_properties(Ork::Platform PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ORK_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}")
    endif()

    if(NOT TARGET Ork::Core)
        add_library(Ork::Core STATIC IMPORTED GLOBAL)
        set_target_properties(Ork::Core PROPERTIES
            IMPORTED_LOCATION "${ORK_MERGED_ARCHIVE}"
            INTERFACE_INCLUDE_DIRECTORIES
                "${ORK_INCLUDE_DIR};${OPENSSL_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}"
            INTERFACE_LINK_LIBRARIES
                "Ork::Thirdparty;\
Threads::Threads;\
${ORK_SYSTEM_LIBRARIES}")
    endif()
else()
    if(NOT TARGET Ork::Platform)
        add_library(Ork::Platform STATIC IMPORTED GLOBAL)
        set_target_properties(Ork::Platform PROPERTIES
            IMPORTED_LOCATION "${ORK_PLATFORM_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ORK_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "${ORK_COMPILE_DEFINITIONS}"
            INTERFACE_LINK_LIBRARIES
                "${ORK_LIB_FMT_PATH};${ORK_LIB_YAML_CPP_PATH};${SODIUM_LIBRARY}")
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
${ORK_LIB_KCP_PATH};\
Ork::Thirdparty;\
OpenSSL::SSL;\
OpenSSL::Crypto;\
${SODIUM_LIBRARY};\
Threads::Threads;\
${ORK_SYSTEM_LIBRARIES}")
    endif()
endif()

# Sodium headers are optional (no public Ork header needs them) — publish the
# include dir on Ork::Core only when it was actually found.
if(SODIUM_INCLUDE_DIR AND EXISTS "${SODIUM_INCLUDE_DIR}/sodium.h")
    set_property(TARGET Ork::Core APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}")
endif()

set(ORK_FOUND TRUE)

if(ORK_MERGED_ARCHIVE)
    set(_ork_required_vars ORK_MERGED_ARCHIVE ORK_INCLUDE_DIR)
else()
    set(_ork_required_vars
        ORK_CORE_LIBRARY ORK_PLATFORM_LIBRARY ORK_INCLUDE_DIR SODIUM_LIBRARY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ork
    REQUIRED_VARS ${_ork_required_vars}
    VERSION_VAR ORK_ABI_VERSION
    FAIL_MESSAGE "Could not find the pre-built Ork SDK")

if(NOT Ork_FIND_QUIETLY)
    message(STATUS "Ork SDK — C ABI version ${ORK_ABI_VERSION}, platform ${ORK_PLATFORM}")
    message(STATUS "  headers : ${ORK_INCLUDE_DIR}")
    message(STATUS "  vendored: ${ORK_THIRDPARTY_INCLUDE_DIR}")
    message(STATUS "  libs    : ${ORK_LIBRARY_DIR}")
    if(ORK_MERGED_ARCHIVE)
        message(STATUS "  payload : merged archive (${ORK_MERGED_ARCHIVE})")
    else()
        message(STATUS "  payload : split archives (ork_core + ork_platform + 3 vendored)")
    endif()
    if(ORK_MANIFEST_SDK_ID)
        message(STATUS "  package : ${ORK_MANIFEST_SDK_ID} — built from "
                       "${ORK_MANIFEST_GIT_SHA}")
    else()
        message(STATUS "  package : no manifest.json next to the payload "
                       "(provenance unknown)")
    endif()
    message(STATUS "  OpenSSL : ${OPENSSL_VERSION} (${OPENSSL_INCLUDE_DIR})")
    if(SODIUM_LIBRARY)
        message(STATUS "  sodium  : ${SODIUM_LIBRARY}")
    else()
        message(STATUS "  sodium  : inside the merged archive")
    endif()
    message(STATUS "  plaintext bootstrap: ${ORK_PLAINTEXT_BOOTSTRAP} "
                   "(must match the archives; override with -DORK_PLAINTEXT_BOOTSTRAP=ON|OFF)")
endif()
