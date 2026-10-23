#!/usr/bin/env bash
set -euo pipefail

ENGINE_ROOT="$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")"

export SDL_VIDEO_DRIVER=x11
export SDL_VIDEODRIVER=x11
# Deliver the click that restores focus from a separate popup menu window.
export SDL_MOUSE_FOCUS_CLICKTHROUGH="${SDL_MOUSE_FOCUS_CLICKTHROUGH:-1}"

# Show the project browser instead of auto-loading the last project when no
# project path is supplied. Keep explicit project arguments first.
exec "$ENGINE_ROOT/Engine/Binaries/Linux/UnrealEditor" "$@" -norecentproject
