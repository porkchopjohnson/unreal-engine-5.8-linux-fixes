# Asset/object picker scrolling: SDL X11 popup synchronization

Patch: `patches/0007-fix-sdl-x11-popup-sync-coordinates.patch`.

Validated 2026-11-01 on UE 5.8.3, KDE Plasma Wayland with Unreal running through XWayland. The user reported that an instance-editable skeletal-mesh property dropdown remained laggy when repeatedly scrolling the same rows. After installing this fix and restarting, the same dropdown was re-tested and the user reported **“Smooth now.”**

## Diagnosis

Short debugger samples caught the main thread waiting here, while the render/RHI threads were idle:

```text
SDL_SYS_DelayNS(10000000)
SDL_Delay_REAL(10)
X11_SyncWindowTimeout
X11_SyncWindow
FLinuxWindow::ReshapeWindow
SWindow::ResizeWindowSize
PrepassWindowAndChildren
FSlateApplication::DrawPrepass
```

A breakpoint inside the synchronization wait found:

```text
SDL popup position:   175, 452     (parent-relative)
X11 expected position: 1174, 791  (root/screen coordinates)
size: 151 x 100
pending operation: X11_PENDING_OP_MOVE
```

`X11_UpdateWindowPosition()` converts requested popup positions to global coordinates before storing the expected position. The synchronization loop compared that against parent-relative `window->x/y`. A completed move could therefore wait for the entire approximately 100 ms timeout. Auto-sized popups/tooltips exposed this during picker interaction.

## Correction

The patch changes only the bundled SDL3 X11 implementation:

```text
Engine/Source/ThirdParty/SDL3/SDL-gui-backend/src/video/x11/SDL_x11window.c
```

`X11_SyncWindowTimeout` and `X11_ExternalResizeMoveSync` compare actual and expected positions in global coordinates, normalize timeout fallback positions including border offsets, and keep emitted SDL move events parent-relative. Synchronization, completion checks and timeouts remain enabled. Thumbnail rendering, asset filtering, focus policy and window/resource lifetimes were not changed.

## Regression probe

From the engine root, set `FIXES_ROOT` to this repository and link the included probe against the archive you want to test:

```bash
SDL_ROOT="$PWD/Engine/Source/ThirdParty/SDL3/SDL-gui-backend"
cc -O2 -I "$SDL_ROOT/include" \
    "$FIXES_ROOT/tests/SDLPopupSyncTest.c" \
    "$SDL_ROOT/lib/Unix/x86_64-unknown-linux-gnu/libSDL3_fPIC.a" \
    -lm -pthread -ldl -o /tmp/sdl-popup-sync-test
/tmp/sdl-popup-sync-test
```

It creates temporary, non-focusable windows: a parent at a nonzero screen position, a popup menu, a tooltip and a nested tooltip. It alternates widths, reapplies positions, synchronizes, and checks the return values and geometry. Six operations per type, 24 total; no Unreal project assets are involved.

The original installed archive failed **15/24 operations** from repeated popup synchronization timeouts. The patched archive passed **all 24**. Median timings for the five repeated operations, excluding each type's initial operation:

| Window type | Original | Patched |
| --- | ---: | ---: |
| Borderless top-level control | 10.348 ms | 10.314 ms |
| Popup menu | 103.324 ms | 0.302 ms |
| Tooltip | 103.040 ms | 0.376 ms |
| Nested tooltip | 102.971 ms | 0.473 ms |

These measure synchronization operations, not GPU throughput or editor frame rate. The user-confirmed smooth dropdown result is a separate live-editor check.

## Build and install

**Editing SDL source does not update the prebuilt static archive.** Its code is linked into ApplicationCore, so both the SDL archive and ApplicationCore must be updated. Other projects' already-linked executables/libraries also require their corresponding relink to pick up an SDL change.

Close the editor before replacing the library. Install the SDL development dependencies for your distribution; the bundled SDL build documentation lists them. Keep both X11 and Wayland backends and Epic extensions enabled when matching this engine's configuration.

### Rebuild SDL

For a local source build, create an isolated build directory. This is an SDL dependency build, not an engine build:

```bash
SDL_ROOT="$PWD/Engine/Source/ThirdParty/SDL3/SDL-gui-backend"
SDL_BUILD=/tmp/ue583-sdl-popup-build

cmake -S "$SDL_ROOT" -B "$SDL_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_C_FLAGS=-gdwarf-4 -fPIC' \
    -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_KMSDRM=OFF \
    -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

# Inspect the configuration's enabled backends and target command graph first.
ninja -C "$SDL_BUILD" -t commands SDL3-static
cmake --build "$SDL_BUILD" --target SDL3-static --parallel 4
```

Link the regression probe against `"$SDL_BUILD/libSDL3.a"` and run it before installing. Back up the existing Development PIC archive, then install the rebuilt archive into the path selected by `SDL3.Build.cs`:

```bash
cp -p "$SDL_ROOT/lib/Unix/x86_64-unknown-linux-gnu/libSDL3_fPIC.a" \
    /tmp/libSDL3_fPIC.before-popup-fix.a
install -m 644 "$SDL_BUILD/libSDL3.a" \
    "$SDL_ROOT/lib/Unix/x86_64-unknown-linux-gnu/libSDL3_fPIC.a"
```

The bundled `Engine/Source/ThirdParty/SDL3/docker/local_build.sh` is another local-build entry point, but builds Debug, non-PIC Release and PIC Release variants. The command above targets just the Development PIC archive. This is a local-machine build procedure, not a portable binary distribution recipe.

### Rebuild only ApplicationCore

Use the [audited precompiled-module workflow](BUILDING.md#reusing-a-precompiled-engine-for-a-targeted-module-fix), with temporary `bUsePrecompiled = false;` in ApplicationCore's constructor:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh UnrealEditor Linux Development \
    -Project="/path/to/Project.uproject" \
    -UsePrecompiled -MaxParallelActions=4 -Module=ApplicationCore \
    -WriteOutdatedActions=/tmp/ue583-applicationcore-actions.json
```

Inspect the actions before repeating without `-WriteOutdatedActions`. The local plan was **one ApplicationCore compile and one link**.

The first local link exposed omitted FreeType transitive PNG dependencies under `-UsePrecompiled`. For that build only, the temporary override also included:

```csharp
if (Target.IsInPlatformGroup(UnrealPlatformGroup.Linux) && Target.bCompileAgainstEditor)
{
    AddEngineThirdPartyPrivateStaticDependencies(Target, "UElibPNG", "zlib");
}
```

The re-audited retry succeeded in 10.86 seconds. Remove **all temporary Build.cs edits** afterward. Neither this override nor machine-specific manifests belong in the engine patch.

### Scope of the validated local installation

The local installation was narrower than replacing SDL wholesale: only the rebuilt `SDL_x11window.c.o` was inserted into a copy of the original PIC archive. An audit of all 312 members confirmed the other 311 were byte-identical. DWARF audits matched sizes and every field offset for nine relevant SDL/X11 internal types before mixing objects. This preserved the original archive's other backend objects/configurations.

That surgical archive update depends on verified internal layout compatibility. The source-build procedure above instead builds a consistent complete SDL archive. No bundled binary or machine-specific generated build configuration is published here.
