# MetaHuman Video Importer

**Optional editor plugin for Unreal Engine 5.8.3 on Linux — version 1.1.**

Import a video directly in Unreal without going through Live Link Hub. The plugin
converts it into image frames and optional audio, then creates a configured
**Monocular Footage MetaHuman Performance** asset. Open that asset and click
**Process** to run MetaHuman Animator.

This folder contains the complete source for the importer. It needs to be built
for your engine; it is not a precompiled plugin download. You can use it without
applying this repository's engine fixes or installing the optional CUDA integration.

## 1. Check the requirements

- Your own **UE 5.8.3 Linux source build** and a project that opens with it.
- MetaHuman Animator, Capture Data and Img Media available in that engine. The
  plugin descriptor enables these dependencies; MetaHuman's normal models and
  processing dependencies must also be installed.
- FFmpeg and FFprobe installed at **`/usr/bin/ffmpeg`** and
  **`/usr/bin/ffprobe`**. Install the appropriate FFmpeg package for your Linux
  distribution. No FFmpeg executables are bundled.
- Space for the converted PNG frames, which can be much larger than the video.

Check the FFmpeg tools in a terminal:

```bash
/usr/bin/ffmpeg -version
/usr/bin/ffprobe -version
```

## 2. Copy the plugin into your project

1. Close Unreal.
2. Open the folder containing your project's `.uproject` file.
3. Create a `Plugins` folder there if it does not exist.
4. Copy this **entire `MetaHumanVideoImporter` folder** into `Plugins`.

The result should look like this:

```text
MyProject/
  MyProject.uproject
  Plugins/
    MetaHumanVideoImporter/
      MetaHumanVideoImporter.uplugin
      README.md
      Source/
```

An engine-wide installation at
`<EngineRoot>/Engine/Plugins/Editor/MetaHumanVideoImporter` is also supported.
Install only one copy: do not leave both a project copy and an engine copy enabled.

## 3. Enable and build it

To enable it before a terminal build, add this entry to the `Plugins` array in
your `.uproject` file using a text editor:

```json
{ "Name": "MetaHumanVideoImporter", "Enabled": true }
```

For example, if the project does not already have a `Plugins` array, add the
following **inside the existing outer JSON object**, with commas separating it
from other fields:

```json
"Plugins": [
  { "Name": "MetaHumanVideoImporter", "Enabled": true }
]
```

If an array already exists, add just the entry to it. Keep the project's other
settings and plugins.

Build the editor with your project, replacing both example paths:

```bash
ENGINE="/path/to/UnrealEngine-5.8.3-release"
PROJECT="/path/to/MyProject/MyProject.uproject"

bash "$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" \
    UnrealEditor Linux Development -Project="$PROJECT" -WaitMutex
```

For an existing engine build, see the repository's
[build-scope instructions](../../BUILDING.md#existing-builds-inspect-scope-first)
before starting a potentially large rebuild. Keep the compiler/configuration
settings consistent with your existing engine outputs.

Open your project with the same engine. You can check the plugin under
**Edit → Plugins → MetaHuman Video Importer**. Restart if prompted. When loaded,
the editor has a **Tools → MetaHuman Video Importer** menu entry.

## 4. Import and process a video

1. Open **Tools → MetaHuman Video Importer**.
2. Click **Add videos…** and choose one or more video files. You can also drop
   files from your Linux file manager into the importer panel.
3. Leave the asset folder at `/Game/MetaHumanFootage`, or choose another folder
   under `/Game`. `/Game` means your project's **Content** folder, not a Linux path.
4. Click **Import**. Videos convert one at a time; the panel shows progress and results.
5. The last successful Performance opens automatically if that option is checked.
   Otherwise find it in the Content Browser.
6. Check the face/body tracking options appropriate for your task, then click
   **Process** in the Performance editor.

Importing prepares the footage; **Process** runs the actual animation solve.
Use the normal Vulkan editor for processing. The optional
[CUDA integration](../../METAHUMAN-BODY-TRACKER-CUDA.md) accelerates supported
offline body-tracking models on NVIDIA GPUs and is installed separately.

## What gets saved

For `Interview.mp4`, the default output is:

```text
MyProject/
  Content/MetaHumanFootage/Interview/
    Images_Interview.uasset
    Audio_Interview.uasset       # only when the video has audio
    Footage_Interview.uasset
    Performance_Interview.uasset
  SourceFootage/MetaHuman/Interview/
    Frames/00000000.png ...
    Audio.wav                   # only when the video has audio
    Import.json
```

Assets are saved automatically. Existing takes are preserved: importing the same
name again creates `Interview_001`, etc. Frame paths use `{project_dir}` so the
project and its converted footage can move together.

**Keep both `Content` and `SourceFootage` when copying or backing up the project.**
The original video is recorded in `Import.json`, but is no longer needed for
processing once conversion is complete.

## Updating older imports: version 1.1

Version 1.0 could request one frame beyond MetaHuman's usable image/audio range.
For example, a 441-frame image sequence can have only 440 usable audio frames
because of audio-duration rounding. Clicking **Process** could then crash in
`ResetOutput`, including with body-only tracking.

Version 1.1 sets the processing range through MetaHuman's range-clamping API.
After updating the plugin and rebuilding:

1. Restart Unreal and reopen an existing imported Performance.
2. The plugin repairs an invalid range when its asset editor opens.
3. **Save the Performance** to keep the repair.

No video reconversion is needed. Tracking selections, existing output, and valid
user-selected subranges are preserved. Repair is limited to monocular Performances
referencing this importer's `SourceFootage/MetaHuman` directory.

## Conversion behavior

- Uses the first non-cover-art video stream and first audio stream.
- FFmpeg applies its normal video rotation handling and decodes full-resolution
  RGB PNGs.
- Reads the rational average frame rate, falling back to the nominal rate.
  Variable-frame-rate video is resampled to a constant rate, duplicating/dropping
  frames as necessary to preserve duration. Supported rates are above 0 and up to
  240 fps.
- Audio becomes 48 kHz mono PCM WAV, imported as a Sound Wave. Videos without
  audio create video-only Performances.
- Uses clip-relative timing rather than embedded production timecode. Audio is
  aligned to its offset from the first video frame and padded/trimmed to the
  converted video duration.
- No MetaHuman Identity is needed for this monocular workflow. This importer
  prepares single-camera footage, not depth/stereo capture.
- Conversion runs in child processes. Cancelling or closing the tab stops the
  active process and removes incomplete conversion files. Completed takes remain
  saved. If asset creation/saving fails, converted files are retained so existing
  references remain valid; see the panel's result log.
- HDR input is decoded to 8-bit RGB without HDR tone mapping. Video/project paths
  containing double quotes or newlines are unsupported.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| No importer entry under Tools | Check the folder layout, enable the plugin, rebuild it for this engine, and restart. |
| Cannot run FFmpeg or FFprobe | Both executables must exist at the exact `/usr/bin` paths listed above. |
| Destination folder rejected | Enter an Unreal asset path such as `/Game/MetaHumanFootage`, not a filesystem path. |
| A converted take cannot find its frames after moving projects | Move the `SourceFootage` folder alongside the project's `Content` folder. |
| Conversion fails | Read the result log in the importer panel; it includes FFmpeg diagnostics. |
| An older import crashes when processing | Rebuild version 1.1, restart, reopen the Performance, and save the repaired range. |

## Validation

The Linux module was compiled and linked against UE 5.8.3. Both existing automation
tests passed on 2026-11-05, and the user confirmed the importer works with their
footage. That footage was subsequently processed through the body-tracking workflow.

- `MetaHuman.VideoImporter.EndToEnd`: synthetic audio/video and silent clips,
  saved asset configuration, rational frame rates, project-relative paths,
  duplicate-name handling, and invalid-input/cancellation cleanup.
- `MetaHuman.VideoImporter.AudioRangeBoundary`: the 441-image/440-audio-frame
  boundary, safe processing ranges, legacy body-only asset repair, preservation
  of output and valid subranges, and automatic repair on editor open.

Run the tests in a **disposable project** with the plugin enabled. They write
test footage and assets into that project and retain saved outputs for inspection:

```bash
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "/path/to/TestProject.uproject" \
    -unattended -nullrhi -nosound \
    "-ExecCmds=Automation RunTests MetaHuman.VideoImporter" \
    "-TestExit=Automation Test Queue Empty" -log
```

These headless tests validate import and range handling, not facial solve quality
or desktop drag-and-drop. Actual footage processing requires the Vulkan RHI.
No models, footage, FFmpeg binaries, compiled plugin libraries, or generated build
metadata are distributed here. See [NOTICE.md](../../NOTICE.md).
