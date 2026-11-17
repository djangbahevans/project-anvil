#!/usr/bin/env bash
# Formats the C++ sources with clang-format, or with --check only reports files that differ.
# The generated proxy identifier table is left alone.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mapfile -t files < <(find src tests bench \( -name '*.cpp' -o -name '*.hpp' \) | grep -v proxy_ids)

if [ "${1:-}" = "--check" ]; then
    clang-format --dry-run -Werror "${files[@]}"
else
    clang-format -i "${files[@]}"
fi
