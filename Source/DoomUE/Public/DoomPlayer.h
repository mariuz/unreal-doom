// The marine: an ACharacter (capsule 16 x 56 units, eye at 41, 24-unit steps,
// Doom's heavy gravity and quick acceleration) driven by Enhanced Input, with
// Doom's inventory, weapons state machine (psprites), damage and powerups.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "DoomInfo.h"
#include "DoomPlayer.generated.h"

class UCameraComponent;
class UPointLightComponent;
class UDoomWorld;
class UDoomInputConfig;

USTRUCT()
struct FDoomInventory
{
	GENERATED_BODY()

	int32 Health = 100;
	int32 Armor = 0;
	int32 ArmorClass = 0;
	int32 Ammo[4] = { 50, 0, 0, 0 };
	bool bBackpack = false;
	bool Weapons[(int32)EDoomWeapon::Count] = { true, true, false, false, false, false, false, false, false };
	EDoomWeapon Ready = EDoomWeapon::Pistol;
	bool Keys[6] = {}; // blue card, yellow card, red card, blue skull, yellow skull, red skull

	void Reset() { *this = FDoomInventory(); }
};

UCLASS()
class DOOMUE_API ADoomPlayer : public ACharacter
{
	GENERATED_BODY()

public:
	ADoomPlayer(const FObjectInitializer& ObjectInitializer);

	void InitForLevel(UDoomWorld* InWorld, bool bPistolStart);
	void DoomTic();
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual float TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** Returns true when the item was taken. */
	bool TryPickup(const FDoomThingDef& Def, bool bDropped);

	/** Save games: position, view, velocity, inventory and powerups. */
	void DoomSerialize(FArchive& Ar);

	FVector2f DoomPos() const;
	float DoomZ() const;
	float DoomAngle() const;
	FVector EyeLocation() const;
	bool IsDead() const { return Inv.Health <= 0; }
	bool HasKey(int32 KeyColor) const; // 0 blue 1 yellow 2 red: card or skull

	FDoomInventory Inv;
	// powerups, in tics remaining
	int32 Invulnerability = 0, Invisibility = 0, RadSuit = 0, LightAmp = 0;
	bool bBerserk = false;
	int32 DamageCount = 0;   // red tint
	int32 BonusCount = 0;    // gold tint
	int32 ExtraLight = 0;    // gun flash
	int32 AttackDown = 0;
	int32 FaceDamageDir = 0; // -1 left, 1 right
	int32 FaceEvilGrin = 0;
	bool bGodMode = false;

	// psprites (Doom 320x200 coordinates)
	FString WeaponSprite;
	TCHAR WeaponFrame = 'A';
	FString FlashSprite;
	TCHAR FlashFrame = 0;     // 0 = no flash
	float WeaponY = 128.f;    // 32 = up, 128 = down
	float BobX = 0, BobY = 0;
	EDoomWeapon PendingWeapon = EDoomWeapon::Count;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> MuzzleLight;

	// input handlers
	void Move(const FInputActionValue& V);
	void Look(const FInputActionValue& V);
	void FirePressed(const FInputActionValue& V) { bFireHeld = true; }
	void FireReleased(const FInputActionValue& V) { bFireHeld = false; }
	void UsePressed(const FInputActionValue& V);
	void RunPressed(const FInputActionValue& V) { bRunHeld = true; }
	void RunReleased(const FInputActionValue& V) { bRunHeld = false; }
	void SelectSlot(int32 Slot);
	void CycleWeapon(int32 Dir);
	void ToggleAutomap(const FInputActionValue& V);

private:
	enum class EWeaponPhase : uint8 { Raising, Ready, Firing, Lowering };
	void TicWeapon();
	void StartFire();
	void FireWeapon(int32 FrameIndex);
	bool CheckAmmo();
	void BringUpWeapon();
	void SetWeaponSprite();
	void HitscanAttack(int32 Pellets, float SpreadYaw, float SpreadPitch, int32 DamageMul, int32 DamageDie, bool bAccurate);
	void MeleeAttack(bool bSaw);
	float AimSlope(float DoomAngleDeg, float Range, AActor*& OutTarget) const;
	void TicPowerups();
	void TicDeath();

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;

	EWeaponPhase Phase = EWeaponPhase::Raising;
	int32 FireIndex = 0;
	int32 FrameTics = 0;
	int32 FlashTics = 0;
	int32 FlashIndex = 0;
	bool bFireHeld = false;
	bool bRunHeld = false;
	bool bRefire = false;
	FVector2f LastDoomPos;
	float BobPhase = 0;
	float ViewHeightOffset = 0;
	int32 DeathTics = 0;
	int32 SawIdle = 0;
	int32 ReactionTime = 0;
	float LastLandingVelocity = 0;
};
