# Linux desktop-portal file chooser

Patch `0005-kde-portal-file-dialogs.patch` routes Linux `DesktopPlatform` Open, Save, and directory-selection calls through `org.freedesktop.portal.FileChooser`. On KDE Plasma, this uses KDE's native file chooser and its Places sidebar.

## Requirements

- The UE 5.8.3 source and the portal patch.
- `xdg-desktop-portal` and a file-chooser backend, such as `xdg-desktop-portal-kde` on Plasma.
- The runtime sd-bus API provided by `libsystemd.so.0`.
- KIO/FUSE support if remote selections need to be made available through a local filesystem path.

The implementation loads sd-bus at runtime, so it requires neither systemd development headers nor a new link-time dependency. If that library or the portal is unavailable, it falls back to the existing Slate picker.

## Source files

```text
Engine/Source/Developer/DesktopPlatform/Private/Linux/DesktopPlatformLinux.cpp
Engine/Source/Developer/DesktopPlatform/Private/Linux/PortalFileDialog.cpp
Engine/Source/Developer/DesktopPlatform/Private/Linux/PortalFileDialog.h
```

The two `PortalFileDialog` files are new. The patch includes the complete integration and the local diagnostic commands; copying only the modified caller is insufficient.

## Supported behavior

- Open-file and multiple-file selection.
- Save-file and directory selection.
- Initial folders and suggested save names.
- Unreal's description/extension filter pairs and returned selected filter index.
- Case-insensitive simple extension matching through expanded portal globs.
- Percent-decoded local paths, including UTF-8 names, spaces, and literal plus signs.
- Cancellation without a second Slate picker appearing.

A private session-bus connection handles each request. The response subscription is installed before opening the picker. Unreal's synchronous dialog call waits for the asynchronous portal response while servicing SDL's native window-manager messages.

On the tested XWayland configuration, the portal receives the parent editor's X11 window identifier. A Wayland identifier is used if SDL has already exported one, but native-Wayland parenting has not been validated here.

The portal returns local `file://` URIs. Remote selections need to be exposed as locally accessible files by the desktop backend, potentially through KDE's KIO/FUSE integration. Direct `smb://` URLs are not passed to Unreal importers. Specific remote services and devices still need workflow testing.

Specialized in-editor asset pickers that do not use `DesktopPlatform` remain separate from this integration.

## Build

For a fresh source build, apply the numbered patches before building the editor as described in [BUILDING.md](BUILDING.md).

If patches 0001–0004 are already applied, apply only the new portal patch from the engine root:

```bash
FIXES_ROOT=/path/to/unreal-engine-5.8-linux-fixes
git apply --check "$FIXES_ROOT/patches/0005-kde-portal-file-dialogs.patch"
git apply "$FIXES_ROOT/patches/0005-kde-portal-file-dialogs.patch"
```

An engine tree that already has the working portal integration does not need the patch applied again.

For an existing editor, rebuild only `DesktopPlatform` using the scoped precompiled-engine technique:

1. Close the editor and other applications using the library.
2. Temporarily add `bUsePrecompiled = false;` inside the `DesktopPlatform` constructor in `Engine/Source/Developer/DesktopPlatform/DesktopPlatform.Build.cs`.
3. Preview the actions from the engine source root:

   ```bash
   bash Engine/Build/BatchFiles/Linux/Build.sh \
       UnrealEditor Linux Development \
       -Project="/path/to/MyProject.uproject" \
       -UsePrecompiled -MaxParallelActions=4 -Module=DesktopPlatform \
       -WriteOutdatedActions=/tmp/ue583-portal-actions.json
   ```

4. Confirm that the only shared-library output is `libUnrealEditor-DesktopPlatform.so`, then repeat without `-WriteOutdatedActions` to build.
5. Remove only the temporary rule assignment added in step 2.
6. Restart the editor through the XWayland launcher.

The temporary override is not included in the patch. The verified local build had three actions and completed in about 17 seconds. No engine-wide metadata regeneration was performed.

## Enable or disable

Portal dialogs are enabled by default. To use Slate for the current session:

```text
Linux.FileDialogs.UsePortal 0
```

Restore portal dialogs with:

```text
Linux.FileDialogs.UsePortal 1
```

To persist a project preference, add or update this entry in `Config/DefaultEngine.ini`:

```ini
[ConsoleVariables]
Linux.FileDialogs.UsePortal=1
```

No INI change is required to enable the default.

## Test without importing or writing files

These commands display a chooser and log the result and selected paths:

```text
Linux.FileDialogs.Test open
Linux.FileDialogs.Test save
Linux.FileDialogs.Test directory
```

The Open test permits multiple selections. The Save test suggests a filename but does not create or overwrite it. These are local additions, not stock UE commands.

Run the parsing regression test with:

```text
Automation RunTests System.DesktopPlatform.LinuxPortal.Parsing
```

## Verified results

- The editor starts with the replacement library.
- The URI/filter parsing automation test passes.
- Actual Import opens the KDE-backed portal chooser and returns a selected file and filter index to Unreal.
- The Save diagnostic returns a selected path without creating a file.
- The directory diagnostic returns a selected folder.
- Cancelling Import does not open a second Slate picker.
- Cancelling Save and directory diagnostic dialogs returns cancellation normally.
- The user reports that Import works well.

Specific remote/device workflows and actual exporter file-writing behavior require their own checks. The selected-filter response is preserved by using the portal interface directly; the bundled SDL portal wrapper did not expose that index in the investigated implementation.
