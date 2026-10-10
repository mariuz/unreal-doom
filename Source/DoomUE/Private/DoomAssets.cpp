#include "DoomAssets.h"
#include "DoomUE.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"

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
