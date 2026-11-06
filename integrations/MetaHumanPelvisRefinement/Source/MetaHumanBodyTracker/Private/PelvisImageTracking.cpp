#include "PelvisImageTracking.h"
#include "MetaHumanSMPLX.h"
#include "MetaHumanBodyTrackerLog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace UE::MetaHuman::Pipeline
{
namespace PelvisImageTracking
{
TArray<TSharedPtr<FJsonValue>> Vec(FVector P)
{
	return {MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y), MakeShared<FJsonValueNumber>(P.Z)};
}
TArray<TSharedPtr<FJsonValue>> Vec2(FVector2f P)
{
	return {MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y)};
}
FVector2f Project(FVector P, float Focal, FVector2f Center)
{
	return FVector2f(Focal * P.X / P.Z + Center.X, Focal * P.Y / P.Z + Center.Y);
}
FQuat Rotation(TConstArrayView64<float> Pose, int32 Frame)
{
	FVector AA(Pose[int64(Frame) * 165], Pose[int64(Frame) * 165 + 1], Pose[int64(Frame) * 165 + 2]);
	const double Angle = AA.Length();
	return Angle > 1.e-8 ? FQuat(AA / Angle, Angle) : FQuat::Identity;
}
struct FSurfacePoint
{
	FVector Position;
	FVector Lever;
};
}

TArray<FPelvisSurfaceObservation> TrackPelvisSurface(
	FMetaHumanSMPLX& Model, const TArray<float>& Shape,
	TConstArrayView64<float> Pose, TConstArrayView64<float> Translation,
	TConstArrayView64<float> Keypoints, TConstArrayView64<bool> ValidFrames,
	const TArray<TArray<uint8>>& GrayFrames, const TArray<TArray<uint8>>& Masks,
	int32 ImageWidth, int32 ImageHeight, FVector2f RasterSize,
	float Focal, FVector2f Center, float Fps, bool bKeepDiagnostics)
{
	using namespace PelvisImageTracking;
	TArray<FPelvisSurfaceObservation> Result;
	const int32 Count = ValidFrames.Num();
	if (Count < 2 || GrayFrames.Num() != Count || Masks.Num() != Count || ImageWidth < 1 || ImageHeight < 1 ||
		Pose.Num() != int64(Count) * 165 || Translation.Num() != int64(Count) * 3 || Keypoints.Num() != int64(Count) * 75 ||
		RasterSize.X <= 0 || RasterSize.Y <= 0 || Focal <= 0)
	{
		return Result;
	}
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("MetaHumanBodyTracker"));
	if (!Plugin) { return Result; }
	const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/TrackPelvisSurface.py"));
	if (!FPaths::FileExists(Script)) { return Result; }
	Model.SetShape(Shape);
	Model.SetPose(TArray<float>());
	const auto Rest = Model.GetBoneHierarchyUE();
	const FVector Root = FMetaHumanSMPLX::UE2SMPL(Rest[1].Get<2>().GetTranslation());
	const FVector Left = FMetaHumanSMPLX::UE2SMPL(Rest[2].Get<2>().GetTranslation());
	const FVector Right = FMetaHumanSMPLX::UE2SMPL(Rest[3].Get<2>().GetTranslation());
	const auto RestVertices = Model.GetSkinnedVerticesSMPL();
	const auto* ModelData = Model.GetSmplxData();
	if (!ModelData || ModelData->Weights.Num() != RestVertices.Num() * 55) { return Result; }
	TArray<int32> Eligible;
	for (int32 V = 0; V < RestVertices.Num(); ++V)
	{
		// The root column of the column-major skin-weight matrix. A rigid local
		// surface approximation is only used where the pelvis dominates skinning.
		const FVector Offset = RestVertices[V] - Root;
		if (ModelData->Weights[V] >= 0.65f && FMath::Abs(Offset.Y) < 0.20 && FMath::Abs(Offset.X) < 0.25)
		{
			Eligible.Add(V);
		}
	}
	Eligible.Sort([&](int32 A, int32 B) { return ModelData->Weights[A] > ModelData->Weights[B]; });
	TArray<int32> Selected;
	TSet<FIntVector> Cells;
	for (int32 V : Eligible)
	{
		const FVector P = (RestVertices[V] - Root) / 0.025;
		const FIntVector Cell(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y), FMath::FloorToInt(P.Z));
		if (!Cells.Contains(Cell)) { Cells.Add(Cell); Selected.Add(V); }
		if (Selected.Num() == 160) { break; }
	}
	if (Selected.Num() < 6) { return Result; }
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("PelvisTracking") / FGuid::NewGuid().ToString());
	if (!IFileManager::Get().MakeDirectory(*Directory, true)) { return Result; }
	ON_SCOPE_EXIT
	{
		if (!bKeepDiagnostics) { IFileManager::Get().DeleteDirectory(*Directory, false, true); }
	};
	const int32 Pixels = ImageWidth * ImageHeight;
	TUniquePtr<FArchive> Images(IFileManager::Get().CreateFileWriter(*(Directory / TEXT("gray.bin"))));
	TUniquePtr<FArchive> MaskFile(IFileManager::Get().CreateFileWriter(*(Directory / TEXT("mask.bin"))));
	if (!Images || !MaskFile) { return Result; }
	TArray<uint8> Blank;
	Blank.SetNumZeroed(Pixels);
	TArray<TArray<FSurfacePoint>> Points;
	Points.SetNum(Count);
	TArray<TSharedPtr<FJsonValue>> JsonFrames;
	TArray<float> CurrentPose;
	CurrentPose.SetNumUninitialized(165);
	const int32 ZWidth = FMath::CeilToInt(RasterSize.X / 4.f);
	const int32 ZHeight = FMath::CeilToInt(RasterSize.Y / 4.f);
	for (int32 F = 0; F < Count; ++F)
	{
		const bool bValid = ValidFrames[F] && GrayFrames[F].Num() == Pixels && Masks[F].Num() == Pixels;
		Images->Serialize(const_cast<uint8*>(bValid ? GrayFrames[F].GetData() : Blank.GetData()), Pixels);
		MaskFile->Serialize(const_cast<uint8*>(bValid ? Masks[F].GetData() : Blank.GetData()), Pixels);
		auto Row = MakeShared<FJsonObject>();
		Row->SetBoolField(TEXT("valid"), bValid);
		TArray<TSharedPtr<FJsonValue>> JsonPoints;
		TArray<TSharedPtr<FJsonValue>> Hips;
		if (bValid)
		{
			FMemory::Memcpy(CurrentPose.GetData(), Pose.GetData() + int64(F) * 165, 165 * sizeof(float));
			Model.SetPose(CurrentPose);
			const auto& Vertices = Model.GetSkinnedVerticesSMPL();
			const FQuat Q = Rotation(Pose, F);
			const FVector T(Translation[int64(F) * 3], Translation[int64(F) * 3 + 1], Translation[int64(F) * 3 + 2]);
			const FVector2f ObservedLeft(Keypoints[int64(F) * 75 + 36], Keypoints[int64(F) * 75 + 37]);
			const FVector2f ObservedRight(Keypoints[int64(F) * 75 + 27], Keypoints[int64(F) * 75 + 28]);
			const FVector2f ModelCenter = (Project(Root + Q.RotateVector(Left) + T, Focal, Center) + Project(Root + Q.RotateVector(Right) + T, Focal, Center)) * 0.5f;
			const FVector2f Offset = (ObservedLeft + ObservedRight) * 0.5f - ModelCenter;
			Hips.Add(MakeShared<FJsonValueArray>(Vec2(ObservedLeft)));
			Hips.Add(MakeShared<FJsonValueArray>(Vec2(ObservedRight)));
			TArray<double> Depth;
			Depth.Init(TNumericLimits<double>::Max(), ZWidth * ZHeight);
			for (const FVector& V : Vertices)
			{
				const FVector Camera = V + T;
				if (Camera.Z <= 0.05) { continue; }
				const FVector2f Pixel = Project(Camera, Focal, Center);
				const int32 X = FMath::FloorToInt(Pixel.X / 4), Y = FMath::FloorToInt(Pixel.Y / 4);
				if (X >= 0 && X < ZWidth && Y >= 0 && Y < ZHeight) { Depth[Y * ZWidth + X] = FMath::Min(Depth[Y * ZWidth + X], Camera.Z); }
			}
			for (int32 V : Selected)
			{
				FSurfacePoint Point{Vertices[V], Q.UnrotateVector(Vertices[V] - Root)};
				Points[F].Add(Point);
				const FVector Camera = Point.Position + T;
				const FVector2f Projected = Camera.Z > 0.05 ? Project(Camera, Focal, Center) : FVector2f(-1000, -1000);
				const int32 X = FMath::FloorToInt(Projected.X / 4), Y = FMath::FloorToInt(Projected.Y / 4);
				const bool bVisible = X >= 0 && X < ZWidth && Y >= 0 && Y < ZHeight && Camera.Z <= Depth[Y * ZWidth + X] + 0.025;
				auto Item = MakeShared<FJsonObject>();
				Item->SetBoolField(TEXT("visible"), bVisible);
				Item->SetArrayField(TEXT("projected"), Vec2(Projected + Offset));
				Item->SetArrayField(TEXT("position"), Vec(Point.Position));
				Item->SetArrayField(TEXT("lever"), Vec(Point.Lever));
				JsonPoints.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Row->SetArrayField(TEXT("hips"), Hips);
		Row->SetArrayField(TEXT("points"), JsonPoints);
		JsonFrames.Add(MakeShared<FJsonValueObject>(Row));
	}
	const bool bWriteFailed = Images->IsError() || MaskFile->IsError();
	Images.Reset(); MaskFile.Reset();
	if (bWriteFailed) { return Result; }
	auto Document = MakeShared<FJsonObject>();
	Document->SetStringField(TEXT("directory"), Directory);
	Document->SetNumberField(TEXT("width"), ImageWidth);
	Document->SetNumberField(TEXT("height"), ImageHeight);
	Document->SetNumberField(TEXT("fps"), Fps);
	Document->SetArrayField(TEXT("image_scale"), Vec2(FVector2f(ImageWidth / RasterSize.X, ImageHeight / RasterSize.Y)));
	Document->SetArrayField(TEXT("frames"), JsonFrames);
	// Observed arms/hands can occlude the pelvis even when the model's arms
	// miss them. Supply these landmarks for conservative image masking.
	TArray<TSharedPtr<FJsonValue>> JsonKeypoints;
	for (float V : Keypoints) { JsonKeypoints.Add(MakeShared<FJsonValueNumber>(V)); }
	Document->SetArrayField(TEXT("keypoints"), JsonKeypoints);
	if (bKeepDiagnostics)
	{
		TArray<TSharedPtr<FJsonValue>> JsonPose, JsonTranslation, JsonRest;
		for (float V : Pose) { JsonPose.Add(MakeShared<FJsonValueNumber>(V)); }
		for (float V : Translation) { JsonTranslation.Add(MakeShared<FJsonValueNumber>(V)); }
		for (const auto& Bone : Rest)
		{
			auto J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("parent"), Bone.Get<1>());
			J->SetArrayField(TEXT("offset"), Vec(Bone.Get<2>().GetTranslation()));
			JsonRest.Add(MakeShared<FJsonValueObject>(J));
		}
		Document->SetArrayField(TEXT("pose"), JsonPose);
		Document->SetArrayField(TEXT("translation"), JsonTranslation);
		Document->SetArrayField(TEXT("rest"), JsonRest);
		Document->SetNumberField(TEXT("focal"), Focal);
		Document->SetArrayField(TEXT("center"), Vec2(Center));
	}
	FString Input;
	FJsonSerializer::Serialize(Document, TJsonWriterFactory<>::Create(&Input));
	const FString InputFile = Directory / TEXT("input.json"), OutputFile = Directory / TEXT("tracks.json");
	if (!FFileHelper::SaveStringToFile(Input, *InputFile)) { return Result; }
	int32 ExitCode = -1;
	FString Stdout, Stderr;
	const FString Arguments = FString::Printf(TEXT("-u PYTHONHOME -u PYTHONPATH -u LD_LIBRARY_PATH /usr/bin/python3 \"%s\" \"%s\" \"%s\""), *Script, *InputFile, *OutputFile);
	const bool bRan = FPlatformProcess::ExecProcess(TEXT("/usr/bin/env"), *Arguments, &ExitCode, &Stdout, &Stderr);
	if (!bRan || ExitCode != 0)
	{
		UE_LOG(LogMetaHumanBodyTracker, Warning, TEXT("Pelvis image tracking unavailable; using hip landmarks. %s"), *Stderr.Left(2000));
		return Result;
	}
	FString Output;
	TSharedPtr<FJsonObject> Tracks;
	if (!FFileHelper::LoadFileToString(Output, *OutputFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output), Tracks) || !Tracks) { return Result; }
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Tracks->TryGetArrayField(TEXT("observations"), Rows)) { return Result; }
	for (const auto& Value : *Rows)
	{
		const auto& Row = Value->AsArray();
		if (Row.Num() != 6) { continue; }
		const int32 F = Row[0]->AsNumber(), Ref = Row[1]->AsNumber(), Candidate = Row[2]->AsNumber();
		if (!Points.IsValidIndex(F) || Ref < 0 || Ref >= F || !Points[F].IsValidIndex(Candidate) || !Points[Ref].IsValidIndex(Candidate)) { continue; }
		FPelvisSurfaceObservation Observation;
		Observation.Frame = F; Observation.ReferenceFrame = Ref;
		Observation.Position = Points[F][Candidate].Position;
		Observation.ReferencePosition = Points[Ref][Candidate].Position;
		Observation.Lever = Points[F][Candidate].Lever;
		Observation.ReferenceLever = Points[Ref][Candidate].Lever;
		Observation.ObservedMotion = FVector2f(Row[3]->AsNumber(), Row[4]->AsNumber());
		Observation.Confidence = Row[5]->AsNumber();
		Result.Add(Observation);
	}
	UE_LOG(LogMetaHumanBodyTracker, Display, TEXT("%s"), *Stdout.TrimStartAndEnd());
	if (bKeepDiagnostics) { UE_LOG(LogMetaHumanBodyTracker, Display, TEXT("Pelvis image tracking diagnostics: %s"), *InputFile); }
	return Result;
}
}
