#pragma once

#include "CoreMinimal.h"

class UPhysicsAsset;

/** Editor-only, name-based transfer. The destination's preview mesh defines the new joint frames. */
class PHYSICSASSETTRANSFERTOOLS_API FPhysicsAssetTransfer
{
public:
	/** Validation never edits either asset. All source bodies and joint bones must exist on the target mesh. */
	static bool Validate(const UPhysicsAsset* Source, const UPhysicsAsset* Destination, FText& OutError);

	/** One undoable replacement; copies profiles and disabled collision pairs, but retains the target mesh/settings.
	 * BeforeReplace runs inside the transaction after validation, to clear stale editor selection.
	 * Neither asset is saved automatically. */
	static bool Replace(const UPhysicsAsset* Source, UPhysicsAsset* Destination, FText& OutError,
		TFunction<void()> BeforeReplace = {});
};
