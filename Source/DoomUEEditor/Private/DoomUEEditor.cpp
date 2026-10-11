#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "DoomContentBuilder.h"
#include "DoomMapActors.h"
#include "ToolMenus.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DoomUEEditor"

namespace
{
	void Notify(const FText& Text, bool bOk)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 4.f;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	/** Imports the map if it has no level yet, then opens its level. */
	void OpenMap(const FString& MapName)
	{
		const FString Package = DoomContent::MapPackage(MapName);
		const bool bExisted = FPackageName::DoesPackageExist(Package);
		if (!bExisted && !DoomContent::ImportMap(MapName, false))
		{
			Notify(FText::Format(LOCTEXT("ImportFailed", "Could not import {0} (see the Output Log)"), FText::FromString(MapName)), false);
			return;
		}
		// offers to save the current level first, like File > Open Level
		if (FEditorFileUtils::SaveDirtyPackages(true, true, false))
		{
			UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()));
			if (!bExisted)
			{
				Notify(FText::Format(LOCTEXT("Imported", "Imported {0} as {1}"), FText::FromString(MapName), FText::FromString(Package)), true);
			}
		}
	}

	void FillMapMenu(UToolMenu* Menu)
	{
		FToolMenuSection& Section = Menu->AddSection(TEXT("DoomMaps"), LOCTEXT("Maps", "WAD maps (opens the level, importing it the first time)"));
		for (const FString& Name : DoomEditorData::MapNames())
		{
			const bool bImported = FPackageName::DoesPackageExist(DoomContent::MapPackage(Name));
			Section.AddMenuEntry(FName(*Name), FText::FromString(bImported ? Name : Name + TEXT("  (import)")), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Name]() { OpenMap(Name); })));
		}
	}

	void RegisterMenus()
	{
		UToolMenu* Main = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
		UToolMenu* Doom = Main->AddSubMenu(TEXT("MainMenu"), NAME_None, TEXT("Doom"), LOCTEXT("DoomMenu", "Doom"), LOCTEXT("DoomMenuTip", "Doom maps and generated content"));
		FToolMenuSection& Maps = Doom->AddSection(TEXT("Maps"), LOCTEXT("MapsSection", "Maps"));
		Maps.AddSubMenu(TEXT("OpenMap"), LOCTEXT("OpenMap", "Open / Import Map"), LOCTEXT("OpenMapTip", "Open a WAD map as an editable level"),
			FNewToolMenuDelegate::CreateStatic(&FillMapMenu));
		FToolMenuSection& Content = Doom->AddSection(TEXT("Content"), LOCTEXT("ContentSection", "Generated content"));
		Content.AddMenuEntry(TEXT("Regenerate"), LOCTEXT("Regenerate", "Regenerate Materials and Effects"),
			LOCTEXT("RegenerateTip", "Rebuilds the Doom materials (long shader compile on the next run) and Niagara effects"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				const bool bOk = DoomContent::Build(true);
				Notify(bOk ? LOCTEXT("RegenOk", "Doom materials and effects regenerated") : LOCTEXT("RegenFail", "Regeneration failed (see the Output Log)"), bOk);
			})));
	}
}

class FDoomUEEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// First editor launch: generate the materials and effects the game needs once the engine is up.
		if (GIsEditor && !IsRunningCommandlet())
		{
			FCoreDelegates::GetOnPostEngineInit().AddLambda([]()
			{
				DoomContent::Build(false); // only what is missing: materials, effects
			});
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
			{
				FToolMenuOwnerScoped Owner(TEXT("DoomUEEditor"));
				RegisterMenus();
			}));
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized() && UToolMenus::TryGet())
		{
			UToolMenus::UnregisterOwner(TEXT("DoomUEEditor"));
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDoomUEEditorModule, DoomUEEditor);
