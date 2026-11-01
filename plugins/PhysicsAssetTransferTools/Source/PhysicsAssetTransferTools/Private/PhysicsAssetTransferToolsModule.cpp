#include "Modules/ModuleManager.h"
#include "PhysicsAssetTransfer.h"

#include "Animation/DebugSkelMeshComponent.h"
#include "AssetRegistry/AssetData.h"
#include "Framework/Application/SlateApplication.h"
#include "IEditableSkeleton.h"
#include "IPhysicsAssetEditor.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PropertyCustomizationHelpers.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "Toolkits/IToolkitHost.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PhysicsAssetTransferTools"

namespace PhysicsAssetTransferUI
{
	struct FDialogState
	{
		TStrongObjectPtr<UPhysicsAsset> Source;
		FText Error;
	};

	void ShowDialog(TWeakPtr<IPhysicsAssetEditor> WeakEditor, TWeakObjectPtr<UPhysicsAsset> WeakDestination)
	{
		const TSharedPtr<IPhysicsAssetEditor> Editor = WeakEditor.Pin();
		if (!Editor || !WeakDestination.IsValid()) { return; }
		const TSharedRef<FDialogState> State = MakeShared<FDialogState>();
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("WindowTitle", "Copy Physics Setup"))
			.ClientSize(FVector2D(610, 310)).SupportsMaximize(false).SupportsMinimize(false)
			.SizingRule(ESizingRule::Autosized);
		TWeakPtr<SWindow> WeakWindow = Window;
		Window->SetContent(
			SNew(SBorder).Padding(18)
			[
				SNew(SBox).WidthOverride(580)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[
						SNew(STextBlock).Text(FText::Format(LOCTEXT("Destination", "Destination: {0}"), FText::FromString(WeakDestination->GetPathName()))).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ SNew(STextBlock).Text(LOCTEXT("Source", "Copy from physics asset:")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
					[
						SNew(SObjectPropertyEntryBox).AllowedClass(UPhysicsAsset::StaticClass())
						.ObjectPath_Lambda([State]() { return State->Source.IsValid() ? State->Source->GetPathName() : FString(); })
						.OnObjectChanged_Lambda([State](const FAssetData& Asset)
						{
							State->Source.Reset(Cast<UPhysicsAsset>(Asset.GetAsset()));
							State->Error = FText::GetEmpty();
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[
						SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("Behavior",
							"Replaces the entire destination body and constraint setup, including profiles and disabled collision pairs. "
							"Body shapes keep their bone-local sizes and offsets. All constraint positions and rotations snap to the destination mesh's reference pose. "
							"Undo is supported. Save the destination when satisfied."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(1, .3f, .2f)).Text_Lambda([State]() { return State->Error; }) ]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SButton).Text(LOCTEXT("Cancel", "Cancel")).OnClicked_Lambda([WeakWindow]()
							{
								if (auto Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); }
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton).Text(LOCTEXT("Transfer", "Replace Setup and Snap"))
							.IsEnabled_Lambda([State]() { return State->Source.IsValid(); })
							.OnClicked_Lambda([State, WeakEditor, WeakDestination, WeakWindow]()
							{
								const TSharedPtr<IPhysicsAssetEditor> PinnedEditor = WeakEditor.Pin();
								if (!PinnedEditor || !WeakDestination.IsValid()) { return FReply::Handled(); }
								if (FPhysicsAssetTransfer::Replace(State->Source.Get(), WeakDestination.Get(), State->Error,
									[PinnedEditor]() { PinnedEditor->ClearSelection(); }))
								{
									const auto Toolkit = PinnedEditor->GetPersonaToolkit();
									if (const auto Skeleton = Toolkit->GetEditableSkeleton()) { Skeleton->RefreshBoneTree(); }
									if (UDebugSkelMeshComponent* Component = Toolkit->GetPreviewMeshComponent()) { Component->RecreatePhysicsState(); }
									Toolkit->GetPreviewScene()->InvalidateViews();
									FNotificationInfo Info(FText::Format(LOCTEXT("Copied", "Copied {0} bodies and snapped {1} constraints. Use Undo to revert."),
										FText::AsNumber(WeakDestination->SkeletalBodySetups.Num()), FText::AsNumber(WeakDestination->ConstraintSetup.Num())));
									Info.ExpireDuration = 6;
									FSlateNotificationManager::Get().AddNotification(Info);
									if (auto Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); }
								}
								return FReply::Handled();
							})
						]
					]
				]
			]);
		FSlateApplication::Get().AddModalWindow(Window, Editor->GetToolkitHost()->GetParentWidget());
	}
}

class FPhysicsAssetTransferToolsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FPhysicsAssetTransferToolsModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("AssetEditor.PhysicsAssetEditor.ToolBar");
		Toolbar->AddDynamicSection("PhysicsAssetTransfer", FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			const UAssetEditorToolkitMenuContext* Context = Menu->FindContext<UAssetEditorToolkitMenuContext>();
			const auto Toolkit = Context ? Context->Toolkit.Pin() : nullptr;
			if (!Toolkit || Toolkit->GetToolkitFName() != "PhysicsAssetEditor") { return; }
			TWeakObjectPtr<UPhysicsAsset> Destination;
			for (UObject* Object : Context->GetEditingObjects())
			{
				if (UPhysicsAsset* Asset = Cast<UPhysicsAsset>(Object)) { Destination = Asset; break; }
			}
			if (!Destination.IsValid()) { return; }
			TWeakPtr<IPhysicsAssetEditor> Editor = StaticCastSharedPtr<IPhysicsAssetEditor>(Toolkit);
			FToolMenuSection& Section = Menu->AddSection("PhysicsAssetTransfer", FText(), FToolMenuInsert("Asset", EToolMenuInsertType::After));
			Section.AddEntry(FToolMenuEntry::InitToolBarButton("CopyPhysicsSetup",
				FUIAction(FExecuteAction::CreateLambda([Editor, Destination]() { PhysicsAssetTransferUI::ShowDialog(Editor, Destination); })),
				LOCTEXT("ToolbarLabel", "Copy Physics Setup"),
				LOCTEXT("ToolbarTip", "Copy another physics asset's entire setup and snap constraints to this asset's reference-pose bones."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy")));
		}));
	}
};

IMPLEMENT_MODULE(FPhysicsAssetTransferToolsModule, PhysicsAssetTransferTools)

#undef LOCTEXT_NAMESPACE
