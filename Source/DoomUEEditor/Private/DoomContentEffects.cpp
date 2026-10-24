// The content builder's Niagara bursts (sparks, blood, explosions, plasma, BFG) and -dumpniagara.
#include "DoomContentBuilder.h"
#include "DoomContentPrivate.h"
#include "UObject/Linker.h"
#include "Misc/FeedbackContext.h"
#include "DoomGameData.h"
#include "DoomMapTiles.h"
#include "DoomAssets.h"
#include "DoomUE.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Engine/Texture2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "DoomMapActors.h"
#include "DoomMapCheck.h"
#include "DoomMapGen.h"
#include "DoomWadWriter.h"
#include "DoomAI.h"
#include "DoomWad.h"
#include "DoomSettings.h"
#include "DoomInput.h"
#include "HAL/FileManager.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomWorld.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "DoomLevel.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "DoomWad.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "NiagaraEmitterHandle.h"

// ---- Niagara effects ---------------------------------------------------------------------------
// Each effect is a copy of the engine's "Directional Burst (Lightweight)" template: a stateless
// emitter whose modules are plain properties, set here as text through reflection (their
// headers are internal to the Niagara plugin). PostEditChangeProperty refreshes the derived
// runtime values the same way the details panel does.
namespace
{
	const TCHAR* BurstTemplate = TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurstLightweight.DirectionalBurstLightweight");

	struct FBurstSpec
	{
		EDoomParticles Kind;
		int32 Count;
		FVector2f Life;            // seconds
		FLinearColor ColorA, ColorB; // HDR: values above 1 bloom in the modern renderer
		FVector2f SizeMin, SizeMax;  // sprite width / length (velocity aligned)
		FVector2f Speed;           // Unreal units per second
		float ConeAngle;           // degrees around the emitter's Z axis (360: all directions)
		float Gravity;
		bool bVelocityAligned;
		float Bounds;
	};

	bool SetText(UObject* Obj, const TCHAR* Name, const FString& Value)
	{
		FProperty* P = Obj ? FindFProperty<FProperty>(Obj->GetClass(), Name) : nullptr;
		if (!P)
		{
			UE_LOG(LogDoomContent, Error, TEXT("Effect: no property %s on %s"), Name, Obj ? *Obj->GetClass()->GetName() : TEXT("null"));
			return false;
		}
		Obj->PreEditChange(P);
		if (!P->ImportText_Direct(*Value, P->ContainerPtrToValuePtr<void>(Obj), Obj, PPF_None))
		{
			UE_LOG(LogDoomContent, Error, TEXT("Effect: could not set %s.%s = %s"), *Obj->GetClass()->GetName(), Name, *Value);
			return false;
		}
		FPropertyChangedEvent Event(P, EPropertyChangeType::ValueSet);
		Obj->PostEditChangeProperty(Event);
		return true;
	}

	UObject* Module(UObject* Emitter, const TCHAR* ClassSuffix)
	{
		FArrayProperty* Arr = FindFProperty<FArrayProperty>(Emitter->GetClass(), TEXT("Modules"));
		FObjectPropertyBase* Inner = Arr ? CastField<FObjectPropertyBase>(Arr->Inner) : nullptr;
		if (!Inner)
		{
			return nullptr;
		}
		FScriptArrayHelper H(Arr, Arr->ContainerPtrToValuePtr<void>(Emitter));
		for (int32 i = 0; i < H.Num(); ++i)
		{
			UObject* M = Inner->GetObjectPropertyValue(H.GetRawPtr(i));
			if (M && M->GetClass()->GetName().EndsWith(ClassSuffix))
			{
				return M;
			}
		}
		UE_LOG(LogDoomContent, Error, TEXT("Effect: no module %s"), ClassSuffix);
		return nullptr;
	}

	UObject* Renderer(UObject* Emitter)
	{
		FArrayProperty* Arr = FindFProperty<FArrayProperty>(Emitter->GetClass(), TEXT("RendererProperties"));
		FObjectPropertyBase* Inner = Arr ? CastField<FObjectPropertyBase>(Arr->Inner) : nullptr;
		if (!Inner)
		{
			return nullptr;
		}
		FScriptArrayHelper H(Arr, Arr->ContainerPtrToValuePtr<void>(Emitter));
		return H.Num() > 0 ? Inner->GetObjectPropertyValue(H.GetRawPtr(0)) : nullptr;
	}

	// Distributions keep their runtime values in ChannelConstantsAndRanges (minimum channels, then
	// maximum channels); PostEditChangeProperty derives Min / Max from it, so both are written.
	FString Range(FVector2f R)
	{
		return FString::Printf(TEXT("(Min=%f,Max=%f,Mode=UniformRange,ChannelConstantsAndRanges=(%f,%f))"), R.X, R.Y, R.X, R.Y);
	}

	FString Color(const FLinearColor& C)
	{
		return FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), C.R, C.G, C.B, C.A);
	}

	bool BuildBurst(const FBurstSpec& S)
	{
		UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, BurstTemplate);
		if (!Template)
		{
			UE_LOG(LogDoomContent, Error, TEXT("Effect: missing Niagara template %s"), BurstTemplate);
			return false;
		}
		const FString Path = DoomAssets::ParticlesPath(S.Kind);
		const FString Name = FPackageName::ObjectPathToObjectName(Path);
		UPackage* Pkg = DoomContent::MakePackage(Name, DoomContent::EffectsFolder);
		UNiagaraSystem* Sys = DuplicateObject<UNiagaraSystem>(Template, Pkg, FName(*Name));
		Sys->SetFlags(RF_Public | RF_Standalone);
		if (FindFProperty<FProperty>(Sys->GetClass(), TEXT("TemplateSpecification")))
		{
			SetText(Sys, TEXT("TemplateSpecification"), TEXT("None")); // a game asset, not a template
		}
		bool bOk = true;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			UObject* E = (UObject*)Handle.GetStatelessEmitter();
			if (!E)
			{
				UE_LOG(LogDoomContent, Error, TEXT("Effect: template emitter is not lightweight"));
				return false;
			}
			const float B = S.Bounds;
			bOk &= SetText(E, TEXT("FixedBounds"), FString::Printf(TEXT("(Min=(X=%f,Y=%f,Z=%f),Max=(X=%f,Y=%f,Z=%f),IsValid=True)"), -B, -B, -B, B, B, B));
			bOk &= SetText(E, TEXT("SpawnInfos"), FString::Printf(TEXT("((Amount=(Min=%d,Max=%d)))"), S.Count, S.Count));
			UObject* Init = Module(E, TEXT("InitializeParticle"));
			bOk &= SetText(Init, TEXT("LifetimeDistribution"), Range(S.Life));
			bOk &= SetText(Init, TEXT("ColorDistribution"), FString::Printf(TEXT("(Values=(%s,%s),Mode=NonUniformRange)"), *Color(S.ColorA), *Color(S.ColorB)));
			bOk &= SetText(Init, TEXT("SpriteSizeDistribution"), FString::Printf(TEXT("(Min=(X=%f,Y=%f),Max=(X=%f,Y=%f),Mode=NonUniformRange,ChannelConstantsAndRanges=(%f,%f,%f,%f))"),
				S.SizeMin.X, S.SizeMin.Y, S.SizeMax.X, S.SizeMax.Y, S.SizeMin.X, S.SizeMin.Y, S.SizeMax.X, S.SizeMax.Y));
			UObject* Vel = Module(E, TEXT("AddVelocity"));
			bOk &= SetText(Vel, TEXT("ConeVelocityDistribution"), Range(S.Speed));
			bOk &= SetText(Vel, TEXT("ConeAngle"), FString::SanitizeFloat(S.ConeAngle));
			// aim the cone along the emitter's +Z (the game turns the emitter to face the shooter)
			bOk &= SetText(Vel, TEXT("ConeRotationType"), TEXT("Direction"));
			bOk &= SetText(Vel, TEXT("ConeDirection"), TEXT("(Min=(X=0,Y=0,Z=1),Max=(X=0,Y=0,Z=1),Mode=NonUniformConstant,ChannelConstantsAndRanges=(0,0,1))"));
			UObject* Gravity = Module(E, TEXT("GravityForce"));
			bOk &= SetText(Gravity, TEXT("GravityDistribution"), FString::Printf(TEXT("(Min=(X=0,Y=0,Z=%f),Max=(X=0,Y=0,Z=%f),Mode=NonUniformConstant,ChannelConstantsAndRanges=(0,0,%f))"), S.Gravity, S.Gravity, S.Gravity));
			if (UObject* R = Renderer(E))
			{
				bOk &= SetText(R, TEXT("Alignment"), S.bVelocityAligned ? TEXT("VelocityAligned") : TEXT("Unaligned"));
				bOk &= SetText(R, TEXT("bCastShadows"), TEXT("False"));
			}
			E->PostEditChange();
		}
		Sys->RequestCompile(false);
		Sys->WaitForCompilationComplete();
		return bOk && DoomContent::Save(Sys);
	}
}

bool DoomContent::BuildEffects()
{
	constexpr float S = Doom::Scale;
	const FBurstSpec Specs[] = {
		// kind                      count life           colour from / to (HDR)                                       size min / max                     speed (u/s)               cone  gravity      aligned bounds
		{ EDoomParticles::Sparks,    10, { 0.15f, 0.4f }, { 6.f, 3.f, 0.8f, 1.f }, { 9.f, 5.5f, 1.5f, 1.f },             { 0.4f * S, 1.f * S }, { 0.7f * S, 3.f * S },   { 120.f * S, 400.f * S }, 70.f, -330.f * S, true,  150.f * S },
		{ EDoomParticles::Blood,     14, { 0.4f, 0.8f },  { 0.25f, 0.f, 0.f, 1.f }, { 0.5f, 0.02f, 0.02f, 1.f },          { 1.f * S, 1.f * S }, { 2.5f * S, 2.5f * S },   { 50.f * S, 170.f * S },  60.f, -650.f * S, false, 150.f * S },
		{ EDoomParticles::Explosion, 40, { 0.4f, 1.0f },  { 8.f, 3.f, 0.5f, 1.f }, { 12.f, 6.f, 1.5f, 1.f },             { 0.7f * S, 2.f * S }, { 1.3f * S, 5.f * S },   { 130.f * S, 500.f * S }, 360.f, -330.f * S, true,  400.f * S },
		{ EDoomParticles::Plasma,    24, { 0.25f, 0.6f }, { 0.8f, 3.f, 12.f, 1.f }, { 1.5f, 6.f, 16.f, 1.f },            { 0.5f * S, 1.5f * S }, { 1.f * S, 4.f * S },   { 100.f * S, 350.f * S }, 360.f, 0.f,        true,  250.f * S },
		{ EDoomParticles::BFG,       60, { 0.5f, 1.2f },  { 1.f, 10.f, 1.f, 1.f }, { 3.f, 14.f, 3.f, 1.f },              { 0.7f * S, 2.f * S }, { 1.5f * S, 6.f * S },   { 200.f * S, 700.f * S }, 360.f, -100.f * S, true,  600.f * S },
	};
	bool bOk = true;
	for (const FBurstSpec& Spec : Specs)
	{
		bOk &= BuildBurst(Spec);
	}
	return bOk;
}

bool DoomContent::EffectsExist()
{
	for (int32 k = 0; k < (int32)EDoomParticles::Count; ++k)
	{
		if (!LoadObject<UNiagaraSystem>(nullptr, DoomAssets::ParticlesPath((EDoomParticles)k)))
		{
			return false;
		}
	}
	return true;
}

namespace
{
	void DumpObject(UObject* Obj, int32 Depth)
	{
		if (!Obj || Depth > 3)
		{
			return;
		}
		const FString Pad = FString::ChrN(Depth * 2, ' ');
		UE_LOG(LogDoomContent, Display, TEXT("%s%s (%s)"), *Pad, *Obj->GetName(), *Obj->GetClass()->GetName());
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FString Value;
			It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Obj), nullptr, Obj, PPF_None);
			UE_LOG(LogDoomContent, Display, TEXT("%s  .%s = %s"), *Pad, *It->GetName(), *Value.Left(600));
			if (FArrayProperty* Arr = CastField<FArrayProperty>(*It))
			{
				if (FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Arr->Inner))
				{
					FScriptArrayHelper H(Arr, Arr->ContainerPtrToValuePtr<void>(Obj));
					for (int32 i = 0; i < H.Num(); ++i)
					{
						DumpObject(Inner->GetObjectPropertyValue(H.GetRawPtr(i)), Depth + 1);
					}
				}
			}
		}
	}
}

void DoomContent::DumpNiagara(const FString& Path)
{
	UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *Path);
	if (!Sys)
	{
		UE_LOG(LogDoomContent, Error, TEXT("No Niagara system %s"), *Path);
		return;
	}
	for (const FNiagaraEmitterHandle& H : Sys->GetEmitterHandles())
	{
		UE_LOG(LogDoomContent, Display, TEXT("Handle %s mode %d"), *H.GetName().ToString(), (int32)H.GetEmitterMode());
		DumpObject((UObject*)H.GetStatelessEmitter(), 1);
	}
}
