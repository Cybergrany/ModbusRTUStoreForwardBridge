#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/modbus-store-forward-bridge-native"
mkdir -p "$build_dir"

compiler="${CXX:-g++}"
"$compiler" \
  -std=c++11 \
  -Os \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic-errors \
  -I"$repo_dir/include" \
  "$repo_dir/test/test_main.cpp" \
  -o "$build_dir/test_main"

"$build_dir/test_main"

