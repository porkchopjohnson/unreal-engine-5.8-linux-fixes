#include "PelvisRefinement.h"
#include "MetaHumanBodyTrackerLog.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MetaHuman::Pipeline
{
namespace PelvisReplay
{
bool Load(const FString& Path, TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}
FVector Vector(const TArray<TSharedPtr<FJsonValue>>& V) { return FVector(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber()); }
float Setting(const TCHAR* Name, float Default)
{
	const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
	return Variable ? Variable->GetFloat() : Default;
}
void Replay(const TArray<FString>& Arguments)
{
	if (Arguments.IsEmpty()) { return; }
	TSharedPtr<FJsonObject> Input, Tracks;
	if (!Load(Arguments[0], Input) || !Load(FPaths::GetPath(Arguments[0]) / TEXT("tracks.json"), Tracks))
	{
		UE_LOG(LogMetaHumanBodyTracker, Error, TEXT("Cannot read saved pelvis image evidence: %s"), *Arguments[0]);
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonPose = nullptr;
	if (!Input->TryGetArrayField(TEXT("pose"), JsonPose)) { return; }
	TArray64<float> Pose, Translation, Keypoints;
	for (const auto& V : *JsonPose) { Pose.Add(V->AsNumber()); }
	for (const auto& V : Input->GetArrayField(TEXT("translation"))) { Translation.Add(V->AsNumber()); }
	for (const auto& V : Input->GetArrayField(TEXT("keypoints"))) { Keypoints.Add(V->AsNumber()); }
	TArray<TTuple<FString, int32, FTransform>> Rest;
	for (const auto& V : Input->GetArrayField(TEXT("rest")))
	{
		const auto J = V->AsObject();
		Rest.Add(MakeTuple(FString::FromInt(Rest.Num()), int32(J->GetNumberField(TEXT("parent"))), FTransform(Vector(J->GetArrayField(TEXT("offset"))))));
	}
	const auto& Frames = Input->GetArrayField(TEXT("frames"));
	TArray64<bool> Valid;
	for (const auto& V : Frames) { Valid.Add(V->AsObject()->GetBoolField(TEXT("valid"))); }
	TArray<FPelvisSurfaceObservation> Surface;
	if (Setting(TEXT("mh.BodyTracker.PelvisRefinement.ImageTracking"), 1) != 0)
	{
		for (const auto& Value : Tracks->GetArrayField(TEXT("observations")))
		{
			const auto& Row = Value->AsArray();
			if (Row.Num() != 6) { continue; }
			const int32 F = Row[0]->AsNumber(), Ref = Row[1]->AsNumber(), Index = Row[2]->AsNumber();
			if (!Frames.IsValidIndex(F) || Ref < 0 || Ref >= F) { continue; }
			const auto& CurrentPoints = Frames[F]->AsObject()->GetArrayField(TEXT("points"));
			const auto& RefPoints = Frames[Ref]->AsObject()->GetArrayField(TEXT("points"));
			if (!CurrentPoints.IsValidIndex(Index) || !RefPoints.IsValidIndex(Index)) { continue; }
			FPelvisSurfaceObservation Observation;
			Observation.Frame = F; Observation.ReferenceFrame = Ref;
			Observation.Position = Vector(CurrentPoints[Index]->AsObject()->GetArrayField(TEXT("position")));
			Observation.ReferencePosition = Vector(RefPoints[Index]->AsObject()->GetArrayField(TEXT("position")));
			Observation.Lever = Vector(CurrentPoints[Index]->AsObject()->GetArrayField(TEXT("lever")));
			Observation.ReferenceLever = Vector(RefPoints[Index]->AsObject()->GetArrayField(TEXT("lever")));
			Observation.ObservedMotion = FVector2f(Row[3]->AsNumber(), Row[4]->AsNumber());
			Observation.Confidence = Row[5]->AsNumber();
			Surface.Add(Observation);
		}
	}
	FPelvisRefinementSettings Settings;
	Settings.TiltStrength = Setting(TEXT("mh.BodyTracker.PelvisRefinement.TiltStrength"), 1);
	Settings.TwistStrength = Setting(TEXT("mh.BodyTracker.PelvisRefinement.TwistStrength"), .5);
	Settings.PitchStrength = Setting(TEXT("mh.BodyTracker.PelvisRefinement.PitchStrength"), 1);
	Settings.RollStrength = Setting(TEXT("mh.BodyTracker.PelvisRefinement.RollStrength"), 1);
	Settings.Exaggeration = Setting(TEXT("mh.BodyTracker.PelvisRefinement.Exaggeration"), 1);
	Settings.EasingStrength = Setting(TEXT("mh.BodyTracker.PelvisRefinement.EasingStrength"), .5);
	Settings.SurfaceWeight = Setting(TEXT("mh.BodyTracker.PelvisRefinement.ImageWeight"), 4);
	Settings.MaxTiltDegrees = Setting(TEXT("mh.BodyTracker.PelvisRefinement.MaxTilt"), 20);
	Settings.MaxTwistDegrees = Setting(TEXT("mh.BodyTracker.PelvisRefinement.MaxTwist"), 15);
	const auto& Center = Input->GetArrayField(TEXT("center"));
	double Fps = 30;
	Input->TryGetNumberField(TEXT("fps"), Fps);
	const auto Stats = RefinePelvisOrientation(Rest, Keypoints, Valid, Translation, Input->GetNumberField(TEXT("focal")),
		FVector2f(Center[0]->AsNumber(), Center[1]->AsNumber()), Fps, Settings, Pose, true, Surface);
	const FString Prefix = Arguments.Num() > 1 ? Arguments[1] : FPaths::GetPath(Arguments[0]) / TEXT("replay");
	TArray<TSharedPtr<FJsonValue>> OutputPose;
	for (float V : Pose) { OutputPose.Add(MakeShared<FJsonValueNumber>(V)); }
	auto Output = MakeShared<FJsonObject>();
	Output->SetArrayField(TEXT("pose"), OutputPose);
	Output->SetNumberField(TEXT("before_cost"), Stats.BeforeSquaredError);
	Output->SetNumberField(TEXT("fitted_cost"), Stats.FittedSquaredError);
	Output->SetNumberField(TEXT("applied_cost"), Stats.AfterSquaredError);
	Output->SetNumberField(TEXT("max_correction_degrees"), Stats.MaxCorrectionDegrees);
	Output->SetNumberField(TEXT("surface_frames"), Stats.SurfaceFrames);
	Output->SetNumberField(TEXT("exaggeration"), Settings.Exaggeration);
	Output->SetNumberField(TEXT("easing_strength"), Settings.EasingStrength);
	FString Text;
	FJsonSerializer::Serialize(Output, TJsonWriterFactory<>::Create(&Text));
	const bool bSaved = FFileHelper::SaveStringToFile(Text, *(Prefix + TEXT(".json"))) && FFileHelper::SaveStringToFile(Stats.DiagnosticCSV, *(Prefix + TEXT(".csv")));
	UE_LOG(LogMetaHumanBodyTracker, Display, TEXT("Pelvis replay: saved=%d, gain=%.2f, surface frames=%d, loss=%.3f -> %.3f -> %.3f, max correction=%.3f deg, prefix=%s"),
		bSaved, Settings.Exaggeration, Stats.SurfaceFrames, Stats.BeforeSquaredError, Stats.FittedSquaredError, Stats.AfterSquaredError, Stats.MaxCorrectionDegrees, *Prefix);
}
static FAutoConsoleCommand Command(TEXT("mh.BodyTracker.PelvisRefinement.Replay"),
	TEXT("Refit saved image evidence without inference or asset modification: <input.json> [output-prefix]. Uses current refinement controls and recorded frame rate."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Replay));
}
}
