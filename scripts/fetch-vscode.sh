#!/usr/bin/env bash
# Fetches the pinned VS Code release into .deps/vscode and transpiles it so the upstream
# extension host can run from out/. Native modules are skipped (--ignore-scripts); the
# extension host tolerates their absence.
set -euo pipefail

VSCODE_TAG="1.146.0"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.deps/vscode"

if [ ! -d "$dest/.git" ]; then
    git clone --depth 1 --branch "$VSCODE_TAG" https://github.com/microsoft/vscode.git "$dest"
fi

actual="$(git -C "$dest" describe --tags --exact-match 2>/dev/null || true)"
if [ "$actual" != "$VSCODE_TAG" ]; then
    echo "error: $dest is at '${actual:-an untagged commit}', expected $VSCODE_TAG" >&2
    exit 1
fi

cd "$dest"
pnpm install --ignore-scripts --frozen-lockfile
(cd build && pnpm install --ignore-scripts --frozen-lockfile)
node build/next/index.ts transpile
