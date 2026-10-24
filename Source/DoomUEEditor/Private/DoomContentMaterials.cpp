// The content builder's materials: the parameter collection and the classic, modern, sky, sprite
// and fuzz materials, with Doom's light diminishing and palette lookups as Custom HLSL nodes.
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

namespace
{

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
		UPackage* Pkg = DoomContent::MakePackage(Name);
		UMaterial* M = NewObject<UMaterial>(Pkg, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		M->TwoSided = true;
		return M;
	}

	UTexture2D* LinearDefault()
	{
		const FString Name = TEXT("T_DoomLinearWhite");
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *(FString(DoomContent::Folder) + Name + TEXT(".") + Name)))
		{
			return Existing;
		}
		UPackage* Pkg = DoomContent::MakePackage(Name);
		UTexture2D* T = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		TArray<FColor> Px;
		Px.Init(FColor(160, 160, 160, 255), 16);
		T->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Px.GetData()));
		T->SRGB = false;
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->Filter = TF_Nearest;
		T->MipGenSettings = TMGS_NoMipmaps;
		T->PostEditChange();
		DoomContent::Save(T);
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
		UPackage* Pkg = DoomContent::MakePackage(TEXT("MPC_Doom"));
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
		DoomContent::Save(MPC);
		return MPC;
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
	BuildGameData(false);
	BuildInput(false);
	BuildAI(false);
	bool bEffectsOk = true;
	if (bEffects && (bForce || !EffectsExist()))
	{
		UE_LOG(LogDoomContent, Display, TEXT("Generating Doom effects in %s"), DoomContent::EffectsFolder);
		bEffectsOk = BuildEffects();
	}
	if (!bMaterials || (!bForce && AssetsExist()))
	{
		UE_LOG(LogDoomContent, Log, TEXT("Doom materials already exist"));
		return bEffectsOk;
	}
	UE_LOG(LogDoomContent, Display, TEXT("Generating Doom materials in %s"), DoomContent::Folder);
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
		bOk &= DoomContent::Save(M);
	}
	return bOk && bEffectsOk;
}
