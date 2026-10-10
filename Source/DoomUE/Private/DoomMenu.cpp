#include "DoomMenu.h"
#include "DoomWorld.h"
#include "DoomGraphics.h"
#include "DoomPlayer.h"
#include "DoomSettings.h"
#include "DoomUE.h"
#include "Kismet/KismetSystemLibrary.h"

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
		break;
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
	case EMenuPage::Episode:
	case EMenuPage::Skill: return FIntPoint(48, 63);
	case EMenuPage::Options: return FIntPoint(60, 37);
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

void FDoomMenu::Close(UDoomWorld& W)
{
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
		case 1: S->bShowMessages = !S->bShowMessages; break;
		case 2: S->MouseSensitivity = SensitivityFor(FMath::Clamp(R[Item].Value + Dir, 0, 9)); break;
		case 5: W.SetModern(!W.IsModern()); S->bModernRenderer = W.IsModern(); break;
		case 6: S->FieldOfView = FMath::Clamp(S->FieldOfView + Dir * 5.f, 60.f, 130.f); W.ApplyUserSettings(); break;
		case 7:
		{
			const int32 N = FMath::Max(1, W.GetWad()->MapNames().Num());
			WarpIndex = (WarpIndex + Dir + N) % N;
			break;
		}
		case 8: S->bPaletteExact = !S->bPaletteExact; break;
		default: bChanged = false; break;
		}
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
	if (R[Item].Kind == FMenuRow::EKind::Slider || R[Item].Kind == FMenuRow::EKind::Toggle || (Page == EMenuPage::Options && (Item == 5 || Item == 6 || Item == 8)))
	{
		Adjust(W, 1);
		return;
	}
	Activate(W);
}

void FDoomMenu::Activate(UDoomWorld& W)
{
	switch (Page)
	{
	case EMenuPage::Main:
	{
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
		if (Item == 0) Push(W, EMenuPage::EndGame);
		else if (Item == 4) Push(W, EMenuPage::Sound);
		else if (Item == 7 && W.GetWad()->MapNames().IsValidIndex(WarpIndex))
		{
			Stack.Reset();
			W.StartNewGame(W.GetWad()->MapNames()[WarpIndex], W.GetSkill());
		}
		break;
	case EMenuPage::Save:
		if (W.SaveGame(Item + 1))
		{
			Close(W);
		}
		break;
	case EMenuPage::Load:
		Stack.Reset();
		W.LoadGame(Item + 1);
		break;
	default:
		break;
	}
}
