#include "DoomSpriteComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "DoomWorld.h"
#include "DoomGraphics.h"
#include "DoomAssets.h"
#include "DoomLevel.h"
#include "DoomUE.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Net/UnrealNetwork.h"

UDoomSpriteComponent::UDoomSpriteComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	bUseAsyncCooking = false;
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
	SetIsReplicatedByDefault(true);
}

void UDoomSpriteComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDoomSpriteComponent, Net);
}

bool UDoomSpriteComponent::IsSimulated() const
{
	const AActor* Owner = GetOwner();
	return !Owner || Owner->HasAuthority();
}

void UDoomSpriteComponent::SetTranslation(int32 InTranslation)
{
	if (Translation == InTranslation)
	{
		return;
	}
	Translation = InTranslation;
	Net.Translation = (uint8)InTranslation;
	LastPicture = nullptr; // the next facing pass picks the recoloured frame
	bDirty = true;
	Wake();
}

void UDoomSpriteComponent::OnRep_Net()
{
	if (!DoomWorld || !MID)
	{
		UDoomWorld* W = GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
		if (!W || !W->EnsureWads())
		{
			return;
		}
		Init(W, Net.Sprite);
	}
	if (Sprite != Net.Sprite)
	{
		Sprite = Net.Sprite;
		bDirty = true;
		LastPicture = nullptr;
	}
	if (Net.Serial != LastSerial)
	{
		LastSerial = Net.Serial;
		PlaySequence(Net.Seq, Net.Tics, Net.bLoop, Net.bBright);
	}
	SetFuzz(Net.bFuzz);
	DoomAngle = Net.Angle;
	SetTranslation(Net.Translation);
}

void UDoomSpriteComponent::BindLevel()
{
	ADoomLevel* Level = DoomWorld ? DoomWorld->GetLevel() : nullptr;
	if (!MID || !Level || !DoomWorld->GetMap())
	{
		return;
	}
	MID->SetTextureParameterValue(TEXT("SectorLights"), Level->GetSectorLightTexture());
	MID->SetScalarParameterValue(TEXT("SectorLightRows"), FMath::Max(1, FMath::DivideAndRoundUp(DoomWorld->GetMap()->Sectors.Num(), 256)));
	bLevelBound = true;
}

void UDoomSpriteComponent::Init(UDoomWorld* InWorld, const FString& InSprite)
{
	DoomWorld = InWorld;
	Sprite = InSprite;

	// unit quad: Y right 0..1, Z up 0..1, facing -X (towards the viewer once rotated)
	const TArray<FVector> V = { FVector(0, 0, 0), FVector(0, 0, 1), FVector(0, 1, 1), FVector(0, 1, 0) };
	const TArray<int32> I = { 0, 1, 2, 0, 2, 3 };
	const TArray<FVector> N = { FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0) };
	const TArray<FVector2D> UV = { FVector2D(0, 1), FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1) };
	const TArray<FLinearColor> C = { FLinearColor::White, FLinearColor::White, FLinearColor::White, FLinearColor::White };
	CreateMeshSection_LinearColor(0, V, I, N, UV, C, {}, false);

	MID = UMaterialInstanceDynamic::Create(DoomAssets::SpriteMaterial(), this);
	MID->SetTextureParameterValue(TEXT("Colormap"), DoomWorld->GetGraphics()->ColormapTexture());
	BindLevel();
	SetMaterial(0, MID);
	if (IsSimulated())
	{
		Net.Sprite = Sprite;
	}
	// generous bounds: the quad is resized every frame
	SetBoundsScale(4.f);
	bDirty = true;
}

void UDoomSpriteComponent::PlaySequence(const FString& Frames, int32 Tics, bool bInLoop, bool bFullBright)
{
	Wake(); // a new sequence animates
	Seq = Frames.IsEmpty() ? FString(TEXT("A")) : Frames;
	Index = 0;
	TicsPerFrame = FMath::Max(1, Tics);
	TicCount = 0;
	bLoop = bInLoop;
	bDone = false;
	bDirty = true;
	SetFullBright(bFullBright);
	if (IsSimulated())
	{
		Net.Seq = Seq;
		Net.Tics = (uint8)FMath::Clamp(TicsPerFrame, 1, 255);
		Net.bLoop = bLoop;
		Net.bBright = bFullBright;
		++Net.Serial;
	}
}

bool UDoomSpriteComponent::AnimateTic()
{
	if (bDone)
	{
		return false;
	}
	if (++TicCount < TicsPerFrame)
	{
		return false;
	}
	TicCount = 0;
	if (Index + 1 < Seq.Len())
	{
		++Index;
	}
	else if (bLoop)
	{
		Index = 0;
	}
	else
	{
		bDone = true;
		return false;
	}
	bDirty = true;
	return true;
}

void UDoomSpriteComponent::SetSector(int32 InSector)
{
	if (InSector != Sector && MID)
	{
		Sector = InSector;
		MID->SetScalarParameterValue(TEXT("Sector"), FMath::Max(0, Sector));
	}
}

void UDoomSpriteComponent::SetFullBright(bool b)
{
	bBright = b;
	if (MID)
	{
		MID->SetScalarParameterValue(TEXT("FullBright"), b ? 1.f : 0.f);
	}
}

void UDoomSpriteComponent::SetFuzz(bool b)
{
	if (b != bFuzzOn && MID)
	{
		// spectres: Doom's fuzz copies the background shifted a row and darkened, which needs
		// a translucent material reading scene colour; everything else stays masked
		UMaterialInterface* Parent = b ? DoomAssets::SpriteFuzzMaterial() : DoomAssets::SpriteMaterial();
		UMaterialInstanceDynamic* Old = MID;
		MID = UMaterialInstanceDynamic::Create(Parent, this);
		MID->CopyParameterOverrides(Old);
		SetMaterial(0, MID);
		LastPicture = nullptr;
	}
	bFuzzOn = b;
	if (IsSimulated())
	{
		Net.bFuzz = b;
	}
	if (MID)
	{
		MID->SetScalarParameterValue(TEXT("Fuzz"), b ? 1.f : 0.f);
	}
}

void UDoomSpriteComponent::DoomSerialize(FArchive& Ar)
{
	bool Bright = bBright, Fuzz = bFuzzOn;
	Ar << Sprite << Seq << Index << TicsPerFrame << TicCount << bLoop << bDone << Bright << Fuzz << DoomAngle;
	if (Ar.IsLoading())
	{
		Index = FMath::Clamp(Index, 0, FMath::Max(0, Seq.Len() - 1));
		SetFullBright(Bright);
		SetFuzz(Fuzz);
		LastPicture = nullptr;
		bDirty = true;
	}
}

void UDoomSpriteComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bLevelBound)
	{
		BindLevel(); // a client's things can arrive before its level is built
	}
	if (IsSimulated())
	{
		const uint16 Angle = (uint16)FMath::RoundToInt(FMath::Fmod(DoomAngle + 720.f, 360.f));
		if (Angle != Net.Angle) Net.Angle = Angle;
		if (Sprite != Net.Sprite) Net.Sprite = Sprite;
	}
	if (DoomWorld && GetNetMode() == NM_Client)
	{
		// clients run no game tics: they animate sprites (replicated ones and their own
		// effects) on a local 35 Hz clock
		ClientTicTime += FMath::Min(DeltaTime, 0.25f);
		while (ClientTicTime >= Doom::TicSeconds)
		{
			ClientTicTime -= Doom::TicSeconds;
			AnimateTic();
			SetSector(DoomWorld->SectorAt(GetOwner()->GetActorLocation()));
		}
	}
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (Cam)
	{
		UpdateVisual(Cam->GetCameraLocation());
	}
	// a finished corpse sleeps: no tick of its own, UDoomWorld faces it to the camera while it shows
	// ... and so does a thing showing one frame for good
	const bool bStill = (bSleepWhenDone && bDone && !bLoop) || (bSleepWhenStatic && Seq.Len() == 1);
	if (bStill && bLevelBound && DoomWorld && !bSleeping)
	{
		bSleeping = true;
		SetComponentTickEnabled(false);
		DoomWorld->AddSleepingSprite(this);
	}
}

void UDoomSpriteComponent::Wake()
{
	if (bSleeping)
	{
		bSleeping = false;
		SetComponentTickEnabled(true); // UDoomWorld drops it from its list on the next pass
	}
}

void UDoomSpriteComponent::UpdateVisual(const FVector& ViewLocation)
{
	if (!DoomWorld || !DoomWorld->GetGraphics() || !GetOwner())
	{
		return;
	}
	const FVector Feet = GetOwner()->GetActorLocation() - FVector(0, 0, Doom::HalfHeight(GetOwner()));
	const FVector ToThing = Feet - ViewLocation;

	// rotation: Doom's (angle_to_thing - thing_angle + 202.5) / 45
	const float AngToThing = FMath::RadiansToDegrees(FMath::Atan2(-ToThing.Y, ToThing.X));
	const float Rel = FMath::Fmod(AngToThing - DoomAngle + 202.5f + 720.f, 360.f);
	const int32 Rotation = FMath::Clamp((int32)(Rel / 45.f), 0, 7) + 1;

	const FDoomSpriteFrame F = DoomWorld->GetGraphics()->SpriteFrame(Sprite, CurrentFrame(), Rotation, Translation);
	if (!F.Picture)
	{
		SetVisibility(false);
		return;
	}
	SetVisibility(true);
	if (F.Picture != LastPicture || F.bMirror != bLastMirror)
	{
		LastPicture = F.Picture;
		bLastMirror = F.bMirror;
		MID->SetTextureParameterValue(TEXT("Tex"), F.Picture->Texture);
		MID->SetTextureParameterValue(TEXT("TexIndex"), F.Picture->IndexTexture);
		MID->SetScalarParameterValue(TEXT("Mirror"), F.bMirror ? 1.f : 0.f);
	}

	// face the camera (Doom's sprites are screen-aligned around the vertical axis)
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(ToThing.Y, ToThing.X));
	const FRotator Rot(0, Yaw, 0);
	const FVector Right = FRotationMatrix(Rot).GetUnitAxis(EAxis::Y);
	const float S = Doom::Scale;
	const float Left = F.bMirror ? (F.Picture->Width - F.Picture->LeftOffset) : F.Picture->LeftOffset;
	const FVector Origin = Feet - Right * (Left * S) + FVector(0, 0, (F.Picture->TopOffset - F.Picture->Height) * S);
	SetWorldLocationAndRotation(Origin, Rot);
	SetWorldScale3D(FVector(1, F.Picture->Width * S, F.Picture->Height * S));
	bDirty = false;
}
