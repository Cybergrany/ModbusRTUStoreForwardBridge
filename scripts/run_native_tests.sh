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

"$compiler" \
  -std=c++11 \
  -Os \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic-errors \
  -I"$repo_dir/include" \
  "$repo_dir/test/engine_main.cpp" \
  -o "$build_dir/engine_main"

"$build_dir/engine_main"

"$compiler" \
  -std=c++11 \
  -O2 \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic-errors \
  -I"$repo_dir/include" \
  "$repo_dir/test/perf_main.cpp" \
  -o "$build_dir/perf_main"

if command -v taskset >/dev/null 2>&1; then
  affinity_cpu="${MBUS_RTU_BRIDGE_PERF_CPU:-}"
  if [[ -z "$affinity_cpu" ]]; then
    affinity_cpu="$(awk '/Cpus_allowed_list:/ { split($2, ranges, ","); split(ranges[1], first, "-"); print first[1]; exit }' /proc/self/status)"
  fi
  taskset -c "$affinity_cpu" "$build_dir/perf_main"
else
  "$build_dir/perf_main"
fi
