# MetaHuman offline body tracking with CUDA on Linux

Optional CUDA inference integration for the **UE 5.8.3 MetaHumanBodyTracker**
plugin. It is enabled by default after installation. On the tested RTX 3090,
both automation tests and a five-stage real-footage solve passed. The user then
processed the **full imported clip** interactively and reported that it **"works
very well"** on 2026-11-05.

## Why processing was slow

The offline tracker prefers `NNERuntimeORTDml` on GPU, then `NNERuntimeORTCpu`.
DirectML is unavailable in native Linux/Vulkan Unreal, so the models were falling
back to CPU. Low GPU usage during inference was a backend-selection problem.

This integration routes the Linux GPU preference to ONNX Runtime's CUDA provider:

| Processing stage | CUDA coverage |
| --- | --- |
| 1: Camera calibration | Offline GPU-preferred models |
| 2: Body detection and segmentation tracking | Detectron2 and SAM2 |
| 3: Keypoints | ViTPose |
| 4: Pose estimation | CHMR and Hue iterative inference |
| 5: Finalization | Existing CPU-only Hue final step retained |

Explicit CPU requests retain their existing backend. The BodyTracker hook is
restricted to `/MetaHumanBodyTracker/Models/Offline/` assets. A CUDA session that
cannot initialize returns control to the original runtime preference list.
An inference failure after initialization reports failure; it does not silently
retry that run on CPU.

## Requirements

- Linux x86-64, your own UE 5.8.3 source tree and MetaHumanBodyTracker plugin,
  including its original offline model assets.
- NVIDIA GPU and driver compatible with CUDA 12.8. Tested with an RTX 3090
  (24 GiB), driver 615.78.08.
- Git, Python 3 with pip, system `g++`, and the engine's downloaded ORT headers.
- Sufficient `/dev/shm`, RAM, GPU memory and temporary storage for model loading.
  Each worker reserves a 256 MiB shared tensor mapping and a 256 MiB scratch
  output buffer; actual memory use also includes the model and CUDA allocations.

No system CUDA toolkit or system-wide Python package installation is required.
The setup script downloads pinned wheels and extracts native libraries into the
plugin. Their Python extensions are not used.

## Install into an existing plugin

Close Unreal before replacing module binaries. Set these absolute paths:

```bash
FIXES_ROOT=/path/to/unreal-engine-5.8-linux-fixes
ENGINE=/path/to/UnrealEngine-5.8.3-release
PLUGIN="/path/to/MyProject/Plugins/Marketplace/MetaHumanBodyTracker"
PROJECT="/path/to/MyProject/MyProject.uproject"
```

First check and apply the NNE engine fix and plugin selection hooks:

```bash
git -C "$ENGINE" apply --check "$FIXES_ROOT/patches/optional/metahuman-nne-64bit-external-data.patch"
git -C "$PLUGIN" apply --ignore-space-change --check "$FIXES_ROOT/patches/optional/metahuman-body-tracker-cuda-hooks.patch"

git -C "$ENGINE" apply "$FIXES_ROOT/patches/optional/metahuman-nne-64bit-external-data.patch"
git -C "$PLUGIN" apply --ignore-space-change "$FIXES_ROOT/patches/optional/metahuman-body-tracker-cuda-hooks.patch"
```

The patches target different roots, as shown above. They are optional and are
not included in the repository's `patches/000*.patch` core series. If a check
fails, inspect the source version; an already-patched tree also fails a forward
check. `git apply --reverse --check` can identify an already-applied patch; retain
`--ignore-space-change` for the plugin patch to accommodate its CRLF source files.

Copy the newly authored adapter, worker, setup script and validation sources:

```bash
cp -a "$FIXES_ROOT/integrations/MetaHumanBodyTrackerCUDA/Source/." "$PLUGIN/Source/"
cp -a "$FIXES_ROOT/integrations/MetaHumanBodyTrackerCUDA/Resources/." "$PLUGIN/Resources/"

python3 "$PLUGIN/Resources/SetupCuda.py" --engine "$ENGINE"
```

Run `SetupCuda.py` **from the installed plugin copy**, since it resolves the
destination relative to itself. Dependencies are pinned to:

- ONNX Runtime GPU 1.24.4
- CUDA runtime 12.8.90, cuBLAS 12.8.4.1, cuDNN 9.8.0.87
- cuFFT 11.3.3.83, cuRAND 10.3.9.90, nvJitLink 12.8.93

Libraries, dependency license files and the native `MetaHumanCudaWorker` go into
`<Plugin>/Binaries/ThirdParty/OnnxCuda/Linux/`. For a worker-only rebuild with the
dependencies already installed, add `--build-only`.

### Rebuild the Unreal modules

Rebuild **NNE** in the engine and **Segmentation** and **BodyTracker** in the
project plugin. The overlay's new `.cpp` files must be included in Segmentation's
compile; no Unreal link dependency on the downloaded CUDA libraries is needed.

For a normal source build, from the engine root:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -Project="$PROJECT" -MaxParallelActions=4 \
    -WriteOutdatedActions=/tmp/metahuman-cuda-actions.json
```

Inspect the action plan, then repeat without `-WriteOutdatedActions` to build.
Preserve the compiler/configuration settings of your existing build. See
[BUILDING.md](BUILDING.md#existing-builds-inspect-scope-first) for incremental
build scope and precompiled-engine handling. An unqualified UBT invocation on
the tested installation planned a large dependency rebuild; the installed local
build instead reused existing compile/link response files for the three modules.
Those machine-specific response files and compiled libraries are not distributed.

The NNE patch fixes `GetAdditionalFileData`: a 32-bit empty array view in a
conditional expression narrowed the stored external weights before returning a
64-bit view. Large CHMR weights triggered `ArrayView.h` assertions above 2 GiB.
Applying source without rebuilding NNE does not fix the loaded engine library.

## Use and backend confirmation

Restart Unreal, open an existing Performance, and **Process** as usual. Existing
imported assets need no reimport. Each initialized model logs:

```text
LogMetaHumanCuda: Display: CUDA session ready (isolated worker, TF32 off): ...
```

Enter `mh.BodyTracker.CUDA 0` before processing to use the original CPU path, or
`mh.BodyTracker.CUDA 1` to re-enable CUDA. The launch switch `-NoMetaHumanCuda`
also disables it. These controls affect newly created inference sessions.

## Architecture and numerical behavior

Loading the stock GPU ORT directly inside the editor crashed because Unreal's
allocator interposition conflicted with the native runtime. A `dlmopen`
experiment also failed during provider loading. The working implementation uses
one standalone system-C++ process per model to isolate ORT and CUDA loading.
Only the child receives its private `LD_LIBRARY_PATH`.

Tensor data uses POSIX shared memory; pipes carry control messages. The adapter
supports dynamic shapes and tensor types, validates bounded buffers, and checks
all output sizes before writing caller outputs. Worker scratch storage preserves
outputs that alias input tensors. Workers terminate on session destruction or
editor death. Temporary model/external-weight files under
`<Project>/Intermediate/MetaHumanCuda/` are removed after session creation.

CUDA uses device 0 with TF32 disabled. Source model precision is unchanged,
including CHMR's existing FP16 layers. CPU promotes some unsupported FP16 kernels
to FP32, so CPU and CUDA outputs are not bit-identical. Some operations may
execute on CPU when CUDA lacks a compatible kernel. Loading, preprocessing,
transfers, optimization and finalization also contribute to total solve time.

## Validation and measured results

Recorded on 2026-11-05 with UE 5.8.3, RTX 3090, driver 615.78.08:

| Model | CPU inference | Warm CUDA inference | Relative L2 difference |
| --- | ---: | ---: | ---: |
| ViTPose | 0.619 s | 0.028 s | 0.00000174 |
| CHMR backbone | 33.579 s | 0.220 s | 0.00254520 |

The CUDA timing includes the shared-memory round trip. CPU and warm CUDA timings
are individual model measurements, not full-clip speedup ratios.

- `MetaHuman.BodyTracker.CUDA.DynamicBindings`: **passed**, covering dynamic
  output dimensions, aliased input/output and undersized-buffer rejection without
  overwriting caller outputs.
- `MetaHuman.BodyTracker.CUDA.ModelParity`: **passed**, comparing finite ViTPose
  and CHMR backbone outputs against CPU with relative L2 tolerances of 0.001 and
  0.005 respectively.
- A **32-frame real-footage solve** completed all five stages and produced 32
  finite body poses in **64.137 s**, including model initialization. Stage times
  were **11.31 / 20.14 / 12.42 / 18.42 / 1.71 s**. Profiles confirmed CUDA
  execution, including Hue iterative inference.
- The user then tested the **full imported clip** interactively and confirmed it
  **"works very well"**. No full-clip elapsed time or CPU/CUDA visual comparison
  was recorded.

### Reproduce the automation tests

```bash
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" \
    -unattended -nullrhi -nosound -NoSplash -NoAutoSave \
    "-ExecCmds=Automation RunTests MetaHuman.BodyTracker.CUDA" \
    "-TestExit=Automation Test Queue Empty"
```

### Validate an imported Performance

Enable Unreal's Python scripting support for the script below:

```bash
MH_CUDA_TEST_ASSET=/Game/MyFootage/Performance_MyClip.Performance_MyClip \
MH_CUDA_TEST_FRAMES=32 \
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" \
    -unattended -RenderOffscreen -nosound -NoSplash -NoAutoSave \
    "-ExecutePythonScript=$PLUGIN/Resources/ValidateCuda.py"
```

The script creates a transient, body-only Performance, checks that body pose
output exists and contains finite values, and logs `CUDA_VALIDATION_SUCCESS`.
It saves no assets. `MH_CUDA_TEST_FRAMES` defaults to 32. Model-only automation
can use `-nullrhi`; a real Performance solve requires Vulkan, so use
`-RenderOffscreen` for that run.

Add `-MetaHumanCudaProfile` to save ORT JSON traces in
`<Project>/Saved/CudaProfiles/`. Entries whose `args.provider` is
`CUDAExecutionProvider` verify actual GPU inference. Omit profiling for normal
processing because it adds overhead.

With the pinned cuDNN version, the first CHMR inference logged a nonfatal
`No execution plans support the graph` frontend message. Inference subsequently
succeeded, and both model comparison and the five-stage solve passed.

## Distributed contents

For additional image-aware pelvis articulation, temporal easing, and saved
Body Tracking controls, see the separate
[pelvis refinement integration](METAHUMAN-PELVIS-REFINEMENT.md). Its source overlay
and patches can be installed alongside CUDA; it does not change this integration's
runtime selection or require CUDA to fit pelvis corrections.

`integrations/MetaHumanBodyTrackerCUDA/` is an overlay of newly authored source,
not a complete MetaHumanBodyTracker plugin. The two optional patches contain only
the engine accessor fix and existing runtime-selection hook changes. Install
them into your own plugin and engine. Model assets, footage, downloaded wheels,
native libraries, built modules, and project-specific build manifests are not
included. See [NOTICE.md](NOTICE.md) for source context.
