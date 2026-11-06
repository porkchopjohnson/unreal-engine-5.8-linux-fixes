# Unreal Engine 5.8 Linux Fixes & Optional Tools

Make **Unreal Engine 5.8.3 on Linux** easier to use: smoother editor interaction,
desktop file dialogs, video importing for MetaHuman Animator, NVIDIA CUDA
acceleration for offline body tracking, and eased pelvis refinement with saved controls.

Tested with a local UE 5.8.3 source build on Fedora/KDE Plasma Wayland, with Unreal
running through XWayland. **This repository contains source code, patches, and
instructions. You build the components you choose for your own engine.**

## Start here: what do you want to do?

| I want to… | Use this | Start with |
| --- | --- | --- |
| Import a video into MetaHuman Animator without Live Link Hub | **Optional MetaHuman Video Importer plugin** | [Install and import your first video](plugins/MetaHumanVideoImporter/README.md) |
| Speed up offline MetaHuman body tracking on an NVIDIA GPU | **Optional CUDA integration** | [CUDA installation and usage](METAHUMAN-BODY-TRACKER-CUDA.md) |
| Improve tracked pelvis articulation and smooth its added correction | **Optional pelvis refinement integration** | [Install and use saved pelvis controls](METAHUMAN-PELVIS-REFINEMENT.md) |
| Copy a physics setup between character meshes | **Optional Physics Asset Transfer Tools plugin** | [Plugin installation and usage](plugins/PhysicsAssetTransferTools/README.md) |
| Fix slow scrolling in asset/object dropdowns | SDL X11 popup fix | [Asset-picker scrolling guide](ASSET-PICKER-SCROLLING.md) |
| Reduce Physics Asset Editor (PhAT) lag | Vulkan deletion-validation fix | [PhAT performance guide](VULKAN-PHAT-PERFORMANCE.md) |
| Use KDE's native Open/Save/folder chooser | Desktop-portal file dialogs | [File-dialog guide](PORTAL-FILE-DIALOGS.md) |
| Fix editor focus/notification issues or build Live Link Hub | Engine patch series and launchers | [Engine fixes below](#engine-fixes) and [technical reference](EDITOR-FIXES.md) |

**You can install just the tool you need.** The optional plugins do not require
the core engine patch series. The CUDA integration has its own small engine fix
and plugin hooks, documented in its guide.

## New to source plugins and patches?

- **Plugin:** an extra tool you copy into a project's `Plugins` folder, build,
  and enable in Unreal. Each plugin guide shows the exact folder layout.
- **Patch:** a set of changes to existing source files. Apply it to your own
  engine, then rebuild the affected component for the change to take effect.
- **Build:** compile the source into the libraries Unreal actually loads.
  Copying source alone does not update an already-running editor.
- **Engine root:** the directory containing `Engine/`, `Setup.sh`, etc.
- **Project folder:** the directory containing your `.uproject` file.
- **XWayland:** lets an X11 application run inside a Wayland desktop session.
  The included launchers select Unreal's X11 backend; your desktop stays on Wayland.

You will need your own UE 5.8.3 source tree and its downloaded dependencies. The
repository does not include Unreal itself, pretrained models, or compiled plugins.
Use the [build guide](BUILDING.md) if you are setting up or updating a source build.

## Get this repository

Clone it with Git:

```bash
git clone https://github.com/porkchopjohnson/unreal-engine-5.8-linux-fixes.git
```

Or use **Code → Download ZIP** on GitHub and extract it. The downloaded repository
and your Unreal Engine source tree are separate folders.

In the example commands, replace `/path/to/...` with your actual paths. Keep
quotes around paths, particularly if they contain spaces.

## Optional MetaHuman tools

### Import videos directly in Unreal

The [MetaHuman Video Importer](plugins/MetaHumanVideoImporter/README.md) adds
**Tools → MetaHuman Video Importer**. Select videos, click **Import**, then open
the generated Performance asset and click **Process**.

- Creates project-local PNG frames, optional WAV audio, footage data and a
  configured monocular Performance.
- Uses installed FFmpeg/FFprobe; no Live Link Hub conversion step is needed.
- Version **1.1** fixes processing-range crashes and repairs affected older
  imports when you reopen their Performance assets.
- Both import/range regression tests passed, and the user confirmed it works.

**[Follow the step-by-step importer guide →](plugins/MetaHumanVideoImporter/README.md)**

### Accelerate offline body tracking with CUDA

The [CUDA integration](METAHUMAN-BODY-TRACKER-CUDA.md) lets the Linux offline body
tracker use ONNX Runtime's NVIDIA GPU backend. After installation, process the
Performance as usual; CUDA is enabled by default.

On the tested RTX 3090, warm ViTPose inference measured **0.028 s** versus
**0.619 s** on CPU, and CHMR backbone inference measured **0.220 s** versus
**33.579 s**. These are individual model timings, not full-clip speedup ratios.
Automation tests and a five-stage real-footage solve passed. The user also
processed the **full clip** and confirmed it **"works very well."**

**[Follow the CUDA setup guide →](METAHUMAN-BODY-TRACKER-CUDA.md)**

### Refine pelvis motion with saved controls and easing

The [pelvis refinement integration](METAHUMAN-PELVIS-REFINEMENT.md) adds
image-aware pitch, roll, and twist corrections to offline body tracking, with
temporal easing and per-Performance controls under **Body Tracking → Pelvis
Refinement**. It works alongside the optional CUDA integration.

Nine native tests and four image-helper tests passed. Saved settings survived a
fresh-editor reload, and full processing with foot locking succeeded. The user
confirmed the refinement, easing, and UI together resolved their current issues.
Keep exaggeration at 1 and adjust easing for smoother corrections.

**[Follow the pelvis refinement setup guide →](METAHUMAN-PELVIS-REFINEMENT.md)**

## Engine fixes

The core patches address these editor and build issues:

| Area | What the change does | More information |
| --- | --- | --- |
| Tooltips and window focus | Respects Slate's window activation policy | [Focus explanation](EDITOR-FIXES.md#notification-focus-problem) |
| Notifications | Raises notification toasts without taking keyboard focus; adds a repeatable test | [Notification diagnostics](EDITOR-FIXES.md#validation-checklist) |
| Vulkan swapchains | Cleans up retained resources when swapchain recreation is skipped | [Crash investigation](EDITOR-FIXES.md#vulkan-dragdialog-crash) |
| Live Link Hub | Builds its launcher against existing engine modules | [Build instructions](BUILDING.md#live-link-hub) |
| Open/Save dialogs | Uses the desktop portal for KDE-native file and folder selection | [Portal file dialogs](PORTAL-FILE-DIALOGS.md) |
| PhAT responsiveness | Replaces a slow linear duplicate-deletion check with a hash lookup | [Measurements and validation](VULKAN-PHAT-PERFORMANCE.md) |
| Asset dropdown scrolling | Fixes SDL popup-coordinate synchronization delays | [SDL fix and rebuild](ASSET-PICKER-SCROLLING.md) |

### Apply and build the core patch series

1. Close Unreal and set the following two paths in a terminal:

   ```bash
   FIXES_ROOT="/path/to/unreal-engine-5.8-linux-fixes"
   ENGINE="/path/to/UnrealEngine-5.8.3-release"
   ```

2. Check whether the patches fit your engine source:

   ```bash
   git -C "$ENGINE" apply --check "$FIXES_ROOT"/patches/000*.patch
   ```

   No output and a successful exit means the check passed. If it reports a
   conflict, inspect the source differences before continuing. An already-patched
   engine also fails a forward check; do not apply the series twice.

3. Apply the patches:

   ```bash
   git -C "$ENGINE" apply "$FIXES_ROOT"/patches/000*.patch
   ```

4. Follow **[BUILDING.md](BUILDING.md)** to rebuild. For an existing engine,
   inspect the planned actions first so a small update does not unexpectedly
   trigger a large rebuild.

   **Patch 0007 also needs a bundled SDL rebuild and an ApplicationCore relink.**
   Follow the [SDL-specific instructions](ASSET-PICKER-SCROLLING.md#build-and-install);
   rebuilding an unrelated module will not install that change.

5. Install the optional XWayland launchers into your engine root:

   ```bash
   install -m 755 "$FIXES_ROOT/scripts/RunEditorXWayland.sh" "$ENGINE/RunEditorXWayland.sh"
   install -m 755 "$FIXES_ROOT/scripts/RunLiveLinkHubXWayland.sh" "$ENGINE/RunLiveLinkHubXWayland.sh"
   ```

6. Launch the rebuilt editor:

   ```bash
   "$ENGINE/RunEditorXWayland.sh" "/path/to/MyProject/MyProject.uproject"
   ```

   Omit the project argument to open the project browser. After building Live
   Link Hub, use `"$ENGINE/RunLiveLinkHubXWayland.sh"` to launch it.

The core series is `patches/0001` through `0007`. Files under `patches/optional/`
are separate, feature-specific changes; follow their associated guides.

## Guides and troubleshooting

| Guide | Covers |
| --- | --- |
| [Building and updating](BUILDING.md) | Initial builds, incremental build scope, module metadata and Live Link Hub |
| [Engine fix reference](EDITOR-FIXES.md) | Patch details, focus diagnostics, validation history and archive verification |
| [MetaHuman Video Importer](plugins/MetaHumanVideoImporter/README.md) | Installation, importing, saved files, range repair and common problems |
| [MetaHuman CUDA](METAHUMAN-BODY-TRACKER-CUDA.md) | Dependencies, rebuilding, GPU confirmation, CPU/CUDA controls and benchmarks |
| [MetaHuman pelvis refinement](METAHUMAN-PELVIS-REFINEMENT.md) | Source installation, saved Body Tracking controls, easing, diagnostics and validation |
| [Physics Asset Transfer Tools](plugins/PhysicsAssetTransferTools/README.md) | Copying a physics setup, requirements and verification |
| [KDE file dialogs](PORTAL-FILE-DIALOGS.md) | Portal setup, fallback behavior and dialog troubleshooting |
| [Asset-picker scrolling](ASSET-PICKER-SCROLLING.md) | SDL diagnosis, regression probe and installation |
| [PhAT performance](VULKAN-PHAT-PERFORMANCE.md) | Vulkan validation changes and controlled comparisons |

Validation is recorded in each feature's guide. The engine fixes target this
specific UE 5.8.3 Linux build; results are not a compatibility guarantee for other
engine releases, GPUs or desktops. See [NOTICE.md](NOTICE.md) for source and
licensing context. This is a community project, not an official Epic Games release.
