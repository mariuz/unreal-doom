// Drawing in Doom's 320x200 frame (aspect-corrected to 4:3 and centred) on any surface: the HUD's
// canvas or a Slate widget. The WAD's patches and the STCFN font, and the whole menu, so the HUD and
// the CommonUI menu screen (DoomMenuWidgets.h) draw it the same way.
#pragma once

#include "CoreMinimal.h"

class UDoomWorld;
class UTexture2D;
struct FDoomPicture;

class FDoomScreenPainter
{
public:
	virtual ~FDoomScreenPainter() = default;

	/** Fits the 320x200 frame in a surface of this size (letterboxed when narrower than 4:3). */
	void Fit(float Width, float Height);

	UDoomWorld* World = nullptr;
	/** PLAYPAL palette the graphics are drawn through (the screen's pain and pickup flashes), 0: none. */
	int32 ScreenPal = 0;
	// 320x200 -> surface
	float OX = 0, OY = 0, SX = 1, SY = 1;
	float SurfaceWidth = 0, SurfaceHeight = 0;

	void DrawPatch(const FDoomPicture* Pic, float X, float Y, bool bUseOffsets = true, FLinearColor Tint = FLinearColor::White);
	void DrawPatchByName(const FString& Name, float X, float Y, bool bUseOffsets = true);
	/** Black behind a full-screen picture (TITLEPIC, HELP1...). */
	void DrawFullscreenPic(const FString& Name);
	/** The STCFN font: capitals, digits and some punctuation; other characters are 4-pixel spaces. */
	void DrawText(const FString& Text, float X, float Y, float Scale = 1.f, FLinearColor Tint = FLinearColor::White);
	float TextWidth(const FString& Text) const;
	/** A filled rectangle in the 320x200 frame. */
	void Rect(float X, float Y, float W, float H, FLinearColor C);
	/** The whole surface. */
	void FillSurface(FLinearColor C) { Fill(0, 0, SurfaceWidth, SurfaceHeight, C); }
	/** A slider's thermometer, Width notches wide. */
	void DrawThermo(float X, float Y, int32 Width, int32 Value);
	/** The menu (FDoomMenu's current page) over whatever is behind it. Seconds drives the blinking. */
	void DrawMenu(double Seconds);

	/** A menu row's area in the 320x200 frame (with the skull's place on its left), for hit testing
	 *  and focus. Empty for rows that can't be selected (gaps, rows scrolled out of view). */
	static FBox2f MenuRowBox(UDoomWorld& W, int32 Row);

	/** A 320x200 box on the surface. */
	FBox2f ToSurface(const FBox2f& Frame) const
	{
		return FBox2f(FVector2f(OX + Frame.Min.X * SX, OY + Frame.Min.Y * SY), FVector2f(OX + Frame.Max.X * SX, OY + Frame.Max.Y * SY));
	}

protected:
	/** A texture over a rectangle of the surface (surface units). */
	virtual void Picture(UTexture2D* Texture, float X, float Y, float W, float H, FLinearColor Tint) = 0;
	/** A filled rectangle of the surface (surface units). */
	virtual void Fill(float X, float Y, float W, float H, FLinearColor C) = 0;

private:
	UTexture2D* PatchTexture(const FDoomPicture* Pic) const;
};
