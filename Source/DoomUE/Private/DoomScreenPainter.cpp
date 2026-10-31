// FDoomScreenPainter: Doom's 320x200 frame on a canvas or a Slate widget (DoomScreenPainter.h).
#include "DoomScreenPainter.h"
#include "DoomWorld.h"
#include "DoomGraphics.h"
#include "DoomMenu.h"
#include "DoomUE.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "DoomScreenPainter"

void FDoomScreenPainter::Fit(float Width, float Height)
{
	SurfaceWidth = Width;
	SurfaceHeight = Height;
	// Doom's 320x200 frame, shown at 4:3 (non-square pixels) and centred
	SX = FMath::Min(Width / 320.f, Height / 240.f);
	SY = SX * 1.2f;
	OX = (Width - 320.f * SX) * 0.5f;
	OY = (Height - 200.f * SY) * 0.5f;
}

UTexture2D* FDoomScreenPainter::PatchTexture(const FDoomPicture* Pic) const
{
	// Doom shifts the whole screen's palette, the status bar and text included
	if (ScreenPal > 0 && Pic && World)
	{
		if (UTexture2D* Shifted = World->GetGraphics()->ColormappedPicture(*Pic, 0, ScreenPal))
		{
			return Shifted;
		}
	}
	return Pic ? Pic->Texture : nullptr;
}

void FDoomScreenPainter::DrawPatch(const FDoomPicture* Pic, float X, float Y, bool bUseOffsets, FLinearColor Tint)
{
	if (!Pic || !Pic->Texture)
	{
		return;
	}
	const float PX = X - (bUseOffsets ? Pic->LeftOffset : 0);
	const float PY = Y - (bUseOffsets ? Pic->TopOffset : 0);
	Picture(PatchTexture(Pic), OX + PX * SX, OY + PY * SY, Pic->Width * SX, Pic->Height * SY, Tint);
}

void FDoomScreenPainter::DrawPatchByName(const FString& Name, float X, float Y, bool bUseOffsets)
{
	if (World && World->GetGraphics())
	{
		DrawPatch(World->GetGraphics()->Patch(Name), X, Y, bUseOffsets);
	}
}

void FDoomScreenPainter::DrawFullscreenPic(const FString& Name)
{
	FillSurface(FLinearColor::Black);
	DrawPatchByName(Name, 0, 0, false);
}

float FDoomScreenPainter::TextWidth(const FString& Text) const
{
	float X = 0;
	for (TCHAR C : Text.ToUpper())
	{
		const FDoomPicture* P = (C > ' ' && C <= '_') ? World->GetGraphics()->Patch(FString::Printf(TEXT("STCFN%03d"), (int32)C)) : nullptr;
		X += P ? P->Width : 4;
	}
	return X;
}

void FDoomScreenPainter::DrawText(const FString& Text, float X, float Y, float Scale, FLinearColor Tint)
{
	float CX = X;
	for (TCHAR C : Text.ToUpper())
	{
		if (C == '\n')
		{
			CX = X;
			Y += 11 * Scale;
			continue;
		}
		const FDoomPicture* P = (C > ' ' && C <= '_') ? World->GetGraphics()->Patch(FString::Printf(TEXT("STCFN%03d"), (int32)C)) : nullptr;
		if (P && P->Texture)
		{
			Picture(PatchTexture(P), OX + CX * SX, OY + Y * SY, P->Width * SX * Scale, P->Height * SY * Scale, Tint);
			CX += P->Width * Scale;
		}
		else
		{
			CX += 4 * Scale;
		}
	}
}

void FDoomScreenPainter::Rect(float X, float Y, float W, float H, FLinearColor C)
{
	Fill(OX + X * SX, OY + Y * SY, W * SX, H * SY, C);
}

void FDoomScreenPainter::DrawThermo(float X, float Y, int32 Width, int32 Value)
{
	DrawPatchByName(TEXT("M_THERML"), X, Y, true);
	for (int32 i = 0; i < Width; ++i)
	{
		DrawPatchByName(TEXT("M_THERMM"), X + 8 + i * 8, Y, true);
	}
	DrawPatchByName(TEXT("M_THERMR"), X + 8 + Width * 8, Y, true);
	DrawPatchByName(TEXT("M_THERMO"), X + 8 + FMath::Clamp(Value, 0, Width - 1) * 8, Y, true);
}

void FDoomScreenPainter::DrawMenu(double Seconds)
{
	UDoomWorld* W = World;
	FDoomMenu& M = W->Menu;
	// over the game, or over the title picture on sub-pages (their titles share its logo's spot)
	if (W->GetMap() || M.GetPage() != EMenuPage::Main)
	{
		FillSurface(FLinearColor(0, 0, 0, W->GetMap() ? 0.45f : 0.6f));
	}

	if (M.GetPage() == EMenuPage::ReadThis)
	{
		const TArray<FString> Pages = M.HelpPages(*W);
		if (Pages.IsValidIndex(M.GetHelpPage()))
		{
			DrawFullscreenPic(Pages[M.GetHelpPage()]);
		}
		return;
	}
	const FString Prompt = M.Prompt();
	if (!Prompt.IsEmpty())
	{
		TArray<FString> Lines;
		Prompt.ParseIntoArray(Lines, TEXT("\n"), false);
		float Y = 100 - Lines.Num() * 11 / 2.f;
		for (const FString& L : Lines)
		{
			DrawText(L, 160 - TextWidth(L) / 2, Y);
			Y += 11;
		}
		return;
	}

	for (const FMenuTitle& T : M.Titles())
	{
		DrawPatchByName(T.Patch, T.X, T.Y, true);
	}
	if (M.GetPage() == EMenuPage::Controls)
	{
		// no M_ graphic for it: the title in the menu font
		const FString Title = LOCTEXT("Controls", "CONTROLS").ToString();
		DrawText(Title, 160 - TextWidth(Title), 8, 2.f);
	}
	const FIntPoint O = M.Origin();
	const TArray<FMenuRow> Rows = M.Rows(*W);
	const FLinearColor Dim(0.5f, 0.5f, 0.5f, 1.f);
	// long pages (Controls) scroll to keep the selection in view
	const int32 First = M.FirstVisibleRow(Rows.Num());
	const int32 Last = FMath::Min(Rows.Num(), First + M.VisibleRows());
	for (int32 i = First; i < Last; ++i)
	{
		const FMenuRow& R = Rows[i];
		const float Y = O.Y + (i - First) * M.RowHeight();
		if (M.IsSaveLoad())
		{
			// Doom's slot box: left cap, 24 middles, right cap, the description inside
			DrawPatchByName(TEXT("M_LSLEFT"), O.X - 8, Y + 7, true);
			for (int32 k = 0; k < 24; ++k) DrawPatchByName(TEXT("M_LSCNTR"), O.X + k * 8, Y + 7, true);
			DrawPatchByName(TEXT("M_LSRGHT"), O.X + 24 * 8, Y + 7, true);
			if (M.IsEditing() && M.GetPage() == EMenuPage::Save && i == M.GetItem())
			{
				const bool bCursor = (int32)(Seconds * 4.0) & 1;
				DrawText(M.GetEditText() + (bCursor ? TEXT("_") : TEXT("")), O.X, Y);
				continue;
			}
			DrawText(R.Text, O.X, Y, 1.f, R.bEnabled || M.GetPage() == EMenuPage::Save ? FLinearColor::White : Dim);
			continue;
		}
		if (R.Kind == FMenuRow::EKind::Gap)
		{
			continue;
		}
		const FDoomPicture* Pic = R.Patch.IsEmpty() ? nullptr : W->GetGraphics()->Patch(R.Patch);
		const FLinearColor Tint = R.bEnabled ? FLinearColor::White : Dim;
		if (Pic)
		{
			DrawPatch(Pic, O.X, Y, true, Tint);
		}
		else
		{
			DrawText(R.Text, O.X, Y + 3, 1.f, R.bEnabled ? FLinearColor(0.9f, 0.15f, 0.1f) : Dim);
			if (M.IsCapturing() && i == M.GetItem())
			{
				// waiting for the new key
				const bool bOn = (int32)(Seconds * 4.0) & 1;
				DrawText(bOn ? LOCTEXT("PressAKey", "PRESS A KEY (ESC: KEEP)").ToString() : FString(), O.X + 136, Y + 3);
			}
			else if (!R.ValueText.IsEmpty())
			{
				DrawText(R.ValueText, O.X + 136, Y + 3);
			}
		}
		if (R.Kind == FMenuRow::EKind::Toggle)
		{
			if (!R.ValuePatch.IsEmpty() && W->GetGraphics()->Patch(R.ValuePatch))
			{
				DrawPatchByName(R.ValuePatch, O.X + 120, Y, true);
			}
			else
			{
				DrawText((R.Value ? LOCTEXT("On", "ON") : LOCTEXT("Off", "OFF")).ToString(), O.X + 120, Y + 3);
			}
		}
		else if (R.Kind == FMenuRow::EKind::Slider)
		{
			DrawThermo(O.X, Y + 16, R.Max, R.Value);
		}
	}
	// the blinking skull, 32 pixels left of the selected row
	const bool bBlink = (int32)(Seconds * Doom::TicRate / 8) & 1;
	if (M.RowHeight() < 16)
	{
		// tight rows: a small arrow, the skull would cover its neighbours
		DrawText(bBlink ? TEXT(">") : TEXT(""), O.X - 10, O.Y + 3 + (M.GetItem() - First) * M.RowHeight());
	}
	else
	{
		DrawPatchByName(bBlink ? TEXT("M_SKULL2") : TEXT("M_SKULL1"), O.X - 32, O.Y - 5 + (M.GetItem() - First) * M.RowHeight(), true);
	}
}

FBox2f FDoomScreenPainter::MenuRowBox(UDoomWorld& W, int32 Row)
{
	FDoomMenu& M = W.Menu;
	if (M.GetPage() == EMenuPage::ReadThis || !M.Prompt().IsEmpty())
	{
		return FBox2f(ForceInit);
	}
	const TArray<FMenuRow> Rows = M.Rows(W);
	const int32 First = M.FirstVisibleRow(Rows.Num());
	if (!Rows.IsValidIndex(Row) || Row < First || Row >= First + M.VisibleRows() || Rows[Row].Kind == FMenuRow::EKind::Gap)
	{
		return FBox2f(ForceInit);
	}
	const FIntPoint O = M.Origin();
	const float H = M.RowHeight();
	const float Top = O.Y + (Row - First) * H - 3.f;
	// sliders take their thermometer row too; the Controls page's keys reach the right edge
	const float Bottom = Top + (Rows[Row].Kind == FMenuRow::EKind::Slider ? 2.f * H : H);
	const float Left = O.X - (H < 16 ? 12.f : 32.f);
	const float Right = M.GetPage() == EMenuPage::Controls ? 316.f : M.IsSaveLoad() ? O.X + 24 * 8 + 16.f : FMath::Min(320.f, FMath::Max(O.X + 160.f, 352.f - O.X));
	return FBox2f(FVector2f(Left, Top), FVector2f(Right, Bottom));
}

#undef LOCTEXT_NAMESPACE
