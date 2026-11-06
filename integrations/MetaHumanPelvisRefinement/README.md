# MetaHuman pelvis refinement source overlay

Install into an existing **UE 5.8.3 MetaHumanBodyTracker** plugin, together with
the Body Tracker hook and MetaHumanPerformance settings patches.

**[Installation, saved controls, easing, and validation →](../../METAHUMAN-PELVIS-REFINEMENT.md)**

- `Source/MetaHumanBodyTracker/Private/PelvisRefinement.*`: bounded image-aware
  orientation fitting, temporal easing, and settings/options types.
- `PelvisImageTracking.*`: visible surface candidates and Linux image-helper transport.
- `PelvisRefinementReplay.cpp`: same-input diagnostic replay console command.
- `PelvisRefinementTests.cpp`: eight native regression tests.
- `Resources/TrackPelvisSurface.py`: masked bidirectional optical flow with
  arm/hand rejection; uses system Python, NumPy, and OpenCV.
- `Resources/TestPelvisSurfaceTracking.py`: four synthetic image tests.
- `Resources/ValidatePelvisRefinement.py`: transient off/on full-solve validation.

This is a source overlay, not a complete plugin. It contains no models, footage,
compiled modules, or machine-specific build manifests.
