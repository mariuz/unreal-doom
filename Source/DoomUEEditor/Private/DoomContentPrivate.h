// What the content builder's files share: its log category, the asset folders, and making and
// saving asset packages. In namespace DoomContent (not an anonymous one), so a unity build doesn't
// spread these names to other files.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/Linker.h"
#include "Misc/PackageName.h"
#include "Misc/FeedbackContext.h"
#include "AssetRegistry/AssetRegistryModule.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDoomContent, Log, All);

namespace DoomContent
{
	inline const TCHAR* const Folder = TEXT("/Game/Doom/Materials/");
	inline const TCHAR* const EffectsFolder = TEXT("/Game/Doom/Effects/");

	inline UPackage* MakePackage(const FString& Name, const TCHAR* InFolder = Folder)
	{
		const FString PkgName = FString(InFolder) + Name;
		// Regenerating: load what is on disk, move the old asset out of the way and let go of the file.
		UPackage* Pkg = FindPackage(nullptr, *PkgName);
		if (!Pkg && FPackageName::DoesPackageExist(PkgName))
		{
			Pkg = LoadPackage(nullptr, *PkgName, LOAD_None);
		}
		if (Pkg)
		{
			if (UObject* Old = FindObject<UObject>(Pkg, *Name))
			{
				Old->ClearFlags(RF_Public | RF_Standalone);
				Old->Rename(*MakeUniqueObjectName(GetTransientPackage(), Old->GetClass(), FName(*(Name + TEXT("_Old")))).ToString(),
					GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			}
			ResetLoaders(Pkg);
			Pkg->FullyLoad();
			return Pkg;
		}
		return CreatePackage(*PkgName);
	}

	inline bool Save(UObject* Asset)
	{
		UPackage* Pkg = Asset->GetOutermost();
		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Asset);
		const FString File = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.Error = GWarn;
		const bool bOk = UPackage::SavePackage(Pkg, Asset, *File, Args);
		UE_LOG(LogDoomContent, Log, TEXT("%s %s"), bOk ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bOk;
	}

	/** The Niagara bursts (DoomContentEffects.cpp). */
	bool BuildEffects();
	bool EffectsExist();
}
