#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Darwin ]; then
    printf '%s\n' 'This script requires macOS. Use scripts/linux.sh for containers.' >&2
    exit 1
fi
xcode-select -p
xcrun --find clang
xcrun --show-sdk-path
mkdir -p "$ROOT/.dev/macos/cache/pip"
export PIP_CACHE_DIR="$ROOT/.dev/macos/cache/pip"
export XDG_CACHE_HOME="$ROOT/.dev/macos/cache"
if [ ! -x "$ROOT/.dev/macos/python/bin/python" ]; then
    /usr/bin/python3 -m venv "$ROOT/.dev/macos/python"
fi
"$ROOT/.dev/macos/python/bin/pip" install -r "$ROOT/scripts/bootstrap-requirements.txt"
exec "$ROOT/.dev/macos/python/bin/python" "$ROOT/scripts/bootstrap.py" --platform macos "$@"
