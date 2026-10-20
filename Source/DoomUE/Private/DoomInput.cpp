#include "DoomInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "PlayerMappableKeySettings.h"
#include "InputCoreTypes.h"

namespace
{
	const TArray<DoomInput::FMappable> GMappables = {
		{ TEXT("MoveForward"), TEXT("MOVE FORWARD") },
		{ TEXT("MoveBackward"), TEXT("MOVE BACKWARD") },
		{ TEXT("StrafeLeft"), TEXT("STRAFE LEFT") },
		{ TEXT("StrafeRight"), TEXT("STRAFE RIGHT") },
		{ TEXT("TurnLeft"), TEXT("TURN LEFT") },
		{ TEXT("TurnRight"), TEXT("TURN RIGHT") },
		{ TEXT("Fire"), TEXT("FIRE") },
		{ TEXT("Use"), TEXT("USE / OPEN") },
		{ TEXT("Run"), TEXT("RUN") },
		{ TEXT("NextWeapon"), TEXT("NEXT WEAPON") },
		{ TEXT("PrevWeapon"), TEXT("PREVIOUS WEAPON") },
		{ TEXT("Weapon1"), TEXT("FIST / CHAINSAW") },
		{ TEXT("Weapon2"), TEXT("PISTOL") },
		{ TEXT("Weapon3"), TEXT("SHOTGUNS") },
		{ TEXT("Weapon4"), TEXT("CHAINGUN") },
		{ TEXT("Weapon5"), TEXT("ROCKET LAUNCHER") },
		{ TEXT("Weapon6"), TEXT("PLASMA GUN") },
		{ TEXT("Weapon7"), TEXT("BFG 9000") },
		{ TEXT("Automap"), TEXT("AUTOMAP") },
		{ TEXT("AutomapZoomIn"), TEXT("MAP ZOOM IN") },
		{ TEXT("AutomapZoomOut"), TEXT("MAP ZOOM OUT") },
		{ TEXT("AutomapFollow"), TEXT("MAP FOLLOW") },
		{ TEXT("AutomapBig"), TEXT("WHOLE MAP") },
		{ TEXT("AutomapMark"), TEXT("MAP MARK") },
		{ TEXT("AutomapClear"), TEXT("CLEAR MARKS") },
		{ TEXT("AutomapGrid"), TEXT("MAP GRID") },
		{ TEXT("QuickSave"), TEXT("QUICK SAVE") },
		{ TEXT("QuickLoad"), TEXT("QUICK LOAD") },
		// the gamepad's buttons (the sticks move and look, and stay as they are)
		{ TEXT("PadFire"), TEXT("PAD FIRE"), true },
		{ TEXT("PadUse"), TEXT("PAD USE / OPEN"), true },
		{ TEXT("PadRun"), TEXT("PAD RUN"), true },
		{ TEXT("PadNextWeapon"), TEXT("PAD NEXT WEAPON"), true },
		{ TEXT("PadPrevWeapon"), TEXT("PAD PREVIOUS WEAPON"), true },
		{ TEXT("PadAutomap"), TEXT("PAD AUTOMAP"), true },
	};

	/** Marks a mapping player-mappable. FEnhancedActionKeyMapping keeps these protected (they're set in
	 *  its editor details panel), so through reflection, as the panel does. */
	void MakeMappable(FEnhancedActionKeyMapping& M, UPlayerMappableKeySettings* Settings)
	{
		UScriptStruct* Struct = FEnhancedActionKeyMapping::StaticStruct();
		if (FEnumProperty* Behavior = FindFProperty<FEnumProperty>(Struct, TEXT("SettingBehavior")))
		{
			Behavior->GetUnderlyingProperty()->SetIntPropertyValue(Behavior->ContainerPtrToValuePtr<void>(&M), (int64)EPlayerMappableKeySettingBehaviors::OverrideSettings);
		}
		if (FObjectPropertyBase* Prop = FindFProperty<FObjectPropertyBase>(Struct, TEXT("PlayerMappableKeySettings")))
		{
			Prop->SetObjectPropertyValue_InContainer(&M, Settings);
		}
	}

	const TCHAR* LabelOf(FName Name)
	{
		for (const DoomInput::FMappable& M : GMappables)
		{
			if (M.Name == Name) return M.Label;
		}
		return TEXT("");
	}
}

const TArray<DoomInput::FMappable>& DoomInput::Mappables()
{
	return GMappables;
}

TArray<UObject*> FDoomInputObjects::All() const
{
	TArray<UObject*> Out = { Move, Look, Fire, Use, Run, Automap, AutomapZoom, AutomapPan, AutomapFollow, AutomapBig, AutomapMark,
		AutomapClear, AutomapGrid, NextWeapon, PrevWeapon, MenuUp, MenuDown, MenuLeft, MenuRight, MenuSelect, MenuBack, MenuClose,
		QuickSave, QuickLoad };
	for (UInputAction* A : Slots) Out.Add(A);
	for (UInputAction* A : Pages) Out.Add(A);
	Out.Add(Gameplay);
	Out.Add(Menu);
	Out.Add(AutomapPanContext);
	return Out;
}

void DoomInput::Build(TFunctionRef<UObject*(UClass*, FName)> New, FDoomInputObjects& Out)
{
	auto Action = [&](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* A = CastChecked<UInputAction>(New(UInputAction::StaticClass(), FName(Name)));
		A->ValueType = Type;
		return A;
	};
	Out.Move = Action(TEXT("IA_Move"), EInputActionValueType::Axis2D);
	Out.Look = Action(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	Out.Fire = Action(TEXT("IA_Fire"), EInputActionValueType::Boolean);
	Out.Use = Action(TEXT("IA_Use"), EInputActionValueType::Boolean);
	Out.Run = Action(TEXT("IA_Run"), EInputActionValueType::Boolean);
	Out.Automap = Action(TEXT("IA_Automap"), EInputActionValueType::Boolean);
	Out.AutomapZoom = Action(TEXT("IA_AutomapZoom"), EInputActionValueType::Axis1D);
	Out.AutomapPan = Action(TEXT("IA_AutomapPan"), EInputActionValueType::Axis2D);
	Out.AutomapFollow = Action(TEXT("IA_AutomapFollow"), EInputActionValueType::Boolean);
	Out.AutomapBig = Action(TEXT("IA_AutomapBig"), EInputActionValueType::Boolean);
	Out.AutomapMark = Action(TEXT("IA_AutomapMark"), EInputActionValueType::Boolean);
	Out.AutomapClear = Action(TEXT("IA_AutomapClearMarks"), EInputActionValueType::Boolean);
	Out.AutomapGrid = Action(TEXT("IA_AutomapGrid"), EInputActionValueType::Boolean);
	Out.NextWeapon = Action(TEXT("IA_NextWeapon"), EInputActionValueType::Boolean);
	Out.PrevWeapon = Action(TEXT("IA_PrevWeapon"), EInputActionValueType::Boolean);
	Out.Slots.Reset();
	for (int32 i = 0; i < 7; ++i)
	{
		Out.Slots.Add(Action(*FString::Printf(TEXT("IA_Slot%d"), i + 1), EInputActionValueType::Boolean));
	}
	Out.MenuUp = Action(TEXT("IA_MenuUp"), EInputActionValueType::Boolean);
	Out.MenuDown = Action(TEXT("IA_MenuDown"), EInputActionValueType::Boolean);
	Out.MenuLeft = Action(TEXT("IA_MenuLeft"), EInputActionValueType::Boolean);
	Out.MenuRight = Action(TEXT("IA_MenuRight"), EInputActionValueType::Boolean);
	Out.MenuSelect = Action(TEXT("IA_MenuSelect"), EInputActionValueType::Boolean);
	Out.MenuBack = Action(TEXT("IA_MenuBack"), EInputActionValueType::Boolean);
	Out.MenuClose = Action(TEXT("IA_MenuClose"), EInputActionValueType::Boolean);
	Out.QuickSave = Action(TEXT("IA_QuickSave"), EInputActionValueType::Boolean);
	Out.QuickLoad = Action(TEXT("IA_QuickLoad"), EInputActionValueType::Boolean);
	Out.Pages.Reset();
	for (int32 i = 0; i < 4; ++i)
	{
		Out.Pages.Add(Action(*FString::Printf(TEXT("IA_MenuF%d"), i + 1), EInputActionValueType::Boolean));
	}

	// a mapping; Mappable names the ones the player may rebind (keyboard and mouse), the rest stay fixed
	auto Map = [](UInputMappingContext* Ctx, UInputAction* A, const FKey& Key, FName Mappable = NAME_None) -> FEnhancedActionKeyMapping&
	{
		FEnhancedActionKeyMapping& M = Ctx->MapKey(A, Key);
		if (!Mappable.IsNone())
		{
			UPlayerMappableKeySettings* S = NewObject<UPlayerMappableKeySettings>(Ctx);
			S->Name = Mappable;
			S->DisplayName = FText::FromString(LabelOf(Mappable));
			S->DisplayCategory = FText::FromString(TEXT("Doom"));
			MakeMappable(M, S);
		}
		return M; // the others inherit the action's settings: none, so they stay fixed
	};

	UInputMappingContext* G = Out.Gameplay = CastChecked<UInputMappingContext>(New(UInputMappingContext::StaticClass(), TEXT("IMC_Doom")));
	auto Move = [&](const FKey& Key, bool bSwizzle, bool bNegate, FName Mappable)
	{
		FEnhancedActionKeyMapping& M = Map(G, Out.Move, Key, Mappable);
		if (bSwizzle) M.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(G));
		if (bNegate) M.Modifiers.Add(NewObject<UInputModifierNegate>(G));
	};
	// forward/back on Y (swizzled), strafe on X
	Move(EKeys::W, true, false, TEXT("MoveForward"));
	Move(EKeys::Up, true, false, TEXT("MoveForward"));
	Move(EKeys::S, true, true, TEXT("MoveBackward"));
	Move(EKeys::Down, true, true, TEXT("MoveBackward"));
	Move(EKeys::D, false, false, TEXT("StrafeRight"));
	Move(EKeys::A, false, true, TEXT("StrafeLeft"));
	Map(G, Out.Move, EKeys::Gamepad_Left2D);
	Map(G, Out.Look, EKeys::Mouse2D);
	{
		FEnhancedActionKeyMapping& Pad = Map(G, Out.Look, EKeys::Gamepad_Right2D);
		UInputModifierNegate* Neg = NewObject<UInputModifierNegate>(G);
		Neg->bX = false; Neg->bY = true; Neg->bZ = false;
		Pad.Modifiers.Add(Neg);
		// stick deflection is a turn rate: per second, not per frame (the old feel at 60 fps)
		UInputModifierScalar* PadRate = NewObject<UInputModifierScalar>(G);
		PadRate->Scalar = FVector(60, 60, 1);
		Pad.Modifiers.Add(PadRate);
		Pad.Modifiers.Add(NewObject<UInputModifierScaleByDeltaTime>(G));
	}
	// arrow keys turn, like Doom: a rate per second (the old per-frame 2.5 at 60 fps), so the frame rate
	// doesn't change how fast you turn
	auto Turn = [&](const FKey& Key, bool bLeft, FName Mappable)
	{
		FEnhancedActionKeyMapping& M = Map(G, Out.Look, Key, Mappable);
		if (bLeft) M.Modifiers.Add(NewObject<UInputModifierNegate>(G));
		UInputModifierScalar* Sc = NewObject<UInputModifierScalar>(G);
		Sc->Scalar = FVector(150, 150, 1);
		M.Modifiers.Add(Sc);
		M.Modifiers.Add(NewObject<UInputModifierScaleByDeltaTime>(G));
	};
	Turn(EKeys::Left, true, TEXT("TurnLeft"));
	Turn(EKeys::Right, false, TEXT("TurnRight"));
	Map(G, Out.Fire, EKeys::LeftMouseButton, TEXT("Fire"));
	Map(G, Out.Fire, EKeys::LeftControl, TEXT("Fire"));
	Map(G, Out.Fire, EKeys::Gamepad_RightTrigger, TEXT("PadFire"));
	Map(G, Out.Use, EKeys::E, TEXT("Use"));
	Map(G, Out.Use, EKeys::SpaceBar, TEXT("Use"));
	Map(G, Out.Use, EKeys::RightMouseButton, TEXT("Use"));
	Map(G, Out.Use, EKeys::Gamepad_FaceButton_Bottom, TEXT("PadUse"));
	Map(G, Out.Run, EKeys::LeftShift, TEXT("Run"));
	Map(G, Out.Run, EKeys::Gamepad_LeftThumbstick, TEXT("PadRun"));
	Map(G, Out.Automap, EKeys::Tab, TEXT("Automap"));
	Map(G, Out.Automap, EKeys::Gamepad_Special_Left, TEXT("PadAutomap"));
	// the automap's keys (they do nothing while it is closed): = and - zoom, F follow, 0 the whole map,
	// M mark, C clear the marks, G grid; the arrows pan when follow mode is off (the pan context)
	Map(G, Out.AutomapZoom, EKeys::Equals, TEXT("AutomapZoomIn"));
	Map(G, Out.AutomapZoom, EKeys::Add, TEXT("AutomapZoomIn"));
	Map(G, Out.AutomapZoom, EKeys::Hyphen, TEXT("AutomapZoomOut")).Modifiers.Add(NewObject<UInputModifierNegate>(G));
	Map(G, Out.AutomapZoom, EKeys::Subtract, TEXT("AutomapZoomOut")).Modifiers.Add(NewObject<UInputModifierNegate>(G));
	Map(G, Out.AutomapFollow, EKeys::F, TEXT("AutomapFollow"));
	Map(G, Out.AutomapBig, EKeys::Zero, TEXT("AutomapBig"));
	Map(G, Out.AutomapMark, EKeys::M, TEXT("AutomapMark"));
	Map(G, Out.AutomapClear, EKeys::C, TEXT("AutomapClear"));
	Map(G, Out.AutomapGrid, EKeys::G, TEXT("AutomapGrid"));
	Map(G, Out.NextWeapon, EKeys::MouseScrollDown, TEXT("NextWeapon"));
	Map(G, Out.NextWeapon, EKeys::Gamepad_RightShoulder, TEXT("PadNextWeapon"));
	Map(G, Out.PrevWeapon, EKeys::MouseScrollUp, TEXT("PrevWeapon"));
	Map(G, Out.PrevWeapon, EKeys::Gamepad_LeftShoulder, TEXT("PadPrevWeapon"));
	const FKey Digits[7] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven };
	for (int32 i = 0; i < 7; ++i)
	{
		Map(G, Out.Slots[i], Digits[i], FName(*FString::Printf(TEXT("Weapon%d"), i + 1)));
	}
	Map(G, Out.QuickSave, EKeys::F6, TEXT("QuickSave"));
	Map(G, Out.QuickLoad, EKeys::F9, TEXT("QuickLoad"));
	Map(G, Out.Pages[0], EKeys::F1);
	Map(G, Out.Pages[1], EKeys::F2);
	Map(G, Out.Pages[2], EKeys::F3);
	Map(G, Out.Pages[3], EKeys::F4);
	Map(G, Out.MenuBack, EKeys::Escape);
	Map(G, Out.MenuBack, EKeys::Gamepad_Special_Right);

	UInputMappingContext* P = Out.AutomapPanContext = CastChecked<UInputMappingContext>(New(UInputMappingContext::StaticClass(), TEXT("IMC_DoomAutomapPan")));
	Map(P, Out.AutomapPan, EKeys::Right);
	Map(P, Out.AutomapPan, EKeys::Left).Modifiers.Add(NewObject<UInputModifierNegate>(P));
	Map(P, Out.AutomapPan, EKeys::Up).Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(P));
	{
		FEnhancedActionKeyMapping& Down = Map(P, Out.AutomapPan, EKeys::Down);
		Down.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(P));
		Down.Modifiers.Add(NewObject<UInputModifierNegate>(P));
	}

	UInputMappingContext* M = Out.Menu = CastChecked<UInputMappingContext>(New(UInputMappingContext::StaticClass(), TEXT("IMC_DoomMenu")));
	Map(M, Out.MenuUp, EKeys::Up);
	Map(M, Out.MenuUp, EKeys::W);
	Map(M, Out.MenuUp, EKeys::Gamepad_DPad_Up);
	Map(M, Out.MenuDown, EKeys::Down);
	Map(M, Out.MenuDown, EKeys::S);
	Map(M, Out.MenuDown, EKeys::Gamepad_DPad_Down);
	Map(M, Out.MenuLeft, EKeys::Left);
	Map(M, Out.MenuLeft, EKeys::A);
	Map(M, Out.MenuLeft, EKeys::Gamepad_DPad_Left);
	Map(M, Out.MenuRight, EKeys::Right);
	Map(M, Out.MenuRight, EKeys::D);
	Map(M, Out.MenuRight, EKeys::Gamepad_DPad_Right);
	Map(M, Out.MenuSelect, EKeys::Enter);
	Map(M, Out.MenuSelect, EKeys::Y);
	Map(M, Out.MenuBack, EKeys::N);
	Map(M, Out.MenuBack, EKeys::BackSpace);
	Map(M, Out.MenuSelect, EKeys::SpaceBar);
	Map(M, Out.MenuSelect, EKeys::LeftMouseButton);
	Map(M, Out.MenuSelect, EKeys::Gamepad_FaceButton_Bottom);
	Map(M, Out.QuickLoad, EKeys::F9);
	// Esc (and Start) close the whole menu, as in Doom; Backspace, N and the gamepad's B go back one page
	Map(M, Out.MenuClose, EKeys::Escape);
	Map(M, Out.MenuClose, EKeys::Gamepad_Special_Right);
	Map(M, Out.MenuBack, EKeys::Gamepad_FaceButton_Right);
}

bool DoomInput::LoadAssets(FDoomInputObjects& Out)
{
	FDoomInputObjects L;
	bool bAll = true;
	auto Load = [&bAll](auto*& Slot, const FString& Name)
	{
		using T = std::remove_pointer_t<std::remove_reference_t<decltype(Slot)>>;
		Slot = LoadObject<T>(nullptr, *(FString(AssetFolder) + Name + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		bAll &= Slot != nullptr;
	};
	Load(L.Move, TEXT("IA_Move")); Load(L.Look, TEXT("IA_Look")); Load(L.Fire, TEXT("IA_Fire")); Load(L.Use, TEXT("IA_Use"));
	Load(L.Run, TEXT("IA_Run")); Load(L.Automap, TEXT("IA_Automap")); Load(L.AutomapZoom, TEXT("IA_AutomapZoom"));
	Load(L.AutomapPan, TEXT("IA_AutomapPan")); Load(L.AutomapFollow, TEXT("IA_AutomapFollow")); Load(L.AutomapBig, TEXT("IA_AutomapBig"));
	Load(L.AutomapMark, TEXT("IA_AutomapMark")); Load(L.AutomapClear, TEXT("IA_AutomapClearMarks")); Load(L.AutomapGrid, TEXT("IA_AutomapGrid"));
	Load(L.NextWeapon, TEXT("IA_NextWeapon")); Load(L.PrevWeapon, TEXT("IA_PrevWeapon"));
	Load(L.MenuUp, TEXT("IA_MenuUp")); Load(L.MenuDown, TEXT("IA_MenuDown")); Load(L.MenuLeft, TEXT("IA_MenuLeft"));
	Load(L.MenuRight, TEXT("IA_MenuRight")); Load(L.MenuSelect, TEXT("IA_MenuSelect")); Load(L.MenuBack, TEXT("IA_MenuBack"));
	Load(L.MenuClose, TEXT("IA_MenuClose")); Load(L.QuickSave, TEXT("IA_QuickSave")); Load(L.QuickLoad, TEXT("IA_QuickLoad"));
	L.Slots.SetNum(7);
	for (int32 i = 0; i < 7; ++i) Load(L.Slots[i], FString::Printf(TEXT("IA_Slot%d"), i + 1));
	L.Pages.SetNum(4);
	for (int32 i = 0; i < 4; ++i) Load(L.Pages[i], FString::Printf(TEXT("IA_MenuF%d"), i + 1));
	Load(L.Gameplay, TEXT("IMC_Doom")); Load(L.Menu, TEXT("IMC_DoomMenu")); Load(L.AutomapPanContext, TEXT("IMC_DoomAutomapPan"));
	if (bAll)
	{
		Out = L;
	}
	return bAll;
}

FString DoomInput::KeyLabel(const FKey& Key)
{
	static const TMap<FName, const TCHAR*> Short = {
		{ EKeys::LeftMouseButton.GetFName(), TEXT("MOUSE1") }, { EKeys::RightMouseButton.GetFName(), TEXT("MOUSE2") },
		{ EKeys::MiddleMouseButton.GetFName(), TEXT("MOUSE3") }, { EKeys::ThumbMouseButton.GetFName(), TEXT("MOUSE4") },
		{ EKeys::ThumbMouseButton2.GetFName(), TEXT("MOUSE5") }, { EKeys::MouseScrollUp.GetFName(), TEXT("WHEEL UP") },
		{ EKeys::MouseScrollDown.GetFName(), TEXT("WHEEL DOWN") }, { EKeys::LeftShift.GetFName(), TEXT("SHIFT") },
		{ EKeys::RightShift.GetFName(), TEXT("RSHIFT") }, { EKeys::LeftControl.GetFName(), TEXT("CTRL") },
		{ EKeys::RightControl.GetFName(), TEXT("RCTRL") }, { EKeys::LeftAlt.GetFName(), TEXT("ALT") },
		{ EKeys::RightAlt.GetFName(), TEXT("RALT") }, { EKeys::SpaceBar.GetFName(), TEXT("SPACE") },
		{ EKeys::Equals.GetFName(), TEXT("=") }, { EKeys::Hyphen.GetFName(), TEXT("-") }, { EKeys::Add.GetFName(), TEXT("KP +") },
		{ EKeys::Subtract.GetFName(), TEXT("KP -") }, { EKeys::Enter.GetFName(), TEXT("ENTER") }, { EKeys::Tab.GetFName(), TEXT("TAB") },
		{ EKeys::BackSpace.GetFName(), TEXT("BACKSPACE") },
		{ EKeys::Gamepad_FaceButton_Bottom.GetFName(), TEXT("PAD A") }, { EKeys::Gamepad_FaceButton_Right.GetFName(), TEXT("PAD B") },
		{ EKeys::Gamepad_FaceButton_Left.GetFName(), TEXT("PAD X") }, { EKeys::Gamepad_FaceButton_Top.GetFName(), TEXT("PAD Y") },
		{ EKeys::Gamepad_LeftShoulder.GetFName(), TEXT("PAD LB") }, { EKeys::Gamepad_RightShoulder.GetFName(), TEXT("PAD RB") },
		{ EKeys::Gamepad_LeftTrigger.GetFName(), TEXT("PAD LT") }, { EKeys::Gamepad_RightTrigger.GetFName(), TEXT("PAD RT") },
		{ EKeys::Gamepad_Special_Left.GetFName(), TEXT("PAD BACK") }, { EKeys::Gamepad_Special_Right.GetFName(), TEXT("PAD START") },
		{ EKeys::Gamepad_LeftThumbstick.GetFName(), TEXT("PAD L3") }, { EKeys::Gamepad_RightThumbstick.GetFName(), TEXT("PAD R3") },
		{ EKeys::Gamepad_DPad_Up.GetFName(), TEXT("PAD UP") }, { EKeys::Gamepad_DPad_Down.GetFName(), TEXT("PAD DOWN") },
		{ EKeys::Gamepad_DPad_Left.GetFName(), TEXT("PAD LEFT") }, { EKeys::Gamepad_DPad_Right.GetFName(), TEXT("PAD RIGHT") },
	};
	if (const TCHAR* const* S = Short.Find(Key.GetFName()))
	{
		return *S;
	}
	// Doom's font: capitals, digits and some punctuation
	FString Label = Key.GetDisplayName(false).ToString().ToUpper();
	return Label.IsEmpty() ? Key.GetFName().ToString().ToUpper() : Label;
}
