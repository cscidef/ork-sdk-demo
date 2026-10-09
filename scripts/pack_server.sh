#!/usr/bin/env bash
#
# pack_server.sh — Package the Ork demo server into a self-contained directory.
#
# Source : ${ORK_ROOT}/build-demo  (embed_minimal + ork.bundle)
# Dest   : server/macos-arm64/{bin,lib,bundle}
#
# The produced bin/embed_minimal has every non-system dylib copied into lib/
# and all install names rewritten to @executable_path/../lib (binary) or
# @loader_path (sibling dylibs), then re-signed with an ad-hoc signature
# (install_name_tool invalidates code signatures on arm64).
#
# Robustness notes:
#   * otool -L prints the Mach-O path as a header on line 1 -> always skip it.
#   * dylib paths may be absolute (/opt/homebrew/...) or already relative
#     (@rpath/@loader_path/@executable_path). Only absolute non-system paths
#     are collected and rewritten; relative ones are left untouched.
#   * Transitive dependencies are resolved (e.g. libssl -> libcrypto).
#
set -euo pipefail

# --- Paths -------------------------------------------------------------------
ORK_ROOT="${ORK_ROOT:-/Users/jonathan/hadoWorld/HADO-PROJ/harmonia_ork}"
BUILD_DIR="${BUILD_DIR:-${ORK_ROOT}/build-demo}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEMO_ROOT="$(dirname "$SCRIPT_DIR")"
DEST="${DEMO_ROOT}/server/macos-arm64"

SRC_BIN="${BUILD_DIR}/source/examples/echo_server/embed_minimal"
SRC_BUNDLE="${BUILD_DIR}/source/core/bundle/ork.bundle"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "ERROR: pack_server.sh must run on macOS (uses otool/install_name_tool/codesign)." >&2
    exit 1
fi

if [[ ! -f "$SRC_BIN" ]]; then
    echo "ERROR: embed_minimal not found at: $SRC_BIN" >&2
    echo "       Build it first:  cmake --build ${BUILD_DIR} --target embed_minimal" >&2
    exit 1
fi

# --- Helpers -----------------------------------------------------------------
# Print the absolute, non-system dylib dependencies of a Mach-O file.
# Skips the otool header line and any already-relative install names.
list_external_deps() {
    local macho="$1"
    otool -L "$macho" \
        | tail -n +2 \
        | awk '{print $1}' \
        | grep -Ev '^(/usr/lib|/System|@)' \
        || true
}

# --- 1. Clean + copy the binary ---------------------------------------------
echo "=== [1/6] Copy embed_minimal ==="
rm -rf "${DEST}/bin" "${DEST}/lib"
mkdir -p "${DEST}/bin" "${DEST}/lib"
cp "${SRC_BIN}" "${DEST}/bin/"
BIN="${DEST}/bin/embed_minimal"
chmod +x "${BIN}"

# --- 2. Collect dylibs transitively -----------------------------------------
# NOTE: macOS ships bash 3.2 (no associative arrays), so we track state in
# temp files:
#   LIST_FILE   : every original absolute install path seen (may contain both
#                 the /opt/homebrew/opt/... symlink form AND the /opt/homebrew/
#                 Cellar/... real form for the SAME dylib). All variants are
#                 recorded so install_name_tool -change can rewrite each one.
#   COPIED_FILE : basenames already copied into lib/ (dedup so we cp once).
# Homebrew dylibs are read-only (0444); we chmod u+w after copying so that
# install_name_tool and codesign can modify them.
echo "=== [2/6] Collect non-system dylibs (transitive) ==="
LIST_FILE="$(mktemp -t orkdeps)"
COPIED_FILE="$(mktemp -t orkcopied)"
trap 'rm -f "$LIST_FILE" "$COPIED_FILE"' EXIT

queue=("${BIN}")
qi=0
while [ "$qi" -lt "${#queue[@]}" ]; do
    current="${queue[$qi]}"
    qi=$((qi + 1))
    while IFS= read -r dep; do
        [ -z "$dep" ] && continue
        base="$(basename "$dep")"
        # Record this path variant for the rewrite pass (dedup exact match).
        if ! grep -Fxq "$dep" "$LIST_FILE"; then
            echo "$dep" >> "$LIST_FILE"
        fi
        # Copy only once per basename.
        if grep -Fxq "$base" "$COPIED_FILE"; then
            continue
        fi
        if [ ! -f "$dep" ]; then
            echo "WARNING: dependency not found on disk, skipping: $dep" >&2
            continue
        fi
        echo "$base" >> "$COPIED_FILE"
        cp "$dep" "${DEST}/lib/"
        chmod u+w "${DEST}/lib/${base}"
        # Recurse into the freshly copied dylib to catch its own deps.
        queue+=("${DEST}/lib/${base}")
        echo "  + ${base}"
    done < <(list_external_deps "$current")
done

if [ ! -s "$COPIED_FILE" ]; then
    echo "  (no external dylibs to bundle)"
fi

# --- 3. Rewrite install names in the binary ---------------------------------
echo "=== [3/6] Rewrite binary install names -> @executable_path/../lib ==="
while IFS= read -r orig; do
    [ -z "$orig" ] && continue
    base="$(basename "$orig")"
    install_name_tool -change "$orig" "@executable_path/../lib/${base}" "${BIN}" 2>/dev/null || true
done < "$LIST_FILE"

# --- 4. Rewrite sibling cross-refs + ids inside each dylib ------------------
echo "=== [4/6] Rewrite dylib cross-references -> @loader_path ==="
for dylib in "${DEST}"/lib/*.dylib; do
    [ -e "$dylib" ] || continue
    self_base="$(basename "$dylib")"
    # Cross references to any other bundled dylib become @loader_path/<base>.
    while IFS= read -r orig; do
        [ -z "$orig" ] && continue
        base="$(basename "$orig")"
        install_name_tool -change "$orig" "@loader_path/${base}" "$dylib" 2>/dev/null || true
    done < "$LIST_FILE"
    # Fix the dylib's own install name (id).
    install_name_tool -id "@loader_path/${self_base}" "$dylib" 2>/dev/null || true
done

# --- 5. Re-sign everything (arm64 requires a valid signature) ---------------
echo "=== [5/6] Codesign (ad-hoc) ==="
for dylib in "${DEST}"/lib/*.dylib; do
    [[ -e "$dylib" ]] || continue
    codesign --force --sign - "$dylib"
done
codesign --force --sign - "${BIN}"

# --- 6. Copy ork.bundle ------------------------------------------------------
echo "=== [6/6] Copy ork.bundle ==="
rm -rf "${DEST}/bundle"
mkdir -p "${DEST}/bundle"
if [[ -d "$SRC_BUNDLE" ]]; then
    cp -R "${SRC_BUNDLE}" "${DEST}/bundle/"
    echo "  + ork.bundle"
else
    echo "  WARNING: ork.bundle not found at ${SRC_BUNDLE} (skipped)" >&2
fi

echo ""
echo "=== Pack complete ==="
echo "Binary : ${BIN}"
echo "Libs   : ${DEST}/lib"
echo "Bundle : ${DEST}/bundle"
echo ""
echo "Verify with:  otool -L ${BIN}"
