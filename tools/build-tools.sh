#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-"$root/build-tools"}
mkdir -p "$out"

command -v pkg-config >/dev/null 2>&1 || {
  echo "build-tools: pkg-config is required" >&2
  exit 2
}
pkg-config --exists libheif || {
  echo "build-tools: libheif development files are required" >&2
  exit 2
}

cc=${CC:-cc}
read -r -a cflags <<<"$(pkg-config --cflags libheif)"
read -r -a libs <<<"$(pkg-config --libs libheif)"

"$cc" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  "${cflags[@]}" \
  "$root/src/csharp_inspect.c" \
  "$root/src/input_classify.c" \
  -o "$out/csharp-inspect" \
  "${libs[@]}"

printf 'built: %s\n' "$out/csharp-inspect"
