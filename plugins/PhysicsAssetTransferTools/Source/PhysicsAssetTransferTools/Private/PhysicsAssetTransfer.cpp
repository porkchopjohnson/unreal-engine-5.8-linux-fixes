#include "PhysicsAssetTransfer.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "PhysicsAssetTransfer"

bool FPhysicsAssetTransfer::Validate(const UPhysicsAsset* Source, const UPhysicsAsset* Destination, FText& OutError)
{
	OutError = FText::GetEmpty();
	if (!Source || !Destination || Source == Destination)
	{
		OutError = LOCTEXT("DifferentAssets", "Choose a source physics asset different from the destination.");
		return false;
	}
	const USkeletalMesh* Mesh = Destination->GetPreviewMesh();
	if (!Mesh || Mesh->GetRefSkeleton().GetNum() == 0)
	{
		OutError = LOCTEXT("NoMesh", "Set the destination physics asset's preview skeletal mesh first. Its reference pose is used for snapping.");
		return false;
	}
	if (Source->SkeletalBodySetups.IsEmpty())
	{
		OutError = LOCTEXT("EmptySource", "The source physics asset contains no bodies.");
		return false;
	}

	TSet<FName> BodyNames;
	TSet<FName> MissingBones;
	for (const USkeletalBodySetup* Body : Source->SkeletalBodySetups)
	{
		if (!Body || Body->BoneName.IsNone() || BodyNames.Contains(Body->BoneName))
		{
			OutError = LOCTEXT("InvalidBodies", "The source contains a null body, an unnamed body, or multiple bodies for the same bone.");
			return false;
		}
		BodyNames.Add(Body->BoneName);
		if (Mesh->GetRefSkeleton().FindBoneIndex(Body->BoneName) == INDEX_NONE)
		{
			MissingBones.Add(Body->BoneName);
		}
	}
	for (const UPhysicsConstraintTemplate* Joint : Source->ConstraintSetup)
	{
		if (!Joint)
		{
			OutError = LOCTEXT("NullJoint", "The source contains a null constraint.");
			return false;
		}
		for (FName Bone : { Joint->DefaultInstance.ConstraintBone1, Joint->DefaultInstance.ConstraintBone2 })
		{
			if (Mesh->GetRefSkeleton().FindBoneIndex(Bone) == INDEX_NONE)
			{
				MissingBones.Add(Bone);
			}
		}
	}
	if (!MissingBones.IsEmpty())
	{
		TArray<FString> Names;
		for (FName Bone : MissingBones) { Names.Add(Bone.ToString()); }
		Names.Sort();
		OutError = FText::Format(LOCTEXT("MissingBones", "The destination mesh is missing source bones: {0}. No setup was replaced."),
			FText::FromString(FString::Join(Names, TEXT(", "))));
		return false;
	}
	for (const auto& Pair : Source->CollisionDisableTable)
	{
		if (!Source->SkeletalBodySetups.IsValidIndex(Pair.Key.Indices[0]) ||
			!Source->SkeletalBodySetups.IsValidIndex(Pair.Key.Indices[1]))
		{
			OutError = LOCTEXT("InvalidCollisionPair", "The source contains a disabled collision pair with an invalid body index.");
			return false;
		}
	}
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		if (It->GetPhysicsAsset() == Destination && It->IsAnySimulatingPhysics())
		{
			OutError = LOCTEXT("Simulating", "Stop simulation on the destination physics asset before replacing its setup.");
			return false;
		}
	}
	return true;
}

bool FPhysicsAssetTransfer::Replace(const UPhysicsAsset* Source, UPhysicsAsset* Destination, FText& OutError,
	TFunction<void()> BeforeReplace)
{
	if (!Validate(Source, Destination, OutError)) { return false; }

	const FScopedTransaction Transaction(LOCTEXT("Transaction", "Copy Physics Setup and Snap Constraints"));
	if (BeforeReplace) { BeforeReplace(); }
	Destination->SetFlags(RF_Transactional);
	Destination->Modify();
	Destination->PreEditChange(nullptr);

	TArray<TObjectPtr<USkeletalBodySetup>> Bodies;
	TArray<TObjectPtr<UPhysicsConstraintTemplate>> Joints;
	for (const USkeletalBodySetup* SourceBody : Source->SkeletalBodySetups)
	{
		USkeletalBodySetup* Body = DuplicateObject<USkeletalBodySetup>(SourceBody, Destination,
			MakeUniqueObjectName(Destination, SourceBody->GetClass(), TEXT("TransferredBody")));
		Body->SetFlags(RF_Transactional);
		Body->InvalidatePhysicsData();
		Bodies.Add(Body);
	}
	for (const UPhysicsConstraintTemplate* SourceJoint : Source->ConstraintSetup)
	{
		UPhysicsConstraintTemplate* Joint = DuplicateObject<UPhysicsConstraintTemplate>(SourceJoint, Destination,
			MakeUniqueObjectName(Destination, SourceJoint->GetClass(), TEXT("TransferredConstraint")));
		Joint->SetFlags(RF_Transactional);
		Joint->DefaultInstance.ConstraintHandle.Reset();
		Joint->DefaultInstance.ConstraintIndex = Joints.Num();
		// A source editor may currently be displaying a named profile. Preserve its
		// saved default as well as all named profiles, rather than baking that preview in.
		SourceJoint->ApplyConstraintProfile(NAME_None, Joint->DefaultInstance, true);
		Joint->DefaultInstance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, Destination);
		Joint->SetDefaultProfile(Joint->DefaultInstance);
		Joints.Add(Joint);
	}

	Destination->SkeletalBodySetups = MoveTemp(Bodies);
	Destination->ConstraintSetup = MoveTemp(Joints);
	// Body order is preserved, so the source's indexed collision pairs still map exactly.
	Destination->CollisionDisableTable = Source->CollisionDisableTable;
	Destination->PhysicalAnimationProfiles = Source->PhysicalAnimationProfiles;
	Destination->ConstraintProfiles = Source->ConstraintProfiles;
	Destination->CurrentConstraintProfileName = NAME_None;
	Destination->CurrentPhysicalAnimationProfileName = NAME_None;
	Destination->EditorBodyFlags.Init(0, Destination->SkeletalBodySetups.Num());
	Destination->UpdateBodySetupIndexMap();
	Destination->UpdateBoundsBodiesArray();
	Destination->PostEditChange();
	Destination->MarkPackageDirty();
	return true;
}

#undef LOCTEXT_NAMESPACE
