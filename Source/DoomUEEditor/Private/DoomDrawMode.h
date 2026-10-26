// Doom > Draw Sectors: an editor mode for drawing new sectors in the level viewport. Each click adds
// a corner where the click ray meets the floor (snapped to an existing corner nearby, else to the
// level editor's grid); clicking the first corner again, a double-click or Enter closes the sector,
// which becomes an ADoomSectorDraw in the level (one undo step). Backspace removes the last corner;
// Esc drops the corners, or leaves the mode when there are none.
#pragma once

#include "CoreMinimal.h"
#include "Tools/LegacyEdModeWidgetHelpers.h"
#include "DoomDrawMode.generated.h"

class ADoomMapActor;
class ADoomSectorDraw;

UCLASS(Transient)
class UDoomDrawMode : public UBaseLegacyWidgetEdMode
{
	GENERATED_BODY()

public:
	static const FEditorModeID Id;

	UDoomDrawMode();

	/** Starts the mode in the level editor (the Doom menu, the map actor's Draw Sectors button). */
	static void Activate();
	/** The active mode, if it is. */
	static UDoomDrawMode* Get();

	// UEdMode
	virtual void Enter() override;
	virtual void Exit() override;
	virtual bool UsesToolkits() const override { return false; }
	virtual bool UsesTransformWidget() const override { return false; }
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override { return false; }
	virtual bool ShouldDrawWidget() const override { return false; }

	// viewport input
	virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click) override;
	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event) override;
	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y) override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;

	/** Adds the corner a ray from the viewport points at; false if it hits nothing to draw on. When
	 *  it lands on the first corner of three or more, the sector closes (Finish). */
	bool AddCornerFromRay(const FVector& Origin, const FVector& Direction);
	/** Adds a corner at a point in the world (snapped); closes the sector on the first corner. */
	void AddCorner(const FVector& WorldPoint);
	/** Closes the sector: a new ADoomSectorDraw, or null (the status line says why). */
	ADoomSectorDraw* Finish();
	void RemoveLastCorner();
	void Clear();

	int32 NumCorners() const { return Corners.Num(); }
	const FText& GetStatus() const { return Status; }

private:
	ADoomMapActor* MapActor() const;
	/** Where a ray meets the level: the geometry it hits, else the height of the last corner (or 0). */
	bool RayToWorld(const FVector& Origin, const FVector& Direction, FVector& Out) const;
	/** Doom 2D: an existing corner within a few units, else the grid. */
	FVector2f Snap(const FVector2f& P) const;
	/** A Doom 2D point at the floor under it, in the world (for drawing). */
	FVector AtFloor(const FVector2f& P) const;

	TArray<FVector2f> Corners;
	TOptional<FVector2f> Hover;
	FText Status;
};
