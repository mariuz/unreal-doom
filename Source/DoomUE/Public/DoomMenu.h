// Doom's menu, built from the WAD's own M_* graphics: main menu, episode and skill
// select, options, sound volume, load / save slots, Read This! pages and the
// confirmation prompts. Rows without a graphic in the WAD (or the port's extra
// options) are drawn with the STCFN font. Logic only; ADoomHUD draws it.
#pragma once

#include "CoreMinimal.h"
#include "DoomMapInfo.h"

class UDoomWorld;

enum class EMenuPage : uint8 { Main, Episode, Skill, Nightmare, Options, Sound, Load, Save, ReadThis, Quit, EndGame };

struct FMenuRow
{
	enum class EKind : uint8 { Action, Slider, Toggle, Choice, Gap };
	EKind Kind = EKind::Action;
	FString Patch;          // M_* graphic, drawn when the WAD has it
	FString Text;           // fallback, or the whole row for text-only rows
	int32 Value = 0;        // sliders: 0..Max-1; the thermometer sits on the next (Gap) row
	int32 Max = 0;
	FString ValuePatch;     // toggles: M_MSGON / M_MSGOFF
	bool bEnabled = true;
};

struct FMenuTitle
{
	FString Patch;
	int32 X, Y;
};

class DOOMUE_API FDoomMenu
{
public:
	EMenuPage Page = EMenuPage::Main;
	int32 Item = 0;
	int32 HelpPage = 0;
	int32 WarpIndex = 0;

	/** Opens the menu on a page (the title screen, Esc, F1-F4). */
	void Open(UDoomWorld& W, EMenuPage P);
	void Close(UDoomWorld& W);

	void Up(UDoomWorld& W);
	void Down(UDoomWorld& W);
	void Left(UDoomWorld& W) { Adjust(W, -1); }
	void Right(UDoomWorld& W) { Adjust(W, 1); }
	void Select(UDoomWorld& W);
	void Back(UDoomWorld& W);

	TArray<FMenuRow> Rows(UDoomWorld& W) const;
	TArray<FMenuTitle> Titles() const;
	/** Top-left of the first row (Doom's menu x / y), rows are 16 pixels apart. */
	FIntPoint Origin() const;
	TArray<FDoomEpisode> Episodes(UDoomWorld& W) const;
	TArray<FString> HelpPages(UDoomWorld& W) const;
	/** Text of a confirmation prompt (Nightmare, Quit, End Game), else empty. */
	FString Prompt() const;
	bool IsSaveLoad() const { return Page == EMenuPage::Load || Page == EMenuPage::Save; }

private:
	void Push(UDoomWorld& W, EMenuPage P);
	void Adjust(UDoomWorld& W, int32 Dir);
	void Activate(UDoomWorld& W);
	void StartGame(UDoomWorld& W, int32 SkillIndex);
	void FixItem(UDoomWorld& W, int32 Dir);

	struct FEntry { EMenuPage Page; int32 Item; };
	TArray<FEntry> Stack;
	int32 Episode = 0;
};
