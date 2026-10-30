# Building the fixes

Commands below are run from your own Unreal Engine 5.8.3 source root unless stated otherwise. Close applications using a module before rebuilding that module.

## Initial engine build

Apply the core patches before the first build when possible. Complete `Setup.sh` and generate Linux project files if needed:

```bash
./Setup.sh
bash Engine/Build/BatchFiles/Linux/GenerateProjectFiles.sh -makefile -CurrentPlatform
```

Build the shader worker and editor:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    ShaderCompileWorker Linux Development \
    -NoDebugInfo -MaxParallelActions=4

bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -NoDebugInfo -MaxParallelActions=4
```

The local build used Epic's bundled Clang 20.1.8 toolchain. `-NoDebugInfo` reduces debug-symbol storage but makes debugging less informative. Adjust parallelism for available memory and CPU resources.

Keep configuration and compiler flags consistent with the build whose outputs you want to reuse. Switching debug-info settings can invalidate cached engine actions even when the requested source change is small.

## Existing builds: inspect scope first

### A module is reported missing, but its library exists

Check the startup log before rebuilding. A line such as:

```text
Skipping out-of-date modules in manifest '.../UnrealEditor.modules' (BuildId OLD != CURRENT)
```

means Unreal is rejecting that manifest, not necessarily that the `.so` is absent. In a later local incident, `GPUTextureTransfer` was present but skipped because the editor and many plugin manifests had different build IDs. A subset metadata write had also omitted existing module registrations, including editor-only modules in UAF-related plugins.

The local repair verified the original full-build receipt and the existing libraries, backed up the affected metadata, aligned the known same-engine build cohort, and restored 63 omitted registrations from that receipt. The project then initialized successfully without rebuilding the libraries.

This was a case-specific metadata repair, not a reason to retag arbitrary downloaded plugins. A matching version string and an existing `.so` alone do not establish binary compatibility. If the binaries were built against different headers, engine versions, architectures, or incompatible settings, rebuild them instead. Keep machine-specific manifests and backups out of this patches repository.

Plugin-only builds should reuse the established engine outputs and preserve their metadata identity. For Live Link Hub, use the target-only receipt action described below rather than regenerating a subset of the editor's engine-wide manifests.

UnrealBuildTool can export the planned actions without compiling them:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -MaxParallelActions=4 \
    -WriteOutdatedActions=/tmp/ue583-planned-actions.json
```

Inspect the JSON's `Actions`, `StatusDescription`, and `ProducedItems` before starting a costly build. An updated makefile does not necessarily imply a full recompile, but a plan containing thousands of engine compilation actions should be investigated if only a small fix is intended.

The exact scope depends on your existing compiler options, rule changes, and caches. Do not assume that adding `-Module=` alone prevents dependencies from being rebuilt.

### Reusing a precompiled engine for a targeted module fix

The local Vulkan fix reused engine binaries and built only `VulkanRHI`:

1. Temporarily add `bUsePrecompiled = false;` inside the `VulkanRHI` constructor in `Engine/Source/Runtime/VulkanRHI/VulkanRHI.Build.cs`.
2. Export and inspect this plan, replacing the project path:

   ```bash
   bash Engine/Build/BatchFiles/Linux/Build.sh \
       UnrealEditor Linux Development \
       -Project="/path/to/MyProject.uproject" \
       -UsePrecompiled -MaxParallelActions=4 -Module=VulkanRHI \
       -WriteOutdatedActions=/tmp/ue583-vulkan-actions.json
   ```

3. Check that the only rebuilt shared library is `libUnrealEditor-VulkanRHI.so`. A project-local shared precompiled header can also be needed.
4. Repeat the command without `-WriteOutdatedActions` to build.
5. Remove only the temporary `bUsePrecompiled` assignment added in step 1.

The temporary rule override is not part of the core patches. The same technique can be used for other intentionally rebuilt engine modules, with their own rule and action-plan checks. `-UsePrecompiled` alone can skip read-only engine modules rather than rebuild the source you edited.

The local Vulkan rebuild completed with seven actions in about 57 seconds. This is an observation, not a timing guarantee.

`-NoEngineChanges` is useful when building project plugins because it refuses changes to existing engine outputs. It will also refuse an intentional rebuild of an existing engine library, so it is not appropriate for the Vulkan replacement itself.

## Native portal file dialogs

For the native KDE-backed file picker, see the separate [portal integration and targeted DesktopPlatform build instructions](PORTAL-FILE-DIALOGS.md#build).

## Live Link Hub

Patch 0004 allows the new Hub launcher to compile while reusing the shared editor modules:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    LiveLinkHub Linux Development \
    -UsePrecompiled -MaxParallelActions=4 -Module=LiveLinkHubLauncher \
    -WriteOutdatedActions=/tmp/ue583-hub-actions.json
```

The validated local plan contained one launcher compilation action and one link action. If the plan is similarly scoped, repeat without `-WriteOutdatedActions`:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    LiveLinkHub Linux Development \
    -UsePrecompiled -MaxParallelActions=4 -Module=LiveLinkHubLauncher
```

The executable is `Engine/Binaries/Linux/LiveLinkHub`.

### Hub target receipt

A module-only build can omit the target receipt and post-build copy. The Hub expects a receipt for its bundled project to avoid a build prompt on startup.

Export the full Hub plan without running it:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    LiveLinkHub Linux Development \
    -UsePrecompiled -MaxParallelActions=4 \
    -WriteOutdatedActions=/tmp/ue583-hub-metadata-actions.json
```

For the local build, only metadata and the post-build copy remained. The target-specific metadata input was:

```text
Engine/Intermediate/Build/Linux/x64/LiveLinkHub/Development/TargetMetadata.json
```

Before executing it, verify that `Version` is null, `FileToManifest` and `FileToLoadOrderManifest` are empty, and `ReceiptFile` points to `Engine/Binaries/Linux/LiveLinkHub.target`. This target-only action writes the new Hub receipt without rewriting the editor's version or module manifests.

For the verified UE 5.8.3 metadata format, write that receipt and run its generated post-build copy:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    -Mode=WriteMetadata \
    -Input="$PWD/Engine/Intermediate/Build/Linux/x64/LiveLinkHub/Development/TargetMetadata.json" \
    -Version=2

bash Engine/Intermediate/Build/Linux/x64/LiveLinkHub/Development/PostBuild-1.sh
```

Inspect the exported plan for your tree rather than assuming these generated paths or format numbers are identical in another version. Do not run the separate `EngineMetadata.json` action simply to create the Hub receipt.

Start the app using `RunLiveLinkHubXWayland.sh`. It selects XWayland and starts from `Engine/Binaries/Linux`, where the in-tree Hub resolves its bundled project correctly. Its first launch may install Python dependencies.

## Project-only plugin rebuilds

For a plugin located under a project's `Plugins` directory, reuse the engine and refuse writes to existing engine outputs. For example, the downloaded MetaHuman Body Tracker plugin already declared Linux support and included source; it needed Linux compilation rather than a source port:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -Project="/path/to/MyProject.uproject" \
    -UsePrecompiled -NoEngineChanges -MaxParallelActions=4 \
    -Module=MetaHumanBodyTracker+BodyTracker+Segmentation+MetaHumanBodyOptimizer \
    -WriteOutdatedActions=/tmp/ue583-project-plugin-actions.json
```

Inspect the plan, confirm its outputs are confined to the project, and repeat without `-WriteOutdatedActions` to compile. This local example built 72 actions, including a project-local shared header, in about three minutes. The plugin and its models are not distributed here.

If module-only compilation omits the project plugin's `.modules` manifest, preview the same `-UsePrecompiled -NoEngineChanges` project build without `-Module=`. In the tested case only a project-local metadata action remained; executing that action completed the receipt and manifest without recompiling the engine.

## Optional custom plugins

`patches/optional/custom-plugins-5.8.patch` records changes for these specific custom plugins:

- `ClothVertexColorTools`: retain separate translation units (`bUseUnity = false`) to avoid duplicate file-local helper names, and allow source compilation while reusing engine binaries.
- `PoseAssetOffsetTools`: infer JSON entry types with `const auto&` to accommodate UE 5.8 shared-string keys.
- Both descriptors use `EngineVersion: 5.8.0`, matching the source-built editor's reported **compatible** version. The executable itself is 5.8.3.

These plugins are not included in the repository. Apply the optional patch only to matching plugin sources.

The original custom plugin descriptors used Windows line endings. From the engine root, check and apply the optional patch with context-whitespace tolerance:

```bash
git apply --check --ignore-space-change "$FIXES_ROOT/patches/optional/custom-plugins-5.8.patch"
git apply --ignore-space-change "$FIXES_ROOT/patches/optional/custom-plugins-5.8.patch"
```

Their module build was scoped with:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -Project="/path/to/MyProject.uproject" \
    -UsePrecompiled -MaxParallelActions=4 \
    -Module=ClothVertexColorTools+PoseAssetOffsetTools \
    -WriteOutdatedActions=/tmp/ue583-custom-plugin-actions.json
```

Inspect the plan and repeat without `-WriteOutdatedActions` to compile. Because these particular plugins live under `Engine/Plugins/Marketplace`, `-NoEngineChanges` would reject their intentional binary replacements too.

Module-only builds can omit manifest generation. Verify the generated Linux `.modules` manifests match the editor's build ID after a real rebuild; changing a build ID alone is not a substitute for compilation. Machine-specific build IDs and compiled outputs are deliberately not part of this repository.
