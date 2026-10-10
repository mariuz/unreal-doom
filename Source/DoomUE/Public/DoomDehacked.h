// DeHackEd patches (DEHACKED lumps and -deh files) applied to the port's tables:
// Thing blocks (health, speed, size, pain chance, missile speed and damage), Misc,
// Text blocks and BEX [SPRITES] / [SOUNDS] (renames, or replacing a loaded string),
// (starting health and bullets, caps, armor classes), Ammo (max and pickup sizes),
// BEX [PARS] and [STRINGS]. Frame / Pointer / [CODEPTR] / Sound / Sprite edits need
// Doom's state table, which this port replaces with frame sequences: they are
// skipped and counted.
#pragma once

#include "CoreMinimal.h"

struct FDehackedResult
{
	int32 Things = 0, Misc = 0, Ammo = 0, Pars = 0, Strings = 0, Renames = 0, Skipped = 0;
	FString ToString() const
	{
		return FString::Printf(TEXT("%d thing fields, %d misc, %d ammo, %d pars, %d strings, %d renames, %d skipped"), Things, Misc, Ammo, Pars, Strings, Renames, Skipped);
	}
};

namespace DoomDehacked
{
	/** Applies a patch's text to DoomInfo's tables, rules, strings and par times. */
	DOOMUE_API FDehackedResult Apply(const FString& Text);
}
