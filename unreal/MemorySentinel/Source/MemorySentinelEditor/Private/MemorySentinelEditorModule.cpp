// The editor side (UC-4.1, UC-1.7): Tools > Memory Sentinel opens the panel as a tab, and a double-clicked source finding
// opens the IDE at its line.
#include "Modules/ModuleManager.h"

#include "SSentinelPanel.h"
#include "SourceCodeNavigation.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MemorySentinelEditor"

static const FName SentinelTab(TEXT("MemorySentinel"));

class FMemorySentinelEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SentinelTab, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
			return SNew(SDockTab).TabRole(ETabRole::NomadTab).Label(LOCTEXT("Tab", "Memory Sentinel"))
				[
					SNew(SSentinelPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "Memory Sentinel"))
		.SetTooltipText(LOCTEXT("TabTip", "Find dangling UObject pointers, shared pointer cycles, level leaks and heap stomps."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsDebugCategory());

		SSentinelPanel::OpenSourceHook = [](const FString& File, int32 Line) {
			FSourceCodeNavigation::OpenSourceFile(File, Line);
		};

		MenuHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]() {
			FToolMenuOwnerScoped Owner(SentinelTab);
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
			FToolMenuSection& Section = Menu->FindOrAddSection("MemorySentinel", LOCTEXT("Section", "Memory"));
			Section.AddMenuEntry("OpenMemorySentinel", LOCTEXT("Entry", "Memory Sentinel"),
				LOCTEXT("EntryTip", "Scan the source, run a level leak test, and read what the heap sampler found."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(SentinelTab); })));
		}));
	}

	virtual void ShutdownModule() override
	{
		SSentinelPanel::OpenSourceHook = nullptr;
		if (UToolMenus* Menus = UToolMenus::TryGet())
		{
			UToolMenus::UnRegisterStartupCallback(MenuHandle);
			UToolMenus::UnregisterOwner(SentinelTab);
		}
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SentinelTab);
		}
	}

private:
	FDelegateHandle MenuHandle;
};

IMPLEMENT_MODULE(FMemorySentinelEditorModule, MemorySentinelEditor)

#undef LOCTEXT_NAMESPACE
