// The few authored assets the port uses: materials and a material parameter
// collection, all generated in code by the DoomUEEditor module (see DoomContentBuilder).
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UMaterialParameterCollection;
class UNiagaraSystem;

/** Modern-renderer particle bursts (Niagara lightweight emitters, see DoomContentBuilder). */
enum class EDoomParticles : uint8
{
	Sparks,     // bullet puffs
	Blood,
	Explosion,  // rockets, barrels, fireballs
	Plasma,
	BFG,
	Count
};

class UStaticMesh;

namespace DoomAssets
{
	inline const TCHAR* WorldClassicPath = TEXT("/Game/Doom/Materials/M_DoomWorld.M_DoomWorld");
	inline const TCHAR* WorldModernPath = TEXT("/Game/Doom/Materials/M_DoomWorldLit.M_DoomWorldLit");
	inline const TCHAR* SkyPath = TEXT("/Game/Doom/Materials/M_DoomSky.M_DoomSky");
	inline const TCHAR* SpritePath = TEXT("/Game/Doom/Materials/M_DoomSprite.M_DoomSprite");
	inline const TCHAR* SpriteFuzzPath = TEXT("/Game/Doom/Materials/M_DoomSpriteFuzz.M_DoomSpriteFuzz");
	inline const TCHAR* CollectionPath = TEXT("/Game/Doom/Materials/MPC_Doom.MPC_Doom");

	DOOMUE_API UMaterialInterface* WorldMaterial(bool bModern);
	DOOMUE_API UMaterialInterface* SkyMaterial();
	DOOMUE_API UMaterialInterface* SpriteMaterial();
	DOOMUE_API UMaterialInterface* SpriteFuzzMaterial();
	/** The unit quad every sprite draws (UDoomSpriteComponent): Y right 0..1, Z up 0..1, facing -X,
	 *  built once at runtime from a mesh description (it needs no asset, so packaged builds have it too). */
	DOOMUE_API UStaticMesh* SpriteQuad();
	DOOMUE_API UMaterialParameterCollection* Collection();
	DOOMUE_API const TCHAR* ParticlesPath(EDoomParticles Kind);
	DOOMUE_API UNiagaraSystem* Particles(EDoomParticles Kind);
}
