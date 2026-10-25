// -editortest (with -warp): the map editor driven through its own API, as the panels and the map view
// drive it, with PASS / FAIL lines in the log and screenshots of the editor (editor_2d.png,
// editor_3d.png). Steps:
//   open        the editor opens over the map: panels, the clock stopped
//   floor       the start sector's floor goes up 16 in the edited map and in the level
//   drawinside  a 64-unit square drawn inside the start sector is a new sector with two-sided walls
//   platform    that sector raised 24 becomes a step up in the level
//   drawroom    a room drawn in the void against a wall of the start sector: the wall becomes an
//               opening, and a monster could walk in (a monster path to its middle)
//   thing       an imp placed in the new room is a monster in the level
//   undo        undo takes the imp and the room away, redo brings them back
//   check       the map checker finds no more errors than the map had before
//   save        the edited map saved as a PWAD reads back with the same sectors, lines and things
//   play        closing the editor plays the edited map: the clock runs, the player walks
#include "DoomEditor.h"
#include "DoomWorld.h"
#include "DoomGameMode.h"
#include "DoomPlayer.h"
#include "DoomMonster.h"
#include "DoomMapCheck.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "Containers/Ticker.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UnrealClient.h"

namespace DoomEditorTest
{
	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		int32 Step = 0;
		int32 Wait = 0;
		int32 Passed = 0, Failed = 0;
		int32 StartSector = INDEX_NONE, NewSector = INDEX_NONE, RoomSector = INDEX_NONE, RoomLine = INDEX_NONE;
		int32 ErrorsBefore = 0;
		FVector2f RoomMid = FVector2f::ZeroVector;
		int32 MonstersBefore = 0;
	};

	void Report(FRun& R, const TCHAR* Name, bool bPass, const FString& Msg)
	{
		(bPass ? R.Passed : R.Failed)++;
		UE_LOG(LogDoomTest, Display, TEXT("Editortest %s %s: %s"), Name, bPass ? TEXT("PASS") : TEXT("FAIL"), *Msg);
	}

	int32 LiveMonsters(UDoomWorld* W)
	{
		int32 N = 0;
		for (const TWeakObjectPtr<ADoomMonster>& M : W->GetMonsters()) N += M.IsValid() && !M->IsDead() ? 1 : 0;
		return N;
	}

	void Shot(const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / FString::Printf(TEXT("editor_%s.png"), Name), true, false);
	}

	/** One step per call; false when the run is over. */
	bool Tick(FRun& R)
	{
		UWorld* World = R.World.Get();
		UDoomWorld* W = World ? World->GetSubsystem<UDoomWorld>() : nullptr;
		UDoomMapEditor* E = World ? World->GetSubsystem<UDoomMapEditor>() : nullptr;
		if (!W || !E)
		{
			return false;
		}
		if (R.Wait > 0)
		{
			--R.Wait;
			return true;
		}
		const FDoomMap* M = E->GetMap();
		switch (R.Step++)
		{
		case 0:
		{
			E->Open();
			M = E->GetMap();
			const ADoomGameMode* GM = World->GetAuthGameMode<ADoomGameMode>();
			R.ErrorsBefore = M ? DoomMapCheck::Count(DoomMapCheck::Run(*M, W->GetGraphics(), M->Name), FDoomMapIssue::ESeverity::Error) : -1;
			Report(R, TEXT("open"), E->IsOpen() && M && E->GetWidget() && E->GetWidget()->IsInViewport() && GM && GM->bClockStopped,
				FString::Printf(TEXT("%s: %d sectors, %d lines, %d things; %d checker errors to begin with"), M ? *M->Name : TEXT("-"),
					M ? M->Sectors.Num() : 0, M ? M->Lines.Num() : 0, M ? M->Things.Num() : 0, R.ErrorsBefore));
			R.Wait = 10;
			return true;
		}
		case 1:
		{
			E->SetMode(EDoomEditMode::Sectors);
			E->SelectAt(W->GetPlayer()->DoomPos(), 8.f);
			R.StartSector = E->Selected;
			const float Old = M->Sectors.IsValidIndex(R.StartSector) ? M->Sectors[R.StartSector].Floor : 0;
			E->SetNumber(EDoomEditField::SectorFloor, Old + 16.f);
			const float Now = W->GetMap()->Sectors.IsValidIndex(R.StartSector) ? W->GetMap()->Sectors[R.StartSector].Floor : -1;
			Report(R, TEXT("floor"), R.StartSector != INDEX_NONE && E->GetMap()->Sectors[R.StartSector].Floor == Old + 16.f && Now == Old + 16.f,
				FString::Printf(TEXT("sector %d floor %.0f -> %.0f (level %.0f)"), R.StartSector, Old, Old + 16.f, Now));
			return true;
		}
		case 2:
		{
			// a spot in the start sector with room for a 64-unit square, clear of its walls
			const FVector2f From = W->GetPlayer()->DoomPos();
			FVector2f Spot;
			bool bFound = false;
			for (int32 Ring = 2; Ring < 24 && !bFound; ++Ring)
			{
				for (int32 a = 0; a < 16 && !bFound; ++a)
				{
					const FVector2f C = DoomMapEdit::Snap(From + FVector2f(FMath::Cos(a * UE_PI / 8), FMath::Sin(a * UE_PI / 8)) * (Ring * 32.f), 8.f);
					bool bOk = true;
					for (int32 dy = -1; dy <= 1 && bOk; dy += 2)
					{
						for (int32 dx = -1; dx <= 1 && bOk; dx += 2)
						{
							bOk = DoomMapEdit::SectorInside(*M, C + FVector2f(dx * 32.f, dy * 32.f)) == R.StartSector;
						}
					}
					bOk = bOk && DoomMapEdit::LineNear(*M, C, 48.f) == INDEX_NONE && DoomMapEdit::ThingNear(*M, C, 48.f) == INDEX_NONE && FVector2f::Distance(C, From) > 64.f;
					if (bOk) { Spot = C; bFound = true; }
				}
			}
			const int32 Before = M->Sectors.Num();
			E->SetMode(EDoomEditMode::Draw);
			E->DrawPoints = { Spot + FVector2f(-32, -32), Spot + FVector2f(-32, 32), Spot + FVector2f(32, 32), Spot + FVector2f(32, -32) };
			const bool bDrawn = bFound && E->FinishDraw();
			M = E->GetMap();
			R.NewSector = bDrawn ? E->Selected : INDEX_NONE;
			int32 TwoSided = 0;
			for (const FDoomLine& L : M->Lines) TwoSided += (L.FrontSector == R.NewSector && L.BackSector == R.StartSector) || (L.BackSector == R.NewSector && L.FrontSector == R.StartSector);
			Report(R, TEXT("drawinside"), bDrawn && M->Sectors.Num() == Before + 1 && TwoSided == 4 && W->GetMap()->Sectors.Num() == Before + 1,
				FString::Printf(TEXT("square at (%.0f,%.0f): sector %d, %d two-sided walls to sector %d; %s"), Spot.X, Spot.Y, R.NewSector, TwoSided, R.StartSector, *E->Status.ToString()));
			return true;
		}
		case 3:
		{
			const float Base = M->Sectors.IsValidIndex(R.NewSector) ? M->Sectors[R.NewSector].Floor : 0;
			E->SetMode(EDoomEditMode::Sectors);
			E->Selected = R.NewSector;
			E->SetNumber(EDoomEditField::SectorFloor, Base + 24.f);
			const float Level = W->GetMap()->Sectors.IsValidIndex(R.NewSector) ? W->GetMap()->Sectors[R.NewSector].Floor : -1;
			const int32 Lower = W->GetMap()->Sectors.IsValidIndex(R.StartSector) ? (int32)W->GetMap()->Sectors[R.StartSector].Floor : -1;
			Report(R, TEXT("platform"), Level == Base + 24.f, FString::Printf(TEXT("sector %d floor %.0f, around it %d"), R.NewSector, Level, Lower));
			return true;
		}
		case 4:
		{
			// a room out of a wall: the nearest walls to the player first, the start sector's before others,
			// 256 deep where it fits, else 128
			const int32 Before = M->Sectors.Num();
			const FVector2f From = W->GetPlayer()->DoomPos();
			TArray<int32> Walls;
			for (int32 l = 0; l < M->Lines.Num(); ++l)
			{
				const FDoomLine& L = M->Lines[l];
				const FVector2f A = M->V(L.V1), B = M->V(L.V2);
				if (L.Side[1] == INDEX_NONE && L.FrontSector != INDEX_NONE && FVector2f::Distance(A, B) >= 96.f && (FMath::IsNearlyEqual(A.X, B.X) || FMath::IsNearlyEqual(A.Y, B.Y))) Walls.Add(l);
			}
			auto Dist = [&](int32 l) { return (M->Lines[l].FrontSector == R.StartSector ? 0.f : 100000.f) + FVector2f::Distance(From, (M->V(M->Lines[l].V1) + M->V(M->Lines[l].V2)) * 0.5f); };
			Walls.Sort([&](int32 X, int32 Y) { return Dist(X) < Dist(Y); });
			bool bDrawn = false;
			for (int32 Depth : { 256, 128 })
			{
				for (int32 i = 0; i < Walls.Num() && i < 200 && !bDrawn; ++i)
				{
					const int32 l = Walls[i];
					const FVector2f A = E->GetMap()->V(E->GetMap()->Lines[l].V1), B = E->GetMap()->V(E->GetMap()->Lines[l].V2);
					const FVector2f D = (B - A).GetSafeNormal();
					const FVector2f Out(-D.Y, D.X); // the front (inside) is on the right: out is the left
					E->SetMode(EDoomEditMode::Draw);
					E->DrawPoints = { A, B, B + Out * (float)Depth, A + Out * (float)Depth };
					if (E->FinishDraw())
					{
						bDrawn = true;
						R.RoomSector = E->Selected;
						R.RoomLine = l;
						R.RoomMid = (A + B) * 0.5f + Out * (Depth * 0.5f);
					}
				}
				if (bDrawn) break;
			}
			M = E->GetMap();
			const bool bOpening = M->Lines.IsValidIndex(R.RoomLine) && M->Lines[R.RoomLine].Side[1] != INDEX_NONE;
			TArray<FVector2f> Path;
			const bool bPath = bDrawn && W->GetNav().FindPath(*W->GetMap(), W->GetPlayer()->DoomPos(), R.RoomMid, 20.f, 56.f, false, Path);
			Report(R, TEXT("drawroom"), bDrawn && bOpening && M->Sectors.Num() == Before + 1 && bPath,
				FString::Printf(TEXT("room sector %d off line %d (now %s), a monster path in: %d waypoints; %s"), R.RoomSector, R.RoomLine,
					bOpening ? TEXT("two-sided") : TEXT("still a wall"), bPath ? Path.Num() : -1, *E->Status.ToString()));
			return true;
		}
		case 5:
		{
			R.MonstersBefore = LiveMonsters(W);
			const int32 Things = M->Things.Num();
			E->AddThing(R.RoomMid, 3001, 270.f);
			const int32 After = LiveMonsters(W);
			Report(R, TEXT("thing"), E->GetMap()->Things.Num() == Things + 1 && After == R.MonstersBefore + 1,
				FString::Printf(TEXT("things %d -> %d, monsters in the level %d -> %d"), Things, E->GetMap()->Things.Num(), R.MonstersBefore, After));
			return true;
		}
		case 6:
		{
			const int32 Sectors = M->Sectors.Num(), Things = M->Things.Num();
			E->Undo();
			E->Undo();
			const bool bGone = E->GetMap()->Things.Num() == Things - 1 && E->GetMap()->Sectors.Num() == Sectors - 1
				&& E->GetMap()->Lines.IsValidIndex(R.RoomLine) && E->GetMap()->Lines[R.RoomLine].Side[1] == INDEX_NONE;
			E->Redo();
			E->Redo();
			const bool bBack = E->GetMap()->Things.Num() == Things && E->GetMap()->Sectors.Num() == Sectors && LiveMonsters(W) == R.MonstersBefore + 1;
			Report(R, TEXT("undo"), bGone && bBack, FString::Printf(TEXT("undo twice: imp and room %s; redo twice: %s"), bGone ? TEXT("gone") : TEXT("STILL THERE"), bBack ? TEXT("back") : TEXT("MISSING")));
			return true;
		}
		case 7:
		{
			const int32 Errors = E->CheckMap();
			Report(R, TEXT("check"), Errors >= 0 && Errors <= R.ErrorsBefore, FString::Printf(TEXT("%d errors (%d before the edits): %s"), Errors, R.ErrorsBefore, *E->Status.ToString()));
			// the 2D view, fitted to the new room and the start
			E->Mode = EDoomEditMode::Sectors;
			E->Selected = R.RoomSector;
			E->b3D = false;
			E->ViewCenter = (R.RoomMid + W->GetPlayer()->DoomPos()) * 0.5f;
			E->Zoom = 1.f;
			E->Refresh();
			R.Wait = 20;
			return true;
		}
		case 8:
			Shot(TEXT("2d"));
			R.Wait = 10;
			return true;
		case 9:
		{
			const FString Path = FPaths::ProjectSavedDir() / TEXT("Test") / TEXT("editortest.wad");
			const bool bSaved = E->SaveWad(Path);
			FDoomWad Back;
			FDoomMap Read;
			const FString Slot = W->GetWad()->IsDoom2() ? TEXT("MAP01") : TEXT("E1M1");
			const FString& Name = E->GetMap()->Name;
			const bool bSlot = (Name.Len() == 4 && Name[0] == 'E') || Name.StartsWith(TEXT("MAP"));
			const bool bRead = bSaved && Back.AddFile(Path) && (Back.Finalize(), Read.Load(Back, bSlot ? Name : Slot));
			const bool bSame = bRead && Read.Sectors.Num() == E->GetMap()->Sectors.Num() && Read.Lines.Num() == E->GetMap()->Lines.Num() && Read.Things.Num() == E->GetMap()->Things.Num();
			Report(R, TEXT("save"), bSame, FString::Printf(TEXT("%s; read back %d sectors, %d lines, %d things"), *E->Status.ToString(), Read.Sectors.Num(), Read.Lines.Num(), Read.Things.Num()));
			// the 3D view: from the start sector, looking into the new room
			E->b3D = true;
			if (ADoomPlayer* P = W->GetPlayer())
			{
				const FVector2f Look = R.RoomMid - P->DoomPos();
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					PC->SetControlRotation(FRotator(-10.f, FMath::RadiansToDegrees(FMath::Atan2(-Look.Y, Look.X)), 0));
				}
			}
			E->Refresh();
			R.Wait = 20;
			return true;
		}
		case 10:
			Shot(TEXT("3d"));
			R.Wait = 10;
			return true;
		case 11:
		{
			const int32 Sectors = M->Sectors.Num();
			E->Close();
			const ADoomGameMode* GM = World->GetAuthGameMode<ADoomGameMode>();
			const ADoomPlayer* P = W->GetPlayer();
			const bool bOk = !E->IsOpen() && GM && !GM->bClockStopped && W->GetState() == EDoomGameState::Playing && P
				&& P->GetCharacterMovement()->MovementMode == MOVE_Walking && W->GetMap()->Sectors.Num() == Sectors;
			Report(R, TEXT("play"), bOk, FString::Printf(TEXT("editor closed, clock %s, state %d, player %s, level has %d sectors (edited %d)"),
				GM && !GM->bClockStopped ? TEXT("running") : TEXT("STOPPED"), (int32)W->GetState(),
				P && P->GetCharacterMovement()->MovementMode == MOVE_Walking ? TEXT("walking") : TEXT("NOT WALKING"), W->GetMap()->Sectors.Num(), Sectors));
			R.Wait = 30;
			return true;
		}
		default:
			UE_LOG(LogDoomTest, Display, TEXT("Editortest: %d passed, %d failed"), R.Passed, R.Failed);
			FPlatformMisc::RequestExit(false);
			return false;
		}
	}

	void Start(UWorld* World)
	{
		TSharedRef<FRun> R = MakeShared<FRun>();
		R->World = World;
		R->Wait = 60; // let the level settle
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([R](float) { return Tick(*R); }));
	}
}
