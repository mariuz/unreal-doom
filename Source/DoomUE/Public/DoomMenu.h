// Doom's menu, built from the WAD's own M_* graphics: main menu, episode and skill
// select, options, sound volume, load / save slots, Read This! pages and the
// confirmation prompts. Rows without a graphic in the WAD (or the port's extra
// options) are drawn with the STCFN font. Logic only; ADoomHUD draws it.
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "DoomMapInfo.h"

class UDoomWorld;

enum class EMenuPage : uint8 { Main, Episode, Skill, Nightmare, Options, Sound, Load, Save, ReadThis, Quit, EndGame, Controls, Multiplayer };

/** The Options page's rows, in the order FDoomMenu::Rows builds them. */
namespace EOptionsRow
{
	enum Type : int32 { EndGame, Messages, Sensitivity, SensitivityBar, Sound, Renderer, FieldOfView, WarpTo, Colors, MonsterAI, Controls, Count };
}

/** The Multiplayer page's rows, in the order FDoomMenu::Rows builds them. */
namespace EMultiplayerRow
{
	enum Type : int32 { Rules, Map, Monsters, Fast, Host, Gap, JoinAddress, Connect, Count };
}

struct FMenuRow
{
	enum class EKind : uint8 { Action, Slider, Toggle, Choice, Gap };
	EKind Kind = EKind::Action;
	FString Patch;          // M_* graphic, drawn when the WAD has it
	FString Text;           // fallback, or the whole row for text-only rows
	int32 Value = 0;        // sliders: 0..Max-1; the thermometer sits on the next (Gap) row
	int32 Max = 0;
	FString ValuePatch;     // toggles: M_MSGON / M_MSGOFF
	FString ValueText;      // drawn to the right of Text (the Controls page's keys)
	bool bEnabled = true;
};

struct FMenuTitle
{
	FString Patch;
	int32 X, Y;
};

/** The Multiplayer page's choices: the game to host and the address to join. */
struct FDoomHostSettings
{
	/** 0 co-op, 1 deathmatch, 2 deathmatch 2.0. */
	int32 Rules = 0;
	/** A map from the WAD, or past the last one a random map. */
	int32 MapIndex = 0;
	bool bNoMonsters = false;
	bool bFast = false;
	FString JoinAddress = TEXT("127.0.0.1");
};

class DOOMUE_API FDoomMenu
{
public:
	EMenuPage GetPage() const { return Page; }
	/** The selected row. */
	int32 GetItem() const { return Item; }
	void SetItem(int32 InItem) { Item = InItem; }
	/** The Read This! page shown. */
	int32 GetHelpPage() const { return HelpPage; }
	const FDoomHostSettings& GetHost() const { return Host; }
	void SetHost(const FDoomHostSettings& InHost) { Host = InHost; }
	/** The host page's choices as travel options (DoomMap, DoomRules, NoMonsters, Fast). */
	FString HostOptions(UDoomWorld& W) const;
	void HostGame(UDoomWorld& W);
	void JoinGame(UDoomWorld& W);

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
	/** Top-left of the first row (Doom's menu x / y), rows are RowHeight() pixels apart. */
	FIntPoint Origin() const;
	/** 16, as in Doom; the Controls page packs its long list tighter and scrolls. */
	int32 RowHeight() const { return Page == EMenuPage::Controls ? 10 : 16; }
	int32 VisibleRows() const { return Page == EMenuPage::Controls ? 15 : Page == EMenuPage::Options ? 10 : 100; }
	/** The first row shown (the Controls page scrolls to keep the selection in view). */
	int32 FirstVisibleRow(int32 NumRows) const { return FMath::Clamp(Item - VisibleRows() / 2, 0, FMath::Max(0, NumRows - VisibleRows())); }
	TArray<FDoomEpisode> Episodes(UDoomWorld& W) const;
	TArray<FString> HelpPages(UDoomWorld& W) const;
	/** Text of a confirmation prompt (Nightmare, Quit, End Game), else empty. */
	FString Prompt() const;
	bool IsSaveLoad() const { return Page == EMenuPage::Load || Page == EMenuPage::Save; }

	/** Typing a save name or the join address (Doom's string entry), and the text so far. */
	bool IsEditing() const { return bEditing; }
	const FString& GetEditText() const { return EditText; }
	static constexpr int32 MaxNameLength = 24;
	/** A key while typing: Enter saves, Escape cancels, Backspace deletes, printable keys type. */
	void EditKey(UDoomWorld& W, const FKey& Key);

	/** The Controls page waits for the key to bind to the selected control. */
	bool IsCapturing() const { return bCapturing; }
	/** That key: binds it (Escape cancels). */
	void CaptureKey(UDoomWorld& W, const FKey& Key);

private:
	EMenuPage Page = EMenuPage::Main;
	int32 Item = 0;
	int32 HelpPage = 0;
	int32 WarpIndex = 0;
	FDoomHostSettings Host;
	bool bEditing = false;
	FString EditText;
	bool bCapturing = false;

	void Push(UDoomWorld& W, EMenuPage P);
	void Adjust(UDoomWorld& W, int32 Dir);
	void Activate(UDoomWorld& W);
	void StartGame(UDoomWorld& W, int32 SkillIndex);
	void FixItem(UDoomWorld& W, int32 Dir);

	struct FEntry { EMenuPage Page; int32 Item; };
	TArray<FEntry> Stack;
	int32 Episode = 0;
};
