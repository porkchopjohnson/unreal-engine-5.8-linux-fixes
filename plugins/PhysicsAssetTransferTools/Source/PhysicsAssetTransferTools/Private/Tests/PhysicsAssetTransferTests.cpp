#include "PhysicsAssetTransfer.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "UObject/StrongObjectPtr.h"

namespace PhysicsAssetTransferTests
{
	USkeletalBodySetup* AddBody(UPhysicsAsset* Asset, FName Bone, float Radius)
	{
		USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transactional);
		Body->BoneName = Bone;
		Body->AggGeom.SphereElems.Add(FKSphereElem(Radius));
		Body->AggGeom.SphereElems[0].Center = FVector(3, 4, 5);
		Asset->SkeletalBodySetups.Add(Body);
		Asset->UpdateBodySetupIndexMap();
		return Body;
	}

	USkeletalMesh* MakeMesh(bool bIncludeChild)
	{
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform(FRotator(0, 20, 0), FVector(30, 0, 0)));
		if (bIncludeChild)
		{
			Modifier.Add(FMeshBoneInfo(TEXT("middle"), TEXT("middle"), 0), FTransform(FRotator(0, 0, 30), FVector(0, 40, 0)));
			Modifier.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 1), FTransform(FRotator(10, 0, 0), FVector(0, 0, 70)));
		}
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsAssetTransferReplacementTest,
	"Editor.PhysicsAssetTransfer.ReplaceSnapAndUndo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsAssetTransferReplacementTest::RunTest(const FString& Parameters)
{
	using namespace PhysicsAssetTransferTests;
	TStrongObjectPtr<UPhysicsAsset> Source(NewObject<UPhysicsAsset>(GetTransientPackage(), NAME_None, RF_Transactional));
	TStrongObjectPtr<UPhysicsAsset> Target(NewObject<UPhysicsAsset>(GetTransientPackage(), NAME_None, RF_Transactional));
	TStrongObjectPtr<USkeletalMesh> Mesh(MakeMesh(true));
	Target->SetPreviewMesh(Mesh.Get(), false);
	// Deliberately not skeleton order: indexed collision pairs must follow body order.
	USkeletalBodySetup* SourceChild = AddBody(Source.Get(), TEXT("child"), 11);
	AddBody(Source.Get(), TEXT("root"), 22);
	Source->DisableCollision(0, 1);
	Source->PhysicalAnimationProfiles.Add(TEXT("Muscles"));
	SourceChild->AddPhysicalAnimationProfile(TEXT("Muscles"));
	SourceChild->FindPhysicalAnimationProfile(TEXT("Muscles"))->PhysicalAnimationData.OrientationStrength = 123;
	UPhysicsConstraintTemplate* SourceJoint = NewObject<UPhysicsConstraintTemplate>(Source.Get());
	Source->ConstraintSetup.Add(SourceJoint);
	SourceJoint->DefaultInstance.JointName = TEXT("child_joint");
	SourceJoint->DefaultInstance.ConstraintBone1 = TEXT("child");
	SourceJoint->DefaultInstance.ConstraintBone2 = TEXT("root");
	SourceJoint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FTransform(FVector(200, 300, 400)));
	SourceJoint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame2, FTransform(FVector(800, 900, 1000)));
	SourceJoint->DefaultInstance.SetAngularSwing1Limit(ACM_Limited, 17);
	SourceJoint->SetDefaultProfile(SourceJoint->DefaultInstance);
	Source->ConstraintProfiles.Add(TEXT("Loose"));
	SourceJoint->DefaultInstance.SetAngularSwing1Limit(ACM_Limited, 61);
	SourceJoint->AddConstraintProfile(TEXT("Loose"));
	Source->CurrentConstraintProfileName = TEXT("Loose");
	const FTransform SourceFrame = SourceJoint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2);
	USkeletalBodySetup* OldBody = AddBody(Target.Get(), TEXT("middle"), 99);
	Target->ConstraintProfiles.Add(TEXT("OldProfile"));
	Target->SolverSettings.PositionIterations = 37;

	FText Error;
	bool bBeforeReplaceCalled = false;
	if (!TestTrue(TEXT("Transfer succeeds"), FPhysicsAssetTransfer::Replace(Source.Get(), Target.Get(), Error,
		[&]() { bBeforeReplaceCalled = true; })))
	{
		AddError(Error.ToString());
		return false;
	}
	TestTrue(TEXT("Selection hook runs"), bBeforeReplaceCalled);
	TestEqual(TEXT("Entire old setup replaced"), Target->SkeletalBodySetups.Num(), 2);
	TestEqual(TEXT("One joint copied"), Target->ConstraintSetup.Num(), 1);
	TestTrue(TEXT("Destination preview mesh retained"), Target->GetPreviewMesh() == Mesh.Get());
	TestEqual(TEXT("Destination solver settings retained"), Target->SolverSettings.PositionIterations, 37);
	TestTrue(TEXT("Body independently owned by destination"), Target->SkeletalBodySetups[0] != SourceChild && Target->SkeletalBodySetups[0]->GetOuter() == Target.Get());
	TestEqual(TEXT("Shape size not rescaled"), Target->SkeletalBodySetups[0]->AggGeom.SphereElems[0].Radius, 11.f);
	TestEqual(TEXT("Bone-local shape offset retained"), Target->SkeletalBodySetups[0]->AggGeom.SphereElems[0].Center, FVector(3, 4, 5));
	TestFalse(TEXT("Disabled collision preserved despite body/skeleton ordering"), Target->IsCollisionEnabled(0, 1));
	TestEqual(TEXT("Body name cache rebuilt"), Target->FindBodyIndex(TEXT("child")), 0);
	TestEqual(TEXT("Old body removed from name cache"), Target->FindBodyIndex(TEXT("middle")), INDEX_NONE);
	TestEqual(TEXT("Named constraint profiles copied"), Target->ConstraintProfiles[0], FName(TEXT("Loose")));
	TestEqual(TEXT("Physical animation profile copied"), Target->SkeletalBodySetups[0]->FindPhysicalAnimationProfile(TEXT("Muscles"))->PhysicalAnimationData.OrientationStrength, 123.f);
	const FConstraintInstance& Joint = Target->ConstraintSetup[0]->DefaultInstance;
	TestTrue(TEXT("Child constraint frame snaps to bone origin and axes"), Joint.GetRefFrame(EConstraintFrame::Frame1).Equals(FTransform::Identity));
	const auto& Pose = Mesh->GetRefSkeleton().GetRefBonePose();
	TestTrue(TEXT("Parent frame uses destination proportions and rotations across intermediate bones"),
		Joint.GetRefFrame(EConstraintFrame::Frame2).Equals(Pose[2] * Pose[1], .001));
	TestEqual(TEXT("Saved default limit survives active source named profile"), Joint.GetAngularSwing1Limit(), 17.f);
	TestEqual(TEXT("Named profile limit retained"), Target->ConstraintSetup[0]->GetConstraintProfilePropertiesOrDefault(TEXT("Loose")).ConeLimit.Swing1LimitDegrees, 61.f);
	TestTrue(TEXT("Source frames untouched"), SourceJoint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2).Equals(SourceFrame));
	TestEqual(TEXT("Source active profile untouched"), SourceJoint->DefaultInstance.GetAngularSwing1Limit(), 61.f);
	TestTrue(TEXT("Undo available"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores original body object"), Target->SkeletalBodySetups.Num() == 1 && Target->SkeletalBodySetups[0] == OldBody);
	TestEqual(TEXT("Undo restores old profiles"), Target->ConstraintProfiles[0], FName(TEXT("OldProfile")));
	TestTrue(TEXT("Undo restores collision table"), Target->CollisionDisableTable.IsEmpty());
	TestTrue(TEXT("Redo available"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restores transferred bodies"), Target->SkeletalBodySetups.Num(), 2);
	TestTrue(TEXT("Redo restores disabled pairs"), !Target->IsCollisionEnabled(0, 1));
	GEditor->UndoTransaction();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsAssetTransferValidationTest,
	"Editor.PhysicsAssetTransfer.ValidationIsAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsAssetTransferValidationTest::RunTest(const FString& Parameters)
{
	using namespace PhysicsAssetTransferTests;
	TStrongObjectPtr<UPhysicsAsset> Source(NewObject<UPhysicsAsset>());
	TStrongObjectPtr<UPhysicsAsset> Target(NewObject<UPhysicsAsset>());
	TStrongObjectPtr<USkeletalMesh> Mesh(MakeMesh(false));
	Target->SetPreviewMesh(Mesh.Get(), false);
	AddBody(Source.Get(), TEXT("child"), 7);
	USkeletalBodySetup* Original = AddBody(Target.Get(), TEXT("root"), 8);
	FText Error;
	bool bCallback = false;
	TestFalse(TEXT("Missing destination bone rejects complete replacement"), FPhysicsAssetTransfer::Replace(Source.Get(), Target.Get(), Error, [&]() { bCallback = true; }));
	TestTrue(TEXT("Missing bone reported"), Error.ToString().Contains(TEXT("child")));
	TestFalse(TEXT("Validation failure never clears editor selection"), bCallback);
	TestTrue(TEXT("Validation failure leaves setup intact"), Target->SkeletalBodySetups.Num() == 1 && Target->SkeletalBodySetups[0] == Original);
	TestFalse(TEXT("Self transfer rejected"), FPhysicsAssetTransfer::Replace(Target.Get(), Target.Get(), Error));
	Target->SetPreviewMesh(nullptr, false);
	TestFalse(TEXT("Missing target preview mesh rejected"), FPhysicsAssetTransfer::Replace(Source.Get(), Target.Get(), Error));
	return true;
}

#endif
