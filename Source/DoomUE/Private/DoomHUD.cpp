#include "DoomHUD.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomGraphics.h"
#include "DoomWad.h"
#include "DoomMap.h"
#include "DoomUE.h"
#include "DoomMenu.h"
#include "DoomSettings.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"

UDoomWorld* ADoomHUD::DW() const
{
	return GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
}

void ADoomHUD::Rect(float X, float Y, float W, float H, FLinearColor C)
{
	DrawRect(C, OX + X * SX, OY + Y * SY, W * SX, H * SY);
}

void ADoomHUD::DrawPatch(const FDoomPicture* Pic, float X, float Y, bool bUseOffsets, FLinearColor Tint)
{
	if (!Pic || !Pic->Texture)
	{
		return;
	}
	const float PX = X - (bUseOffsets ? Pic->LeftOffset : 0);
	const float PY = Y - (bUseOffsets ? Pic->TopOffset : 0);
	DrawTexture(Pic->Texture, OX + PX * SX, OY + PY * SY, Pic->Width * SX, Pic->Height * SY, 0, 0, 1, 1, Tint, BLEND_Translucent);
}

void ADoomHUD::DrawPatchByName(const FString& Name, float X, float Y, bool bUseOffsets)
{
	if (UDoomWorld* W = DW())
	{
		DrawPatch(W->GetGraphics()->Patch(Name), X, Y, bUseOffsets);
	}
}

void ADoomHUD::DrawFullscreenPic(const FString& Name)
{
	DrawRect(FLinearColor::Black, 0, 0, Canvas->ClipX, Canvas->ClipY);
	DrawPatchByName(Name, 0, 0, false);
}

float ADoomHUD::TextWidth(const FString& Text) const
{
	UDoomWorld* W = DW();
	float X = 0;
	for (TCHAR C : Text.ToUpper())
	{
		const FDoomPicture* P = (C > ' ' && C <= '_') ? W->GetGraphics()->Patch(FString::Printf(TEXT("STCFN%03d"), (int32)C)) : nullptr;
		X += P ? P->Width : 4;
	}
	return X;
}

void ADoomHUD::DrawText(const FString& Text, float X, float Y, float Scale, FLinearColor Tint)
{
	UDoomWorld* W = DW();
	float CX = X;
	for (TCHAR C : Text.ToUpper())
	{
		if (C == '\n')
		{
			CX = X;
			Y += 11 * Scale;
			continue;
		}
		const FDoomPicture* P = (C > ' ' && C <= '_') ? W->GetGraphics()->Patch(FString::Printf(TEXT("STCFN%03d"), (int32)C)) : nullptr;
		if (P && P->Texture)
		{
			DrawTexture(P->Texture, OX + CX * SX, OY + Y * SY, P->Width * SX * Scale, P->Height * SY * Scale, 0, 0, 1, 1, Tint, BLEND_Translucent);
			CX += P->Width * Scale;
		}
		else
		{
			CX += 4 * Scale;
		}
	}
}

void ADoomHUD::DrawNumber(int32 Value, float RightX, float Y, const FString& Prefix, int32 MaxDigits)
{
	UDoomWorld* W = DW();
	const bool bNeg = Value < 0;
	FString Digits = FString::FromInt(FMath::Abs(Value));
	if (Digits.Len() > MaxDigits)
	{
		Digits = Digits.Right(MaxDigits);
	}
	float X = RightX;
	for (int32 i = Digits.Len() - 1; i >= 0; --i)
	{
		const FDoomPicture* P = W->GetGraphics()->Patch(Prefix + Digits[i]);
		if (!P) return;
		X -= P->Width;
		DrawPatch(P, X, Y, false);
	}
	if (bNeg)
	{
		if (const FDoomPicture* M = W->GetGraphics()->Patch(Prefix == TEXT("STTNUM") ? TEXT("STTMINUS") : TEXT("WIMINUS")))
		{
			DrawPatch(M, X - M->Width, Y, false);
		}
	}
}

void ADoomHUD::DrawHUD()
{
	Super::DrawHUD();
	UDoomWorld* W = DW();
	if (!Canvas || !W || !W->GetGraphics())
	{
		return;
	}
	// Doom's 320x200 frame, shown at 4:3 (non-square pixels) and centred
	SY = Canvas->ClipY / 200.f;
	SX = Canvas->ClipY / 240.f;
	OX = (Canvas->ClipX - 320.f * SX) * 0.5f;
	OY = 0;

	switch (W->GetState())
	{
	case EDoomGameState::Title:
		DrawFullscreenPic(TEXT("TITLEPIC"));
		if ((GetWorld()->GetTimeSeconds() * 2.f) - FMath::FloorToFloat(GetWorld()->GetTimeSeconds() * 2.f) < 0.6f)
		{
			const FString Press = TEXT("PRESS ENTER");
			DrawText(Press, 160 - TextWidth(Press) / 2, 186);
		}
		return;
	case EDoomGameState::Menu:
		if (!W->GetMap())
		{
			DrawFullscreenPic(TEXT("TITLEPIC"));
		}
		DrawMenu();
		return;
	case EDoomGameState::Intermission:
		DrawIntermission();
		return;
	case EDoomGameState::Finale:
		DrawFinale();
		return;
	default:
		break;
	}

	ADoomPlayer* P = Cast<ADoomPlayer>(GetOwningPawn());
	if (!P)
	{
		return;
	}
	if (W->bAutomap)
	{
		DrawAutomap(P);
	}
	else
	{
		DrawWeapon(P);
		// crosshair
		const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * (84.f / 200.f) + (Canvas->ClipY * 16.f / 200.f);
		DrawRect(FLinearColor(1.f, 0.85f, 0.2f, 0.7f), CX - 1, Canvas->ClipY * 0.5f - 1, 2, 2);
		(void)CY;
	}
	DrawTint(P);
	DrawStatusBar(P);
	if (W->MessageTics > 0 && GetDefault<UDoomSettings>()->bShowMessages)
	{
		DrawText(W->Message, 2, 2);
	}
}

void ADoomHUD::DrawWeapon(ADoomPlayer* P)
{
	UDoomWorld* W = DW();
	if (P->WeaponSprite.IsEmpty())
	{
		return;
	}
	// light the weapon with the player's sector, like Doom's spritelights for the psprites
	const int32 Sec = W->SectorAt(P->GetActorLocation());
	float Light = Sec >= 0 ? W->GetMap()->Sectors[Sec].Light / 255.f : 1.f;
	Light = FMath::Clamp(Light + P->ExtraLight * 0.12f + 0.1f, 0.1f, 1.f);
	if (P->LightAmp > 0 || P->Invulnerability > 0 || W->IsModern())
	{
		Light = 1.f;
	}
	FLinearColor Tint(Light, Light, Light, 1.f);
	if (P->Invisibility > 4 * 32 || (P->Invisibility & 8))
	{
		Tint = FLinearColor(0.05f, 0.05f, 0.05f, 0.45f);
	}
	const float SXp = 1 + P->BobX;
	const float SYp = P->WeaponY + P->BobY - 16.f;
	const FDoomSpriteFrame F = W->GetGraphics()->SpriteFrame(P->WeaponSprite, P->WeaponFrame, 0);
	if (F.Picture)
	{
		DrawPatch(F.Picture, SXp, SYp, true, Tint);
	}
	if (P->FlashFrame && !P->FlashSprite.IsEmpty())
	{
		const FDoomSpriteFrame FF = W->GetGraphics()->SpriteFrame(P->FlashSprite, P->FlashFrame, 0);
		if (FF.Picture)
		{
			DrawPatch(FF.Picture, SXp, SYp, true, FLinearColor::White);
		}
	}
}

void ADoomHUD::DrawTint(ADoomPlayer* P)
{
	// PLAYPAL's red, gold and green palettes as screen overlays
	if (P->DamageCount > 0)
	{
		const int32 Pal = FMath::Min((P->DamageCount + 7) / 8, 8);
		DrawRect(FLinearColor(1.f, 0.f, 0.f, Pal * 0.09f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
	else if (P->bBerserk && P->Inv.Ready == EDoomWeapon::Fist)
	{
		DrawRect(FLinearColor(1.f, 0.f, 0.f, 0.08f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
	if (P->BonusCount > 0)
	{
		const int32 Pal = FMath::Min((P->BonusCount + 7) / 8, 4);
		DrawRect(FLinearColor(1.f, 0.85f, 0.3f, Pal * 0.05f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
	if (P->RadSuit > 4 * 32 || (P->RadSuit & 8))
	{
		DrawRect(FLinearColor(0.f, 1.f, 0.f, 0.12f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
	if (P->IsDead())
	{
		DrawRect(FLinearColor(0.4f, 0.f, 0.f, 0.25f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
}

void ADoomHUD::DrawFace(ADoomPlayer* P)
{
	UDoomWorld* W = DW();
	FString Face;
	const int32 Bracket = FMath::Clamp((100 - FMath::Max(P->Inv.Health, 0)) * 5 / 101, 0, 4);
	if (P->IsDead())
	{
		Face = TEXT("STFDEAD0");
	}
	else if (P->Invulnerability > 0 || P->bGodMode)
	{
		Face = TEXT("STFGOD0");
	}
	else if (P->FaceEvilGrin > 0)
	{
		Face = FString::Printf(TEXT("STFEVL%d"), Bracket);
	}
	else if (P->DamageCount > 20)
	{
		Face = FString::Printf(TEXT("STFOUCH%d"), Bracket);
	}
	else if (P->DamageCount > 0 && P->FaceDamageDir != 0)
	{
		Face = P->FaceDamageDir > 0 ? FString::Printf(TEXT("STFTR%d0"), Bracket) : FString::Printf(TEXT("STFTL%d0"), Bracket);
	}
	else if (P->AttackDown > 0 && P->Inv.Ready == EDoomWeapon::Chaingun)
	{
		Face = FString::Printf(TEXT("STFKILL%d"), Bracket);
	}
	else
	{
		if (++FaceTic > 40)
		{
			FaceTic = 0;
			FaceIndex = DoomInfo::Random() % 3;
		}
		Face = FString::Printf(TEXT("STFST%d%d"), Bracket, FaceIndex);
	}
	DrawPatchByName(Face, 143, 168, true);
}

void ADoomHUD::DrawStatusBar(ADoomPlayer* P)
{
	UDoomWorld* W = DW();
	// widescreen: fill the sides of the bar
	DrawRect(FLinearColor(0.06f, 0.06f, 0.06f, 1.f), 0, OY + 168 * SY, Canvas->ClipX, 32 * SY);
	DrawPatchByName(TEXT("STBAR"), 0, 168, false);

	const FDoomWeaponDef& WD = DoomInfo::Weapon(P->Inv.Ready);
	if (WD.Ammo != EDoomAmmo::None)
	{
		DrawNumber(P->Inv.Ammo[(int32)WD.Ammo], 44, 171, TEXT("STTNUM"), 3);
	}
	DrawNumber(FMath::Max(P->Inv.Health, 0), 90, 171, TEXT("STTNUM"), 3);
	DrawPatchByName(TEXT("STTPRCNT"), 90, 171, false);
	DrawNumber(P->Inv.Armor, 221, 171, TEXT("STTNUM"), 3);
	DrawPatchByName(TEXT("STTPRCNT"), 221, 171, false);

	// arms
	DrawPatchByName(TEXT("STARMS"), 104, 168, false);
	const bool Owned[6] = {
		P->Inv.Weapons[(int32)EDoomWeapon::Pistol],
		P->Inv.Weapons[(int32)EDoomWeapon::Shotgun] || P->Inv.Weapons[(int32)EDoomWeapon::SuperShotgun],
		P->Inv.Weapons[(int32)EDoomWeapon::Chaingun], P->Inv.Weapons[(int32)EDoomWeapon::Rocket],
		P->Inv.Weapons[(int32)EDoomWeapon::Plasma], P->Inv.Weapons[(int32)EDoomWeapon::BFG] };
	for (int32 i = 0; i < 6; ++i)
	{
		DrawPatchByName(FString::Printf(TEXT("%s%d"), Owned[i] ? TEXT("STYSNUM") : TEXT("STGNUM"), i + 2), 111 + (i % 3) * 12, 172 + (i / 3) * 10, false);
	}

	DrawFace(P);

	// keys: skull keys win over cards in the same slot
	for (int32 k = 0; k < 3; ++k)
	{
		const int32 Patch = P->Inv.Keys[k + 3] ? k + 3 : P->Inv.Keys[k] ? k : -1;
		if (Patch >= 0)
		{
			DrawPatchByName(FString::Printf(TEXT("STKEYS%d"), Patch), 239, 171 + k * 10, false);
		}
	}

	// ammo table
	const EDoomAmmo Order[4] = { EDoomAmmo::Bullets, EDoomAmmo::Shells, EDoomAmmo::Rockets, EDoomAmmo::Cells };
	for (int32 i = 0; i < 4; ++i)
	{
		DrawNumber(P->Inv.Ammo[(int32)Order[i]], 288, 173 + i * 6, TEXT("STYSNUM"), 3);
		DrawNumber(DoomInfo::MaxAmmo(Order[i], P->Inv.bBackpack), 314, 173 + i * 6, TEXT("STYSNUM"), 3);
	}
}

void ADoomHUD::DrawAutomap(ADoomPlayer* P)
{
	UDoomWorld* W = DW();
	FDoomMap* Map = W->GetMap();
	if (!Map)
	{
		return;
	}
	DrawRect(FLinearColor::Black, 0, 0, Canvas->ClipX, Canvas->ClipY * 168.f / 200.f);

	// lines become known when the player is in, or next to, their sectors
	const int32 Here = Map->SectorAt(P->DoomPos());
	if (Here >= 0)
	{
		for (int32 L : Map->Sectors[Here].Lines)
		{
			Map->Lines[L].bSeen = true;
			const int32 O = Map->OtherSector(L, Here);
			float Top, Bottom;
			if (O != INDEX_NONE && Map->LineOpening(L, Top, Bottom))
			{
				for (int32 L2 : Map->Sectors[O].Lines) Map->Lines[L2].bSeen = true;
			}
		}
	}

	const FVector2f Center = P->DoomPos();
	const float Zoom = Canvas->ClipY / 1600.f;
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 84.f / 200.f;
	auto ToScreen = [&](const FVector2f& V) { return FVector2D(CX + (V.X - Center.X) * Zoom, CY - (V.Y - Center.Y) * Zoom); };

	for (const FDoomLine& L : Map->Lines)
	{
		if (L.Flags & DoomLineFlags::DontDraw)
		{
			continue;
		}
		const bool bKnown = L.bSeen || (L.Flags & DoomLineFlags::Mapped);
		if (!bKnown && !W->bFullMap)
		{
			continue;
		}
		FLinearColor C;
		if (L.BackSector == INDEX_NONE || (L.Flags & DoomLineFlags::Secret))
		{
			C = FLinearColor(0.9f, 0.f, 0.f);
		}
		else if (Map->Sectors[L.FrontSector].Floor != Map->Sectors[L.BackSector].Floor)
		{
			C = FLinearColor(0.6f, 0.4f, 0.25f);
		}
		else if (Map->Sectors[L.FrontSector].Ceiling != Map->Sectors[L.BackSector].Ceiling)
		{
			C = FLinearColor(0.95f, 0.9f, 0.3f);
		}
		else if (W->bFullMap)
		{
			C = FLinearColor(0.4f, 0.4f, 0.4f);
		}
		else
		{
			continue;
		}
		if (!bKnown)
		{
			C = FLinearColor(0.45f, 0.45f, 0.45f); // computer map: unseen lines in grey
		}
		const FVector2D A = ToScreen(Map->V(L.V1)), B = ToScreen(Map->V(L.V2));
		DrawLine(A.X, A.Y, B.X, B.Y, C, 1.5f);
	}
	// player arrow
	const float Ang = FMath::DegreesToRadians(P->DoomAngle());
	auto Rot = [&](float X, float Y) { return FVector2f(Center.X + X * FMath::Cos(Ang) - Y * FMath::Sin(Ang), Center.Y + X * FMath::Sin(Ang) + Y * FMath::Cos(Ang)); };
	const FVector2f Arrow[] = { Rot(26, 0), Rot(-20, 0), Rot(26, 0), Rot(10, 10), Rot(26, 0), Rot(10, -10) };
	for (int32 i = 0; i < 6; i += 2)
	{
		const FVector2D A = ToScreen(Arrow[i]), B = ToScreen(Arrow[i + 1]);
		DrawLine(A.X, A.Y, B.X, B.Y, FLinearColor::White, 2.f);
	}
	DrawText(W->LevelTitle(W->GetMapName()), 2, 160);
}

void ADoomHUD::DrawThermo(float X, float Y, int32 Width, int32 Value)
{
	DrawPatchByName(TEXT("M_THERML"), X, Y, true);
	for (int32 i = 0; i < Width; ++i)
	{
		DrawPatchByName(TEXT("M_THERMM"), X + 8 + i * 8, Y, true);
	}
	DrawPatchByName(TEXT("M_THERMR"), X + 8 + Width * 8, Y, true);
	DrawPatchByName(TEXT("M_THERMO"), X + 8 + FMath::Clamp(Value, 0, Width - 1) * 8, Y, true);
}

void ADoomHUD::DrawMenu()
{
	UDoomWorld* W = DW();
	FDoomMenu& M = W->Menu;
	// over the game, or over the title picture on sub-pages (their titles share its logo's spot)
	if (W->GetMap() || M.Page != EMenuPage::Main)
	{
		DrawRect(FLinearColor(0, 0, 0, W->GetMap() ? 0.45f : 0.6f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}

	if (M.Page == EMenuPage::ReadThis)
	{
		const TArray<FString> Pages = M.HelpPages(*W);
		if (Pages.IsValidIndex(M.HelpPage))
		{
			DrawFullscreenPic(Pages[M.HelpPage]);
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
	const FIntPoint O = M.Origin();
	const TArray<FMenuRow> Rows = M.Rows(*W);
	const FLinearColor Dim(0.5f, 0.5f, 0.5f, 1.f);
	for (int32 i = 0; i < Rows.Num(); ++i)
	{
		const FMenuRow& R = Rows[i];
		const float Y = O.Y + i * 16;
		if (M.IsSaveLoad())
		{
			// Doom's slot box: left cap, 24 middles, right cap, the description inside
			DrawPatchByName(TEXT("M_LSLEFT"), O.X - 8, Y + 7, true);
			for (int32 k = 0; k < 24; ++k) DrawPatchByName(TEXT("M_LSCNTR"), O.X + k * 8, Y + 7, true);
			DrawPatchByName(TEXT("M_LSRGHT"), O.X + 24 * 8, Y + 7, true);
			DrawText(R.Text, O.X, Y, 1.f, R.bEnabled || M.Page == EMenuPage::Save ? FLinearColor::White : Dim);
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
		}
		if (R.Kind == FMenuRow::EKind::Toggle)
		{
			if (!R.ValuePatch.IsEmpty() && W->GetGraphics()->Patch(R.ValuePatch))
			{
				DrawPatchByName(R.ValuePatch, O.X + 120, Y, true);
			}
			else
			{
				DrawText(R.Value ? TEXT("ON") : TEXT("OFF"), O.X + 120, Y + 3);
			}
		}
		else if (R.Kind == FMenuRow::EKind::Slider)
		{
			DrawThermo(O.X, Y + 16, R.Max, R.Value);
		}
	}
	// the blinking skull, 32 pixels left of the selected row
	const bool bBlink = (int32)(GetWorld()->GetRealTimeSeconds() * Doom::TicRate / 8) & 1;
	DrawPatchByName(bBlink ? TEXT("M_SKULL2") : TEXT("M_SKULL1"), O.X - 32, O.Y - 5 + M.Item * 16, true);
}

void ADoomHUD::DrawIntermission()
{
	UDoomWorld* W = DW();
	const bool bDoom2 = W->GetWad() && W->GetWad()->IsDoom2();
	if (W->GetGraphics()->Patch(TEXT("INTERPIC")) && (bDoom2 || !W->GetGraphics()->Patch(TEXT("WIMAP0"))))
	{
		DrawFullscreenPic(TEXT("INTERPIC"));
	}
	else
	{
		const FString& M = W->GetMapName();
		DrawFullscreenPic(FString::Printf(TEXT("WIMAP%d"), M.Len() > 1 ? FMath::Clamp(M[1] - '1', 0, 2) : 0));
	}
	const float T = W->IntermissionTic;
	auto Count = [&](int32 Got, int32 Total, float Start)
	{
		const int32 Pct = Total > 0 ? Got * 100 / Total : 100;
		return FMath::Clamp((int32)((T - Start) * 2.f), 0, Pct);
	};
	const FString Title = W->LevelTitle(W->GetMapName());
	DrawText(Title, 160 - TextWidth(Title) / 2, 4, 1.f);
	DrawText(TEXT("FINISHED"), 160 - TextWidth(TEXT("FINISHED")) / 2, 16, 1.f);

	auto Line = [&](const FString& Label, const FString& Patch, int32 Value, float Y, bool bPercent)
	{
		const FDoomPicture* Pic = W->GetGraphics()->Patch(Patch);
		if (Pic) DrawPatch(Pic, 50, Y, false);
		else DrawText(Label, 50, Y, 1.5f);
		DrawNumber(Value, bPercent ? 270 : 286, Y, TEXT("WINUM"), 4);
		if (bPercent) DrawPatchByName(TEXT("WIPCNT"), 270, Y, false);
	};
	Line(TEXT("KILLS"), TEXT("WIOSTK"), Count(W->Kills, W->TotalKills, 10), 50, true);
	if (T > 40) Line(TEXT("ITEMS"), TEXT("WIOSTI"), Count(W->Items, W->TotalItems, 40), 70, true);
	if (T > 70) Line(TEXT("SECRET"), TEXT("WISCRT2"), Count(W->Secrets, W->TotalSecrets, 70), 90, true);
	if (T > 100)
	{
		const int32 Secs = W->GetTic() / Doom::TicRate;
		const FDoomPicture* Pic = W->GetGraphics()->Patch(TEXT("WITIME"));
		if (Pic) DrawPatch(Pic, 16, 168, false);
		else DrawText(TEXT("TIME"), 16, 168, 1.5f);
		DrawText(FString::Printf(TEXT("%d:%02d"), Secs / 60, Secs % 60), 100, 170, 1.5f);
		const int32 Par = DoomInfo::ParTime(W->GetMapName());
		if (Par >= 0)
		{
			DrawText(FString::Printf(TEXT("PAR %d:%02d"), Par / 60, Par % 60), 200, 170, 1.5f);
		}
	}
	if (T > Doom::TicRate * 4 && !W->PendingMap.IsEmpty())
	{
		const FString Next = TEXT("ENTERING ") + W->LevelTitle(W->PendingMap);
		DrawText(Next, 160 - TextWidth(Next) / 2, 130, 1.f);
	}
	if (T > Doom::TicRate)
	{
		DrawText(W->PendingMap.IsEmpty() ? TEXT("EPISODE COMPLETE - PRESS ENTER") : TEXT("PRESS ENTER"), 90, 150, 1.f, FLinearColor(0.8f, 0.8f, 0.8f));
	}
}

void ADoomHUD::DrawFinale()
{
	UDoomWorld* W = DW();
	DrawRect(FLinearColor::Black, 0, 0, Canvas->ClipX, Canvas->ClipY);
	using EStage = UDoomWorld::EFinaleStage;

	if (W->FinaleStage == EStage::Picture)
	{
		DrawPatchByName(W->FinalePicture, 0, 0, false);
		return;
	}
	if (W->FinaleStage == EStage::Bunny)
	{
		// PFUB2 slides left to reveal PFUB1, then THE END is spelled out
		const float Scrolled = FMath::Clamp((W->FinaleTic - 230) / 2.f, 0.f, 320.f);
		const FDoomPicture* P2 = W->GetGraphics()->Patch(TEXT("PFUB2"));
		const FDoomPicture* P1 = W->GetGraphics()->Patch(TEXT("PFUB1"));
		if (P2 && P2->Texture && Scrolled < 320.f)
		{
			const float Width = 320.f - Scrolled;
			DrawTexture(P2->Texture, OX, OY, Width * SX, 200 * SY, Scrolled / P2->Width, 0, Width / P2->Width, 1, FLinearColor::White, BLEND_Opaque);
		}
		if (P1 && P1->Texture && Scrolled > 0.f)
		{
			DrawTexture(P1->Texture, OX + (320.f - Scrolled) * SX, OY, Scrolled * SX, 200 * SY, 0, 0, Scrolled / P1->Width, 1, FLinearColor::White, BLEND_Opaque);
		}
		if (W->FinaleTic >= 1180)
		{
			const int32 Stage = FMath::Min((W->FinaleTic - 1180) / 5, 6);
			DrawPatchByName(FString::Printf(TEXT("END%d"), Stage), (320 - 13 * 8) / 2, (200 - 8 * 8) / 2, true);
		}
		return;
	}
	if (W->FinaleStage == EStage::Cast)
	{
		DrawPatchByName(TEXT("BOSSBACK"), 0, 0, false);
		const UDoomWorld::FCastState& C = W->CastCall;
		const TCHAR Frame = C.Sequence.IsValidIndex(C.Frame) ? C.Sequence[C.Frame] : TEXT('A');
		const FDoomSpriteFrame F = W->GetGraphics()->SpriteFrame(W->CastSprite(), Frame, 1);
		if (F.Picture && F.Picture->Texture)
		{
			const float X = 160.f - (F.bMirror ? F.Picture->Width - F.Picture->LeftOffset : F.Picture->LeftOffset);
			const float Y = 170.f - F.Picture->TopOffset;
			// mirrored frames draw with reversed U
			DrawTexture(F.Picture->Texture, OX + X * SX, OY + Y * SY, F.Picture->Width * SX, F.Picture->Height * SY,
				F.bMirror ? 1.f : 0.f, 0, F.bMirror ? -1.f : 1.f, 1, FLinearColor::White, BLEND_Translucent);
		}
		const FString Name = W->CastName().ToUpper();
		DrawText(Name, 160 - TextWidth(Name) / 2, 180, 1.f);
		return;
	}

	// story text, typed out a character every 3 tics on the tiled backdrop flat
	if (const FDoomPicture* Flat = W->GetGraphics()->Flat(FName(*W->FinaleBackdrop)))
	{
		for (int32 Y = 0; Y < 200; Y += 64)
		{
			for (int32 X = 0; X < 320; X += 64)
			{
				const float H = FMath::Min(64, 200 - Y);
				DrawTexture(Flat->Texture, OX + X * SX, OY + Y * SY, 64 * SX, H * SY, 0, 0, 1, H / 64.f, FLinearColor::White, BLEND_Opaque);
			}
		}
	}
	const int32 Shown = W->FinaleTic / 3;
	TArray<FString> Parts;
	for (const FString& Line : W->FinaleText)
	{
		TArray<FString> Split;
		Line.ParseIntoArray(Split, TEXT("\n"), false);
		Parts.Append(Split);
	}
	// texts longer than Doom's usual 15 lines shrink to stay above the prompt
	const float Scale = FMath::Min(1.f, 172.f / FMath::Max(1, Parts.Num() * 11));
	int32 Count = 0;
	float Y = 6;
	for (const FString& Part : Parts)
	{
		const int32 Visible = FMath::Clamp(Shown - Count, 0, Part.Len());
		DrawText(Part.Left(Visible), 10, Y, Scale);
		Count += Part.Len();
		Y += 11 * Scale;
	}
	if (W->IsFinaleTextComplete())
	{
		DrawText(TEXT("PRESS ENTER"), 160 - TextWidth(TEXT("PRESS ENTER")) / 2, 188, 1.f, FLinearColor(0.8f, 0.8f, 0.8f));
	}
}
