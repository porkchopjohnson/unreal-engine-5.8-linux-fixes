# Physics Asset Transfer Tools

Optional UE 5.8 editor plugin that adds **Copy Physics Setup** to the Physics Asset Editor toolbar. It requires no engine patches or private-header access. Tested in UE 5.8.3 on Linux; the user confirmed it works in their transfer workflow.

## Install

Copy this directory into either your project's `Plugins/PhysicsAssetTransferTools` directory or your engine's `Engine/Plugins/Marketplace/PhysicsAssetTransferTools` directory. Build it for your editor and enable **Physics Asset Transfer Tools** under Edit → Plugins if it is disabled. The source descriptor enables it by default; restart an already-running editor to discover a newly installed plugin.

The repository contains source only. `Binaries`, `Intermediate`, generated manifests and user assets are excluded.

## Use

1. Open the **destination** physics asset in the Physics Asset Editor.
2. Set its preview skeletal mesh to the destination character's mesh. Its reference pose defines the new joint frames.
3. Click **Copy Physics Setup** near the left end of the toolbar.
4. Choose the source physics asset and click **Replace Setup and Snap**.
5. Inspect the result and save the destination normally. **Ctrl+Z** undoes the transfer; redo works too.

The transfer replaces the **entire destination body/constraint setup**, including constraint profiles, physical animation profiles and disabled collision pairs. It independently duplicates the objects and matches bones by name, so the skeleton assets can differ.

- Body collision shapes keep their existing **bone-local sizes and offsets**. They follow the destination bones but are not resized to a different silhouette.
- Both constraint frames' **positions and rotations** snap using the same public engine function as PhAT's **Snap All Transforms**. This uses the destination reference pose, including intermediate-bone transforms; copied custom frame offsets/orientations are replaced.
- Joint limits, drives and other constraint parameters remain copied. The saved default profile and all named profiles are retained even when the source editor is showing a named profile. The destination starts with its default profile selected.
- The destination preview mesh and asset-level solver settings are retained.
- Source data remains untouched. Neither asset is saved automatically.

All source body bones and both bones of every constraint must exist on the destination preview mesh. Missing bones produce an error without replacing the setup or clearing selection. Self-copy, an empty source, invalid source body/constraint entries or collision indices, and active destination simulation are rejected.

## Build only the plugin

From the engine root, export and inspect the plan first:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh UnrealEditor Linux Development \
    -Project="/path/to/Project.uproject" -UsePrecompiled -MaxParallelActions=4 \
    -Module=PhysicsAssetTransferTools \
    -WriteOutdatedActions=/tmp/ue583-physics-transfer-actions.json
```

Repeat without `-WriteOutdatedActions` only after confirming the intended outputs. The local build contained **one plugin compile and one link**, completing in 11.47 seconds. This custom plugin intentionally has `bUsePrecompiled=false` to compile its source while reusing engine modules. A project-local plugin can additionally use `-NoEngineChanges`; the engine-local installation cannot use that guard for its own binary replacement.

Module-only builds can omit the plugin's `.modules` manifest. Prefer the normal generated metadata from a scoped build. The local installation registered only this newly built library with the current engine BuildId read at installation time; it did not regenerate engine-wide metadata. Rebuild for a different engine build rather than retagging an incompatible library. See [BUILDING.md](../../BUILDING.md).

## Verification

In an editor console:

```text
Automation RunTests Editor.PhysicsAssetTransfer
```

Or headlessly:

```bash
Engine/Binaries/Linux/UnrealEditor-Cmd "/path/to/Project.uproject" \
    -nullrhi -unattended -nosplash -nosound -nop4 \
    '-ExecCmds=Automation RunTests Editor.PhysicsAssetTransfer' \
    '-TestExit=Automation Test Queue Empty' \
    -abslog=/tmp/ue583-physics-transfer-tests.log
```

Both tests passed on 2026-11-01 using transient fixtures:

- `ReplaceSnapAndUndo`: complete replacement, independent ownership, unchanged body dimensions/offsets, target-pose snapping across rotated intermediate bones, profiles/default handling, non-skeleton-order collision indices, source immutability, preserved destination mesh/settings, Undo and Redo.
- `ValidationIsAtomic`: missing bones, self-copy and missing target preview mesh reject without altering the destination; validation failure does not clear selection.

No project assets are created or saved by the tests. The toolbar/dialog were also checked interactively, followed by the user's successful real-workflow test.
