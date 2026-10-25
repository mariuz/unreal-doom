#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "DoomContentBuilder.h"
#include "DoomMapActors.h"
#include "ToolMenus.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "MessageLogModule.h"
#include "Editor.h"
#include "DoomMapCheck.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "DoomEditor.h"
#include "DoomUE.h"

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

	/** The map checker's findings on the "Doom Map Check" page of the Message Log: each with its actor
	 *  (click to select it) and a "Go there" link that moves the viewport camera to the spot. */
	void ReportMapCheck(ADoomMapActor* MapActor, const TArray<FDoomMapIssue>& Issues, const TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		FMessageLog Log(TEXT("DoomMapCheck"));
		Log.NewPage(FText::FromString(MapActor->MapName));
		for (int32 i = 0; i < Issues.Num(); ++i)
		{
			const FDoomMapIssue& I = Issues[i];
			const EMessageSeverity::Type Severity = I.Severity == FDoomMapIssue::ESeverity::Error ? EMessageSeverity::Error
				: I.Severity == FDoomMapIssue::ESeverity::Warning ? EMessageSeverity::Warning : EMessageSeverity::Info;
			TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity);
			if (Actors.IsValidIndex(i) && Actors[i].IsValid())
			{
				Message->AddToken(FUObjectToken::Create(Actors[i].Get()));
			}
			Message->AddToken(FTextToken::Create(I.Text));
			if (I.Line != INDEX_NONE || I.Sector != INDEX_NONE || I.Thing != INDEX_NONE)
			{
				const FVector Where = Doom::ToUE(I.Where, I.Z);
				Message->AddToken(FActionToken::Create(LOCTEXT("GoThere", "Go there"), LOCTEXT("GoThereTip", "Move the viewport camera to it"),
					FOnActionTokenExecuted::CreateLambda([Where]()
					{
						if (GEditor)
						{
							GEditor->MoveViewportCamerasToBox(FBox(Where - FVector(300.f), Where + FVector(300.f)), true);
						}
					})));
			}
			Log.AddMessage(Message);
		}
		const int32 Errors = DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Error);
		const int32 Warnings = DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Warning);
		if (Issues.Num() == 0)
		{
			Log.Info(FText::Format(LOCTEXT("CheckClean", "{0}: no problems found"), FText::FromString(MapActor->MapName)));
		}
		Log.Open(EMessageSeverity::Info, true);
		Notify(FText::Format(LOCTEXT("Checked", "{0}: {1} errors, {2} warnings (Message Log, Doom Map Check)"),
			FText::FromString(MapActor->MapName), Errors, Warnings), Errors == 0);
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
			Section.AddMenuEntry(FName(*Name), (bImported ? FText::FromString(Name) : FText::Format(LOCTEXT("ImportEntry", "{0}  (import)"), FText::FromString(Name))), FText::GetEmpty(), FSlateIcon(),
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
		Maps.AddMenuEntry(TEXT("CheckMap"), LOCTEXT("CheckMap", "Check Map"),
			LOCTEXT("CheckMapTip", "List the open map's problems in the Message Log (tags with no sectors, missing textures, open sectors, locks without keys...)"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				ADoomMapActor* MapActor = GEditor ? ADoomMapActor::Find(GEditor->GetEditorWorldContext().World()) : nullptr;
				if (MapActor)
				{
					MapActor->CheckMap();
				}
				else
				{
					Notify(LOCTEXT("CheckNoMap", "Open a Doom map level first (Doom > Open / Import Map)"), false);
				}
			})));
		Maps.AddMenuEntry(TEXT("EditInDoomBuilder"), LOCTEXT("EditInDoomBuilder", "Edit in Doom Builder"),
			LOCTEXT("EditInDoomBuilderTip", "Open the map in Ultimate Doom Builder (an IWAD map is exported with this level's edits as a PWAD first); saving there updates this level"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				ADoomMapActor* MapActor = GEditor ? ADoomMapActor::Find(GEditor->GetEditorWorldContext().World()) : nullptr;
				if (MapActor)
				{
					MapActor->EditInDoomBuilder();
					Notify(FText::Format(LOCTEXT("EditingIn", "{0}: editing {1}; saves there reload here"), FText::FromString(MapActor->MapName),
						FText::FromString(FPaths::GetCleanFilename(DoomEditorData::MapSource(MapActor->MapName)))), true);
				}
				else
				{
					Notify(LOCTEXT("EditNoMap", "Open a Doom map level first (Doom > Open / Import Map)"), false);
				}
			})));
		Maps.AddMenuEntry(TEXT("PlayMapEditor"), LOCTEXT("PlayMapEditor", "Play in the Map Editor"),
			LOCTEXT("PlayMapEditorTip", "Play In Editor with Doom's map editor open over the game: draw sectors, place things, edit properties, undo, generate random maps and save PWADs (DoomEdit in the console toggles it)"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				if (GEditor && !GEditor->IsPlaySessionInProgress())
				{
					UDoomMapEditor::bOpenOnNextPlay = true;
					GEditor->RequestPlaySession(FRequestPlaySessionParams());
				}
			})));
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
			FMessageLogModule& MessageLog = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
			MessageLog.RegisterLogListing(TEXT("DoomMapCheck"), LOCTEXT("MapCheckLog", "Doom Map Check"));
			ADoomMapActor::CheckReportHook = &ReportMapCheck;
			ADoomMapActor::BakeHook = [](ADoomMapActor* MapActor)
			{
				const bool bOk = DoomContent::BakeMapTiles(MapActor);
				Notify(bOk ? FText::Format(LOCTEXT("Baked", "Baked {0}'s static mesh tiles (save the level to keep them)"), FText::FromString(MapActor->MapName))
					: LOCTEXT("BakeFailed", "Baking failed (see the Output Log)"), bOk);
				return bOk;
			};
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
			{
				FToolMenuOwnerScoped Owner(TEXT("DoomUEEditor"));
				RegisterMenus();
			}));
			// the round trip with a Doom editor: a WAD saved there reloads here, and the open level's
			// preview follows (its markers and edits stay)
			WatchTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				if (GEditor && !GEditor->IsPlaySessionInProgress() && DoomEditorData::ReloadIfChanged())
				{
					UWorld* World = GEditor->GetEditorWorldContext().World();
					for (TActorIterator<ADoomMapActor> It(World); It; ++It)
					{
						It->RebuildPreview();
						Notify(FText::Format(LOCTEXT("WadReloaded", "{0} changed on disk: {1} reloaded"),
							FText::FromString(FPaths::GetCleanFilename(DoomEditorData::MapSource(It->MapName))), FText::FromString(It->MapName)), true);
					}
				}
				return true;
			}), 1.5f);
		}
	}
	FTSTicker::FDelegateHandle WatchTicker;

	virtual void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WatchTicker);
		ADoomMapActor::BakeHook = nullptr;
		ADoomMapActor::CheckReportHook = nullptr;
		if (UObjectInitialized() && UToolMenus::TryGet())
		{
			UToolMenus::UnregisterOwner(TEXT("DoomUEEditor"));
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDoomUEEditorModule, DoomUEEditor);
