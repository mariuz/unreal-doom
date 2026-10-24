#include "DoomAssets.h"
#include "MaterialDomain.h"
#include "DoomUE.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "NiagaraSystem.h"

namespace
{
	template <typename T>
	T* LoadOrWarn(const TCHAR* Path)
	{
		T* Obj = LoadObject<T>(nullptr, Path);
		if (!Obj)
		{
			UE_LOG(LogDoom, Error, TEXT("Missing asset %s: open the project in the editor once (it generates the Doom materials) or run -run=DoomContent"), Path);
		}
		return Obj;
	}
}

UMaterialInterface* DoomAssets::WorldMaterial(bool bModern)
{
	UMaterialInterface* M = LoadOrWarn<UMaterialInterface>(bModern ? WorldModernPath : WorldClassicPath);
	return M ? M : UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* DoomAssets::SkyMaterial()
{
	UMaterialInterface* M = LoadOrWarn<UMaterialInterface>(SkyPath);
	return M ? M : UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* DoomAssets::SpriteMaterial()
{
	UMaterialInterface* M = LoadOrWarn<UMaterialInterface>(SpritePath);
	return M ? M : UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* DoomAssets::SpriteFuzzMaterial()
{
	UMaterialInterface* M = LoadOrWarn<UMaterialInterface>(SpriteFuzzPath);
	return M ? M : SpriteMaterial();
}

UMaterialParameterCollection* DoomAssets::Collection()
{
	return LoadOrWarn<UMaterialParameterCollection>(CollectionPath);
}

UNiagaraSystem* DoomAssets::Particles(EDoomParticles Kind)
{
	// a missing effect (content generated before effects existed) is reported once, then skipped
	static bool Missing[(int32)EDoomParticles::Count] = {};
	if (Missing[(int32)Kind])
	{
		return nullptr;
	}
	UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, ParticlesPath(Kind));
	if (!Sys)
	{
		Missing[(int32)Kind] = true;
		UE_LOG(LogDoom, Warning, TEXT("Missing effect %s: run -run=DoomContent -force"), ParticlesPath(Kind));
	}
	return Sys;
}

const TCHAR* DoomAssets::ParticlesPath(EDoomParticles Kind)
{
	switch (Kind)
	{
	case EDoomParticles::Blood:     return TEXT("/Game/Doom/Effects/NS_DoomBlood.NS_DoomBlood");
	case EDoomParticles::Explosion: return TEXT("/Game/Doom/Effects/NS_DoomExplosion.NS_DoomExplosion");
	case EDoomParticles::Plasma:    return TEXT("/Game/Doom/Effects/NS_DoomPlasma.NS_DoomPlasma");
	case EDoomParticles::BFG:       return TEXT("/Game/Doom/Effects/NS_DoomBFG.NS_DoomBFG");
	default:                        return TEXT("/Game/Doom/Effects/NS_DoomSparks.NS_DoomSparks");
	}
}
