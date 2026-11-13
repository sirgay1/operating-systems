#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/lab2-build.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build_dir/bin"
cmake --build "$build_dir" "$@"
for binary in "$build_dir"/bin/{host,client}_*; do
    install -m 755 "$binary" "$project_dir/"
done
