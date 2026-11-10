#include "DoomAssets.h"
#include "MaterialDomain.h"
#include "DoomUE.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "NiagaraSystem.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

namespace
{
	template <typename T>
	T* LoadOrWarn(const TCHAR* Path)
	{
		T* Obj = LoadObject<T>(nullptr, Path);
		if (!Obj)
		{
			UE_LOG(LogDoomRender, Error, TEXT("Missing asset %s: open the project in the editor once (it generates the Doom materials) or run -run=DoomContent"), Path);
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

UStaticMesh* DoomAssets::SpriteQuad()
{
	static TObjectPtr<UStaticMesh> Quad; // rooted: shared by every sprite for the program's life
	if (Quad)
	{
		return Quad;
	}
	FMeshDescription Desc;
	FStaticMeshAttributes Attr(Desc);
	Attr.Register();
	// the corners and UVs the procedural quad had: the picture's top-left at (0, 0, 1)
	const FVector3f Corners[4] = { FVector3f(0, 0, 0), FVector3f(0, 0, 1), FVector3f(0, 1, 1), FVector3f(0, 1, 0) };
	const FVector2f UVs[4] = { FVector2f(0, 1), FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1) };
	const FPolygonGroupID Group = Desc.CreatePolygonGroup();
	Attr.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Sprite");
	FVertexInstanceID Inst[4];
	for (int32 i = 0; i < 4; ++i)
	{
		const FVertexID V = Desc.CreateVertex();
		Attr.GetVertexPositions()[V] = Corners[i];
		Inst[i] = Desc.CreateVertexInstance(V);
		Attr.GetVertexInstanceNormals()[Inst[i]] = FVector3f(-1, 0, 0);
		Attr.GetVertexInstanceTangents()[Inst[i]] = FVector3f(0, 1, 0);
		Attr.GetVertexInstanceBinormalSigns()[Inst[i]] = 1.f;
		Attr.GetVertexInstanceUVs().Set(Inst[i], 0, UVs[i]);
		Attr.GetVertexInstanceColors()[Inst[i]] = FVector4f(1, 1, 1, 1);
	}
	Desc.CreateTriangle(Group, { Inst[0], Inst[1], Inst[2] });
	Desc.CreateTriangle(Group, { Inst[0], Inst[2], Inst[3] });
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), TEXT("DoomSpriteQuad"), RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(SpriteMaterial(), TEXT("Sprite")));
	// the runtime build leaves the UV density unset (the editor's computes it), and the renderer
	// ensures on that in cooked games
	Mesh->GetStaticMaterials()[0].UVChannelData = FMeshUVChannelInfo(1.f);
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bBuildSimpleCollision = false;
	Params.bFastBuild = true; // the runtime path: no editor build, works in packaged games
	Mesh->BuildFromMeshDescriptions({ &Desc }, Params);
	Mesh->AddToRoot();
	Quad = Mesh;
	return Quad;
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
		UE_LOG(LogDoomRender, Warning, TEXT("Missing effect %s: run -run=DoomContent -force"), ParticlesPath(Kind));
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
