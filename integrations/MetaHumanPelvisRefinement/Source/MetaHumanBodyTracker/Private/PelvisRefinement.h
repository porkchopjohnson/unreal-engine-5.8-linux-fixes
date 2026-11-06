// Optional image-landmark refinement of SMPL-X pelvis orientation.
#pragma once

#include "CoreMinimal.h"

class UMetaHumanPerformance;

namespace UE::MetaHuman::Pipeline
{
struct FPelvisRefinementSettings
{
	float TiltStrength = 1.0f;
	float TwistStrength = 0.5f;
	float PitchStrength = 1.0f;
	float RollStrength = 1.0f;
	float Exaggeration = 1.0f;
	float EasingStrength = 0.5f;
	float SurfaceWeight = 4.0f;
	float MaxTiltDegrees = 20.0f;
	float MaxTwistDegrees = 15.0f;
	float MinConfidence = 0.3f;
	float TemporalWeight = 4.0f;
};

// Snapshot once per solve, shared by image collection and final fitting.
struct FPelvisRefinementOptions
{
	bool bEnabled = true;
	bool bImageTracking = true;
	bool bDumpDiagnostics = false;
	bool bUsingConsoleSettings = false;
	FPelvisRefinementSettings Fit;
};

FPelvisRefinementOptions ResolvePelvisRefinementOptions(const UMetaHumanPerformance* Performance);

struct FPelvisRefinementStats
{
	int32 EligibleFrames = 0;
	int32 RefinedFrames = 0;
	double BeforeSquaredError = 0;
	double AfterSquaredError = 0;
	double MaxCorrectionDegrees = 0;
	double LandmarkScale = 1;
	int32 SurfaceFrames = 0;
	int32 SurfaceObservations = 0;
	double FittedSquaredError = 0;
	FString DiagnosticCSV;
};

// Tracked image motion of a visible, pelvis-dominant SMPL-X surface vertex.
// Positions/levers are camera-space meters before ApplyHps. ObservedMotion is
// relative to the detected hip center, in the 2D tracker's raster coordinates.
struct FPelvisSurfaceObservation
{
	int32 Frame = 0;
	int32 ReferenceFrame = 0;
	FVector Position = FVector::ZeroVector;
	FVector ReferencePosition = FVector::ZeroVector;
	FVector Lever = FVector::ZeroVector;
	FVector ReferenceLever = FVector::ZeroVector;
	FVector2f ObservedMotion = FVector2f::ZeroVector;
	float Confidence = 0;
};

// Inputs are camera-space SMPL-X coordinates BEFORE ApplyHps. RestHierarchy is
// obtained with zero pose and SetAccountForHeight(false). Confidence is BODY_25.
// Corrects global orientation and compensates the first spine/thigh rotations,
// retaining their original global orientations. Translation is never modified.
FPelvisRefinementStats RefinePelvisOrientation(
	const TArray<TTuple<FString, int32, FTransform>>& RestHierarchy,
	TConstArrayView64<float> Keypoints, TConstArrayView64<bool> ValidFrames,
	TConstArrayView64<float> Translation, float FocalLength, FVector2f ImageCenter,
	float Fps, const FPelvisRefinementSettings& Settings,
	TArray64<float>& Pose, bool bDiagnostics = false,
	TConstArrayView<FPelvisSurfaceObservation> SurfaceObservations = {});
}
