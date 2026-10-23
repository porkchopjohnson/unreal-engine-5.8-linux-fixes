# Unreal Engine 5.8 Linux Fixes

Patches, XWayland launchers, and build notes for **Unreal Engine 5.8.3** on a KDE Plasma Wayland desktop.

This repository records the fixes used in a local Fedora/Linux source build. It contains focused patches rather than an Unreal Engine checkout, assets, compiled libraries, or machine-specific build manifests.

## Included fixes

| Change | Files | Validation |
| --- | --- | --- |
| Respect Slate's activation policy when creating native windows | `LinuxWindow.cpp` | Built; the original approach passed tooltip single-click testing in 5.7.4. The 5.8.3 tree already had explicit non-focusable tooltips. |
| Raise notification toasts without activating them | `LinuxApplication.cpp` | Built; addresses the focus/raise feedback loop reproduced with overlapping notifications. |
| Repeatable notification test | `MessageTesting.cpp` | `Slate.TestNotification` adds a toast after three seconds, with a Dismiss button and a 60-second expiry. |
| Clean up retained Vulkan swapchain resources when recreation is skipped | `VulkanViewport.cpp` | Built; the first repeated drag/dialog retest in 5.8.3 completed without freezing or crashing. Longer-term testing is still needed. |
| Build the standalone Live Link Hub launcher against existing engine modules | `LiveLinkHubLauncher.Build.cs` | Built and started successfully through XWayland. Capture-device workflows require separate testing. |
| Consistent editor and Hub launch settings | Shell launchers | XWayland startup verified. Editor launcher supports both the project browser and explicit project paths. |

The optional custom-plugin patch records the 5.8 migration of `ClothVertexColorTools` and `PoseAssetOffsetTools`. It is not needed for the engine fixes.

## What was already in the 5.8.3 source

The source archive used as the baseline already contained:

- Explicit non-focusable tooltip creation.
- Tooltip/menu parenting and SDL popup roles.
- `SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH=1`.
- Additional Linux window and Vulkan surface lifecycle handling.

Those implementations are retained. These patches are the remaining local changes, not a replacement of the newer window handling with older 5.7 code.

## Requirements

- Your own UE 5.8.3 source tree and downloaded dependencies.
- Git for applying patches.
- Bash and Linux `readlink` for the launchers.
- Python 3 only if using the patch verification tool.

## Apply the patches

Run from the **engine source root** and set `FIXES_ROOT` to this repository's location:

```bash
FIXES_ROOT=/path/to/unreal-engine-5.8-linux-fixes

git apply --check "$FIXES_ROOT"/patches/000*.patch
git apply "$FIXES_ROOT"/patches/000*.patch

install -m 755 "$FIXES_ROOT/scripts/RunEditorXWayland.sh" ./RunEditorXWayland.sh
install -m 755 "$FIXES_ROOT/scripts/RunLiveLinkHubXWayland.sh" ./RunLiveLinkHubXWayland.sh
```

The patches are ordered:

1. Slate/native focus policy and notification raising.
2. Notification test command.
3. Vulkan swapchain cleanup.
4. Live Link Hub launcher build setting.

They have been checked against the original `UnrealEngine-5.8.3-release.zip` used for this build. If `git apply --check` reports a conflict, inspect the source differences before applying. Do not apply them a second time to an already patched tree.

## Build and launch

See [BUILDING.md](BUILDING.md) for initial builds, incremental build scope, and Live Link Hub's target receipt.

Open the editor's project browser:

```bash
./RunEditorXWayland.sh
```

Open a particular project:

```bash
./RunEditorXWayland.sh "/path/to/MyProject.uproject"
```

Start Live Link Hub after building it:

```bash
./RunLiveLinkHubXWayland.sh
```

Both launchers select SDL's X11 backend for XWayland and enable focus-click-through. The editor launcher appends `-norecentproject` so a no-project launch shows the browser instead of reopening the last project. Explicit project arguments remain first so Unreal parses them correctly.

Confirm the backend in the startup log:

```text
Using SDL video driver 'x11'
```

The desktop itself remains on Wayland. A conflicting `-sdlvideodriver=wayland` argument overrides the environment in this source version, so omit it when testing XWayland.

## Notification focus problem

Slate marks standard notifications with `EWindowActivationPolicy::Never`. The native window must respect that policy as well. Non-focusable does not mean mouse-transparent: notification buttons and hyperlinks must remain clickable.

Raising a notification must also be a stacking operation rather than an activation request. Otherwise a focus event can raise another notification, which gains focus and raises the notifications again. The failing investigation recorded hundreds of thousands of focus events and severe lag with overlapping toasts.

The fix maps Slate's policy to `SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN` and temporarily disables `SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED` around notification raising.

## Menu focus-restoring clicks

SDL normally discards clicks that activate a window. The launcher sets:

```bash
SDL_MOUSE_FOCUS_CLICKTHROUGH=1
```

This lets the click returning from a menu reach the editor. Menus retain focus and keyboard navigation; they are not treated like non-focusable tooltips. This also permits focus-restoring clicks to perform their normal UI action.

## Vulkan drag/dialog crash

Two crash reports from the same 5.8.3 project failed at:

```text
Assertion failed: RecreateInfo.Surface == nullptr
Runtime/VulkanRHI/Private/VulkanViewport.cpp:351
```

During swapchain recreation, the old swapchain and surface can be retained for reuse. If the window becomes hidden or invalid before creation, the creation step is skipped. The retained handles were then left behind, violating the caller's ownership checks.

The patch moves the existing cleanup so it also runs for this skipped-creation path. It destroys retained resources in the existing order and clears their handles. The assertions remain enabled. It does not merely suppress the assertion or disable thumbnails.

The first interactive retest passed, but the failure is intermittent. This is not evidence that every Linux GPU or drag-and-drop crash is fixed. With Vulkan verbose logging, the cleanup branch reports:

```text
Releasing unused swapchain recreation resources for window ...
```

## Validation checklist

1. Verify that you launched the patched executable with the `x11` SDL backend.
2. Ensure `Slate.EnableTooltips` and `Slate.bAllowNotifications` are enabled. Old project overrides setting either to zero can mask the symptoms.
3. Hover assets until tooltips appear, then test single-click selection.
4. Run `Slate.TestNotification` once and twice; check notification buttons and editor interaction while toasts are visible.
5. Open File/Edit menus, dismiss them with an outside click, and test submenus, arrow keys, and Escape.
6. Test dialogs relevant to your workflow.
7. Repeatedly drag meshes into a level and open/cancel dialogs, watching for the swapchain assertion.

`Slate.TestNotification` is a local diagnostic added by patch 0002, not a stock UE 5.8.3 command.

## Verify patches against an archive

The verifier extracts only the source files touched by the core patches into a temporary directory, checks and applies the patches there, and optionally compares the result with a patched tree:

```bash
python3 tools/verify_patches.py \
    --archive /path/to/UnrealEngine-5.8.3-release.zip \
    --compare-tree /path/to/UnrealEngine-5.8.3-release
```

It does not patch the archive or your engine tree. To check the optional custom-plugin patch too, supply `--custom-plugin-baseline` pointing to a tree containing the earlier 5.7 plugin sources.

## Scope

- Targeted and built for UE 5.8.3 on Linux, using KDE Plasma's Wayland session with Unreal through XWayland.
- These changes do not constitute a native-Wayland port.
- Notifications remain Slate notifications, not operating-system notifications.
- No compositor fork or desktop-wide window rule is required.
- Functional plugin tools, capture hardware, other compositors, and every dialog have not been exhaustively tested.

See [NOTICE.md](NOTICE.md) for source and licensing context.
