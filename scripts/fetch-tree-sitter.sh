#!/usr/bin/env bash
# Fetches the pinned tree-sitter runtime and the grammars Anvil highlights with into
# .deps/tree-sitter/<name>/. Each source is a release archive whose SHA-256 is checked before it
# is unpacked, so every machine builds the same bytes. With --check nothing is downloaded: the
# script verifies that what is there is what the pins describe and fails otherwise.
#
# Adding a grammar: add a line to SOURCES and see docs/howto/adding-a-grammar.md.
set -euo pipefail

# name|archive url|SHA-256 of the archive
SOURCES=(
    "runtime|https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.2.tar.gz|356528363bf7fbad12a2cfeb5a2678c0118ef886a4d69ad9a379db5fdef0735f"
    "json|https://github.com/tree-sitter/tree-sitter-json/archive/refs/tags/v0.24.8.tar.gz|acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213"
    "c|https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.1.tar.gz|25dd4bb3dec770769a407e0fc803f424ce02c494a56ce95fedc525316dcf9b48"
    "cpp|https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz|7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051"
)

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.deps/tree-sitter"

check_only=0
case "${1:-}" in
    "") ;;
    --check) check_only=1 ;;
    *)
        echo "usage: $0 [--check]" >&2
        exit 2
        ;;
esac

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# The archive stays next to its directory; the directory's stamp names the hash it was unpacked
# from, so a directory that was edited or left from another pin is noticed by --check.
verify() {
    local name="$1" expected="$2"
    local archive="$dest/$name.tar.gz"
    if [ ! -f "$archive" ]; then
        echo "error: $archive is missing; run scripts/fetch-tree-sitter.sh" >&2
        return 1
    fi
    local actual
    actual="$(sha256_of "$archive")"
    if [ "$actual" != "$expected" ]; then
        echo "error: $archive has SHA-256 $actual, expected $expected" >&2
        return 1
    fi
    if [ "$(cat "$dest/$name/.anvil-sha256" 2>/dev/null || true)" != "$expected" ]; then
        echo "error: $dest/$name was not unpacked from the pinned archive; run scripts/fetch-tree-sitter.sh" >&2
        return 1
    fi
}

mkdir -p "$dest"
status=0
for source in "${SOURCES[@]}"; do
    IFS='|' read -r name url expected <<<"$source"
    if [ "$check_only" = 1 ]; then
        verify "$name" "$expected" || status=1
        continue
    fi
    if verify "$name" "$expected" 2>/dev/null; then
        continue
    fi
    archive="$dest/$name.tar.gz"
    curl --fail --silent --show-error --location --retry 3 --output "$archive.part" "$url"
    actual="$(sha256_of "$archive.part")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$archive.part"
        echo "error: $url has SHA-256 $actual, expected $expected" >&2
        exit 1
    fi
    mv "$archive.part" "$archive"
    rm -rf "${dest:?}/$name"
    mkdir "$dest/$name"
    tar -xzf "$archive" -C "$dest/$name" --strip-components=1
    echo "$expected" >"$dest/$name/.anvil-sha256"
done
exit "$status"
