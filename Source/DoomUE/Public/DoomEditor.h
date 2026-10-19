// The map editor, at runtime and in the Unreal editor's Play In Editor: a top-down map view (Slate)
// with UMG panels around it, over the live game.
//
//  * Modes: Things, Sectors, Lines, Vertices (select, drag, edit properties, delete) and Draw (click
//    the corners of a new sector: in the void, inside a sector, or against existing walls, which
//    become doorways).
//  * Undo / Redo, Check Map (the map checker), Save WAD (DoomWadWriter), a new random map (the
//    generator), a new empty room to start from.
//  * Every change rebuilds the BSP (the node builder) and reloads the level from the edited map.
//    The game's clock stays stopped while editing; the 3D view flies a camera (WASD, Q / E, right
//    mouse to look) and picks what's under the cursor.
//  * Play (or closing the editor) carries on playing the edited map from where the camera is.
//
// Open it from the main menu (MAP EDITOR), the console (DoomEdit) or with -mapeditor.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/Button.h"
#include "Components/SpinBox.h"
#include "Components/ComboBoxString.h"
#include "Components/CheckBox.h"
#include "DoomMap.h"
#include "DoomMapEdit.h"
#include "DoomEditor.generated.h"

class UDoomWorld;
class UDoomEditorWidget;
class SDoomMapView;
class UTextBlock;
class UVerticalBox;

UENUM()
enum class EDoomEditMode : uint8 { Things, Sectors, Lines, Vertices, Draw };

/** What a property control edits. */
UENUM()
enum class EDoomEditField : uint8
{
	None,
	SectorFloor, SectorCeiling, SectorLight, SectorSpecial, SectorTag, SectorFloorPic, SectorCeilingPic,
	LineSpecial, LineTag, LineFlag, FrontUpper, FrontMiddle, FrontLower, BackUpper, BackMiddle, BackLower, FrontXOffset, FrontYOffset,
	ThingType, ThingAngle, ThingFlag,
	VertexX, VertexY,
	PlaceType, Grid,
};

UCLASS()
class DOOMUE_API UDoomMapEditor : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	bool IsOpen() const { return bOpen; }
	/** Set by the Unreal editor's Doom > Play in the Map Editor: the next Play opens the editor. */
	static bool bOpenOnNextPlay;
	/** Edits the map being played (a new random map if none is). */
	void Open();
	/** Back to playing the edited map, from the camera's spot (or the start). */
	void Close();
	void Toggle() { IsOpen() ? Close() : Open(); }

	const FDoomMap* GetMap() const { return EditMap.Get(); }

	// --- selection and view -------------------------------------------------------------------
	EDoomEditMode Mode = EDoomEditMode::Sectors;
	int32 Selected = INDEX_NONE;   // thing, sector, line or vertex, by mode
	int32 Hovered = INDEX_NONE;
	void SetMode(EDoomEditMode NewMode);
	/** Selects what the mode picks at P (map units); Radius in map units. */
	void SelectAt(const FVector2f& P, float Radius);
	bool b3D = false;
	FVector2f ViewCenter = FVector2f::ZeroVector;
	float Zoom = 0.5f;            // pixels per map unit
	float Grid = 8.f;
	FVector2f Cursor = FVector2f::ZeroVector; // map units, snapped
	TArray<FVector2f> DrawPoints;
	int32 PlaceType = 3001;       // Things mode: what Add Thing places
	FString Status;

	// --- edits (each one can be undone; the level reloads after it) --------------------------
	bool SetNumber(EDoomEditField Field, float Value);
	bool SetName(EDoomEditField Field, FName Value);
	bool SetFlag(EDoomEditField Field, int32 Bit, bool bOn);
	/** Draw mode: closes the outline in DrawPoints into a new sector. */
	bool FinishDraw();
	bool AddThing(const FVector2f& P, int32 Type, float Angle = 90.f);
	bool DeleteSelected();
	/** Things and vertices: moves the selection by Delta (map units). */
	bool MoveSelected(const FVector2f& Delta);
	bool Undo();
	bool Redo();
	int32 NumUndo() const { return UndoStack.Num(); }
	/** The map checker's errors and warnings, to the status line and the log. */
	int32 CheckMap();
	/** A PWAD of the edited map: Saved/Maps/<name>.wad unless Path says otherwise. */
	bool SaveWad(const FString& Path = FString());
	/** A new map from the generator (a random seed unless Seed is given), or a starter room. */
	void NewRandomMap(uint32 Seed = 0);
	void NewEmptyMap();

	/** The sector new ones copy (the last one selected or drawn). */
	FDoomSector DrawTemplate;
	FName DrawWall;

	// --- 3D view: a flying camera -------------------------------------------------------------
	void FlyTick(float DeltaSeconds, const FVector& Move, const FVector2f& Look);
	/** 3D view: what is under a screen point (viewport pixels) is selected. */
	void PickInView(const FVector2D& ScreenPos);

	UDoomEditorWidget* GetWidget() const { return Widget; }
	/** Selection or map changed: the panels refresh. */
	void Refresh();

private:
	bool bOpen = false;
	TUniquePtr<FDoomMap> EditMap;
	TArray<DoomMapEdit::FSnapshot> UndoStack, RedoStack;
	UPROPERTY()
	TObjectPtr<UDoomEditorWidget> Widget;

	UDoomWorld* DoomWorld() const;
	void Begin();                       // an edit starts: an undo snapshot
	void Apply(bool bGeometry);         // an edit is done: rebuild the BSP (geometry), reload the level
	void LoadFresh(const FString& Name); // the map's data as the WAD (or generator) has it
	void SetPlayerFlying(bool bFly);
	void FitView();
};

/** -editortest: the editor's scripted test (DoomEditorTest.cpp). */
namespace DoomEditorTest
{
	DOOMUE_API void Start(UWorld* World);
}

/** The top-down map view (a Slate leaf widget in a UMG wrapper). */
UCLASS()
class DOOMUE_API UDoomMapView : public UWidget
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<UDoomMapEditor> Editor;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	void FocusView();
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	TSharedPtr<SDoomMapView> View;
};

// Panel controls that report to the editor widget with an id (UMG's events have no parameters
// saying which control fired).
UCLASS()
class DOOMUE_API UDoomEditButton : public UButton
{
	GENERATED_BODY()
public:
	int32 Action = 0;
	TWeakObjectPtr<UDoomEditorWidget> Owner;
	void Bind();
	UFUNCTION()
	void Clicked();
};

UCLASS()
class DOOMUE_API UDoomEditSpin : public USpinBox
{
	GENERATED_BODY()
public:
	EDoomEditField Field = EDoomEditField::None;
	TWeakObjectPtr<UDoomEditorWidget> Owner;
	void Bind();
	UFUNCTION()
	void Committed(float InValue, ETextCommit::Type CommitMethod);
};

UCLASS()
class DOOMUE_API UDoomEditCombo : public UComboBoxString
{
	GENERATED_BODY()
public:
	EDoomEditField Field = EDoomEditField::None;
	TWeakObjectPtr<UDoomEditorWidget> Owner;
	void Bind();
	UFUNCTION()
	void Changed(FString Item, ESelectInfo::Type SelectionType);
};

UCLASS()
class DOOMUE_API UDoomEditCheck : public UCheckBox
{
	GENERATED_BODY()
public:
	EDoomEditField Field = EDoomEditField::None;
	int32 Bit = 0;
	TWeakObjectPtr<UDoomEditorWidget> Owner;
	void Bind();
	UFUNCTION()
	void Changed(bool bIsChecked);
};

/** The panels: a toolbar, the selection's properties, a status line, over the map view. */
UCLASS()
class DOOMUE_API UDoomEditorWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<UDoomMapEditor> Editor;
	void RefreshPanels();
	void OnAction(int32 Action);
	void OnNumber(EDoomEditField Field, float Value);
	void OnName(EDoomEditField Field, const FString& Value);
	void OnFlag(EDoomEditField Field, int32 Bit, bool bOn);
	UDoomMapView* GetMapView() const { return MapView; }

	enum EAction : int32
	{
		ModeThings, ModeSectors, ModeLines, ModeVertices, ModeDraw,
		ActUndo, ActRedo, ActView, ActCheck, ActSave, ActRandom, ActEmpty, ActPlay, ActAddThing, ActDelete,
	};

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void Build();
	UPROPERTY()
	TObjectPtr<UDoomMapView> MapView;
	UPROPERTY()
	TObjectPtr<UVerticalBox> Props;
	UPROPERTY()
	TObjectPtr<UTextBlock> StatusText;
	UPROPERTY()
	TObjectPtr<UTextBlock> TitleText;
	UPROPERTY()
	TArray<TObjectPtr<UDoomEditButton>> ModeButtons;

	UTextBlock* Label(const FString& Text, int32 Size = 10);
	UDoomEditButton* MakeButton(const FString& Text, int32 Action);
	static FButtonStyle ButtonStyle(bool bActive);
	void AddRow(const FString& Name, UWidget* Control);
	void AddNumber(const FString& Name, EDoomEditField Field, float Value, float Min, float Max, bool bInt = true);
	void AddNames(const FString& Name, EDoomEditField Field, FName Value, const TArray<FName>& Options);
	void AddCheck(const FString& Name, EDoomEditField Field, int32 Bit, bool bOn);
};
