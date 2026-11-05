#include "Modules/ModuleManager.h"

#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Input/DragAndDrop.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IContentBrowserSingleton.h"
#include "ImportedPerformanceUtils.h"
#include "MetaHumanPerformance.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "VideoImportJob.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MetaHumanVideoImporter"

namespace
{
const FName ImporterTabName(TEXT("MetaHumanVideoImporter"));

class SMetaHumanVideoImporter : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMetaHumanVideoImporter) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBorder).Padding(16)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
				[
					SNew(STextBlock).Text(LOCTEXT("Instructions", "Drop video files here, or add them with the file picker.\nCreates project-local PNG frames, optional audio, Footage Capture Data and a monocular Performance asset."))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[ SNew(STextBlock).Text(LOCTEXT("AssetFolder", "Asset folder")) ]
					+ SHorizontalBox::Slot().FillWidth(1)
					[
						SAssignNew(Destination, SEditableTextBox).Text(FText::FromString(TEXT("/Game/MetaHumanFootage")))
						.IsEnabled_Lambda([this] { return !bRunning; })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock).Text(LOCTEXT("MediaFolder", "Media output: <Project>/SourceFootage/MetaHuman/<Take>/\nFFmpeg and FFprobe must be installed in /usr/bin. Variable-frame-rate footage is resampled to a constant rate."))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bOpenPerformance = State == ECheckBoxState::Checked; })
					[ SNew(STextBlock).Text(LOCTEXT("OpenPerformance", "Open the last Performance asset when the batch finishes")) ]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton).Text(LOCTEXT("Add", "Add videos…"))
						.IsEnabled_Lambda([this] { return !bRunning; })
						.OnClicked(this, &SMetaHumanVideoImporter::PickFiles)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton).Text(LOCTEXT("Import", "Import"))
						.IsEnabled_Lambda([this] { return !bRunning && !Files.IsEmpty(); })
						.OnClicked_Lambda([this]
						{
							bRunning = true;
							FileIndex = Succeeded = Failed = 0;
							LastPerformance.Reset();
							Log.Reset();
							AssetRoot = Destination->GetText().ToString().TrimStartAndEnd();
							AssetRoot.RemoveFromEnd(TEXT("/"));
							StartNext();
							RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SMetaHumanVideoImporter::TickImport));
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton).Text(LOCTEXT("Clear", "Clear queue"))
						.IsEnabled_Lambda([this] { return !bRunning; })
						.OnClicked_Lambda([this] { Files.Reset(); RefreshQueue(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton).Text(LOCTEXT("Cancel", "Cancel"))
						.IsEnabled_Lambda([this] { return bRunning; })
						.OnClicked_Lambda([this]
						{
							if (Job) { Job->Cancel(); Log += Job->GetResult() + TEXT("\n"); Job.Reset(); }
							bRunning = false;
							Status = TEXT("Cancelled. Completed takes are saved; the unfinished conversion was removed.");
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(SProgressBar).Percent_Lambda([this]() -> TOptional<float>
					{
						return bRunning && !Files.IsEmpty() ? (FileIndex + (Job ? Job->GetProgress() : 0.f)) / Files.Num() : 0.f;
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Job ? Job->GetStatus() : Status); }).AutoWrapText(true) ]
				+ SVerticalBox::Slot().FillHeight(0.4f)
				[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(QueueText, STextBlock).AutoWrapText(true) ] ]
				+ SVerticalBox::Slot().FillHeight(0.6f).Padding(0, 8, 0, 0)
				[ SNew(SScrollBox) + SScrollBox::Slot()[ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Log); }).AutoWrapText(true) ] ]
			]
		];
		RefreshQueue();
	}

	virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
	{
		const TSharedPtr<FExternalDragOperation> External = Event.GetOperationAs<FExternalDragOperation>();
		if (!bRunning && External.IsValid() && External->HasFiles())
		{
			AddFiles(External->GetFiles());
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual void OnDragEnter(const FGeometry&, const FDragDropEvent& Event) override
	{
		if (!bRunning && Event.GetOperationAs<FExternalDragOperation>().IsValid())
		{
			Status = TEXT("Drop videos to add them to the import queue.");
		}
	}

private:
	FReply PickFiles()
	{
		TArray<FString> Picked;
		if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
		{
			Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
				TEXT("Select videos"), FPaths::ProjectDir(), TEXT(""),
				TEXT("Video files|*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.MP4;*.MOV|All files|*"),
				EFileDialogFlags::Multiple, Picked);
		}
		AddFiles(Picked);
		return FReply::Handled();
	}

	void AddFiles(const TArray<FString>& Added)
	{
		for (const FString& File : Added)
		{
			if (FPaths::FileExists(File)) { Files.AddUnique(FPaths::ConvertRelativePathToFull(File)); }
		}
		RefreshQueue();
	}

	void RefreshQueue()
	{
		QueueText->SetText(FText::FromString(Files.IsEmpty() ? TEXT("Drop videos here.") : FString::Join(Files, TEXT("\n"))));
	}

	void StartNext()
	{
		Job = MakeUnique<FVideoImportJob>(Files[FileIndex], AssetRoot);
		Job->Start();
	}

	EActiveTimerReturnType TickImport(double, float)
	{
		if (!bRunning) { return EActiveTimerReturnType::Stop; }
		Job->Tick();
		if (!Job->IsFinished()) { return EActiveTimerReturnType::Continue; }
		Log += Job->GetResult() + TEXT("\n\n");
		if (Job->WasSuccessful())
		{
			++Succeeded;
			LastPerformance = Job->GetPerformance();
		}
		else { ++Failed; }
		Job.Reset();
		if (++FileIndex < Files.Num())
		{
			StartNext();
			return EActiveTimerReturnType::Continue;
		}
		bRunning = false;
		Status = FString::Printf(TEXT("Finished: %d imported, %d failed. Open a Performance asset and click Process."), Succeeded, Failed);
		if (LastPerformance.IsValid() && GEditor)
		{
			const TArray<UObject*> Assets = {LastPerformance.Get()};
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(Assets);
			if (bOpenPerformance) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(LastPerformance.Get()); }
		}
		return EActiveTimerReturnType::Stop;
	}

	TSharedPtr<SEditableTextBox> Destination;
	TSharedPtr<STextBlock> QueueText;
	TArray<FString> Files;
	TUniquePtr<FVideoImportJob> Job;
	TWeakObjectPtr<UMetaHumanPerformance> LastPerformance;
	FString AssetRoot, Log, Status;
	int32 FileIndex = 0, Succeeded = 0, Failed = 0;
	bool bRunning = false;
	bool bOpenPerformance = true;
};
}

class FMetaHumanVideoImporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (IsRunningCommandlet()) { return; }
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ImporterTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SMetaHumanVideoImporter)];
		})).SetDisplayName(LOCTEXT("Title", "MetaHuman Video Importer")).SetMenuType(ETabSpawnerMenuType::Hidden);
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMetaHumanVideoImporterModule::RegisterMenus));
		if (GEditor)
		{
			AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			AssetEditorSubsystem->OnAssetOpenedInEditor().AddRaw(this, &FMetaHumanVideoImporterModule::OnAssetOpened);
		}
	}

	virtual void ShutdownModule() override
	{
		if (IsRunningCommandlet()) { return; }
		if (AssetEditorSubsystem.IsValid())
		{
			AssetEditorSubsystem->OnAssetOpenedInEditor().RemoveAll(this);
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(ImporterTabName))
		{
			Tab->RequestCloseTab();
			Tab->SetContent(SNullWidget::NullWidget);
		}
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ImporterTabName);
	}

private:
	void OnAssetOpened(UObject* Asset, IAssetEditorInstance*)
	{
		UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(Cast<UMetaHumanPerformance>(Asset));
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"))->FindOrAddSection(TEXT("MetaHuman"))
			.AddMenuEntry(TEXT("MetaHumanVideoImporter"), LOCTEXT("Menu", "MetaHuman Video Importer"),
				LOCTEXT("Tooltip", "Convert videos and create MetaHuman Performance assets"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(ImporterTabName); })));
	}

	TWeakObjectPtr<UAssetEditorSubsystem> AssetEditorSubsystem;
};

IMPLEMENT_MODULE(FMetaHumanVideoImporterModule, MetaHumanVideoImporter)

#undef LOCTEXT_NAMESPACE
