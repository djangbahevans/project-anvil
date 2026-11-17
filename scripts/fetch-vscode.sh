#!/usr/bin/env bash
# Fetches the pinned VS Code release into .deps/vscode and transpiles it so the upstream
# extension host can run from out/. Dependency install scripts are skipped (--ignore-scripts);
# the two native modules the extension host loads are then built, which needs make, a C++
# compiler and Python. Without them the extension host still runs but writes no log files and
# does not watch its parent process.
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
if ! pnpm rebuild @vscode/spdlog @vscode/native-watchdog; then
    echo "warning: the native modules did not build; extension logging is limited" >&2
fi
node build/next/index.ts transpile
