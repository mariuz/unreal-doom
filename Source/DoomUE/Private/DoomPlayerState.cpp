#include "DoomPlayerState.h"
#include "Net/UnrealNetwork.h"

void FDoomInventory::Serialize(FArchive& Ar)
{
	Ar << Health << Armor << ArmorClass << bBackpack;
	for (int32& A : Ammo) Ar << A;
	for (bool& W : Weapons) Ar << W;
	for (bool& K : Keys) Ar << K;
	uint8 R = (uint8)Ready;
	Ar << R;
	if (Ar.IsLoading())
	{
		Ready = (EDoomWeapon)FMath::Min<uint8>(R, (uint8)EDoomWeapon::Count - 1);
	}
}

void ADoomPlayerState::DoomSerialize(FArchive& Ar)
{
	Inv.Serialize(Ar);
	Ar << LevelKills << LevelItems << LevelSecrets << Frags;
}

void ADoomPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomPlayerState, Inv);
	DOREPLIFETIME(ADoomPlayerState, LevelKills);
	DOREPLIFETIME(ADoomPlayerState, LevelItems);
	DOREPLIFETIME(ADoomPlayerState, LevelSecrets);
	DOREPLIFETIME(ADoomPlayerState, Frags);
}

void ADoomPlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);
	if (ADoomPlayerState* To = Cast<ADoomPlayerState>(PlayerState))
	{
		To->Inv = Inv;
		To->Frags = Frags;
	}
}
