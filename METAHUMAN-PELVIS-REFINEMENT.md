# MetaHuman pelvis refinement, easing, and saved controls

Optional **UE 5.8.3 Linux** source integration for offline MetaHuman body tracking.
Adds image-aware pelvis pitch, roll, and twist correction, temporal easing, and
settings saved separately on each Performance asset. The installed implementation
was built, tested, and approved in interactive use on 2026-11-06.

The controls appear under **Processing Parameters → Body Tracking → Pelvis
Refinement**. They are additions from this integration, not stock Epic settings.

## What it adds

- Fits extra pelvis articulation from confidence-weighted hip landmarks and
  visible pelvis-surface image motion after Hue pose estimation.
- Rejects background, poor optical flow, and observed arms/hands crossing the hips.
- Applies bounded corrections before camera/world conversion, ground placement,
  final leg/foot optimization, and MetaHuman retargeting.
- Eases the **added correction** without filtering the original body animation or
  root translation. Stronger easing reduces rapid correction changes.
- Saves enable, strength, easing, image-tracking, and advanced controls on the
  Performance. One settings snapshot is used throughout each processing run.

The [CUDA integration](METAHUMAN-BODY-TRACKER-CUDA.md) is a separate optional
inference accelerator. The pelvis integration does not require CUDA, and its
patches touch different files from the CUDA integration's patches. They can be
installed together. The Video Importer and core editor patch series are also
independent.

## Requirements

- Your own UE **5.8.3 source build**, MetaHuman Animator, and an existing
  **MetaHumanBodyTracker** plugin with its original source and offline models.
- A project already able to process a monocular, body-tracked Performance.
- Git and the engine's normal Linux build toolchain and dependencies.
- For image-aware refinement: **`/usr/bin/python3` with NumPy and OpenCV**.
  Tested with OpenCV 5.0.0. The image helper currently launches through Linux's
  `/usr/bin/env`; this publication targets Linux.
- Unreal Python scripting support only for the optional full-solve validation
  script below.

The integration reuses video frames, SAM2 masks, camera calibration, and the
shaped SMPL-X model from the existing solve. It downloads no models or Python
packages during processing.

## Install

Close Unreal before changing source and rebuilding. Set these absolute paths,
replacing the examples with your own locations:

```bash
FIXES_ROOT="/path/to/unreal-engine-5.8-linux-fixes"
ENGINE="/path/to/UnrealEngine-5.8.3-release"
PLUGIN="/path/to/MyProject/Plugins/Marketplace/MetaHumanBodyTracker"
PROJECT="/path/to/MyProject/MyProject.uproject"
```

`PLUGIN` must be the existing plugin containing `MetaHumanBodyTracker.uplugin`
and `Source/`. The included integration is a source overlay, not a standalone
plugin to enable alongside MetaHumanBodyTracker.

### 1. Check both patches

The saved controls belong to the engine's **MetaHumanPerformance** module; the
algorithm and processing hooks belong to the project **MetaHumanBodyTracker**
module. **Install both patches and the overlay.**

```bash
git -C "$ENGINE" apply --check \
    "$FIXES_ROOT/patches/optional/metahuman-performance-pelvis-settings.patch"
git -C "$PLUGIN" apply --ignore-space-change --check \
    "$FIXES_ROOT/patches/optional/metahuman-body-tracker-pelvis-hooks.patch"
```

Successful checks produce no output. If a check fails, inspect the differences
before applying. An already-applied patch also fails a forward check; use
`git apply --reverse --check` with the same root and filename to identify it.
Retain `--ignore-space-change` for the plugin patch because plugin source may
have CRLF line endings. These optional patches are separate from `000*.patch`.

### 2. Apply and copy the source overlay

```bash
git -C "$ENGINE" apply \
    "$FIXES_ROOT/patches/optional/metahuman-performance-pelvis-settings.patch"
git -C "$PLUGIN" apply --ignore-space-change \
    "$FIXES_ROOT/patches/optional/metahuman-body-tracker-pelvis-hooks.patch"

cp -a "$FIXES_ROOT/integrations/MetaHumanPelvisRefinement/Source/." "$PLUGIN/Source/"
cp -a "$FIXES_ROOT/integrations/MetaHumanPelvisRefinement/Resources/." "$PLUGIN/Resources/"
```

The overlay adds six C++ source/header files and three Python helpers. Existing
module dependencies already include Json, Projects, ImageCore, and
MetaHumanPerformance; no extra Build.cs dependency is needed for this version.

### 3. Check the system image helper

Use the same interpreter and environment isolation that Unreal uses:

```bash
/usr/bin/env -u PYTHONHOME -u PYTHONPATH -u LD_LIBRARY_PATH /usr/bin/python3 \
    -c 'import numpy, cv2; print("NumPy", numpy.__version__, "OpenCV", cv2.__version__)'
```

If those imports fail, install NumPy and OpenCV for **that system interpreter**.
On Fedora, the distribution packages are `python3-numpy` and `python3-opencv`.
Installing into Unreal's bundled Python or an unrelated virtual environment does
not satisfy this helper. You can also turn off **Image-Aware Refinement** to use
hip-landmark evidence alone. A missing or unsuccessful helper falls back to that
landmark path; a failed child process logs a warning.

### 4. Regenerate reflection and rebuild

Use normal UHT/UBT processing to regenerate reflected Performance properties and
rebuild **MetaHumanPerformance**, **MetaHumanBodyTracker**, and any dependents
affected by the changed public headers. UBT discovers the new `.cpp` files.
Copying source without rebuilding both modules does not install the feature.

From the engine root, inspect the build plan first:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -Project="$PROJECT" -MaxParallelActions=4 \
    -WriteOutdatedActions=/tmp/metahuman-pelvis-actions.json
```

Then build with the same configuration:

```bash
bash Engine/Build/BatchFiles/Linux/Build.sh \
    UnrealEditor Linux Development \
    -Project="$PROJECT" -MaxParallelActions=4
```

Preserve your existing compiler/configuration settings. See
[BUILDING.md](BUILDING.md#existing-builds-inspect-scope-first) for build scope and
precompiled-engine handling. A precompiled installation that skips engine header
generation must be configured to rebuild MetaHumanPerformance from source; a
project-plugin-only rebuild is insufficient. The validated local installation
used targeted builds and regenerated Performance reflection; its machine-specific
response files and binaries are not part of this package.

Restart Unreal after the build, then open your Performance.

## Use the saved controls

1. Open a **monocular Performance** with **Body Tracking** enabled.
2. Expand **Processing Parameters → Body Tracking → Pelvis Refinement**.
3. Adjust the settings below, save the Performance, and **Process** again.
   Processing the whole clip provides consistent range-wide hip-width calibration.
4. Preview the result and export a new Animation Sequence when satisfied.

Changes affect the next processing run. Previously exported animations are not
updated automatically. Existing assets receive these defaults until edited and
saved; old console values are not migrated into saved settings.

| Main control | Default | Meaning |
| --- | ---: | --- |
| Enable Pelvis Refinement | On | Enable or bypass the added correction |
| Pitch Strength | 1 | Forward/back correction blend, 0–1 |
| Roll Strength | 1 | Lateral correction blend, 0–1 |
| Twist Strength | 0.5 | Yaw/twist correction blend, 0–1 |
| Easing Strength | 0.5 | Correction smoothing, 0–1; 0 disables easing |
| Image-Aware Refinement | On | Include visible pelvis-surface image motion |

Under **Pelvis Refinement → Advanced**:

| Advanced control | Default | Meaning |
| --- | ---: | --- |
| Tilt Strength | 1 | Master pitch/roll blend, 0–1 |
| Image Evidence Weight | 4 | Surface evidence weight, 0–10 |
| Exaggeration | 1 | Artistic gain, 0–3 |
| Maximum Tilt | 20° | Combined pitch/roll correction cap, 0–45° |
| Maximum Twist | 15° | Twist correction cap, 0–45° |
| Write Refinement Diagnostics | Off | Retain correction CSV and image evidence |
| Use Console Settings | Off | Replace saved refinement options with session CVars |

**Suggested starting point:** keep Exaggeration at **1**, use Pitch/Roll/Twist
Strength **1** for the user-tested all-strengths setup, and start Easing at
**0.5**. Try **0.75** or **1** for stronger smoothing. The shipped Twist default
remains **0.5**. Higher exaggeration amplifies noise as well as articulation;
easing is the control to adjust for rapid correction changes. Select foot locking
according to your footage.

Strengths blend an estimated correction; they do not multiply the original
animation. Exaggeration 2 doubles the added correction before final caps.
At gain 1 and default Twist Strength 0.5, maximum added twist is 7.5°.
Unchecking Enable Pelvis Refinement bypasses this pass.

When processing starts, the log identifies the effective settings source:

```text
Pelvis settings (Performance): enabled=1, pitch=..., roll=..., twist=..., easing=..., exaggeration=..., image=1
```

The saved options are snapshotted once for image collection and final fitting.
They take precedence over CVars unless **Use Console Settings** is enabled.

## Easing and fitting behavior

Hip-line angle constrains lateral tilt; projected width supplies weaker twist
evidence. The fitter calibrates one robust hip-width scale over the processed
range and uses confidence-weighted robust pixel loss, bounded corrections,
orientation priors, and temporal regularization. Visible pelvis-dominated surface
anchors add pitch/twist evidence through bidirectional optical flow. Visibility,
foreground, flow-quality, spatial-spread, and observed-arm gates reject unreliable
tracks. Short tracks are reanchored to limit drift.

Immediate spine/thigh local rotations are compensated to retain their original
global orientations. Descendant positions may shift; the existing downstream
optimizer handles translation and foot constraints. Invalid/low-confidence hip
frames retain the original pose, and correction smoothing does not cross them.

Easing applies a normalized symmetric Gaussian to the bounded local rotation-vector
correction after fitting, strengths, and gain, before applying that correction.
Its standard deviation is **80 ms × Easing Strength**, converted using source fps,
with support extending three standard deviations each way. At 30 fps and strength
0.5, sigma is 1.2 frames. Weights renormalize at endpoints and invalid gaps;
constant corrections and final caps are preserved. Using future and past samples
avoids a consistent causal delay. Strong easing can soften intentional sharp
motion and increase image-fitting loss in exchange for temporal continuity.

One camera cannot uniquely recover absolute depth or front/back orientation.
Clothing deformation and model alignment also affect surface evidence. This is
an experimental refinement: lower fitting loss and correction roughness are not
ground-truth 3D or whole-body accuracy scores.

The pass retains grayscale frames and masks at a longest dimension of 448.
Memory and temporary storage scale with clip length. The helper runs synchronously
during finalization. Diagnostics off removes temporary evidence after fitting;
diagnostics on keeps it under `<Project>/Intermediate/PelvisTracking/<GUID>/`.
Correction CSV files go under `<Project>/Saved/PelvisRefinement/`.

## Validation

Recorded on the installed UE 5.8.3 Linux build on 2026-11-06:

- **Nine native automation tests passed:** eight under
  `MetaHuman.BodyTracker.PelvisRefinement` (`SyntheticTilt`, `SyntheticTwist`,
  `ConfidenceAndDisabled`, `LandmarkCalibration`, `SurfacePitch`, `Exaggeration`,
  `Easing`, `SavedSettings`) and `MetaHuman.Performance.PelvisSettingsDetails`.
- Tests cover synthetic rotation recovery, confidence/bypass behavior, calibration,
  gain/caps, easing roughness and slow-motion retention, constant/end/gap behavior,
  asset/console precedence, immutable settings snapshots, and actual detail rows.
- All **13 saved controls**, including nondefault values, survived saving and
  reloading in a fresh editor process using a disposable test asset.
- A fresh **50-frame solve with foot locking** used saved strengths 1, gain 1,
  easing 0.75 despite conflicting disabled/zero session CVars, and produced 50
  finite poses. The disposable asset was removed afterward.
- **Four Python image-helper tests passed:** known translation, empty-mask
  fallback, occlusion, and crossing-arm rejection.
- Earlier full-clip image-aware processing produced **243 finite poses**. In one
  gain-1 run before easing, combined image loss fell **80.4%**; usable surface
  evidence covered 59 frames. Coverage varies with texture, occlusion, and inference.
- The user tested the installed refinement, easing, and saved UI and confirmed
  that together they resolved the issues in their workflow.

Same-input replay of a 243-frame, 30 fps clip, with strengths 1 and gain 1:

| Easing | Correction acceleration RMS (degrees/frame²) | Reduction | Correction RMS retained |
| ---: | ---: | ---: | ---: |
| 0 | 4.189 | Reference | 100% |
| 0.5 | 0.933 | 77.7% | 94.9% |
| 1 | 0.342 | 91.8% | 90.2% |

Acceleration is the second finite difference of the **added local correction**,
not a perceptual jitter score. Replaying identical evidence isolates the easing
change from model-inference variability. Final corrections remained finite and
within component caps. Source Performances and exports were not overwritten by
the validation; footage and diagnostic pose data are not distributed here.

### Run native and image-helper tests

After building, run a separate unattended editor:

```bash
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" \
    -unattended -nullrhi -nosound -NoSplash -NoAutoSave \
    "-ExecCmds=Automation RunTests Pelvis" \
    "-TestExit=Automation Test Queue Empty"

/usr/bin/env -u PYTHONHOME -u PYTHONPATH -u LD_LIBRARY_PATH /usr/bin/python3 \
    "$PLUGIN/Resources/TestPelvisSurfaceTracking.py"
```

### Run a transient full-solve comparison

Enable Unreal Python scripting support. Replace the asset path and range with a
valid range from your footage; the end is exclusive. The output parent directory
must already exist. Use Vulkan (`-RenderOffscreen`) for actual processing:

```bash
MH_PELVIS_ASSET="/Game/MyFootage/Performance_MyClip.Performance_MyClip" \
MH_PELVIS_START=0 MH_PELVIS_END=50 \
MH_PELVIS_OUTPUT=/tmp/pelvis-validation.json \
MH_PELVIS_EASING=0.5 MH_PELVIS_GAIN=1 \
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" \
    -unattended -RenderOffscreen -nosound -NoSplash -NoAutoSave \
    "-ExecutePythonScript=$PLUGIN/Resources/ValidatePelvisRefinement.py"
```

The script processes separate transient body-only Performances with refinement
off/on and saves no assets. It copies axis strengths/caps from the source asset
and explicitly sets saved transient refinement options. It validates finite pose
output and logs `PELVIS_AB_VALIDATION_SUCCESS`. Set `MH_PELVIS_MODES=1` for a
single refined smoke test. Optional foot-locking, image-tracking, and baseline
reuse variables are documented in the script header. The checker expects a pose
for every requested frame, so use a range with a continuously detected subject.

## Console controls and diagnostic replay

To use session console values for processing, first enable the Performance's
**Advanced → Use Console Settings**. The other saved refinement controls become
disabled. The following are **Unreal editor console commands**, not shell commands:

```text
mh.BodyTracker.PelvisRefinement 1
mh.BodyTracker.PelvisRefinement.PitchStrength 1
mh.BodyTracker.PelvisRefinement.RollStrength 1
mh.BodyTracker.PelvisRefinement.TwistStrength 1
mh.BodyTracker.PelvisRefinement.Exaggeration 1
mh.BodyTracker.PelvisRefinement.EasingStrength 0.5
```

| Console variable | Default |
| --- | ---: |
| `mh.BodyTracker.PelvisRefinement` | 1 |
| `mh.BodyTracker.PelvisRefinement.TiltStrength` | 1 |
| `mh.BodyTracker.PelvisRefinement.PitchStrength` | 1 |
| `mh.BodyTracker.PelvisRefinement.RollStrength` | 1 |
| `mh.BodyTracker.PelvisRefinement.TwistStrength` | 0.5 |
| `mh.BodyTracker.PelvisRefinement.EasingStrength` | 0.5 |
| `mh.BodyTracker.PelvisRefinement.ImageTracking` | 1 |
| `mh.BodyTracker.PelvisRefinement.ImageWeight` | 4 |
| `mh.BodyTracker.PelvisRefinement.Exaggeration` | 1 |
| `mh.BodyTracker.PelvisRefinement.MaxTilt` | 20 |
| `mh.BodyTracker.PelvisRefinement.MaxTwist` | 15 |
| `mh.BodyTracker.PelvisRefinement.Dump` | 0 |

Meanings and ranges match the saved controls above. Local axes are SMPL-X
X=pitch, Y=twist, Z=roll. Console values affect the next processing run when console
mode is selected; they do not modify saved asset values.

For rapid same-input fitting/easing checks, enable diagnostics, process footage,
then use the retained `input.json` and adjacent `tracks.json`:

```text
mh.BodyTracker.PelvisRefinement.Replay "/absolute/path/to/input.json" "/existing/output/directory/replay"
```

Replay always uses current console strength/gain/easing/image/cap values directly.
It writes `<prefix>.json` and `<prefix>.csv` using recorded poses, landmarks,
surface tracks, and fps (30 fps for old inputs without fps). It does not rerun
inference, final leg/foot optimization, or retargeting. CSV columns
`uneased_pitch_deg`, `uneased_yaw_deg`, `uneased_roll_deg` retain the post-gain
correction before easing; `tilt_x_deg`, `twist_y_deg`, `tilt_z_deg` record the final
eased correction, and `easing_strength` records the setting.

## Troubleshooting and distributed contents

- **No controls:** confirm monocular input and Body Tracking, then verify that
  MetaHumanPerformance reflection and the loaded module were rebuilt.
- **Compile errors about missing Performance members:** the engine UI patch and
  its reflection rebuild are required alongside the Body Tracker hooks/overlay.
- **Changing CVars has no effect:** saved settings are authoritative unless the
  Performance has Use Console Settings enabled.
- **Little image evidence:** check system Python imports and the helper log;
  texture/visibility gates may legitimately reject most surface tracks.
- **Rapid changes:** keep exaggeration 1 and increase easing. Existing base-solve
  jitter is outside the correction-only filter's scope.
- **Patch conflict:** compare your plugin/engine version and local edits. The
  engine patch was checked against the original UE 5.8.3 archive; the plugin
  hook patch was round-tripped against the validated installed plugin and its
  reconstructed pre-refinement source. Check applicability to your own copy.

`integrations/MetaHumanPelvisRefinement/` contains newly authored algorithm,
image transport/helper, replay, and validation sources. The two optional patches
contain focused edits to existing engine/plugin files. Engine binaries, complete
Epic implementation files, model weights, user footage, and diagnostic pose data
are not included. See [NOTICE.md](NOTICE.md).
