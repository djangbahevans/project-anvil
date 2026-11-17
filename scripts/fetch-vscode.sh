#!/usr/bin/env bash
# Fetches the pinned VS Code release into .deps/vscode and transpiles it so the upstream
# extension host can run from out/. Dependency install scripts are skipped (--ignore-scripts);
# the two native modules the extension host loads are then built, which needs make, a C++
# compiler and Python. Without them the extension host still runs but writes no log files and
# does not watch its parent process, so the script fails when they are missing. Set
# ANVIL_ALLOW_MISSING_NATIVE_MODULES=1 to continue with a warning instead.
set -euo pipefail

VSCODE_TAG="1.146.0"

# package:binary pairs; the binary is build/Release/<binary>.node inside the package.
NATIVE_MODULES=("@vscode/spdlog:spdlog" "@vscode/native-watchdog:watchdog")

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

native_binary() {
    echo "node_modules/${1%%:*}/build/Release/${1##*:}.node"
}

# The modules are built one after the other: node-gyp unpacks the same header tarball into one
# shared cache directory, and two builds at once can read a half-written header and fail to
# compile. A module that is already built is left alone; delete its build directory to force a
# rebuild. A transient failure, such as a download error, is retried once.
for module in "${NATIVE_MODULES[@]}"; do
    package="${module%%:*}"
    for attempt in 1 2; do
        if [ -f "$(native_binary "$module")" ]; then
            break
        fi
        pnpm rebuild "$package" || echo "warning: building $package failed (attempt $attempt)" >&2
    done
done

missing=()
for module in "${NATIVE_MODULES[@]}"; do
    if [ ! -f "$(native_binary "$module")" ]; then
        missing+=("${module%%:*}")
    fi
done
if [ "${#missing[@]}" -gt 0 ]; then
    if [ "${ANVIL_ALLOW_MISSING_NATIVE_MODULES:-}" = "1" ]; then
        echo "warning: native modules not built: ${missing[*]}; extension logging is limited" >&2
    else
        echo "error: native modules not built: ${missing[*]}" >&2
        echo "They need make, a C++ compiler and Python. Set ANVIL_ALLOW_MISSING_NATIVE_MODULES=1 to continue without them." >&2
        exit 1
    fi
fi

node build/next/index.ts transpile
