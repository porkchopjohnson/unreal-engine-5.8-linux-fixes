# Source and licensing context

Unreal Engine is developed by Epic Games. The patch files contain limited context from Unreal Engine source; that context remains subject to its original copyright and applicable Unreal Engine license terms.

This repository does not include an Unreal Engine checkout, engine assets, proprietary binary dependencies, compiled engine or plugin libraries, or capture models. Apply the patches to your own appropriately obtained UE 5.8.3 source tree.

The optional custom-plugin patch records changes to the named locally maintained plugins; their complete source is not distributed here. No blanket license over Epic's source or third-party plugin code is asserted by this repository.

This is a community/local-build record, not an official Epic Games release or compatibility guarantee.

Patch 0007 modifies the bundled SDL3 X11 implementation. SDL is copyright Sam Lantinga and contributors and uses the zlib license; retain the original notices in your SDL source tree. Its complete source and compiled libraries are not distributed here.

`plugins/PhysicsAssetTransferTools` contains the newly authored optional editor plugin and its regression tests, using public Unreal APIs. It does not include Unreal Engine implementation files or the unrelated third-party plugins mentioned above. `tests/SDLPopupSyncTest.c` and the Python profiling script are standalone diagnostic helpers.
