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

uint16 FDoomInventory::WeaponMask() const
{
	uint16 M = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Weapons); ++i) if (Weapons[i]) M |= 1 << i;
	return M;
}

uint8 FDoomInventory::KeyMask() const
{
	uint8 M = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Keys); ++i) if (Keys[i]) M |= 1 << i;
	return M;
}

bool FDoomInventory::operator==(const FDoomInventory& O) const
{
	return Health == O.Health && Armor == O.Armor && ArmorClass == O.ArmorClass && bBackpack == O.bBackpack && Ready == O.Ready
		&& FMemory::Memcmp(Ammo, O.Ammo, sizeof(Ammo)) == 0 && WeaponMask() == O.WeaponMask() && KeyMask() == O.KeyMask();
}

bool FDoomInventory::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	static_assert(UE_ARRAY_COUNT(Weapons) <= 16, "the weapon mask is 16 bits");
	static_assert(UE_ARRAY_COUNT(Keys) <= 8, "the key mask is 8 bits");
	static_assert((int32)EDoomWeapon::Count <= 255, "the ready weapon is sent as a byte");
	uint16 W = WeaponMask();
	uint8 K = KeyMask();
	uint8 R = (uint8)Ready;
	uint8 Flags = bBackpack ? 1 : 0;
	Ar << Health << Armor << ArmorClass << W << K << R << Flags;
	for (int32& A : Ammo) Ar << A;
	if (Ar.IsLoading())
	{
		for (int32 i = 0; i < UE_ARRAY_COUNT(Weapons); ++i) Weapons[i] = (W & (1 << i)) != 0;
		for (int32 i = 0; i < UE_ARRAY_COUNT(Keys); ++i) Keys[i] = (K & (1 << i)) != 0;
		Ready = (EDoomWeapon)FMath::Min<uint8>(R, (uint8)EDoomWeapon::Count - 1);
		bBackpack = (Flags & 1) != 0;
	}
	bOutSuccess = true;
	return true;
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
	DOREPLIFETIME(ADoomPlayerState, PlayerNumber);
}

void ADoomPlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);
	if (ADoomPlayerState* To = Cast<ADoomPlayerState>(PlayerState))
	{
		To->Inv = Inv;
		To->Frags = Frags;
		To->PlayerNumber = PlayerNumber;
	}
}
