#include "DoomGameState.h"
#include "Engine/World.h"
#include "DoomPlayerState.h"
#include "DoomWorld.h"
#include "DoomUE.h"
#include "DoomMap.h"
#include "Net/UnrealNetwork.h"

ADoomGameState::ADoomGameState()
{
	SectorNet.Owner = this;
	SideNet.Owner = this;
}

void ADoomGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomGameState, Flow);
	DOREPLIFETIME(ADoomGameState, SectorNet);
	DOREPLIFETIME(ADoomGameState, SideNet);
	DOREPLIFETIME(ADoomGameState, Finale);
}

void ADoomGameState::PublishFinale(const UDoomWorld& W)
{
	FDoomFinaleNet N;
	N.Stage = (uint8)W.FinaleStage;
	N.Text = W.FinaleText;
	N.Backdrop = W.FinaleBackdrop;
	N.Picture = W.FinalePicture;
	N.Tic = W.FinaleTic;
	N.CastIndex = W.CastCall.Index;
	N.CastSequence = W.CastCall.Sequence;
	N.CastFrame = W.CastCall.Frame;
	N.bCastAttacking = W.CastCall.bAttacking;
	N.bCastDying = W.CastCall.bDying;
	Finale = MoveTemp(N);
}

void ADoomGameState::OnRep_Finale()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W || !W->IsNetClient())
	{
		return;
	}
	// the client draws what the server shows: text typed out to the server's tic, the picture, the cast
	W->FinaleStage = (UDoomWorld::EFinaleStage)Finale.Stage;
	W->FinaleText = Finale.Text;
	W->FinaleBackdrop = Finale.Backdrop;
	W->FinalePicture = Finale.Picture;
	W->FinaleTic = Finale.Tic;
	W->CastCall.Index = Finale.CastIndex;
	W->CastCall.Sequence = Finale.CastSequence;
	W->CastCall.Frame = Finale.CastFrame;
	W->CastCall.bAttacking = Finale.bCastAttacking;
	W->CastCall.bDying = Finale.bCastDying;
	if (Finale.Stage != LastFinaleStage)
	{
		LastFinaleStage = Finale.Stage;
		UE_LOG(LogDoom, Log, TEXT("Finale from the server: stage %d, %d text lines, backdrop %s, picture %s"), Finale.Stage, Finale.Text.Num(), *Finale.Backdrop, *Finale.Picture);
	}
}

void ADoomGameState::OnRep_Flow()
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
	{
		W->OnFlowReplicated();
	}
}

TArray<ADoomPlayerState*> ADoomGameState::DoomPlayers() const
{
	TArray<ADoomPlayerState*> Out;
	for (APlayerState* PS : PlayerArray)
	{
		if (ADoomPlayerState* D = Cast<ADoomPlayerState>(PS)) Out.Add(D);
	}
	Out.Sort([](const ADoomPlayerState& A, const ADoomPlayerState& B) { return A.PlayerNumber < B.PlayerNumber; });
	return Out;
}

void ADoomGameState::ResetWorldNet()
{
	SectorNet.Items.Reset();
	SectorNet.MarkArrayDirty();
	SideNet.Items.Reset();
	SideNet.MarkArrayDirty();
	SectorItem.Reset();
	SideItem.Reset();
}

void ADoomGameState::PublishWorld(const FDoomMap& Map, const TArray<FDoomSector>& Baseline, const TArray<FDoomSide>& BaselineSides)
{
	const int32 Serial = Flow.LevelSerial;
	for (int32 i = 0; i < Map.Sectors.Num() && i < Baseline.Num(); ++i)
	{
		const FDoomSector& S = Map.Sectors[i];
		const int32* Found = SectorItem.Find(i);
		if (!Found)
		{
			const FDoomSector& B = Baseline[i];
			if (S.Floor == B.Floor && S.Ceiling == B.Ceiling && S.Light == B.Light && S.Special == B.Special && S.FloorPic == B.FloorPic && S.CeilingPic == B.CeilingPic)
			{
				continue; // never changed: clients have it from the WAD
			}
			const int32 NewIndex = SectorNet.Items.AddDefaulted();
			SectorNet.Items[NewIndex].Sector = i;
			Found = &SectorItem.Add(i, NewIndex);
		}
		FDoomSectorNet& N = SectorNet.Items[*Found];
		if (N.LevelSerial != Serial || N.Floor != S.Floor || N.Ceiling != S.Ceiling || N.Light != S.Light || N.Special != S.Special || N.FloorPic != S.FloorPic || N.CeilingPic != S.CeilingPic)
		{
			N.LevelSerial = Serial;
			N.Floor = S.Floor;
			N.Ceiling = S.Ceiling;
			N.Light = S.Light;
			N.Special = S.Special;
			N.FloorPic = S.FloorPic;
			N.CeilingPic = S.CeilingPic;
			SectorNet.MarkItemDirty(N);
		}
	}
	for (int32 i = 0; i < Map.Sides.Num() && i < BaselineSides.Num(); ++i)
	{
		const FDoomSide& S = Map.Sides[i];
		const int32* Found = SideItem.Find(i);
		if (!Found)
		{
			const FDoomSide& B = BaselineSides[i];
			if (S.Upper == B.Upper && S.Middle == B.Middle && S.Lower == B.Lower)
			{
				continue;
			}
			const int32 NewIndex = SideNet.Items.AddDefaulted();
			SideNet.Items[NewIndex].Side = i;
			Found = &SideItem.Add(i, NewIndex);
		}
		FDoomSideNet& N = SideNet.Items[*Found];
		if (N.LevelSerial != Serial || N.Upper != S.Upper || N.Middle != S.Middle || N.Lower != S.Lower)
		{
			N.LevelSerial = Serial;
			N.Upper = S.Upper;
			N.Middle = S.Middle;
			N.Lower = S.Lower;
			SideNet.MarkItemDirty(N);
		}
	}
}

void FDoomSectorNet::PostReplicatedAdd(const FDoomSectorNetArray& Array) { PostReplicatedChange(Array); }

void FDoomSectorNet::PostReplicatedChange(const FDoomSectorNetArray& Array)
{
	if (Array.Owner && Array.Owner->GetWorld())
	{
		if (UDoomWorld* W = Array.Owner->GetWorld()->GetSubsystem<UDoomWorld>())
		{
			W->ApplySectorNet(*this);
		}
	}
}

void FDoomSideNet::PostReplicatedAdd(const FDoomSideNetArray& Array) { PostReplicatedChange(Array); }

void FDoomSideNet::PostReplicatedChange(const FDoomSideNetArray& Array)
{
	if (Array.Owner && Array.Owner->GetWorld())
	{
		if (UDoomWorld* W = Array.Owner->GetWorld()->GetSubsystem<UDoomWorld>())
		{
			W->ApplySideNet(*this);
		}
	}
}

// Multicasts also run on the server, which already did the work locally.
void ADoomGameState::MulticastSound_Implementation(const FString& Name, FVector_NetQuantize Location, bool bLocated, float Volume, APlayerState* Except)
{
	if (HasAuthority())
	{
		return;
	}
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (W && !(Except && PC && PC->PlayerState == Except))
	{
		const FVector L = Location;
		W->PlaySoundLocal(Name, bLocated ? &L : nullptr, Volume);
	}
}

void ADoomGameState::MulticastEffect_Implementation(const FString& Sprite, const FString& Frames, uint8 Tics, FVector_NetQuantize Location, bool bFullBright, bool bLoop)
{
	if (!HasAuthority())
	{
		if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
		{
			W->SpawnEffectLocal(Sprite, Frames, Tics, Location, bFullBright, bLoop);
		}
	}
}

void ADoomGameState::MulticastParticles_Implementation(uint8 Kind, FVector_NetQuantize Location, FVector_NetQuantizeNormal Up)
{
	if (!HasAuthority())
	{
		if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>())
		{
			W->SpawnParticlesLocal((EDoomParticles)Kind, Location, Up);
		}
	}
}
