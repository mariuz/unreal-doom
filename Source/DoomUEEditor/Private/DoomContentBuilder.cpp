#include "DoomContentBuilder.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogDoomContent, Log, All);

namespace
{
	const TCHAR* Folder = TEXT("/Game/Doom/Materials/");

	// ---- Doom's light diminishing (r_main.c's scalelight / zlight tables) in HLSL ------------------
	// Inputs: Light (0..255 incl. fake contrast), Kind (0 wall/sprite, 0.5 plane, 1 full bright),
	// Depth (view depth, Unreal units), Extra (gun flash), Fixed (fixed colormap or -1), Scale.
	// Output: the COLORMAP row 0 (bright) .. 31 (dark), or -1 for full bright.
	const TCHAR* DoomLevelHLSL = TEXT(R"(
float doomLevel = -1.0;
if (Kind < 0.75)
{
	float lightnum = clamp(floor(Light / 16.0) + Extra, 0.0, 15.0);
	float startmap = (15.0 - lightnum) * 4.0;
	float dist = max(Depth / Scale, 1.0);
	if (Kind > 0.25)
	{
		float idx = min(floor(dist / 16.0), 127.0);          // zlight: distance
		doomLevel = startmap - floor(floor(160.0 / (idx + 1.0)) / 2.0);
	}
	else
	{
		float idx = min(floor(2560.0 / dist), 47.0);         // scalelight: projected scale
		doomLevel = startmap - floor(idx / 2.0);
	}
	doomLevel = clamp(doomLevel, 0.0, 31.0);
}
if (Fixed >= 0.0 && Fixed < 31.5) doomLevel = Fixed;
)");

	// Colour through the chosen colormap row: Doom's rows darken in palette (gamma) space.
	const TCHAR* DoomShadeHLSL = TEXT(R"(
float3 outc = c;
if (Fixed > 31.5)
{
	float g = 1.0 - dot(pow(saturate(c), 1.0 / 2.2), float3(0.299, 0.587, 0.114));
	outc = pow(saturate(g), 2.2).xxx;    // invulnerability: inverted greys
}
else if (doomLevel >= 0.0)
{
	outc = c * pow((32.0 - doomLevel) / 32.0, 2.2);
}
)");

	UPackage* MakePackage(const FString& Name, const TCHAR* InFolder = Folder)
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

	bool Save(UObject* Asset)
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

	template <typename T>
	T* Expr(UMaterial* M, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(M, T::StaticClass(), X, Y));
	}

	UMaterialExpressionCustom* Custom(UMaterial* M, int32 X, int32 Y, const FString& Desc, const FString& Code, ECustomMaterialOutputType Out, std::initializer_list<const TCHAR*> InputNames)
	{
		UMaterialExpressionCustom* C = Expr<UMaterialExpressionCustom>(M, X, Y);
		C->Description = Desc;
		C->Code = Code;
		C->OutputType = Out;
		C->Inputs.Reset();
		for (const TCHAR* N : InputNames)
		{
			FCustomInput In;
			In.InputName = FName(N);
			C->Inputs.Add(In);
		}
		return C;
	}

	void WireIndex(UMaterialExpression* From, int32 OutIndex, UMaterialExpressionCustom* To, const TCHAR* InputName)
	{
		for (FCustomInput& In : To->Inputs)
		{
			if (In.InputName == FName(InputName))
			{
				In.Input.Connect(OutIndex, From);
				return;
			}
		}
		UE_LOG(LogDoomContent, Error, TEXT("No input %s on %s"), InputName, *To->Description);
	}

	/** Connects a named output pin ("" = the first) to a Custom node input. */
	void Wire(UMaterialExpression* From, const TCHAR* FromOutput, UMaterialExpressionCustom* To, const TCHAR* InputName)
	{
		int32 OutIndex = INDEX_NONE;
		const TArray<FExpressionOutput>& Outs = From->GetOutputs();
		for (int32 i = 0; i < Outs.Num(); ++i)
		{
			if (Outs[i].OutputName == FName(FromOutput))
			{
				OutIndex = i;
				break;
			}
		}
		if (OutIndex == INDEX_NONE)
		{
			UE_LOG(LogDoomContent, Error, TEXT("No output '%s' on %s"), FromOutput, *From->GetName());
			OutIndex = 0;
		}
		WireIndex(From, OutIndex, To, InputName);
	}

	UMaterialExpressionScalarParameter* Scalar(UMaterial* M, int32 X, int32 Y, const TCHAR* Name, float Default)
	{
		UMaterialExpressionScalarParameter* P = Expr<UMaterialExpressionScalarParameter>(M, X, Y);
		P->ParameterName = Name;
		P->DefaultValue = Default;
		return P;
	}

	UMaterialExpressionCollectionParameter* Global(UMaterial* M, UMaterialParameterCollection* MPC, int32 X, int32 Y, const TCHAR* Name)
	{
		UMaterialExpressionCollectionParameter* P = Expr<UMaterialExpressionCollectionParameter>(M, X, Y);
		P->Collection = MPC;
		P->ParameterName = Name;
		P->ParameterId = MPC->GetParameterId(Name);
		return P;
	}

	UMaterialExpressionTextureSampleParameter2D* TexParam(UMaterial* M, int32 X, int32 Y, const TCHAR* Name, UTexture* Default, EMaterialSamplerType Type)
	{
		UMaterialExpressionTextureSampleParameter2D* T = Expr<UMaterialExpressionTextureSampleParameter2D>(M, X, Y);
		T->ParameterName = Name;
		T->Texture = Default;
		T->SamplerType = Type;
		return T;
	}

	UMaterial* NewMaterial(const FString& Name)
	{
		UPackage* Pkg = MakePackage(Name);
		UMaterial* M = NewObject<UMaterial>(Pkg, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		M->TwoSided = true;
		return M;
	}

	UTexture2D* LinearDefault()
	{
		const FString Name = TEXT("T_DoomLinearWhite");
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *(FString(Folder) + Name + TEXT(".") + Name)))
		{
			return Existing;
		}
		UPackage* Pkg = MakePackage(Name);
		UTexture2D* T = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		TArray<FColor> Px;
		Px.Init(FColor(160, 160, 160, 255), 16);
		T->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Px.GetData()));
		T->SRGB = false;
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->Filter = TF_Nearest;
		T->MipGenSettings = TMGS_NoMipmaps;
		T->PostEditChange();
		Save(T);
		return T;
	}

	/** Light texture lookup shared by world and sprite materials: returns the sample expression. */
	UMaterialExpressionTextureSampleParameter2D* SectorLightLookup(UMaterial* M, UMaterialExpression* SectorSource, const TCHAR* SectorOutput, UTexture* LinearTex, int32 X, int32 Y)
	{
		UMaterialExpressionScalarParameter* Rows = Scalar(M, X - 400, Y + 120, TEXT("SectorLightRows"), 1.f);
		UMaterialExpressionCustom* UV = Custom(M, X - 200, Y, TEXT("SectorUV"), TEXT(
			"float s = floor(Sector + 0.5);\n"
			"return float2((fmod(s, 256.0) + 0.5) / 256.0, (floor(s / 256.0) + 0.5) / max(Rows, 1.0));"),
			CMOT_Float2, { TEXT("Sector"), TEXT("Rows") });
		Wire(SectorSource, SectorOutput, UV, TEXT("Sector"));
		Wire(Rows, TEXT(""), UV, TEXT("Rows"));
		UMaterialExpressionTextureSampleParameter2D* L = TexParam(M, X, Y, TEXT("SectorLights"), LinearTex, SAMPLERTYPE_LinearColor);
		L->Coordinates.Connect(0, UV);
		return L;
	}

	/**
	 * Palette-exact lighting: the texel's palette index (TexIndex) and the colormap row (Level,
	 * 0 bright .. 31 dark, 32 = the invulnerability greys) pick the colour from the WAD's
	 * COLORMAP rows run through PLAYPAL (the Colormap texture, 256 x 34). With PaletteExact off
	 * the smooth approximation (Approx) is used instead.
	 */
	UMaterialExpression* PaletteExact(UMaterial* M, UMaterialParameterCollection* MPC, UMaterialExpression* UV, UMaterialExpression* Level,
		UMaterialExpression* Approx, UTexture* ColorDefault, UTexture* LinearDefaultTex, int32 X, int32 Y, bool bMipZero = false)
	{
		UMaterialExpressionTextureSampleParameter2D* Index = TexParam(M, X - 600, Y, TEXT("TexIndex"), LinearDefaultTex, SAMPLERTYPE_LinearColor);
		Index->Coordinates.Connect(0, UV);
		if (bMipZero)
		{
			// the sky: no derivative-based mips (its u jumps where atan2 wraps)
			Index->MipValueMode = TMVM_MipLevel;
			UMaterialExpressionConstant* Mip0 = Expr<UMaterialExpressionConstant>(M, X - 800, Y + 100);
			Mip0->R = 0.f;
			Index->MipValue.Connect(0, Mip0);
		}
		// rows: 34 per PLAYPAL palette; Palette picks the screen palette (damage red, pickup gold...)
		UMaterialExpressionCollectionParameter* Pal = Global(M, MPC, X - 600, Y + 200, TEXT("Palette"));
		UMaterialExpressionCollectionParameter* Rows = Global(M, MPC, X - 600, Y + 280, TEXT("PaletteRows"));
		UMaterialExpressionCustom* CmapUV = Custom(M, X - 400, Y, TEXT("ColormapUV"), TEXT(
			"float i = floor(Index * 255.0 + 0.5);\n"
			"float row = floor(Palette + 0.5) * 34.0 + clamp(Level, 0.0, 33.0);\n"
			"return float2((i + 0.5) / 256.0, (row + 0.5) / max(Rows, 34.0));"),
			CMOT_Float2, { TEXT("Index"), TEXT("Level"), TEXT("Palette"), TEXT("Rows") });
		Wire(Index, TEXT("R"), CmapUV, TEXT("Index"));
		WireIndex(Level, 0, CmapUV, TEXT("Level"));
		Wire(Pal, TEXT(""), CmapUV, TEXT("Palette"));
		Wire(Rows, TEXT(""), CmapUV, TEXT("Rows"));
		UMaterialExpressionTextureSampleParameter2D* Cmap = TexParam(M, X - 200, Y, TEXT("Colormap"), ColorDefault, SAMPLERTYPE_Color);
		Cmap->Coordinates.Connect(0, CmapUV);
		Cmap->MipValueMode = TMVM_MipLevel;
		UMaterialExpressionConstant* Zero = Expr<UMaterialExpressionConstant>(M, X - 400, Y + 120);
		Zero->R = 0.f;
		Cmap->MipValue.Connect(0, Zero);
		UMaterialExpressionCollectionParameter* Flag = Global(M, MPC, X - 200, Y + 200, TEXT("PaletteExact"));
		UMaterialExpressionCustom* Final = Custom(M, X, Y, TEXT("PaletteSelect"), TEXT("return Flag > 0.5 ? Exact : Approx;"),
			CMOT_Float3, { TEXT("Exact"), TEXT("Approx"), TEXT("Flag") });
		Wire(Cmap, TEXT("RGB"), Final, TEXT("Exact"));
		WireIndex(Approx, 0, Final, TEXT("Approx"));
		Wire(Flag, TEXT(""), Final, TEXT("Flag"));
		return Final;
	}

	UMaterial* BuildWorld(UMaterialParameterCollection* MPC, UTexture* ColorDefault, UTexture* LinearDefaultTex, bool bModern)
	{
		UMaterial* M = NewMaterial(bModern ? TEXT("M_DoomWorldLit") : TEXT("M_DoomWorld"));
		M->SetShadingModel(bModern ? MSM_DefaultLit : MSM_Unlit);
		M->BlendMode = BLEND_Masked;
		M->OpacityMaskClipValue = 0.5f;

		UMaterialExpressionTextureCoordinate* UV0 = Expr<UMaterialExpressionTextureCoordinate>(M, -1400, -200);
		UMaterialExpressionTextureCoordinate* UV1 = Expr<UMaterialExpressionTextureCoordinate>(M, -1400, 200);
		UV1->CoordinateIndex = 1;
		UMaterialExpressionScalarParameter* ScrollU = Scalar(M, -1400, -80, TEXT("ScrollU"), 0.f);
		UMaterialExpressionCollectionParameter* Tic = Global(M, MPC, -1400, 0, TEXT("Tic"));

		// scrolling walls (linedef special 48): one texel per tic
		UMaterialExpressionCustom* Scroll = Custom(M, -1100, -200, TEXT("ScrollUV"), TEXT("return UV + float2(ScrollU * Tic, 0);"), CMOT_Float2,
			{ TEXT("UV"), TEXT("ScrollU"), TEXT("Tic") });
		Wire(UV0, TEXT(""), Scroll, TEXT("UV"));
		Wire(ScrollU, TEXT(""), Scroll, TEXT("ScrollU"));
		Wire(Tic, TEXT(""), Scroll, TEXT("Tic"));

		UMaterialExpressionTextureSampleParameter2D* Tex = TexParam(M, -800, -250, TEXT("Tex"), ColorDefault, SAMPLERTYPE_Color);
		Tex->Coordinates.Connect(0, Scroll);

		UMaterialExpressionComponentMask* SectorMask = Expr<UMaterialExpressionComponentMask>(M, -1150, 200);
		SectorMask->R = 1; SectorMask->G = 0; SectorMask->B = 0; SectorMask->A = 0;
		SectorMask->Input.Connect(0, UV1);
		UMaterialExpressionTextureSampleParameter2D* Light = SectorLightLookup(M, SectorMask, TEXT(""), LinearDefaultTex, -800, 150);

		UMaterialExpressionVertexColor* VC = Expr<UMaterialExpressionVertexColor>(M, -800, 400);
		UMaterialExpressionPixelDepth* Depth = Expr<UMaterialExpressionPixelDepth>(M, -800, 500);
		UMaterialExpressionCollectionParameter* Extra = Global(M, MPC, -800, 580, TEXT("ExtraLight"));
		UMaterialExpressionCollectionParameter* Fixed = Global(M, MPC, -800, 660, TEXT("FixedColormap"));
		UMaterialExpressionCollectionParameter* Scale = Global(M, MPC, -800, 740, TEXT("DoomScale"));

		FString Code;
		if (bModern)
		{
			// Lit: the sector light becomes emissive "ambient" that Lumen spreads; full bright stays bright.
			Code = TEXT(
				"float3 c = TexRGBA.rgb;\n"
				"float Light = LightTex * 255.0 + UV1.y;\n"
				"if (Fixed > 31.5) { float g = 1.0 - dot(pow(saturate(c), 1.0/2.2), float3(0.299,0.587,0.114)); return pow(saturate(g), 2.2).xxx; }\n"
				"if (Kind > 0.75) return c;\n"
				"float b = saturate((Light + Extra * 16.0) / 255.0);\n"
				"return c * pow(b, 2.2) * 0.35;");
		}
		else
		{
			Code = FString(TEXT("float3 c = TexRGBA.rgb;\nfloat Light = LightTex * 255.0 + UV1.y;\n")) + DoomLevelHLSL + DoomShadeHLSL + TEXT("return outc;");
		}
		UMaterialExpressionCustom* Shade = Custom(M, -350, 0, TEXT("DoomLight"), Code, CMOT_Float3,
			{ TEXT("TexRGBA"), TEXT("LightTex"), TEXT("UV1"), TEXT("Kind"), TEXT("Depth"), TEXT("Extra"), TEXT("Fixed"), TEXT("Scale") });
		Wire(Tex, TEXT("RGBA"), Shade, TEXT("TexRGBA"));
		Wire(Light, TEXT("R"), Shade, TEXT("LightTex"));
		Wire(UV1, TEXT(""), Shade, TEXT("UV1"));
		WireIndex(VC, 1, Shade, TEXT("Kind")); // vertex colour pins are unnamed: 0 RGB, 1 R
		Wire(Depth, TEXT(""), Shade, TEXT("Depth"));
		Wire(Extra, TEXT(""), Shade, TEXT("Extra"));
		Wire(Fixed, TEXT(""), Shade, TEXT("Fixed"));
		Wire(Scale, TEXT(""), Shade, TEXT("Scale"));

		UMaterialExpression* Emissive = Shade;
		if (!bModern)
		{
			UMaterialExpressionCustom* Level = Custom(M, -350, 300, TEXT("DoomLevel"), FString(TEXT("float Light = LightTex * 255.0 + UV1.y;\n")) + DoomLevelHLSL
				+ TEXT("if (Fixed > 31.5) doomLevel = 32.0;\nreturn max(doomLevel, 0.0);"),
				CMOT_Float1, { TEXT("LightTex"), TEXT("UV1"), TEXT("Kind"), TEXT("Depth"), TEXT("Extra"), TEXT("Fixed"), TEXT("Scale") });
			Wire(Light, TEXT("R"), Level, TEXT("LightTex"));
			Wire(UV1, TEXT(""), Level, TEXT("UV1"));
			WireIndex(VC, 1, Level, TEXT("Kind"));
			Wire(Depth, TEXT(""), Level, TEXT("Depth"));
			Wire(Extra, TEXT(""), Level, TEXT("Extra"));
			Wire(Fixed, TEXT(""), Level, TEXT("Fixed"));
			Wire(Scale, TEXT(""), Level, TEXT("Scale"));
			Emissive = PaletteExact(M, MPC, Scroll, Level, Shade, ColorDefault, LinearDefaultTex, 0, 500);
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Emissive, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("A"), MP_OpacityMask);
		if (bModern)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Tex, TEXT("RGB"), MP_BaseColor);
			UMaterialExpressionConstant* Rough = Expr<UMaterialExpressionConstant>(M, -350, 300);
			Rough->R = 0.8f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
			UMaterialExpressionConstant* Spec = Expr<UMaterialExpressionConstant>(M, -350, 380);
			Spec->R = 0.3f;
			UMaterialEditingLibrary::ConnectMaterialProperty(Spec, TEXT(""), MP_Specular);
		}
		return M;
	}

	UMaterial* BuildSky(UMaterialParameterCollection* MPC, UTexture* ColorDefault, UTexture* LinearDefaultTex)
	{
		UMaterial* M = NewMaterial(TEXT("M_DoomSky"));
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Opaque;

		UMaterialExpressionScalarParameter* Height = Scalar(M, -900, 0, TEXT("SkyHeight"), 128.f);
		// Doom maps 1024 columns to a full turn (a 256-wide sky repeats 4 times) and puts
		// row R at slope (100 - R) / 160 from the eye; below row 0 is stretched, it wraps below.
		UMaterialExpressionCustom* UV = Custom(M, -650, 0, TEXT("SkyUV"), TEXT(
			"float3 d = -Parameters.CameraVector;\n"
			"float ang = atan2(-d.y, d.x);\n"
			"float u = -ang / (2.0 * 3.14159265) * 4.0;\n"
			"float slope = d.z / max(length(d.xy), 1e-4);\n"
			"float row = max(100.0 - 160.0 * slope, 0.0);\n"
			"return float2(u, row / max(SkyHeight, 1.0));"),
			CMOT_Float2, { TEXT("SkyHeight") });
		Wire(Height, TEXT(""), UV, TEXT("SkyHeight"));

		UMaterialExpressionTextureSampleParameter2D* Tex = TexParam(M, -400, 0, TEXT("Tex"), ColorDefault, SAMPLERTYPE_Color);
		Tex->Coordinates.Connect(0, UV);
		Tex->MipValueMode = TMVM_MipLevel; // no seam where atan2 wraps
		UMaterialExpressionConstant* Zero = Expr<UMaterialExpressionConstant>(M, -600, 150);
		Zero->R = 0.f;
		Tex->MipValue.Connect(0, Zero);

		UMaterialExpressionCollectionParameter* Fixed = Global(M, MPC, -400, 250, TEXT("FixedColormap"));
		UMaterialExpressionCustom* Out = Custom(M, -150, 0, TEXT("SkyColor"), TEXT(
			"float3 c = TexRGB;\n"
			"if (Fixed > 31.5) { float g = 1.0 - dot(pow(saturate(c), 1.0/2.2), float3(0.299,0.587,0.114)); return pow(saturate(g), 2.2).xxx; }\n"
			"return c;"), CMOT_Float3, { TEXT("TexRGB"), TEXT("Fixed") });
		Wire(Tex, TEXT("RGB"), Out, TEXT("TexRGB"));
		Wire(Fixed, TEXT(""), Out, TEXT("Fixed"));

		// classic renderer, exact colours: the sky's palette indices through COLORMAP row 0
		// (or 32 under invulnerability) of the current screen palette
		UMaterialExpressionCustom* SkyLevel = Custom(M, -150, 300, TEXT("SkyLevel"),
			TEXT("return Fixed > 31.5 ? 32.0 : 0.0;"), CMOT_Float1, { TEXT("Fixed") });
		Wire(Fixed, TEXT(""), SkyLevel, TEXT("Fixed"));
		UMaterialExpression* Exact = PaletteExact(M, MPC, UV, SkyLevel, Out, ColorDefault, LinearDefaultTex, 300, 300, true);
		UMaterialExpressionCollectionParameter* Modern = Global(M, MPC, 300, 600, TEXT("Modern"));
		UMaterialExpressionCustom* Final = Custom(M, 550, 100, TEXT("SkySelect"), TEXT("return Modern > 0.5 ? Lit : Classic;"),
			CMOT_Float3, { TEXT("Lit"), TEXT("Classic"), TEXT("Modern") });
		WireIndex(Out, 0, Final, TEXT("Lit"));
		WireIndex(Exact, 0, Final, TEXT("Classic"));
		Wire(Modern, TEXT(""), Final, TEXT("Modern"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Final, TEXT(""), MP_EmissiveColor);
		return M;
	}

	UMaterial* BuildSprite(UMaterialParameterCollection* MPC, UTexture* ColorDefault, UTexture* LinearDefaultTex)
	{
		UMaterial* M = NewMaterial(TEXT("M_DoomSprite"));
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Masked;
		M->OpacityMaskClipValue = 0.5f;

		UMaterialExpressionTextureCoordinate* UV0 = Expr<UMaterialExpressionTextureCoordinate>(M, -1300, -200);
		UMaterialExpressionScalarParameter* Mirror = Scalar(M, -1300, -100, TEXT("Mirror"), 0.f);
		UMaterialExpressionCustom* MirrorUV = Custom(M, -1050, -200, TEXT("MirrorUV"), TEXT("return float2(Mirror > 0.5 ? 1.0 - UV.x : UV.x, UV.y);"),
			CMOT_Float2, { TEXT("UV"), TEXT("Mirror") });
		Wire(UV0, TEXT(""), MirrorUV, TEXT("UV"));
		Wire(Mirror, TEXT(""), MirrorUV, TEXT("Mirror"));
		UMaterialExpressionTextureSampleParameter2D* Tex = TexParam(M, -800, -250, TEXT("Tex"), ColorDefault, SAMPLERTYPE_Color);
		Tex->Coordinates.Connect(0, MirrorUV);

		UMaterialExpressionScalarParameter* Sector = Scalar(M, -1300, 150, TEXT("Sector"), 0.f);
		UMaterialExpressionTextureSampleParameter2D* Light = SectorLightLookup(M, Sector, TEXT(""), LinearDefaultTex, -800, 150);

		UMaterialExpressionScalarParameter* Bright = Scalar(M, -800, 400, TEXT("FullBright"), 0.f);
		UMaterialExpressionScalarParameter* Fuzz = Scalar(M, -800, 480, TEXT("Fuzz"), 0.f);
		UMaterialExpressionPixelDepth* Depth = Expr<UMaterialExpressionPixelDepth>(M, -800, 560);
		UMaterialExpressionCollectionParameter* Extra = Global(M, MPC, -800, 640, TEXT("ExtraLight"));
		UMaterialExpressionCollectionParameter* Fixed = Global(M, MPC, -800, 720, TEXT("FixedColormap"));
		UMaterialExpressionCollectionParameter* Scale = Global(M, MPC, -800, 800, TEXT("DoomScale"));
		UMaterialExpressionCollectionParameter* Tic = Global(M, MPC, -800, 880, TEXT("Tic"));
		UMaterialExpressionCollectionParameter* Modern = Global(M, MPC, -800, 960, TEXT("Modern"));

		const FString Code = FString(TEXT(
			"float3 c = TexRGBA.rgb;\n"
			"float Light = LightTex * 255.0;\n"
			"float Kind = FullBright > 0.5 ? 1.0 : 0.0;\n"))
			+ DoomLevelHLSL
			+ TEXT("if (Modern > 0.5 && Kind < 0.75 && Fixed < 0.0) doomLevel = clamp((255.0 - Light) / 8.0 - Extra * 2.0, 0.0, 31.0);\n")
			+ DoomShadeHLSL
			+ TEXT(
			"float mask = TexRGBA.a;\n"
			"if (Fuzz > 0.5)\n"
			"{\n"
			"	// spectre fuzz: speckled darkening in Doom's 320x200 grain, reshuffled every tic\n"
			"	float2 p = floor(Parameters.SvPosition.xy / max(View.ViewSizeAndInvSize.y / 200.0, 1.0));\n"
			"	float h = frac(sin(dot(p + floor(Tic) * float2(1.7, 9.3), float2(12.9898, 78.233))) * 43758.5453);\n"
			"	outc = float3(0, 0, 0);\n"
			"	mask = (TexRGBA.a > 0.5 && h > 0.45) ? 1.0 : 0.0;\n"
			"}\n"
			"return float4(outc, mask);");
		UMaterialExpressionCustom* Shade = Custom(M, -350, 0, TEXT("DoomSprite"), Code, CMOT_Float4,
			{ TEXT("TexRGBA"), TEXT("LightTex"), TEXT("FullBright"), TEXT("Fuzz"), TEXT("Depth"), TEXT("Extra"), TEXT("Fixed"), TEXT("Scale"), TEXT("Tic"), TEXT("Modern") });
		Wire(Tex, TEXT("RGBA"), Shade, TEXT("TexRGBA"));
		Wire(Light, TEXT("R"), Shade, TEXT("LightTex"));
		Wire(Bright, TEXT(""), Shade, TEXT("FullBright"));
		Wire(Fuzz, TEXT(""), Shade, TEXT("Fuzz"));
		Wire(Depth, TEXT(""), Shade, TEXT("Depth"));
		Wire(Extra, TEXT(""), Shade, TEXT("Extra"));
		Wire(Fixed, TEXT(""), Shade, TEXT("Fixed"));
		Wire(Scale, TEXT(""), Shade, TEXT("Scale"));
		Wire(Tic, TEXT(""), Shade, TEXT("Tic"));
		Wire(Modern, TEXT(""), Shade, TEXT("Modern"));

		UMaterialExpressionComponentMask* RGB = Expr<UMaterialExpressionComponentMask>(M, -100, -50);
		RGB->R = 1; RGB->G = 1; RGB->B = 1; RGB->A = 0;
		RGB->Input.Connect(0, Shade);
		UMaterialExpressionComponentMask* A = Expr<UMaterialExpressionComponentMask>(M, -100, 50);
		A->R = 0; A->G = 0; A->B = 0; A->A = 1;
		A->Input.Connect(0, Shade);

		// the sprite's colormap row (spritelights), then the exact COLORMAP colour
		UMaterialExpressionCustom* Level = Custom(M, -350, 400, TEXT("SpriteLevel"), FString(TEXT(
			"float Light = LightTex * 255.0;\n"
			"float Kind = FullBright > 0.5 ? 1.0 : 0.0;\n"))
			+ DoomLevelHLSL
			+ TEXT("if (Modern > 0.5 && Kind < 0.75 && Fixed < 0.0) doomLevel = clamp((255.0 - Light) / 8.0 - Extra * 2.0, 0.0, 31.0);\n"
				"if (Fixed > 31.5) doomLevel = 32.0;\nreturn max(doomLevel, 0.0);"),
			CMOT_Float1, { TEXT("LightTex"), TEXT("FullBright"), TEXT("Depth"), TEXT("Extra"), TEXT("Fixed"), TEXT("Scale"), TEXT("Modern") });
		Wire(Light, TEXT("R"), Level, TEXT("LightTex"));
		Wire(Bright, TEXT(""), Level, TEXT("FullBright"));
		Wire(Depth, TEXT(""), Level, TEXT("Depth"));
		Wire(Extra, TEXT(""), Level, TEXT("Extra"));
		Wire(Fixed, TEXT(""), Level, TEXT("Fixed"));
		Wire(Scale, TEXT(""), Level, TEXT("Scale"));
		Wire(Modern, TEXT(""), Level, TEXT("Modern"));
		UMaterialExpression* Emissive = PaletteExact(M, MPC, MirrorUV, Level, RGB, ColorDefault, LinearDefaultTex, 0, 600);

		UMaterialEditingLibrary::ConnectMaterialProperty(Emissive, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(A, TEXT(""), MP_OpacityMask);
		return M;
	}

	/**
	 * Spectres: Doom draws their pixels as the background one row above or below, darkened
	 * by colormap 6. A translucent material can read the scene behind it, so it does the same,
	 * shifting by one 320x200 row in a pattern that changes every tic.
	 */
	UMaterial* BuildSpriteFuzz(UMaterialParameterCollection* MPC, UTexture* ColorDefault)
	{
		UMaterial* M = NewMaterial(TEXT("M_DoomSpriteFuzz"));
		M->SetShadingModel(MSM_Unlit);
		M->BlendMode = BLEND_Translucent;

		UMaterialExpressionTextureCoordinate* UV0 = Expr<UMaterialExpressionTextureCoordinate>(M, -1000, -200);
		UMaterialExpressionScalarParameter* Mirror = Scalar(M, -1000, -100, TEXT("Mirror"), 0.f);
		UMaterialExpressionCustom* MirrorUV = Custom(M, -800, -200, TEXT("MirrorUV"), TEXT("return float2(Mirror > 0.5 ? 1.0 - UV.x : UV.x, UV.y);"),
			CMOT_Float2, { TEXT("UV"), TEXT("Mirror") });
		Wire(UV0, TEXT(""), MirrorUV, TEXT("UV"));
		Wire(Mirror, TEXT(""), MirrorUV, TEXT("Mirror"));
		UMaterialExpressionTextureSampleParameter2D* Tex = TexParam(M, -600, -250, TEXT("Tex"), ColorDefault, SAMPLERTYPE_Color);
		Tex->Coordinates.Connect(0, MirrorUV);

		UMaterialExpressionCollectionParameter* Tic = Global(M, MPC, -1000, 100, TEXT("Tic"));
		UMaterialExpressionCustom* Offset = Custom(M, -800, 100, TEXT("FuzzOffset"), TEXT(
			"float2 p = floor(Parameters.SvPosition.xy / max(View.ViewSizeAndInvSize.y / 200.0, 1.0));\n"
			"float h = frac(sin(dot(p + floor(Tic) * float2(1.7, 9.3), float2(12.9898, 78.233))) * 43758.5453);\n"
			"return float2(0.0, (h < 0.5 ? -1.0 : 1.0) / 200.0);"),
			CMOT_Float2, { TEXT("Tic") });
		Wire(Tic, TEXT(""), Offset, TEXT("Tic"));
		UMaterialExpressionSceneColor* Scene = Expr<UMaterialExpressionSceneColor>(M, -600, 100);
		Scene->InputMode = EMaterialSceneAttributeInputMode::OffsetFraction;
		Scene->Input.Connect(0, Offset);
		UMaterialExpressionCustom* Dark = Custom(M, -350, 100, TEXT("FuzzDarken"), TEXT("return SceneRGB * 0.63;"), CMOT_Float3, { TEXT("SceneRGB") });
		WireIndex(Scene, 0, Dark, TEXT("SceneRGB"));
		UMaterialExpressionCustom* Coverage = Custom(M, -350, -150, TEXT("FuzzCoverage"), TEXT("return TexA > 0.5 ? 1.0 : 0.0;"), CMOT_Float1, { TEXT("TexA") });
		Wire(Tex, TEXT("A"), Coverage, TEXT("TexA"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Dark, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Coverage, TEXT(""), MP_Opacity);
		return M;
	}

	UMaterialParameterCollection* BuildCollection()
	{
		UPackage* Pkg = MakePackage(TEXT("MPC_Doom"));
		UMaterialParameterCollection* MPC = NewObject<UMaterialParameterCollection>(Pkg, TEXT("MPC_Doom"), RF_Public | RF_Standalone);
		auto Add = [&](const TCHAR* Name, float Value)
		{
			FCollectionScalarParameter P;
			P.ParameterName = Name;
			P.DefaultValue = Value;
			MPC->ScalarParameters.Add(P);
		};
		Add(TEXT("Tic"), 0.f);
		Add(TEXT("ExtraLight"), 0.f);
		Add(TEXT("FixedColormap"), -1.f);
		Add(TEXT("Modern"), 0.f);
		Add(TEXT("DoomScale"), 3.f);
		Add(TEXT("PaletteExact"), 1.f);
		Add(TEXT("Palette"), 0.f);
		Add(TEXT("PaletteRows"), 34.f);
		MPC->PostEditChange();
		Save(MPC);
		return MPC;
	}
}

// ---- Niagara effects ---------------------------------------------------------------------------
// Each effect is a copy of the engine's "Directional Burst (Lightweight)" template: a stateless
// emitter whose modules are plain properties, set here as text through reflection (their
// headers are internal to the Niagara plugin). PostEditChangeProperty refreshes the derived
// runtime values the same way the details panel does.
namespace
{
	const TCHAR* EffectsFolder = TEXT("/Game/Doom/Effects/");
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
		UPackage* Pkg = MakePackage(Name, EffectsFolder);
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
		return bOk && Save(Sys);
	}

	bool BuildEffects()
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

	bool EffectsExist()
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
}

bool DoomContent::AssetsExist()
{
	return LoadObject<UMaterialParameterCollection>(nullptr, DoomAssets::CollectionPath) != nullptr
		&& LoadObject<UMaterialInterface>(nullptr, DoomAssets::WorldClassicPath) != nullptr
		&& LoadObject<UMaterialInterface>(nullptr, DoomAssets::WorldModernPath) != nullptr
		&& LoadObject<UMaterialInterface>(nullptr, DoomAssets::SkyPath) != nullptr
		&& LoadObject<UMaterialInterface>(nullptr, DoomAssets::SpritePath) != nullptr
		&& LoadObject<UMaterialInterface>(nullptr, DoomAssets::SpriteFuzzPath) != nullptr;
}

bool DoomContent::Build(bool bForce, bool bMaterials, bool bEffects)
{
	bool bEffectsOk = true;
	if (bEffects && (bForce || !EffectsExist()))
	{
		UE_LOG(LogDoomContent, Display, TEXT("Generating Doom effects in %s"), EffectsFolder);
		bEffectsOk = BuildEffects();
	}
	if (!bMaterials || (!bForce && AssetsExist()))
	{
		UE_LOG(LogDoomContent, Log, TEXT("Doom materials already exist"));
		return bEffectsOk;
	}
	UE_LOG(LogDoomContent, Display, TEXT("Generating Doom materials in %s"), Folder);
	UTexture* ColorDefault = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	UTexture* Linear = LinearDefault();
	UMaterialParameterCollection* MPC = BuildCollection();

	TArray<UMaterial*> Mats = {
		BuildWorld(MPC, ColorDefault, Linear, false),
		BuildWorld(MPC, ColorDefault, Linear, true),
		BuildSky(MPC, ColorDefault, Linear),
		BuildSprite(MPC, ColorDefault, Linear),
		BuildSpriteFuzz(MPC, ColorDefault),
	};
	bool bOk = true;
	for (UMaterial* M : Mats)
	{
		M->PreEditChange(nullptr);
		M->PostEditChange();
		const TArray<FString> Errors = UMaterialEditingLibrary::RecompileMaterial(M);
		for (const FString& E : Errors)
		{
			UE_LOG(LogDoomContent, Error, TEXT("%s: %s"), *M->GetName(), *E);
			bOk = false;
		}
		bOk &= Save(M);
	}
	return bOk && bEffectsOk;
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


// ---- Export for Lyra -----------------------------------------------------------------------------
// A WAD map as an arena for Lyra's Shooter Core: doors open, lifts lowered (Lyra launch pads take
// their place), geometry as OBJ tiles grouped by surface role so Lyra's materials can be applied,
// invisible collision ramps over Doom's steps (taller than a Lyra character's step), and a JSON
// list of gameplay spots. Tools/Lyra/build_doom_level.py builds the Lyra level from it.

namespace
{
	FString SurfaceRole(const FString& Key, const FVector3f& Normal)
	{
		const FString Name = Key.Mid(2).ToUpper();
		auto Has = [&](std::initializer_list<const TCHAR*> Parts)
		{
			for (const TCHAR* P : Parts) if (Name.Contains(P)) return true;
			return false;
		};
		if (Has({ TEXT("NUKAGE"), TEXT("FWATER"), TEXT("LAVA"), TEXT("BLOOD"), TEXT("SLIME") })) return TEXT("Liquid");
		if (Has({ TEXT("LITE"), TEXT("LIGHT"), TEXT("TLITE"), TEXT("CEIL1_2"), TEXT("CEIL1_3") })) return TEXT("Light");
		if (Has({ TEXT("DOOR"), TEXT("DOORTRAK") })) return TEXT("Door");
		if (Name.StartsWith(TEXT("SW1")) || Name.StartsWith(TEXT("SW2")) || Has({ TEXT("EXIT") })) return TEXT("Accent");
		if (Normal.Z > 0.5f) return TEXT("Floor");
		if (Normal.Z < -0.5f) return TEXT("Ceiling");
		if (Has({ TEXT("STEP"), TEXT("METAL"), TEXT("SUPPORT"), TEXT("PIPE"), TEXT("COMP") })) return TEXT("Metal");
		return TEXT("Wall");
	}

	struct FObjWriter
	{
		TMap<FString, TArray<FVector3f>> Tris; // role -> 3 positions per triangle
		TMap<FString, TArray<FVector3f>> Norms;

		// every triangle is wound so its front faces N: the level builder's walls come wound the other way
		// from its floors, which a two-sided material hides but one-sided collision does not
		void Add(const FString& Role, const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& N)
		{
			const bool bFlip = FVector3f::DotProduct(FVector3f::CrossProduct(B - A, C - A), N) > 0;
			TArray<FVector3f>& T = Tris.FindOrAdd(Role);
			T.Add(A); T.Add(bFlip ? C : B); T.Add(bFlip ? B : C);
			Norms.FindOrAdd(Role).Add(N);
		}

		bool Save(const FString& File) const
		{
			// Unreal centimetres, Z up; planar UVs: 1 UV per 4 m, floors from X/Y, walls along the wall
			FString Out = TEXT("# DoomUE export for Lyra\n");
			int32 Base = 1;
			for (const auto& Pair : Tris)
			{
				Out += FString::Printf(TEXT("g %s\nusemtl %s\n"), *Pair.Key, *Pair.Key);
				const TArray<FVector3f>& T = Pair.Value;
				const TArray<FVector3f>& NN = Norms[Pair.Key];
				for (int32 t = 0; t * 3 + 2 < T.Num(); ++t)
				{
					const FVector3f N = NN[t];
					for (int32 k = 0; k < 3; ++k)
					{
						const FVector3f& P = T[t * 3 + k];
						float U, V;
						if (FMath::Abs(N.Z) > 0.5f) { U = P.X / 400.f; V = P.Y / 400.f; }
						else { const FVector3f Tan(-N.Y, N.X, 0); U = FVector3f::DotProduct(P, Tan) / 400.f; V = -P.Z / 400.f; }
						Out += FString::Printf(TEXT("v %.2f %.2f %.2f\nvt %.4f %.4f\nvn %.4f %.4f %.4f\n"), P.X, P.Y, P.Z, U, V, N.X, N.Y, N.Z);
					}
					Out += FString::Printf(TEXT("f %d/%d/%d %d/%d/%d %d/%d/%d\n"), Base, Base, Base, Base + 1, Base + 1, Base + 1, Base + 2, Base + 2, Base + 2);
					Base += 3;
				}
			}
			return FFileHelper::SaveStringToFile(Out, *File);
		}
	};

	TSharedRef<FJsonObject> JVec(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
}

bool DoomContent::ExportLyraLevel(const FString& InMapName, const FString& OutDir)
{
	const FString MapName = InMapName.ToUpper();
	FDoomWad* Wad = DoomEditorData::Wad();
	FDoomGraphics* Gfx = DoomEditorData::Graphics();
	FDoomMap Map;
	if (!Wad || !Gfx || !Map.Load(*Wad, MapName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("ExportLyra: no map %s"), *MapName);
		return false;
	}
	const float S = Doom::Scale;
	auto Feet = [&](const FVector2f& P) { const int32 Sec = Map.SectorAt(P); return Doom::ToUE(P, Sec >= 0 ? Map.Sectors[Sec].Floor : 0); };

	// doors open, lifts down (their old top height goes to the launch pads)
	TArray<UDoomWorld::ESectorRole> Roles;
	UDoomWorld::ClassifySectors(Map, Roles);
	TArray<float> LiftTop;
	LiftTop.Init(0, Map.Sectors.Num());
	auto Neighbours = [&](int32 Sec, TFunctionRef<void(const FDoomSector&)> Fn)
	{
		for (int32 Li : Map.Sectors[Sec].Lines)
		{
			const int32 O = Map.OtherSector(Li, Sec);
			if (O != INDEX_NONE) Fn(Map.Sectors[O]);
		}
	};
	int32 Doors = 0, Lifts = 0;
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		FDoomSector& Sec = Map.Sectors[s];
		if (Roles[s] == UDoomWorld::ESectorRole::Door)
		{
			float Lowest = TNumericLimits<float>::Max();
			Neighbours(s, [&](const FDoomSector& N) { Lowest = FMath::Min(Lowest, N.Ceiling); });
			if (Lowest < TNumericLimits<float>::Max()) Sec.Ceiling = FMath::Max(Sec.Ceiling, Lowest - 4.f);
			++Doors;
		}
		else if (Roles[s] == UDoomWorld::ESectorRole::Lift)
		{
			float Lowest = Sec.Floor;
			Neighbours(s, [&](const FDoomSector& N) { Lowest = FMath::Min(Lowest, N.Floor); });
			LiftTop[s] = Sec.Floor;
			Sec.Floor = Lowest;
			++Lifts;
		}
	}

	// geometry through the game's own builder
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false);
	ADoomLevel* Level = World->SpawnActor<ADoomLevel>();
	Level->bEditorPreview = true;
	Level->Build(Map, *Gfx, TSet<int32>(), false, FDoomGraphics::SkyTextureFor(MapName));
	TMap<FName, ADoomLevel::FSectionData> Solid, NonSolid;
	Level->GetAllSections(Solid, NonSolid);

	IFileManager::Get().MakeDirectory(*OutDir, true);
	TMap<FIntPoint, FObjWriter> Tiles;
	FBox Bounds(ForceInit);
	const float TileSize = 1024.f * S;
	int32 Triangles = 0;
	// collision-only surfaces (hidden in Lyra): step ramps, impassable lines, and Doom's sky as a shell
	FObjWriter Collision;
	for (const auto& Pair : Solid)
	{
		const FString Key = Pair.Key.ToString();
		const ADoomLevel::FSectionData& D = Pair.Value;
		if (Key.StartsWith(TEXT("S:")))
		{
			// sky: open to Lyra's sky, but as solid as Doom's (nobody jumps out of the map)
			for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
			{
				Collision.Add(TEXT("Blocker"), FVector3f(D.V[D.I[t]]), FVector3f(D.V[D.I[t + 1]]), FVector3f(D.V[D.I[t + 2]]), FVector3f(D.N[D.I[t]]));
			}
			continue;
		}
		for (int32 t = 0; t + 2 < D.I.Num(); t += 3)
		{
			const FVector3f A(D.V[D.I[t]]), B(D.V[D.I[t + 1]]), C(D.V[D.I[t + 2]]);
			const FVector3f N(D.N[D.I[t]]);
			const FVector3f Mid = (A + B + C) / 3.f;
			FObjWriter& W = Tiles.FindOrAdd(FIntPoint(FMath::FloorToInt32(Mid.X / TileSize), FMath::FloorToInt32(Mid.Y / TileSize)));
			W.Add(SurfaceRole(Key, N), A, B, C, N);
			Bounds += FVector(A); Bounds += FVector(B); Bounds += FVector(C);
			++Triangles;
		}
	}
	World->DestroyWorld(false);

	TArray<TSharedPtr<FJsonValue>> TileFiles;
	for (const auto& Tile : Tiles)
	{
		const FString Name = FString::Printf(TEXT("%s_Tile_%s%d_%s%d.obj"), *MapName, Tile.Key.X < 0 ? TEXT("N") : TEXT(""), FMath::Abs(Tile.Key.X),
			Tile.Key.Y < 0 ? TEXT("N") : TEXT(""), FMath::Abs(Tile.Key.Y));
		Tile.Value.Save(OutDir / Name);
		TileFiles.Add(MakeShared<FJsonValueString>(Name));
	}

	// collision ramps over steps up to 24 map units (a Lyra character steps 45 cm; Doom's 72), and
	// walls on impassable two-sided lines (window ledges, railings), both sides, floor to ceiling
	int32 NumRamps = 0, NumBlockers = 0;
	for (const FDoomLine& L : Map.Lines)
	{
		if (L.FrontSector == INDEX_NONE || L.BackSector == INDEX_NONE)
		{
			continue;
		}
		const FDoomSector& F = Map.Sectors[L.FrontSector];
		const FDoomSector& K = Map.Sectors[L.BackSector];
		if (L.Flags & DoomLineFlags::Blocking)
		{
			const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
			const float Low = FMath::Min(F.Floor, K.Floor), High = FMath::Max(F.Ceiling, K.Ceiling);
			const FVector3f A0(Doom::ToUE(A, Low)), B0(Doom::ToUE(B, Low)), A1(Doom::ToUE(A, High)), B1(Doom::ToUE(B, High));
			const FVector2f Dir = (B - A).GetSafeNormal();
			const FVector3f Side(Doom::ToUE(FVector2f(Dir.Y, -Dir.X), 0) / Doom::Scale);
			for (const FVector3f& Facing : { Side, -Side })
			{
				Collision.Add(TEXT("Blocker"), A0, B0, B1, Facing);
				Collision.Add(TEXT("Blocker"), A0, B1, A1, Facing);
			}
			++NumBlockers;
			continue;
		}
		const float Rise = FMath::Abs(F.Floor - K.Floor);
		if (Rise < 14.f || Rise > 24.f)
		{
			continue;
		}
		const bool bFrontLow = F.Floor < K.Floor;
		const float Low = FMath::Min(F.Floor, K.Floor), High = FMath::Max(F.Floor, K.Floor);
		const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
		// the front side is on the right of V1->V2 in Doom: step out into the lower sector
		const FVector2f Dir = (B - A).GetSafeNormal();
		const FVector2f Right(Dir.Y, -Dir.X);
		const FVector2f Out = bFrontLow ? Right : -Right;
		const FVector2f A2 = A + Out * Rise * 2.2f, B2 = B + Out * Rise * 2.2f;
		const FVector3f TA(Doom::ToUE(A, High)), TB(Doom::ToUE(B, High)), BA(Doom::ToUE(A2, Low)), BB(Doom::ToUE(B2, Low));
		const FVector3f N = FVector3f::CrossProduct(TB - TA, BA - TA).GetSafeNormal();
		const FVector3f Up = N.Z < 0 ? -N : N;
		Collision.Add(TEXT("Ramp"), TA, TB, BB, Up);
		Collision.Add(TEXT("Ramp"), TA, BB, BA, Up);
		++NumRamps;
	}
	const FString RampFile = FString::Printf(TEXT("%s_Collision.obj"), *MapName);
	Collision.Save(OutDir / RampFile);

	// gameplay spots
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("map"), MapName);
	Root->SetStringField(TEXT("title"), FString::Printf(TEXT("%s"), *MapName));
	Root->SetArrayField(TEXT("tiles"), TileFiles);
	Root->SetStringField(TEXT("collision"), RampFile);
	Root->SetStringField(TEXT("ramps"), RampFile); // older builder scripts
	Root->SetObjectField(TEXT("boundsMin"), JVec(Bounds.Min));
	Root->SetObjectField(TEXT("boundsMax"), JVec(Bounds.Max));
	TArray<TSharedPtr<FJsonValue>> Starts, Items, LiftsJ, Teleports, Lights;
	auto Spot = [&](const FVector& At, float Yaw, const TCHAR* Kind)
	{
		TSharedRef<FJsonObject> O = JVec(At);
		O->SetNumberField(TEXT("yaw"), Yaw);
		O->SetStringField(TEXT("kind"), Kind);
		return MakeShared<FJsonValueObject>(O);
	};
	for (const FDoomMapThing& T : Map.Things)
	{
		const FVector At = Feet(T.P);
		const float Yaw = Doom::AngleToYaw(T.Angle);
		const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
		if (T.Type >= 1 && T.Type <= 4) { Starts.Add(Spot(At, Yaw, TEXT("coop"))); continue; }
		if (T.Type == 11) { Starts.Add(Spot(At, Yaw, TEXT("deathmatch"))); continue; }
		if (!Def) continue;
		const TCHAR* Kind = nullptr;
		switch (Def->Pickup)
		{
		case EPickup::Shotgun: case EPickup::SuperShotgun: Kind = TEXT("shotgun"); break;
		case EPickup::Chaingun: case EPickup::Plasma: case EPickup::BFG: case EPickup::RocketLauncher: Kind = TEXT("rifle"); break;
		case EPickup::Chainsaw: Kind = TEXT("pistol"); break;
		case EPickup::Health: case EPickup::Soulsphere: case EPickup::Megasphere: case EPickup::Berserk: Kind = TEXT("heal_big"); break;
		case EPickup::GreenArmor: case EPickup::BlueArmor: Kind = TEXT("heal_small"); break;
		default: break;
		}
		if (Kind)
		{
			Items.Add(Spot(At, Yaw, Kind));
		}
		if (Def->LightColor.A > 0 || ((Def->Flags & EThingFlag::FullBright) && !(Def->Flags & EThingFlag::Monster) && !(Def->Flags & EThingFlag::Pickup)))
		{
			TSharedRef<FJsonObject> O = JVec(At + FVector(0, 0, Def->Height * 0.8f * S));
			const FLinearColor C = Def->LightColor.A > 0 ? Def->LightColor : FLinearColor(0.7f, 0.8f, 1.f);
			O->SetNumberField(TEXT("r"), C.R); O->SetNumberField(TEXT("g"), C.G); O->SetNumberField(TEXT("b"), C.B);
			O->SetNumberField(TEXT("intensity"), 1.f);
			Lights.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		const FDoomSector& Sec = Map.Sectors[s];
		if (Roles[s] == UDoomWorld::ESectorRole::Lift && LiftTop[s] > Sec.Floor + 8.f)
		{
			TSharedRef<FJsonObject> O = JVec(Doom::ToUE(Sec.Center, Sec.Floor));
			O->SetNumberField(TEXT("rise"), (LiftTop[s] - Sec.Floor) * S);
			LiftsJ.Add(MakeShared<FJsonValueObject>(O));
		}
		// bright rooms get a soft light near the ceiling (Lyra's lighting does the rest)
		const FVector2f Size = Sec.Bounds.GetSize();
		if (Sec.Light >= 192 && !FDoomGraphics::IsSkyFlat(Sec.CeilingPic) && Size.X * Size.Y > 64 * 64 && Sec.Ceiling - Sec.Floor > 40.f)
		{
			TSharedRef<FJsonObject> O = JVec(Doom::ToUE(Sec.Center, Sec.Ceiling - 16.f));
			O->SetNumberField(TEXT("r"), 1.f); O->SetNumberField(TEXT("g"), 0.95f); O->SetNumberField(TEXT("b"), 0.9f);
			O->SetNumberField(TEXT("intensity"), Sec.Light / 255.f);
			O->SetNumberField(TEXT("radius"), FMath::Max(Size.X, Size.Y) * S);
			Lights.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	TArray<TPair<int32, int32>> Links;
	UDoomWorld::FindTeleporters(Map, Links);
	for (const TPair<int32, int32>& Link : Links)
	{
		const FDoomLine& L = Map.Lines[Link.Key];
		const FVector2f A = Map.V(L.V1), B = Map.V(L.V2);
		const FVector2f Mid = (A + B) * 0.5f;
		const FDoomMapThing& Dest = Map.Things[Link.Value];
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("from"), JVec(Feet(Mid)));
		O->SetNumberField(TEXT("fromYaw"), FMath::RadiansToDegrees(FMath::Atan2(-(B - A).Y, (B - A).X)));
		O->SetObjectField(TEXT("to"), JVec(Feet(Dest.P)));
		O->SetNumberField(TEXT("toYaw"), Doom::AngleToYaw(Dest.Angle));
		Teleports.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("playerStarts"), Starts);
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetArrayField(TEXT("lifts"), LiftsJ);
	Root->SetArrayField(TEXT("teleporters"), Teleports);
	Root->SetArrayField(TEXT("lights"), Lights);
	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	const bool bOk = FFileHelper::SaveStringToFile(Json, *(OutDir / FString::Printf(TEXT("%s.json"), *MapName)));
	UE_LOG(LogDoomContent, Display, TEXT("ExportLyra: %s -> %s: %d tiles, %d triangles, %d ramps, %d impassable lines walled, %d doors opened, %d lifts, %d starts, %d items, %d teleporters, %d lights"),
		*MapName, *OutDir, Tiles.Num(), Triangles, NumRamps, NumBlockers, Doors, Lifts, Starts.Num(), Items.Num(), Teleports.Num(), Lights.Num());
	return bOk;
}

// ---- Map import --------------------------------------------------------------------------------

bool DoomContent::BakeMapTiles(ADoomMapActor* MapActor)
{
	if (!MapActor)
	{
		return false;
	}
	// the preview builds the same chunks as the game, sector edits included
	MapActor->RebuildPreview();
	TMap<FName, ADoomLevel::FSectionData> Solid, NonSolid;
	MapActor->GetStaticSections(Solid, NonSolid);
	if (Solid.Num() == 0 && NonSolid.Num() == 0)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Bake: no geometry for %s (is the IWAD in Content/WADs?)"), *MapActor->MapName);
		return false;
	}
	const uint32 Sig = ADoomLevel::Signature(Solid, NonSolid);
	int32 Verts = 0;
	for (const auto& P : Solid) Verts += P.Value.V.Num();
	for (const auto& P : NonSolid) Verts += P.Value.V.Num();
	UE_LOG(LogDoomContent, Display, TEXT("Bake: static chunk %d + %d sections, %d vertices"), Solid.Num(), NonSolid.Num(), Verts);
	TArray<ADoomLevel::FTile> Tiles;
	ADoomLevel::SplitTiles(Solid, NonSolid, Tiles);
	// slots keep their section key as the name; the game puts its own material instances there
	UMaterialInterface* Placeholder = DoomAssets::WorldMaterial(true);
	const FString MapName = MapActor->MapName.ToUpper();
	const FString TileFolder = FString::Printf(TEXT("/Game/Doom/Maps/%s_Tiles/"), *MapName);
	auto Coord = [](int32 V) { return V < 0 ? FString::Printf(TEXT("N%d"), -V) : FString::FromInt(V); };
	TArray<UStaticMesh*> Meshes;
	for (const ADoomLevel::FTile& Tile : Tiles)
	{
		const FString Name = FString::Printf(TEXT("SM_%s_%s_%s"), *MapName, *Coord(Tile.Cell.X), *Coord(Tile.Cell.Y));
		UPackage* Pkg = MakePackage(Name, *TileFolder);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		ADoomLevel::FillTileMesh(Mesh, Tile, [Placeholder](FName) { return Placeholder; });
		Mesh->Build(true);
		Meshes.Add(Mesh);
	}
	FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
	bool bOk = true;
	MapActor->Modify();
	MapActor->BakedTiles.Reset();
	for (UStaticMesh* Mesh : Meshes)
	{
		bOk &= Save(Mesh);
		MapActor->BakedTiles.Add(Mesh);
	}
	MapActor->BakedTileSignature = Sig;
	MapActor->MarkPackageDirty();
	UE_LOG(LogDoomContent, Display, TEXT("Bake: %s: %d static mesh tiles in %s (signature %08x)"), *MapName, Meshes.Num(), *TileFolder, Sig);
	return bOk;
}

FString DoomContent::MapPackage(const FString& MapName)
{
	return FString(TEXT("/Game/Doom/Maps/")) + MapName.ToUpper();
}

bool DoomContent::ImportMap(const FString& InMapName, bool bOverwrite)
{
	const FString MapName = InMapName.ToUpper();
	FDoomWad* Wad = DoomEditorData::Wad();
	if (!Wad || !Wad->MapNames().Contains(MapName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("Import: no map %s in the WAD"), *MapName);
		return false;
	}
	const FString PkgName = MapPackage(MapName);
	if (FPackageName::DoesPackageExist(PkgName) && !bOverwrite)
	{
		UE_LOG(LogDoomContent, Display, TEXT("Import: %s already exists (use -force to replace it)"), *PkgName);
		return true;
	}
	if (FindPackage(nullptr, *PkgName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("Import: %s is loaded; close it before replacing it"), *PkgName);
		return false;
	}
	UPackage* Pkg = CreatePackage(*PkgName);
	Pkg->MarkAsFullyLoaded(); // replacing a level on disk: this new package is the whole of it
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*MapName), Pkg);
	World->SetFlags(RF_Public | RF_Standalone);

	FActorSpawnParameters SP;
	SP.Name = TEXT("DoomMap");
	ADoomMapActor* MapActor = World->SpawnActor<ADoomMapActor>(ADoomMapActor::StaticClass(), FTransform::Identity, SP);
	MapActor->MapName = MapName;
	MapActor->SetActorLabel(FString::Printf(TEXT("DoomMap %s"), *MapName));
	MapActor->SetFolderPath(TEXT("Doom"));
	MapActor->ImportThings();
	BakeMapTiles(MapActor); // static geometry as tile assets: distance fields in packaged builds

	FAssetRegistryModule::AssetCreated(World);
	Pkg->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(PkgName, FPackageName::GetMapPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Standalone;
	Args.Error = GWarn;
	const bool bOk = UPackage::SavePackage(Pkg, World, *File, Args);
	int32 Markers = 0;
	for (TActorIterator<ADoomThingMarker> It(World); It; ++It) ++Markers;
	UE_LOG(LogDoomContent, Display, TEXT("Import: %s %s (%d thing markers)"), bOk ? TEXT("saved") : TEXT("FAILED to save"), *File, Markers);
	World->DestroyWorld(false);
	World->RemoveFromRoot();
	if (bOk)
	{
		// packaged builds cook the maps listed in the packaging settings: add one line to
		// DefaultGame.ini (written as text so the rest of the file stays as it is)
		const FString Ini = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
		const FString Line = FString::Printf(TEXT("+MapsToCook=(FilePath=\"%s\")"), *PkgName);
		const FString Section = TEXT("[/Script/UnrealEd.ProjectPackagingSettings]");
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Ini) && !Text.Contains(Line))
		{
			const int32 At = Text.Find(Section);
			if (At == INDEX_NONE)
			{
				Text += FString::Printf(TEXT("\r\n%s\r\n%s\r\n"), *Section, *Line);
			}
			else
			{
				Text.InsertAt(At + Section.Len(), TEXT("\r\n") + Line);
			}
			FFileHelper::SaveStringToFile(Text, *Ini);
			UE_LOG(LogDoomContent, Display, TEXT("Import: %s added to the maps to cook"), *PkgName);
		}
	}
	return bOk;
}

// Editor round trip: open an imported level as the editor would, build the preview, edit a
// sector and a marker, and save the result as /Game/Doom/Tests/<MAP>_Edit for -game checks.
bool DoomContent::EditTest(const FString& InMapName)
{
	const FString MapName = InMapName.ToUpper();
	if (!FPackageName::DoesPackageExist(MapPackage(MapName)) && !ImportMap(MapName, false))
	{
		return false;
	}
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(MapPackage(MapName), FPackageName::GetMapPackageExtension()));
	ADoomMapActor* MapActor = ADoomMapActor::Find(World);
	if (!MapActor)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: no map actor in %s"), *MapPackage(MapName));
		return false;
	}
	MapActor->RebuildPreview();
	const int32 PreviewComponents = MapActor->NumMeshComponents();
	const FDoomMap* Map = MapActor->GetPreviewMap();
	if (!Map || PreviewComponents == 0)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: the preview did not build"));
		return false;
	}
	// raise the player start's sector by 48 and brighten it; turn the first imp into a baron
	ADoomThingMarker* Start = nullptr;
	ADoomThingMarker* Imp = nullptr;
	int32 Markers = 0;
	for (TActorIterator<ADoomThingMarker> It(World); It; ++It)
	{
		++Markers;
		if (It->DoomEdNum == 1 && !Start) Start = *It;
		if (It->DoomEdNum == 3001 && !Imp) Imp = *It;
	}
	if (!Start)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: no player start marker"));
		return false;
	}
	const int32 Sec = MapActor->SectorAtLocation(Start->GetActorLocation());
	const float OldFloor = Map->Sectors[Sec].Floor;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADoomSectorEdit* Edit = World->SpawnActor<ADoomSectorEdit>(ADoomSectorEdit::StaticClass(), FTransform(Start->GetActorLocation() + FVector(0, 0, 64)), SP);
	Edit->ReadFromSector();
	Edit->bFloor = true;
	Edit->FloorHeight = OldFloor + 48.f;
	Edit->bLight = true;
	Edit->Light = 255;
	Edit->SetActorLabel(TEXT("Raised start"));
	Edit->SetFolderPath(TEXT("Doom/Sector Edits"));
	if (Imp)
	{
		Imp->DoomEdNum = 3003;
		Imp->ThingType = TEXT("Baron");
	}
	MapActor->RebuildPreview();
	const float NewFloor = MapActor->GetPreviewMap()->Sectors[Sec].Floor;
	const FString TestPackage = FString(TEXT("/Game/Doom/Tests/")) + MapName + TEXT("_Edit");
	const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, TestPackage);
	const bool bPass = PreviewComponents > 0 && FMath::IsNearlyEqual(NewFloor, OldFloor + 48.f) && Edit->Sector == Sec && bSaved;
	UE_LOG(LogDoomContent, Display, TEXT("Edittest %s: preview %d components, %d markers, start sector %d floor %.0f -> %.0f, saved %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), PreviewComponents, Markers, Sec, OldFloor, NewFloor, *TestPackage);
	return bPass;
}

int32 UDoomContentCommandlet::Main(const FString& Params)
{
	// -exportlyra=E1M1 -out=<dir>: the map as a Lyra arena (OBJ tiles, ramps, gameplay JSON)
	FString LyraMap;
	if (FParse::Value(*Params, TEXT("-exportlyra="), LyraMap))
	{
		FString OutDir = FPaths::ProjectSavedDir() / TEXT("LyraExport");
		FParse::Value(*Params, TEXT("-out="), OutDir);
		return DoomContent::ExportLyraLevel(LyraMap, OutDir) ? 0 : 1;
	}
	FString EditMap;
	if (FParse::Value(*Params, TEXT("-edittest="), EditMap))
	{
		return DoomContent::EditTest(EditMap) ? 0 : 1;
	}
	// -importmap=E1M1[,E1M2...] or -importmap=all: save WAD maps as editable levels
	FString Import;
	if (FParse::Value(*Params, TEXT("-importmap="), Import, false))
	{
		TArray<FString> Names;
		if (Import.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			Names = DoomEditorData::MapNames();
		}
		else
		{
			Import.ParseIntoArray(Names, TEXT(","));
		}
		bool bOk = true;
		for (const FString& N : Names)
		{
			bOk &= DoomContent::ImportMap(N, Params.Contains(TEXT("-force")));
		}
		return bOk ? 0 : 1;
	}
	FString Dump;
	if (FParse::Value(*Params, TEXT("-dumpniagara="), Dump))
	{
		DoomContent::DumpNiagara(Dump);
		return 0;
	}
	const bool bForce = Params.Contains(TEXT("-force"));
	// -effects / -materials: regenerate only one kind (new materials cost a long shader compile)
	const bool bOnlyEffects = Params.Contains(TEXT("-effects")), bOnlyMaterials = Params.Contains(TEXT("-materials"));
	return DoomContent::Build(bForce, !bOnlyEffects, !bOnlyMaterials) ? 0 : 1;
}
