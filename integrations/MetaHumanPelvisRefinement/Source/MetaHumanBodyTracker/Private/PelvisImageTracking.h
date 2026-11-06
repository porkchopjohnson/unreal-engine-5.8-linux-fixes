#pragma once
#include "PelvisRefinement.h"

class FMetaHumanSMPLX;

namespace UE::MetaHuman::Pipeline
{
// Native model projection + optional system-Python/OpenCV optical-flow helper.
// The helper is isolated to a child process. Missing evidence yields no surface
// observations and the existing landmark-only fit remains available.
TArray<FPelvisSurfaceObservation> TrackPelvisSurface(
	FMetaHumanSMPLX& Model, const TArray<float>& Shape,
	TConstArrayView64<float> Pose, TConstArrayView64<float> Translation,
	TConstArrayView64<float> Keypoints, TConstArrayView64<bool> ValidFrames,
	const TArray<TArray<uint8>>& GrayFrames, const TArray<TArray<uint8>>& Masks,
	int32 ImageWidth, int32 ImageHeight, FVector2f RasterSize,
	float Focal, FVector2f Center, float Fps, bool bKeepDiagnostics);
}
