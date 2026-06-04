#!/usr/bin/env bash
# Build the Firebird DuckDB extension and append the metadata footer that turns
# the raw shared library into a loadable `.duckdb_extension` file.
#
# Output: build/firebird.duckdb_extension
#
# Environment overrides (used by CI):
#   DUCKDB_PLATFORM  Force the platform string in the footer (e.g. linux_arm64).
#   BUILD_TARGET     Cargo target triple for cross-compilation
#                    (e.g. aarch64-unknown-linux-gnu); output is read from
#                    target/<triple>/release instead of target/release.
set -euo pipefail

cd "$(dirname "$0")/.."

# C API version the extension targets. Must match the macro's min_duckdb_version
# in src/lib.rs. The same binary works across all DuckDB releases sharing this
# C API version (e.g. 1.4.x and 1.5.x).
C_API_VERSION="v1.2.0"
EXT_NAME="firebird"
EXT_VERSION="$(grep -m1 '^version' Cargo.toml | sed -E 's/.*"(.*)".*/\1/')"

# Detect the DuckDB platform string for the metadata footer.
detect_platform() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "$os" in
    Darwin) case "$arch" in
        arm64|aarch64) echo "osx_arm64" ;;
        x86_64)        echo "osx_amd64" ;;
        *) echo "unknown_$arch" ;;
      esac ;;
    Linux) case "$arch" in
        x86_64|amd64)  echo "linux_amd64" ;;
        aarch64|arm64) echo "linux_arm64" ;;
        *) echo "unknown_$arch" ;;
      esac ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "windows_amd64" ;;
    *) echo "unknown_${os}_${arch}" ;;
  esac
}

PLATFORM="${DUCKDB_PLATFORM:-$(detect_platform)}"
TARGET="${BUILD_TARGET:-}"

# Prefer python3, fall back to python (Windows runners ship `python`).
PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  echo "ERROR: python3/python not found (needed for the metadata footer)" >&2
  exit 1
fi

# The macro derives the C entrypoint version from this env var at compile time.
export DUCKDB_EXTENSION_MIN_DUCKDB_VERSION="$C_API_VERSION"

echo "==> Building $EXT_NAME v$EXT_VERSION for $PLATFORM (C API $C_API_VERSION)${TARGET:+, target $TARGET}"
if [[ -n "$TARGET" ]]; then
  rustup target add "$TARGET" 2>/dev/null || true
  cargo build --release --target "$TARGET"
  RELDIR="target/$TARGET/release"
else
  cargo build --release
  RELDIR="target/release"
fi

# Locate the freshly built dynamic library (.dylib / .so / .dll).
LIB=""
for candidate in \
  "$RELDIR/lib${EXT_NAME}.dylib" \
  "$RELDIR/lib${EXT_NAME}.so" \
  "$RELDIR/${EXT_NAME}.dll"; do
  if [[ -f "$candidate" ]]; then LIB="$candidate"; break; fi
done
if [[ -z "$LIB" ]]; then
  echo "ERROR: could not find built library in $RELDIR" >&2
  exit 1
fi

mkdir -p build
OUT="build/${EXT_NAME}.duckdb_extension"

echo "==> Appending extension metadata (from $LIB)"
"$PY" scripts/append_extension_metadata.py \
  -l "$LIB" \
  -n "$EXT_NAME" \
  -dv "$C_API_VERSION" \
  -ev "$EXT_VERSION" \
  --abi-type C_STRUCT \
  -p "$PLATFORM" \
  -o "$OUT"

echo "==> Done: $OUT"
