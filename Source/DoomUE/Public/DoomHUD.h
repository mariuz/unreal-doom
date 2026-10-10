// Everything 2D, drawn with UCanvas from the WAD's own graphics in Doom's 320x200
// coordinate frame (aspect-corrected, centred): title screen and menu, weapon
// sprites, status bar (STBAR, digits, face, keys, arms, ammo table), messages in
// the STCFN font, palette-style screen tints, automap and intermission.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DoomHUD.generated.h"

class UDoomWorld;
class ADoomPlayer;
struct FDoomPicture;

UCLASS()
class DOOMUE_API ADoomHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	/** Menu entries (shared with the controller). */
	static TArray<FString> MenuEntries(UDoomWorld* W);

private:
	void DrawPatch(const FDoomPicture* Pic, float X, float Y, bool bUseOffsets = true, FLinearColor Tint = FLinearColor::White);
	void DrawPatchByName(const FString& Name, float X, float Y, bool bUseOffsets = true);
	void DrawFullscreenPic(const FString& Name);
	void DrawText(const FString& Text, float X, float Y, float Scale = 1.f, FLinearColor Tint = FLinearColor::White);
	float TextWidth(const FString& Text) const;
	void DrawNumber(int32 Value, float RightX, float Y, const FString& Prefix, int32 MaxDigits);
	void DrawWeapon(ADoomPlayer* P);
	void DrawStatusBar(ADoomPlayer* P);
	void DrawFace(ADoomPlayer* P);
	void DrawTint(ADoomPlayer* P);
	void DrawAutomap(ADoomPlayer* P);
	void DrawMenu();
	void DrawIntermission();
	void Rect(float X, float Y, float W, float H, FLinearColor C);

	UDoomWorld* DW() const;

	// 320x200 -> screen
	float OX = 0, OY = 0, SX = 1, SY = 1;
	int32 FaceTic = 0;
	int32 FaceIndex = 0;
};
