// Doom's controls as Enhanced Input objects, built in one place: the input actions, the gameplay,
// menu and automap-pan mapping contexts, and which keyboard and mouse mappings the player may rebind
// (each with a player-mappable name, in the order the Controls menu lists them).
//
// The content builder saves the set as assets in /Game/Doom/Input (editable in the editor, cooked
// into packaged games); ADoomPlayerController loads those, and builds the same set in code when they
// are missing. Rebinding goes through Enhanced Input's user settings, saved per player.
#pragma once

#include "CoreMinimal.h"

class UInputAction;
class UInputMappingContext;
struct FKey;

struct DOOMUE_API FDoomInputObjects
{
	UInputAction* Move = nullptr;
	UInputAction* Look = nullptr;
	UInputAction* Fire = nullptr;
	UInputAction* Use = nullptr;
	UInputAction* Run = nullptr;
	UInputAction* Automap = nullptr;
	UInputAction* AutomapZoom = nullptr;
	UInputAction* AutomapPan = nullptr;
	UInputAction* AutomapFollow = nullptr;
	UInputAction* AutomapBig = nullptr;
	UInputAction* AutomapMark = nullptr;
	UInputAction* AutomapClear = nullptr;
	UInputAction* AutomapGrid = nullptr;
	UInputAction* NextWeapon = nullptr;
	UInputAction* PrevWeapon = nullptr;
	TArray<UInputAction*> Slots;       // weapon slots 1-7
	UInputAction* MenuUp = nullptr;
	UInputAction* MenuDown = nullptr;
	UInputAction* MenuLeft = nullptr;
	UInputAction* MenuRight = nullptr;
	UInputAction* MenuSelect = nullptr;
	UInputAction* MenuBack = nullptr;
	UInputAction* MenuClose = nullptr;
	UInputAction* QuickSave = nullptr;
	UInputAction* QuickLoad = nullptr;
	TArray<UInputAction*> Pages;       // F1-F4
	UInputMappingContext* Gameplay = nullptr;
	UInputMappingContext* Menu = nullptr;
	UInputMappingContext* AutomapPanContext = nullptr;

	/** Every object, for saving (actions first, then the contexts that use them). */
	TArray<class UObject*> All() const;
};

namespace DoomInput
{
	/** Where the content builder saves the set, and the controller looks for it. */
	constexpr const TCHAR* AssetFolder = TEXT("/Game/Doom/Input/");

	/** Creates the set: New(Class, Name) makes each top-level object (an action or a context). */
	DOOMUE_API void Build(TFunctionRef<class UObject*(class UClass*, FName)> New, FDoomInputObjects& Out);

	/** Loads the saved set; false (and Out untouched) if any asset is missing. */
	DOOMUE_API bool LoadAssets(FDoomInputObjects& Out);

	/** A rebindable control: its player-mappable name and the Controls menu's label. */
	struct FMappable
	{
		FName Name;
		FText Label;
		/** A gamepad button's binding (its row takes only gamepad buttons; the keyboard rows none). */
		bool bGamepad = false;
	};
	/** Every rebindable control, in menu order. */
	DOOMUE_API const TArray<FMappable>& Mappables();

	/** A key as Doom's font can show it: "MOUSE1", "CTRL", "WHEEL UP", "KP -"... */
	DOOMUE_API FString KeyLabel(const FKey& Key);
}
