#include "DoomMenu.h"
#include "DoomWorld.h"
#include "DoomMapGen.h"
#include "DoomEditor.h"
#include "DoomGraphics.h"
#include "DoomPlayer.h"
#include "DoomSettings.h"
#include "DoomUE.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "DoomPlayerController.h"
#include "DoomInput.h"

namespace
{
	const TCHAR* SkillPatches[5] = { TEXT("M_JKILL"), TEXT("M_ROUGH"), TEXT("M_HURT"), TEXT("M_ULTRA"), TEXT("M_NMARE") };
	const TCHAR* SkillTexts[5] = { TEXT("VERY EASY"), TEXT("EASY"), TEXT("NORMAL"), TEXT("HARD"), TEXT("NIGHTMARE") };
	constexpr int32 NumSlots = 6;

	int32 SensitivityStep(float Sens) { return FMath::Clamp(FMath::RoundToInt(Sens * 4.f) - 1, 0, 9); }
	float SensitivityFor(int32 Step) { return (Step + 1) / 4.f; }
}

// ------------------------------------------------------------------------------------------------
// Pages
// ------------------------------------------------------------------------------------------------

TArray<FDoomEpisode> FDoomMenu::Episodes(UDoomWorld& W) const
{
	TArray<FDoomEpisode> Out;
	const FDoomMapInfoSet& Infos = W.GetMapInfos();
	if (!Infos.bClearEpisodes)
	{
		if (W.GetWad()->IsDoom2())
		{
			Out.Add({ TEXT("MAP01"), FString(), TEXT("NEW GAME") });
		}
		else
		{
			for (int32 E = 1; E <= 9; ++E)
			{
				const FString Map = FString::Printf(TEXT("E%dM1"), E);
				if (W.GetWad()->FindLump(Map) == INDEX_NONE) break;
				Out.Add({ Map, FString::Printf(TEXT("M_EPI%d"), E), FString::Printf(TEXT("EPISODE %d"), E) });
			}
		}
	}
	Out.Append(Infos.Episodes);
	// a map from the generator, on a new seed each time (its exit leads to the next seed)
	const uint32 Seed = (uint32)(FPlatformTime::Cycles64() % 0xFFFFF) + 1;
	Out.Add({ DoomMapGen::NameForSeed(Seed), FString(), TEXT("RANDOM MAP") });
	return Out;
}

TArray<FString> FDoomMenu::HelpPages(UDoomWorld& W) const
{
	TArray<FString> Out;
	for (const TCHAR* P : { TEXT("HELP1"), TEXT("HELP2"), TEXT("HELP"), TEXT("CREDIT") })
	{
		if (W.GetGraphics()->Patch(P)) Out.Add(P);
	}
	return Out;
}

TArray<FMenuRow> FDoomMenu::Rows(UDoomWorld& W) const
{
	using EKind = FMenuRow::EKind;
	TArray<FMenuRow> R;
	const UDoomSettings* S = GetDefault<UDoomSettings>();
	switch (Page)
	{
	case EMenuPage::Main:
		R.Add({ EKind::Action, TEXT("M_NGAME"), TEXT("NEW GAME") });
		R.Add({ EKind::Action, TEXT("M_OPTION"), TEXT("OPTIONS") });
		R.Add({ EKind::Action, TEXT("M_LOADG"), TEXT("LOAD GAME") });
		R.Add({ EKind::Action, TEXT("M_SAVEG"), TEXT("SAVE GAME") });
		R.Last().bEnabled = W.GetMap() != nullptr;
		if (!W.GetWad()->IsDoom2())
		{
			R.Add({ EKind::Action, TEXT("M_RDTHIS"), TEXT("READ THIS!") });
		}
		R.Add({ EKind::Action, TEXT("M_QUITG"), TEXT("QUIT GAME") });
		R.Add({ EKind::Choice, FString(), TEXT("MULTIPLAYER") }); // after Doom's own rows, which keep their places
		R.Add({ EKind::Choice, FString(), TEXT("MAP EDITOR") });
		break;
	case EMenuPage::Multiplayer:
	{
		static const TCHAR* Rules[] = { TEXT("CO-OP"), TEXT("DEATHMATCH"), TEXT("DEATHMATCH 2.0") };
		const TArray<FString>& Maps = W.GetWad()->MapNames();
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("GAME: %s"), Rules[FMath::Clamp(Host.Rules, 0, 2)]) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("MAP: < %s >"), Maps.IsValidIndex(Host.MapIndex) ? *Maps[Host.MapIndex] : TEXT("RANDOM")) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("MONSTERS: %s"), Host.bNoMonsters ? TEXT("NONE") : TEXT("YES")) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("FAST MONSTERS: %s"), Host.bFast ? TEXT("YES") : TEXT("NO")) });
		R.Add({ EKind::Choice, FString(), TEXT("HOST GAME") });
		R.Add({ EKind::Gap });
		FMenuRow Join{ EKind::Choice, FString(), TEXT("JOIN ADDRESS:") };
		Join.ValueText = bEditing ? EditText + TEXT("_") : Host.JoinAddress;
		R.Add(Join);
		R.Add({ EKind::Choice, FString(), TEXT("CONNECT") });
		checkf(R.Num() == EMultiplayerRow::Count, TEXT("the Multiplayer page has %d rows, EMultiplayerRow %d"), R.Num(), (int32)EMultiplayerRow::Count);
		break;
	}
	case EMenuPage::Episode:
		for (const FDoomEpisode& E : Episodes(W))
		{
			R.Add({ EKind::Action, E.Patch, E.Name.ToUpper() });
		}
		break;
	case EMenuPage::Skill:
		for (int32 i = 0; i < 5; ++i)
		{
			R.Add({ EKind::Action, SkillPatches[i], SkillTexts[i] });
		}
		break;
	case EMenuPage::Options:
	{
		R.Add({ EKind::Action, TEXT("M_ENDGAM"), TEXT("END GAME") });
		R.Last().bEnabled = W.GetMap() != nullptr;
		FMenuRow Msg{ EKind::Toggle, TEXT("M_MESSG"), TEXT("MESSAGES:") };
		Msg.Value = S->bShowMessages ? 1 : 0;
		Msg.ValuePatch = S->bShowMessages ? TEXT("M_MSGON") : TEXT("M_MSGOFF");
		R.Add(Msg);
		FMenuRow Sens{ EKind::Slider, TEXT("M_MSENS"), TEXT("MOUSE SENSITIVITY") };
		Sens.Max = 10;
		Sens.Value = SensitivityStep(S->MouseSensitivity);
		R.Add(Sens);
		R.Add({ EKind::Gap });
		R.Add({ EKind::Action, TEXT("M_SVOL"), TEXT("SOUND VOLUME") });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("RENDERER: %s"), W.IsModern() ? TEXT("MODERN") : TEXT("CLASSIC")) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("FIELD OF VIEW: %.0f"), S->FieldOfView) });
		const TArray<FString>& Maps = W.GetWad()->MapNames();
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("WARP TO: < %s >"), Maps.IsValidIndex(WarpIndex) ? *Maps[WarpIndex] : TEXT("-")) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("COLORS: %s"), S->bPaletteExact ? TEXT("EXACT") : TEXT("SMOOTH")) });
		R.Add({ EKind::Choice, FString(), FString::Printf(TEXT("MONSTER AI: %s"), W.IsSmartAI() ? TEXT("SMART") : TEXT("CLASSIC")) });
		R.Add({ EKind::Choice, FString(), TEXT("CONTROLS") });
		checkf(R.Num() == EOptionsRow::Count, TEXT("the Options page has %d rows, EOptionsRow %d"), R.Num(), (int32)EOptionsRow::Count);
		break;
	}
	case EMenuPage::Controls:
	{
		// one row per rebindable control with its keys, then a reset
		const ADoomPlayerController* PC = Cast<ADoomPlayerController>(UGameplayStatics::GetPlayerController(W.GetWorld(), 0));
		if (PC)
		{
			for (const ADoomPlayerController::FBinding& B : PC->GetBindings())
			{
				FMenuRow Row{ EKind::Choice, FString(), B.Label };
				TArray<FString> Keys;
				for (const FKey& K : B.Keys) Keys.Add(DoomInput::KeyLabel(K));
				Row.ValueText = Keys.Num() ? FString::Join(Keys, TEXT(", ")) : TEXT("-");
				R.Add(Row);
			}
		}
		R.Add({ EKind::Choice, FString(), TEXT("RESET TO DEFAULTS") });
		break;
	}
	case EMenuPage::Sound:
	{
		FMenuRow Sfx{ EKind::Slider, TEXT("M_SFXVOL"), TEXT("SFX VOLUME") };
		Sfx.Max = 16;
		Sfx.Value = FMath::RoundToInt(S->SfxVolume * 15.f);
		R.Add(Sfx);
		R.Add({ EKind::Gap });
		FMenuRow Mus{ EKind::Slider, TEXT("M_MUSVOL"), TEXT("MUSIC VOLUME") };
		Mus.Max = 16;
		Mus.Value = FMath::RoundToInt(S->MusicVolume * 15.f);
		R.Add(Mus);
		R.Add({ EKind::Gap });
		break;
	}
	case EMenuPage::Load:
	case EMenuPage::Save:
		for (int32 i = 1; i <= NumSlots; ++i)
		{
			FMenuRow Slot{ EKind::Action, FString(), W.SlotDescription(i) };
			if (Page == EMenuPage::Load) Slot.bEnabled = Slot.Text != TEXT("EMPTY");
			R.Add(Slot);
		}
		break;
	default:
		break;
	}
	return R;
}

TArray<FMenuTitle> FDoomMenu::Titles() const
{
	switch (Page)
	{
	case EMenuPage::Main: return { { TEXT("M_DOOM"), 94, 2 } };
	case EMenuPage::Episode: return { { TEXT("M_EPISOD"), 54, 38 } };
	case EMenuPage::Skill: return { { TEXT("M_NEWG"), 96, 14 }, { TEXT("M_SKILL"), 54, 38 } };
	case EMenuPage::Options: return { { TEXT("M_OPTTTL"), 108, 15 } };
	case EMenuPage::Sound: return { { TEXT("M_SVOL"), 60, 38 } };
	case EMenuPage::Load: return { { TEXT("M_LOADG"), 72, 28 } };
	case EMenuPage::Save: return { { TEXT("M_SAVEG"), 72, 28 } };
	default: return {};
	}
}

FIntPoint FDoomMenu::Origin() const
{
	switch (Page)
	{
	case EMenuPage::Main: return FIntPoint(97, 64);
	case EMenuPage::Multiplayer: return FIntPoint(56, 40);
	case EMenuPage::Episode:
	case EMenuPage::Skill: return FIntPoint(48, 63);
	case EMenuPage::Options: return FIntPoint(60, 37);
	case EMenuPage::Controls: return FIntPoint(40, 28);
	case EMenuPage::Sound: return FIntPoint(80, 64);
	case EMenuPage::Load:
	case EMenuPage::Save: return FIntPoint(80, 54);
	default: return FIntPoint(0, 0);
	}
}

FString FDoomMenu::Prompt() const
{
	switch (Page)
	{
	case EMenuPage::Nightmare: return TEXT("NIGHTMARE: THE MONSTERS ARE\nFASTER AND SHOW NO MERCY.\n\nPLAY IT ANYWAY? (Y/N)");
	case EMenuPage::Quit: return TEXT("LEAVE THE GAME? (Y/N)");
	case EMenuPage::EndGame: return TEXT("END THIS GAME? (Y/N)");
	default: return FString();
	}
}

// ------------------------------------------------------------------------------------------------
// Navigation
// ------------------------------------------------------------------------------------------------

void FDoomMenu::Open(UDoomWorld& W, EMenuPage P)
{
	Stack.Reset();
	W.RefreshSlots();
	W.SetState(EDoomGameState::Menu);
	Page = P;
	Item = P == EMenuPage::Skill ? 2 : 0;
	HelpPage = 0;
	FixItem(W, 1);
	W.PlaySound(TEXT("SWTCHN"), nullptr);
}

void FDoomMenu::EditKey(UDoomWorld& W, const FKey& Key)
{
	// gamepads and mice can finish too: A or a click saves, B or Start cancels
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::LeftMouseButton)
	{
		bEditing = false;
		if (Page == EMenuPage::Multiplayer)
		{
			// the address to join
			if (!EditText.IsEmpty()) Host.JoinAddress = EditText;
			W.PlaySound(TEXT("PISTOL"), nullptr);
			return;
		}
		if (W.SaveGame(Item + 1, EditText))
		{
			W.PlaySound(TEXT("PISTOL"), nullptr);
			Close(W);
		}
		return;
	}
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right || Key == EKeys::Gamepad_Special_Right || Key == EKeys::RightMouseButton)
	{
		bEditing = false;
		W.PlaySound(TEXT("SWTCHX"), nullptr);
		return;
	}
	if (Key == EKeys::BackSpace)
	{
		if (EditText.Len()) EditText.LeftChopInline(1);
		W.PlaySound(TEXT("STNMOV"), nullptr);
		return;
	}
	// what Doom's font can draw: capitals, digits and common punctuation
	static const TMap<FKey, TCHAR> Punctuation = {
		{ EKeys::SpaceBar, ' ' }, { EKeys::Hyphen, '-' }, { EKeys::Period, '.' }, { EKeys::Comma, ',' },
		{ EKeys::Slash, '/' }, { EKeys::Semicolon, ';' }, { EKeys::Apostrophe, '\'' }, { EKeys::Equals, '=' },
		{ EKeys::LeftBracket, '[' }, { EKeys::RightBracket, ']' }, { EKeys::Backslash, '\\' }, { EKeys::Underscore, '_' } };
	TCHAR C = 0;
	const FString KeyName = Key.GetFName().ToString();
	if (KeyName.Len() == 1 && FChar::IsAlpha(KeyName[0]))
	{
		C = FChar::ToUpper(KeyName[0]);
	}
	else if (const TCHAR* P = Punctuation.Find(Key))
	{
		C = *P;
	}
	else
	{
		static const FKey Digits[10] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
		static const FKey Pad[10] = { EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour,
			EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine };
		for (int32 d = 0; d < 10; ++d)
		{
			if (Key == Digits[d] || Key == Pad[d]) C = '0' + d;
		}
	}
	if (C && EditText.Len() < MaxNameLength)
	{
		EditText.AppendChar(C);
		W.PlaySound(TEXT("STNMOV"), nullptr);
	}
}

void FDoomMenu::CaptureKey(UDoomWorld& W, const FKey& Key)
{
	bCapturing = false;
	if (Key == EKeys::Escape)
	{
		W.PlaySound(TEXT("SWTCHX"), nullptr);
		return;
	}
	const TArray<DoomInput::FMappable>& Controls = DoomInput::Mappables();
	if (Controls.IsValidIndex(Item) && Key.IsGamepadKey() != Controls[Item].bGamepad)
	{
		// a gamepad row takes a gamepad button, a keyboard row a key or mouse button: keep waiting
		bCapturing = true;
		W.PlaySound(TEXT("OOF"), nullptr);
		return;
	}
	ADoomPlayerController* PC = Cast<ADoomPlayerController>(UGameplayStatics::GetPlayerController(W.GetWorld(), 0));
	if (PC && Controls.IsValidIndex(Item) && PC->Rebind(Controls[Item].Name, Key))
	{
		W.PlaySound(TEXT("PISTOL"), nullptr);
	}
	else
	{
		W.PlaySound(TEXT("OOF"), nullptr);
	}
}

void FDoomMenu::Close(UDoomWorld& W)
{
	bEditing = false;
	bCapturing = false;
	Stack.Reset();
	W.SetState(W.GetMap() ? EDoomGameState::Playing : EDoomGameState::Title);
}

void FDoomMenu::Push(UDoomWorld& W, EMenuPage P)
{
	Stack.Add({ Page, Item });
	Page = P;
	Item = P == EMenuPage::Skill ? 2 : 0;
	HelpPage = 0;
	FixItem(W, 1);
}

void FDoomMenu::FixItem(UDoomWorld& W, int32 Dir)
{
	// never rest on a thermometer row
	const TArray<FMenuRow> R = Rows(W);
	for (int32 Guard = 0; Guard < R.Num() && R.IsValidIndex(Item) && R[Item].Kind == FMenuRow::EKind::Gap; ++Guard)
	{
		Item = (Item + Dir + R.Num()) % R.Num();
	}
}

void FDoomMenu::Up(UDoomWorld& W)
{
	const int32 N = Rows(W).Num();
	if (N == 0) return;
	Item = (Item + N - 1) % N;
	FixItem(W, -1);
	W.PlaySound(TEXT("PSTOP"), nullptr);
}

void FDoomMenu::Down(UDoomWorld& W)
{
	const int32 N = Rows(W).Num();
	if (N == 0) return;
	Item = (Item + 1) % N;
	FixItem(W, 1);
	W.PlaySound(TEXT("PSTOP"), nullptr);
}

void FDoomMenu::Back(UDoomWorld& W)
{
	W.PlaySound(TEXT("SWTCHX"), nullptr);
	if (Page == EMenuPage::ReadThis && HelpPage > 0)
	{
		--HelpPage;
		return;
	}
	if (Stack.Num() == 0)
	{
		Close(W);
		return;
	}
	const FEntry E = Stack.Pop();
	Page = E.Page;
	Item = E.Item;
}

void FDoomMenu::Adjust(UDoomWorld& W, int32 Dir)
{
	const TArray<FMenuRow> R = Rows(W);
	if (!R.IsValidIndex(Item))
	{
		return;
	}
	UDoomSettings* S = GetMutableDefault<UDoomSettings>();
	bool bChanged = true;
	if (Page == EMenuPage::Options)
	{
		switch (Item)
		{
		case EOptionsRow::Messages: S->bShowMessages = !S->bShowMessages; break;
		case EOptionsRow::Sensitivity: S->MouseSensitivity = SensitivityFor(FMath::Clamp(R[Item].Value + Dir, 0, 9)); break;
		case EOptionsRow::Renderer: W.SetModern(!W.IsModern()); S->bModernRenderer = W.IsModern(); break;
		case EOptionsRow::FieldOfView: S->FieldOfView = FMath::Clamp(S->FieldOfView + Dir * 5.f, 60.f, 130.f); W.ApplyUserSettings(); break;
		case EOptionsRow::WarpTo:
		{
			const int32 N = FMath::Max(1, W.GetWad()->MapNames().Num());
			WarpIndex = (WarpIndex + Dir + N) % N;
			break;
		}
		case EOptionsRow::Colors: S->bPaletteExact = !S->bPaletteExact; break;
		case EOptionsRow::MonsterAI: W.SetSmartAI(!W.IsSmartAI()); S->bSmartMonsters = W.IsSmartAI(); break;
		default: bChanged = false; break;
		}
	}
	else if (Page == EMenuPage::Multiplayer && Item <= EMultiplayerRow::Fast)
	{
		switch (Item)
		{
		case EMultiplayerRow::Rules: Host.Rules = (Host.Rules + Dir + 3) % 3; break;
		case EMultiplayerRow::Map:
		{
			const int32 N = W.GetWad()->MapNames().Num() + 1; // the last choice: a random map
			Host.MapIndex = (Host.MapIndex + Dir + N) % N;
			break;
		}
		case EMultiplayerRow::Monsters: Host.bNoMonsters = !Host.bNoMonsters; break;
		default: Host.bFast = !Host.bFast; break;
		}
		W.PlaySound(TEXT("STNMOV"), nullptr);
		return;
	}
	else if (Page == EMenuPage::Sound && (Item == 0 || Item == 2))
	{
		float& V = Item == 0 ? S->SfxVolume : S->MusicVolume;
		V = FMath::Clamp(FMath::RoundToInt(V * 15.f) + Dir, 0, 15) / 15.f;
		W.ApplyUserSettings();
	}
	else
	{
		bChanged = false;
	}
	if (bChanged)
	{
		S->SaveConfig();
		W.PlaySound(TEXT("STNMOV"), nullptr);
	}
}

void FDoomMenu::StartGame(UDoomWorld& W, int32 SkillIndex)
{
	const TArray<FDoomEpisode> Eps = Episodes(W);
	const FString Map = Eps.IsValidIndex(Episode) ? Eps[Episode].Map : (W.GetWad()->MapNames().Num() ? W.GetWad()->MapNames()[0] : FString());
	Stack.Reset();
	if (!Map.IsEmpty())
	{
		W.StartNewGame(Map, SkillIndex + 1);
	}
}

void FDoomMenu::Select(UDoomWorld& W)
{
	if (Page == EMenuPage::ReadThis)
	{
		W.PlaySound(TEXT("PISTOL"), nullptr);
		if (++HelpPage >= HelpPages(W).Num())
		{
			HelpPage = 0;
			Back(W);
		}
		return;
	}
	if (!Prompt().IsEmpty())
	{
		// confirmations: Enter / Y
		W.PlaySound(TEXT("PISTOL"), nullptr);
		if (Page == EMenuPage::Nightmare)
		{
			StartGame(W, 4);
		}
		else if (Page == EMenuPage::Quit)
		{
			UKismetSystemLibrary::QuitGame(W.GetWorld(), nullptr, EQuitPreference::Quit, false);
		}
		else if (Page == EMenuPage::EndGame)
		{
			Stack.Reset();
			W.UnloadMap();
			W.SetState(EDoomGameState::Title);
			W.PlayTitleMusic();
		}
		return;
	}
	const TArray<FMenuRow> R = Rows(W);
	if (!R.IsValidIndex(Item) || !R[Item].bEnabled)
	{
		W.PlaySound(TEXT("OOF"), nullptr);
		return;
	}
	W.PlaySound(TEXT("PISTOL"), nullptr);
	if (R[Item].Kind == FMenuRow::EKind::Slider || R[Item].Kind == FMenuRow::EKind::Toggle || (Page == EMenuPage::Options && (Item == EOptionsRow::Renderer || Item == EOptionsRow::FieldOfView || Item == EOptionsRow::Colors || Item == EOptionsRow::MonsterAI))
		|| (Page == EMenuPage::Multiplayer && Item <= EMultiplayerRow::Fast))
	{
		Adjust(W, 1);
		return;
	}
	Activate(W);
}

FString FDoomMenu::HostOptions(UDoomWorld& W) const
{
	const TArray<FString>& Maps = W.GetWad()->MapNames();
	const FString Map = Maps.IsValidIndex(Host.MapIndex) ? Maps[Host.MapIndex] : DoomMapGen::NameForSeed((uint32)FMath::RandRange(1, 0xFFFFF));
	return FString::Printf(TEXT("listen?DoomMap=%s?DoomRules=%d%s%s"), *Map, Host.Rules, Host.bNoMonsters ? TEXT("?NoMonsters") : TEXT(""), Host.bFast ? TEXT("?Fast") : TEXT(""));
}

void FDoomMenu::HostGame(UDoomWorld& W)
{
	// a listen server on Doom's one Unreal level; the game mode reads the options (InitGame)
	const FString Options = HostOptions(W);
	UE_LOG(LogDoomNet, Log, TEXT("Hosting: %s"), *Options);
	Stack.Reset();
	Close(W);
	UGameplayStatics::OpenLevel(W.GetWorld(), FName(TEXT("/Engine/Maps/Entry")), true, Options);
}

void FDoomMenu::JoinGame(UDoomWorld& W)
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(W.GetWorld(), 0);
	if (!PC || Host.JoinAddress.IsEmpty())
	{
		W.PlaySound(TEXT("OOF"), nullptr);
		return;
	}
	UE_LOG(LogDoomNet, Log, TEXT("Joining %s"), *Host.JoinAddress);
	Stack.Reset();
	Close(W);
	PC->ClientTravel(Host.JoinAddress, TRAVEL_Absolute);
}

void FDoomMenu::Activate(UDoomWorld& W)
{
	switch (Page)
	{
	case EMenuPage::Multiplayer:
		if (Item == EMultiplayerRow::Host) HostGame(W);
		else if (Item == EMultiplayerRow::JoinAddress)
		{
			EditText = Host.JoinAddress;
			bEditing = true; // typed like a save name; Enter keeps it
		}
		else if (Item == EMultiplayerRow::Connect) JoinGame(W);
		return;
	case EMenuPage::Controls:
		if (Item >= DoomInput::Mappables().Num())
		{
			if (ADoomPlayerController* PC = Cast<ADoomPlayerController>(UGameplayStatics::GetPlayerController(W.GetWorld(), 0)))
			{
				PC->ResetBindings();
			}
		}
		else
		{
			bCapturing = true; // the next key binds (ADoomPlayerController::InputKey -> CaptureKey)
		}
		return;
	case EMenuPage::Main:
	{
		if (Item == Rows(W).Num() - 2)
		{
			Push(W, EMenuPage::Multiplayer);
			return;
		}
		if (Item == Rows(W).Num() - 1)
		{
			// MAP EDITOR: the map being played, or a new random one
			Stack.Reset();
			Close(W);
			if (UDoomMapEditor* E = W.GetWorld()->GetSubsystem<UDoomMapEditor>()) E->Open();
			return;
		}
		const bool bDoom2 = W.GetWad()->IsDoom2();
		// rows: new game, options, load, save, [read this], quit
		int32 Action = Item;
		if (bDoom2 && Action >= 4) ++Action;
		switch (Action)
		{
		case 0:
			Episode = 0;
			Push(W, Episodes(W).Num() > 1 ? EMenuPage::Episode : EMenuPage::Skill);
			break;
		case 1: Push(W, EMenuPage::Options); break;
		case 2: Push(W, EMenuPage::Load); break;
		case 3: Push(W, EMenuPage::Save); break;
		case 4: Push(W, EMenuPage::ReadThis); break;
		default: Push(W, EMenuPage::Quit); break;
		}
		break;
	}
	case EMenuPage::Episode:
		Episode = Item;
		Push(W, EMenuPage::Skill);
		break;
	case EMenuPage::Skill:
		if (Item == 4) Push(W, EMenuPage::Nightmare);
		else StartGame(W, Item);
		break;
	case EMenuPage::Options:
		if (Item == EOptionsRow::EndGame) Push(W, EMenuPage::EndGame);
		else if (Item == EOptionsRow::Sound) Push(W, EMenuPage::Sound);
		else if (Item == EOptionsRow::Controls) Push(W, EMenuPage::Controls);
		else if (Item == EOptionsRow::WarpTo && W.GetWad()->MapNames().IsValidIndex(WarpIndex))
		{
			Stack.Reset();
			W.StartNewGame(W.GetWad()->MapNames()[WarpIndex], W.GetSkill());
		}
		break;
	case EMenuPage::Save:
	{
		const ADoomPlayer* P = W.GetPlayer();
		if (!W.GetMap() || !P || P->IsDead())
		{
			W.ShowMessage(TEXT("You can't save now."));
			break;
		}
		// name the save like Doom: the slot's old name (if any) with a cursor
		const FString Old = W.SlotDescription(Item + 1);
		EditText = Old == TEXT("EMPTY") || Old.StartsWith(TEXT("(")) ? FString() : Old.Left(MaxNameLength);
		bEditing = true;
		break;
	}
	case EMenuPage::Load:
		Stack.Reset();
		W.LoadGame(Item + 1);
		break;
	default:
		break;
	}
}
