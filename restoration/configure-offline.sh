#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 2 ]]; then
  echo 'Usage: configure-offline.sh DEPENDENCY_ROOT BUILD_DIR [additional CMake arguments...]' >&2
  exit 2
fi
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
deps=$(CDPATH= cd -- "$1" && pwd)
build_dir=$2
shift 2
"${PYTHON:-python3}" "$source_root/restoration/prepare-main-sdk.py" --deps-root "$deps"
"${CMAKE:-cmake}" -S "$source_root" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSLANG_USE_MIMALLOC=OFF \
  -DSLANG_SERVER_INCLUDE_INSTALL=OFF \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2="$deps/catch2-src" \
  -DFETCHCONTENT_SOURCE_DIR_FMT="$deps/fmt-src" \
  -DFETCHCONTENT_SOURCE_DIR_BOOST_REGEX="$deps/boost_regex-src" \
  -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS="$deps/tomlplusplus-src" \
  "$@"
