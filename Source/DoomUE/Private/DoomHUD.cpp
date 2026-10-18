#include "DoomHUD.h"
#include "DoomPlayerState.h"
#include "DoomGameState.h"
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
	DrawTexture(PatchTexture(Pic), OX + PX * SX, OY + PY * SY, Pic->Width * SX, Pic->Height * SY, 0, 0, 1, 1, Tint, BLEND_Translucent);
}

UTexture2D* ADoomHUD::PatchTexture(const FDoomPicture* Pic)
{
	// Doom shifts the whole screen's palette, the status bar and text included
	if (ScreenPal > 0 && Pic)
	{
		if (UTexture2D* Shifted = DW()->GetGraphics()->ColormappedPicture(*Pic, 0, ScreenPal))
		{
			return Shifted;
		}
	}
	return Pic ? Pic->Texture : nullptr;
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
			DrawTexture(PatchTexture(P), OX + CX * SX, OY + Y * SY, P->Width * SX * Scale, P->Height * SY * Scale, 0, 0, 1, 1, Tint, BLEND_Translucent);
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
	ScreenPal = (W->GetState() == EDoomGameState::Playing && W->IsPaletteExact()) ? W->ScreenPalette() : 0;
	// Doom's 320x200 frame, shown at 4:3 (non-square pixels) and centred
	// (fit inside the window: letterboxed on screens narrower than 4:3)
	SX = FMath::Min(Canvas->ClipX / 320.f, Canvas->ClipY / 240.f);
	SY = SX * 1.2f;
	OX = (Canvas->ClipX - 320.f * SX) * 0.5f;
	OY = (Canvas->ClipY - 200.f * SY) * 0.5f;

	if (W->IsLoadingMap())
	{
		DrawLoading();
		return;
	}
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
	OY = Canvas->ClipY - 200.f * SY; // in play the status bar sits at the bottom, whatever the shape
	MarkSeenLines(P);
	if (Automap.Map != W->GetMapName())
	{
		Automap = FAutomapView(); // a new map: following, default zoom, no marks
		Automap.Map = W->GetMapName();
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
	const bool bExact = !W->IsModern() && GetDefault<UDoomSettings>()->bPaletteExact && Tint.A >= 1.f;
	if (F.Picture && bExact)
	{
		// Doom lights the weapon with spritelights[MAXLIGHTSCALE-1] of the player's sector
		int32 Row;
		if (P->Invulnerability > 4 * 32 || (P->Invulnerability & 8)) Row = 32;
		else if (P->LightAmp > 4 * 32 || (P->LightAmp & 8)) Row = 1;
		else
		{
			const int32 SecLight = Sec >= 0 ? W->GetMap()->Sectors[Sec].Light : 255;
			const int32 LightNum = FMath::Clamp(SecLight / 16 + P->ExtraLight, 0, 15);
			Row = FMath::Clamp((15 - LightNum) * 4 - 23, 0, 31);
		}
		if (UTexture2D* Lit = W->GetGraphics()->ColormappedPicture(*F.Picture, Row, ScreenPal))
		{
			DrawTexture(Lit, OX + (SXp - F.Picture->LeftOffset) * SX, OY + (SYp - F.Picture->TopOffset) * SY,
				F.Picture->Width * SX, F.Picture->Height * SY, 0, 0, 1, 1, FLinearColor::White, BLEND_Translucent);
		}
	}
	else if (F.Picture)
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
	if (DW()->IsPaletteExact())
	{
		return; // the real PLAYPAL palettes are applied to the world and the 2D graphics instead
	}
	// PLAYPAL's red, gold and green palettes as screen overlays
	if (P->DamageCount > 0)
	{
		const int32 Pal = FMath::Min((P->DamageCount + 7) / 8, 8);
		DrawRect(FLinearColor(1.f, 0.f, 0.f, Pal * 0.09f), 0, 0, Canvas->ClipX, Canvas->ClipY);
	}
	else if (P->bBerserk && P->Inv().Ready == EDoomWeapon::Fist)
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
	const int32 Bracket = FMath::Clamp((100 - FMath::Max(P->Inv().Health, 0)) * 5 / 101, 0, 4);
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
	else if (P->DamageCount > 0)
	{
		Face = FString::Printf(TEXT("STFKILL%d"), Bracket); // hit head-on, or hurt by the floor
	}
	else if (P->AttackDown > 0 && P->Inv().Ready == EDoomWeapon::Chaingun)
	{
		Face = FString::Printf(TEXT("STFKILL%d"), Bracket);
	}
	else
	{
		// looks around every half second of game time (ST_STRAIGHTFACECOUNT), not every N frames, and
		// with its own random numbers: the HUD must not change the game's random sequence. Derived from
		// the tic (like vanilla's separate M_Random it repeats run to run, so screenshots stay exact)
		if (FMath::Abs(W->GetTic() - FaceTic) >= Doom::TicRate / 2)
		{
			FaceTic = W->GetTic();
			FaceIndex = FRandomStream((int32)(FaceTic * 2654435761u)).RandRange(0, 2);
		}
		Face = FString::Printf(TEXT("STFST%d%d"), Bracket, FaceIndex);
	}
	DrawPatchByName(Face, 143, 168, true);
}

void ADoomHUD::DrawNetgameStats(const TArray<ADoomPlayerState*>& Players, bool bDeathmatch, float T)
{
	// WI_drawNetgameStats / WI_drawDeathmatchStats: one row per player, coloured by player number
	UDoomWorld* W = DW();
	const FDoomLevelFlow& F = W->Flow();
	const APlayerController* PC = GetOwningPlayerController();
	const float Top = 44;
	auto Pct = [](int32 Got, int32 Total) { return Total > 0 ? Got * 100 / Total : 100; };
	auto Header = [&](const TCHAR* Label, const TCHAR* Patch, float X)
	{
		if (const FDoomPicture* Pic = W->GetGraphics()->Patch(Patch)) DrawPatch(Pic, X - Pic->Width, Top - 12, false);
		else DrawText(Label, X - TextWidth(Label), Top - 10, 1.f);
	};
	if (bDeathmatch)
	{
		Header(TEXT("FRAGS"), TEXT("WIFRGS"), 230);
	}
	else
	{
		Header(TEXT("KILLS"), TEXT("WIOSTK"), 150);
		Header(TEXT("ITEMS"), TEXT("WIOSTI"), 210);
		Header(TEXT("SCRT"), TEXT("WIOSTS"), 260);
		Header(TEXT("FRAGS"), TEXT("WIFRGS"), 310);
	}
	for (int32 i = 0; i < Players.Num(); ++i)
	{
		const ADoomPlayerState* PS = Players[i];
		const float Y = Top + 8 + i * 22;
		// the player's colour square (STPB0..3) and a marker on your own row
		DrawPatchByName(FString::Printf(TEXT("STPB%d"), FMath::Clamp(PS->PlayerNumber, 0, 3)), 24, Y - 2, false);
		if (PC && PC->PlayerState == PS) DrawPatchByName(TEXT("STFST01"), 24, Y - 2, false);
		DrawText(FString::Printf(TEXT("PLAYER %d"), PS->PlayerNumber + 1), 54, Y + 2, 1.f);
		if (T < 10)
		{
			continue;
		}
		if (bDeathmatch)
		{
			DrawNumber(PS->Frags, 230, Y, TEXT("WINUM"), 3);
		}
		else
		{
			DrawNumber(Pct(PS->LevelKills, F.TotalKills), 150, Y, TEXT("WINUM"), 3);
			DrawPatchByName(TEXT("WIPCNT"), 150, Y, false);
			DrawNumber(Pct(PS->LevelItems, F.TotalItems), 210, Y, TEXT("WINUM"), 3);
			DrawPatchByName(TEXT("WIPCNT"), 210, Y, false);
			DrawNumber(Pct(PS->LevelSecrets, F.TotalSecrets), 260, Y, TEXT("WINUM"), 3);
			DrawPatchByName(TEXT("WIPCNT"), 260, Y, false);
			DrawNumber(PS->Frags, 310, Y, TEXT("WINUM"), 3);
		}
	}
}

void ADoomHUD::DrawStatusBar(ADoomPlayer* P)
{
	UDoomWorld* W = DW();
	// widescreen: fill the sides of the bar
	DrawRect(FLinearColor(0.06f, 0.06f, 0.06f, 1.f), 0, OY + 168 * SY, Canvas->ClipX, 32 * SY);
	DrawPatchByName(TEXT("STBAR"), 0, 168, false);

	const FDoomWeaponDef& WD = DoomInfo::Weapon(P->Inv().Ready);
	if (WD.Ammo != EDoomAmmo::None)
	{
		DrawNumber(P->Inv().Ammo[(int32)WD.Ammo], 44, 171, TEXT("STTNUM"), 3);
	}
	DrawNumber(FMath::Max(P->Inv().Health, 0), 90, 171, TEXT("STTNUM"), 3);
	DrawPatchByName(TEXT("STTPRCNT"), 90, 171, false);
	DrawNumber(P->Inv().Armor, 221, 171, TEXT("STTNUM"), 3);
	DrawPatchByName(TEXT("STTPRCNT"), 221, 171, false);

	const ADoomPlayerState* PS = P->GetDoomPlayerState();
	if (W->IsDeathmatch() && PS)
	{
		// deathmatch: the bar's FRAGS panel instead of the arms
		DrawNumber(PS->Frags, 138, 171, TEXT("STTNUM"), 2);
	}
	else
	{
	// arms
	DrawPatchByName(TEXT("STARMS"), 104, 168, false);
	const bool Owned[6] = {
		P->Inv().Weapons[(int32)EDoomWeapon::Pistol],
		P->Inv().Weapons[(int32)EDoomWeapon::Shotgun] || P->Inv().Weapons[(int32)EDoomWeapon::SuperShotgun],
		P->Inv().Weapons[(int32)EDoomWeapon::Chaingun], P->Inv().Weapons[(int32)EDoomWeapon::Rocket],
		P->Inv().Weapons[(int32)EDoomWeapon::Plasma], P->Inv().Weapons[(int32)EDoomWeapon::BFG] };
	for (int32 i = 0; i < 6; ++i)
	{
		DrawPatchByName(FString::Printf(TEXT("%s%d"), Owned[i] ? TEXT("STYSNUM") : TEXT("STGNUM"), i + 2), 111 + (i % 3) * 12, 172 + (i / 3) * 10, false);
	}
	}

	// network games: the face sits on the player's colour (STFB0..3)
	if (W->IsNetGame() && PS)
	{
		DrawPatchByName(FString::Printf(TEXT("STFB%d"), FMath::Clamp(PS->PlayerNumber, 0, 3)), 143, 169, false);
	}
	DrawFace(P);

	// keys: skull keys win over cards in the same slot
	for (int32 k = 0; k < 3; ++k)
	{
		const int32 Patch = P->Inv().Keys[k + 3] ? k + 3 : P->Inv().Keys[k] ? k : -1;
		if (Patch >= 0)
		{
			DrawPatchByName(FString::Printf(TEXT("STKEYS%d"), Patch), 239, 171 + k * 10, false);
		}
	}

	// ammo table
	const EDoomAmmo Order[4] = { EDoomAmmo::Bullets, EDoomAmmo::Shells, EDoomAmmo::Rockets, EDoomAmmo::Cells };
	for (int32 i = 0; i < 4; ++i)
	{
		DrawNumber(P->Inv().Ammo[(int32)Order[i]], 288, 173 + i * 6, TEXT("STYSNUM"), 3);
		DrawNumber(DoomInfo::MaxAmmo(Order[i], P->Inv().bBackpack), 314, 173 + i * 6, TEXT("STYSNUM"), 3);
	}
}

void ADoomHUD::MarkSeenLines(ADoomPlayer* P)
{
	// vanilla marks every wall the renderer draws (ML_MAPPED); here: lines in the sectors reachable
	// through open lines within 2048 units, with a clear 2D line of sight to their middle
	FDoomMap* Map = DW()->GetMap();
	const double Now = GetWorld()->GetRealTimeSeconds();
	if (!Map || Now < NextSeenScan)
	{
		return;
	}
	NextSeenScan = Now + 0.15;
	const FVector2f Eye = P->DoomPos();
	const int32 Here = Map->SectorAt(Eye);
	if (Here < 0)
	{
		return;
	}
	TArray<int32> Stack = { Here };
	TSet<int32> Visited = { Here };
	TArray<FLineHit> Hits;
	while (Stack.Num())
	{
		const int32 S = Stack.Pop(EAllowShrinking::No);
		for (int32 Li : Map->Sectors[S].Lines)
		{
			FDoomLine& L = Map->Lines[Li];
			const FVector2f Mid = (Map->V(L.V1) + Map->V(L.V2)) * 0.5f;
			if (FVector2f::Distance(Mid, Eye) > 2048.f)
			{
				continue;
			}
			// blocked by a solid line (one-sided, or a closed opening) on the way?
			Hits.Reset();
			Map->TraceLines(Eye, Mid, Hits);
			bool bVisible = true;
			for (const FLineHit& H : Hits)
			{
				float Top, Bottom;
				if (H.Line != Li && H.Frac < 0.999f && !Map->LineOpening(H.Line, Top, Bottom))
				{
					bVisible = false;
					break;
				}
			}
			if (!bVisible)
			{
				continue;
			}
			L.bSeen = true;
			const int32 O = Map->OtherSector(Li, S);
			float Top, Bottom;
			if (O != INDEX_NONE && !Visited.Contains(O) && Map->LineOpening(Li, Top, Bottom))
			{
				Visited.Add(O);
				Stack.Add(O);
			}
		}
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
	const FVector2f Center = Automap.bFollow ? P->DoomPos() : Automap.Center;
	const float Zoom = Canvas->ClipY / 1600.f * Automap.Scale;
	// Doom draws the map in its 320x200 frame too, so it is stretched 1.2 times vertically
	const float ZoomY = Zoom * 1.2f;
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 84.f / 200.f;
	auto ToScreen = [&](const FVector2f& V) { return FVector2D(CX + (V.X - Center.X) * Zoom, CY - (V.Y - Center.Y) * ZoomY); };
	Automap.PixelsPerUnit = Zoom;
	Automap.ViewSize = FVector2f(Canvas->ClipX / Zoom, Canvas->ClipY * 168.f / 200.f / ZoomY);

	if (Automap.bGrid)
	{
		// the blockmap's 128-unit grid, from the map's corner
		const float Step = 128.f;
		const FVector2f Lo = Center - Automap.ViewSize * 0.5f, Hi = Center + Automap.ViewSize * 0.5f;
		const FLinearColor Grid(0.05f, 0.05f, 0.05f); // GRIDCOLORS: a dark grey (linear colour)
		for (float X = Map->Bounds.Min.X + FMath::FloorToFloat((Lo.X - Map->Bounds.Min.X) / Step) * Step; X <= Hi.X; X += Step)
		{
			const FVector2D A = ToScreen(FVector2f(X, Lo.Y)), B = ToScreen(FVector2f(X, Hi.Y));
			DrawLine(A.X, A.Y, B.X, B.Y, Grid, 1.f);
		}
		for (float Y = Map->Bounds.Min.Y + FMath::FloorToFloat((Lo.Y - Map->Bounds.Min.Y) / Step) * Step; Y <= Hi.Y; Y += Step)
		{
			const FVector2D A = ToScreen(FVector2f(Lo.X, Y)), B = ToScreen(FVector2f(Hi.X, Y));
			DrawLine(A.X, A.Y, B.X, B.Y, Grid, 1.f);
		}
	}

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
		if (!bKnown)
		{
			C = FLinearColor(0.45f, 0.45f, 0.45f); // computer map: unseen lines in grey
		}
		else if (L.BackSector == INDEX_NONE || (L.Flags & DoomLineFlags::Secret))
		{
			C = FLinearColor(0.9f, 0.f, 0.f);
		}
		else if (L.Special == 39)
		{
			C = FLinearColor(0.95f, 0.45f, 0.45f); // teleporters: a lighter red
		}
		else if (Map->Sectors[L.FrontSector].Floor != Map->Sectors[L.BackSector].Floor)
		{
			C = FLinearColor(0.6f, 0.4f, 0.25f);
		}
		else if (Map->Sectors[L.FrontSector].Ceiling != Map->Sectors[L.BackSector].Ceiling)
		{
			C = FLinearColor(0.95f, 0.9f, 0.3f);
		}
		else
		{
			continue;
		}
		const FVector2D A = ToScreen(Map->V(L.V1)), B = ToScreen(Map->V(L.V2));
		DrawLine(A.X, A.Y, B.X, B.Y, C, 1.5f);
	}
	// the players' arrows: yours in white, the others in their colours (green, grey, brown, red)
	static const FLinearColor PlayerColors[4] = { FLinearColor(0.3f, 0.8f, 0.3f), FLinearColor(0.6f, 0.6f, 0.6f), FLinearColor(0.65f, 0.45f, 0.25f), FLinearColor(0.85f, 0.15f, 0.15f) };
	for (ADoomPlayer* Other : W->GetPlayers())
	{
		if (Other->IsDead() && Other != P)
		{
			continue;
		}
		const FVector2f Pos = Other->DoomPos();
		const float Ang = FMath::DegreesToRadians(Other->DoomAngle());
		auto Rot = [&](float X, float Y) { return FVector2f(Pos.X + X * FMath::Cos(Ang) - Y * FMath::Sin(Ang), Pos.Y + X * FMath::Sin(Ang) + Y * FMath::Cos(Ang)); };
		const FVector2f Arrow[] = { Rot(26, 0), Rot(-20, 0), Rot(26, 0), Rot(10, 10), Rot(26, 0), Rot(10, -10) };
		const ADoomPlayerState* PS = Other->GetDoomPlayerState();
		const FLinearColor C = Other == P ? FLinearColor::White : PlayerColors[(PS ? PS->PlayerNumber : 0) & 3];
		for (int32 i = 0; i < 6; i += 2)
		{
			const FVector2D A = ToScreen(Arrow[i]), B = ToScreen(Arrow[i + 1]);
			DrawLine(A.X, A.Y, B.X, B.Y, C, 2.f);
		}
	}
	// marks, numbered with the map font
	for (int32 i = 0; i < Automap.Marks.Num(); ++i)
	{
		const FVector2D S = ToScreen(Automap.Marks[i]);
		if (S.X < 0 || S.Y < 0 || S.X > Canvas->ClipX || S.Y > Canvas->ClipY * 168.f / 200.f)
		{
			continue;
		}
		const FString Num = FString::Printf(TEXT("AMMNUM%d"), i);
		if (W->GetGraphics()->Patch(Num))
		{
			DrawPatchByName(Num, (S.X - OX) / SX - 2.f, (S.Y - OY) / SY - 3.f, false);
		}
		else
		{
			DrawText(FString::FromInt(i), (S.X - OX) / SX - 2.f, (S.Y - OY) / SY - 3.f);
		}
	}
	DrawText(W->LevelTitle(W->GetMapName()), 2, 160);
}

void ADoomHUD::AutomapZoom(float Dir, float DeltaSeconds)
{
	if (Automap.bBig)
	{
		return; // vanilla: no zooming on the big map
	}
	Automap.Scale = FMath::Clamp(Automap.Scale * FMath::Pow(2.f, Dir * DeltaSeconds), 0.125f, 8.f);
}

void ADoomHUD::AutomapPan(const FVector2D& Dir, float DeltaSeconds)
{
	if (Automap.bFollow)
	{
		return;
	}
	Automap.Center += FVector2f(Dir.X, Dir.Y) * Automap.ViewSize.X * 0.5f * DeltaSeconds;
}

void ADoomHUD::AutomapToggleFollow()
{
	Automap.bFollow = !Automap.bFollow;
	if (!Automap.bFollow)
	{
		if (const ADoomPlayer* P = Cast<ADoomPlayer>(GetOwningPawn()))
		{
			Automap.Center = P->DoomPos(); // panning starts from where you are
		}
	}
	DW()->ShowMessage(Automap.bFollow ? TEXT("FOLLOW MODE ON") : TEXT("FOLLOW MODE OFF"));
}

void ADoomHUD::AutomapToggleBig()
{
	const FDoomMap* Map = DW()->GetMap();
	if (!Map)
	{
		return;
	}
	Automap.bBig = !Automap.bBig;
	if (Automap.bBig)
	{
		Automap.SavedScale = Automap.Scale;
		Automap.bSavedFollow = Automap.bFollow;
		Automap.SavedCenter = Automap.Center;
		// fit the whole map into the view (Scale is relative to the default ClipY / 1600 pixels per unit)
		const FVector2f Size = Map->Bounds.GetSize();
		const float Base = Automap.PixelsPerUnit / Automap.Scale;
		const float ViewW = Automap.ViewSize.X * Automap.PixelsPerUnit, ViewH = Automap.ViewSize.Y * Automap.PixelsPerUnit * 1.2f;
		Automap.Scale = 0.95f * FMath::Min(ViewW / FMath::Max(Size.X, 1.f), ViewH / FMath::Max(Size.Y * 1.2f, 1.f)) / Base;
		Automap.bFollow = false;
		Automap.Center = Map->Bounds.GetCenter();
	}
	else
	{
		Automap.Scale = Automap.SavedScale;
		Automap.bFollow = Automap.bSavedFollow;
		Automap.Center = Automap.SavedCenter;
	}
}

void ADoomHUD::AutomapMark()
{
	const ADoomPlayer* P = Cast<ADoomPlayer>(GetOwningPawn());
	const FVector2f Where = Automap.bFollow && P ? P->DoomPos() : Automap.Center;
	if (Automap.Marks.Num() < 10)
	{
		Automap.Marks.Add(Where);
	}
	else
	{
		Automap.Marks[Automap.NextMark % 10] = Where;
	}
	Automap.NextMark = (Automap.NextMark + 1) % 10;
	DW()->ShowMessage(FString::Printf(TEXT("MARKED SPOT %d"), (Automap.NextMark + 9) % 10));
}

void ADoomHUD::AutomapClearMarks()
{
	Automap.Marks.Reset();
	Automap.NextMark = 0;
	DW()->ShowMessage(TEXT("ALL MARKS CLEARED"));
}

void ADoomHUD::AutomapToggleGrid()
{
	Automap.bGrid = !Automap.bGrid;
	DW()->ShowMessage(Automap.bGrid ? TEXT("GRID ON") : TEXT("GRID OFF"));
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
			if (M.bEditing && M.Page == EMenuPage::Save && i == M.Item)
			{
				const bool bCursor = (int32)(GetWorld()->GetRealTimeSeconds() * 4.f) & 1;
				DrawText(M.EditText + (bCursor ? TEXT("_") : TEXT("")), O.X, Y);
				continue;
			}
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

void ADoomHUD::DrawLoading()
{
	// the intermission's "entering" screen, with a progress bar while the map loads
	UDoomWorld* W = DW();
	const FString& M = W->GetMapName();
	DrawFullscreenPic(IntermissionBackground());
	if (const FDoomPicture* Entering = W->GetGraphics()->Patch(TEXT("WIENTER")))
	{
		DrawPatch(Entering, 160 - Entering->Width / 2, 4, false);
	}
	else
	{
		DrawText(TEXT("ENTERING"), 160 - TextWidth(TEXT("ENTERING")) / 2, 4, 1.f);
	}
	const FString Title = W->LevelTitle(M);
	DrawText(Title, 160 - TextWidth(Title) / 2, 22, 1.f);
	const float Progress = FMath::Clamp(W->LoadingProgress(), 0.f, 1.f);
	Rect(80, 184, 160, 6, FLinearColor(0.05f, 0.05f, 0.05f, 0.85f));
	Rect(81, 185, 158 * Progress, 4, FLinearColor(0.75f, 0.08f, 0.05f, 1.f));
}

FString ADoomHUD::IntermissionBackground() const
{
	// WI_loadData: Doom 2 and Ultimate Doom's episode 4 use INTERPIC; episodes 1-3 their WIMAP0-2
	UDoomWorld* W = DW();
	const FString& M = W->GetMapName();
	const bool bExMy = M.Len() >= 4 && M[0] == 'E' && FChar::IsDigit(M[1]) && M[2] == 'M';
	const int32 Episode = bExMy ? M[1] - '1' : -1;
	const bool bInterpic = W->GetGraphics()->Patch(TEXT("INTERPIC")) != nullptr;
	if (bInterpic && ((W->GetWad() && W->GetWad()->IsDoom2()) || Episode < 0 || Episode >= 3 || !W->GetGraphics()->Patch(TEXT("WIMAP0"))))
	{
		return TEXT("INTERPIC");
	}
	return FString::Printf(TEXT("WIMAP%d"), FMath::Clamp(Episode, 0, 2));
}

void ADoomHUD::DrawIntermission()
{
	UDoomWorld* W = DW();
	DrawFullscreenPic(IntermissionBackground());
	const float T = W->Flow().IntermissionTic;
	auto Count = [&](int32 Got, int32 Total, float Start)
	{
		const int32 Pct = Got * 100 / FMath::Max(Total, 1); // vanilla: nothing to find shows 0%
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
	const ADoomGameState* GS = GetWorld()->GetGameState<ADoomGameState>();
	const TArray<ADoomPlayerState*> Players = GS ? GS->DoomPlayers() : TArray<ADoomPlayerState*>();
	if (Players.Num() > 1)
	{
		DrawNetgameStats(Players, W->Flow().bDeathmatch, T);
	}
	else
	{
		Line(TEXT("KILLS"), TEXT("WIOSTK"), Count(W->Flow().Kills, W->Flow().TotalKills, 10), 50, true);
		if (T > 40) Line(TEXT("ITEMS"), TEXT("WIOSTI"), Count(W->Flow().Items, W->Flow().TotalItems, 40), 70, true);
		if (T > 70) Line(TEXT("SECRET"), TEXT("WISCRT2"), Count(W->Flow().Secrets, W->Flow().TotalSecrets, 70), 90, true);
	}
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
	if (T > Doom::TicRate * 4 && !W->Flow().PendingMap.IsEmpty())
	{
		const FString Next = TEXT("ENTERING ") + W->LevelTitle(W->Flow().PendingMap);
		DrawText(Next, 160 - TextWidth(Next) / 2, 130, 1.f);
	}
	if (T > Doom::TicRate)
	{
		DrawText(W->Flow().PendingMap.IsEmpty() ? TEXT("EPISODE COMPLETE - PRESS ENTER") : TEXT("PRESS ENTER"), 90, 150, 1.f, FLinearColor(0.8f, 0.8f, 0.8f));
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
		// F_BunnyScroll: PFUB1 first, sliding right as PFUB2 comes in from the left, then THE END is
		// spelled out; Scrolled counts down from 320 (all PFUB1) to 0 (all PFUB2)
		const float Scrolled = 320.f - FMath::Clamp((W->FinaleTic - 230) / 2.f, 0.f, 320.f);
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
