#!/bin/bash
set -eux

./tools/scripts/format_build_files.sh -t
./tools/scripts/format_cxx.sh -t
./tools/scripts/lint_sh.sh -t
./tools/scripts/generate_compdb_targets.sh -t
./tools/scripts/build_compilation_db.sh

err=0

# Disable leak sanatizer. Does not work in docker
# https://github.com/google/sanitizers/issues/764

ASAN_SYMBOLIZER_PATH="$(bazel info output_base)/external/clang_6_0_0_linux/bin/llvm-symbolizer"
export ASAN_SYMBOLIZER_PATH

bazel test --config=ci --config=dbg --config=sanitize //... --test_output=errors --test_env="ASAN_OPTIONS=detect_leaks=0" --test_env="UBSAN_OPTIONS=print_stacktrace=1" || err=$?

mkdir -p /log/junit
find bazel-testlogs/ -name '*.xml' | while read -r line; do
    # shellcheck disable=SC2001
    cp "$line" /log/junit/"$(echo "${line#bazel-testlogs/}" | sed s,/,_,g)"
done

if [ "$err" -ne 0 ]; then
    exit "$err"
fi
