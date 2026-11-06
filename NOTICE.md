# Source and licensing context

Unreal Engine is developed by Epic Games. The patch files contain limited context from Unreal Engine source; that context remains subject to its original copyright and applicable Unreal Engine license terms.

This repository does not include an Unreal Engine checkout, engine assets, proprietary binary dependencies, compiled engine or plugin libraries, or capture models. Apply the patches to your own appropriately obtained UE 5.8.3 source tree.

The optional custom-plugin patch records changes to the named locally maintained plugins; their complete source is not distributed here. No blanket license over Epic's source or third-party plugin code is asserted by this repository.

This is a community/local-build record, not an official Epic Games release or compatibility guarantee.

Patch 0007 modifies the bundled SDL3 X11 implementation. SDL is copyright Sam Lantinga and contributors and uses the zlib license; retain the original notices in your SDL source tree. Its complete source and compiled libraries are not distributed here.

`plugins/PhysicsAssetTransferTools` contains the newly authored optional editor plugin and its regression tests, using public Unreal APIs. It does not include Unreal Engine implementation files or the unrelated third-party plugins mentioned above. `tests/SDLPopupSyncTest.c` and the Python profiling script are standalone diagnostic helpers.

`integrations/MetaHumanBodyTrackerCUDA` contains newly authored adapter, worker, setup, and validation sources for use with an existing MetaHumanBodyTracker plugin. It is not the complete plugin and contains no model weights or capture assets. The optional `metahuman-*.patch` files contain limited context from the existing engine/plugin sources under their original terms. `SetupCuda.py` downloads ONNX Runtime and NVIDIA dependency wheels separately and retains their packaged license files alongside the installed libraries; those dependencies are not committed here.

`plugins/MetaHumanVideoImporter` contains the newly authored optional editor plugin and its automation tests, using Unreal's editor and MetaHuman APIs. It requires the user's own engine and MetaHuman dependencies. It invokes separately installed FFmpeg/FFprobe executables; no FFmpeg source/binaries, Unreal implementation files, pretrained models, user footage, or compiled plugin libraries are included.

`integrations/MetaHumanPelvisRefinement` contains newly authored pelvis fitting, temporal easing, image-helper transport, replay, and validation sources for an existing MetaHumanBodyTracker plugin. The associated optional pelvis hook and Performance-settings patches contain limited context from the user's engine/plugin sources under their original terms. The image helper uses separately installed system Python, NumPy, and OpenCV; those dependencies, complete Epic implementation files, model weights, footage, diagnostic pose data, and compiled modules are not distributed here.
