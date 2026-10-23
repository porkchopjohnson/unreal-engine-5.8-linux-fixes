#!/usr/bin/env bash
set -euo pipefail

ENGINE_ROOT="$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")"

export SDL_VIDEO_DRIVER=x11
export SDL_VIDEODRIVER=x11
export SDL_MOUSE_FOCUS_CLICKTHROUGH="${SDL_MOUSE_FOCUS_CLICKTHROUGH:-1}"

# The in-tree Hub resolves its bundled project relative to this directory.
cd -- "$ENGINE_ROOT/Engine/Binaries/Linux"
exec ./LiveLinkHub "$@"
