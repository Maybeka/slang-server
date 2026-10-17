#!/usr/bin/env bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=${1:-"$here/../.."}
source_root=$(CDPATH= cd -- "$source_root" && pwd)
build_dir="$here/build"
mkdir -p "$build_dir"
compiler=${CXX:-c++}
link_flags=()
compile_flags=()
if [[ $(uname -s) == Darwin ]]; then
    compile_flags=(-isysroot "$(xcrun --show-sdk-path)")
    link_flags=(-Wl,-adhoc_codesign -framework Security -framework CoreFoundation)
fi
set -x
"$compiler" "${compile_flags[@]}" -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$source_root/include" \
    "$source_root/src/diagnostics/DiagnosticProcess.cpp" \
    "$here/diagnostic_transport_tests.cpp" \
    -o "$build_dir/diagnostic_transport_tests" "${link_flags[@]}"
if [[ $(uname -s) == Darwin ]]; then
    "$compiler" "${compile_flags[@]}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
        "$here/replacement_worker.cpp" -Wl,-adhoc_codesign -o "$build_dir/replacement_worker"
    # Ad-hoc signing makes the intended supported build explicit on both Intel
    # and Apple Silicon; this does not install software or change trust settings.
    /usr/bin/codesign --force --sign - "$build_dir/diagnostic_transport_tests"
    /usr/bin/codesign --force --sign - "$build_dir/replacement_worker"
    cp "$build_dir/diagnostic_transport_tests" "$build_dir/diagnostic_transport_unsigned"
    /usr/bin/codesign --remove-signature "$build_dir/diagnostic_transport_unsigned"
fi
"$build_dir/diagnostic_transport_tests"
