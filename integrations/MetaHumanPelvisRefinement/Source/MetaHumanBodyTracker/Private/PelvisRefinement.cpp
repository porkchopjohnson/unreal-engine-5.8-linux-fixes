#include "PelvisRefinement.h"
#include "SupressWarnings.h"

MH_DISABLE_EIGEN_WARNINGS
#include "Eigen/Dense"
MH_ENABLE_WARNINGS

namespace UE::MetaHuman::Pipeline
{
namespace PelvisRefinement
{
using FVec = Eigen::Vector3d;
using FMat = Eigen::Matrix3d;
constexpr int32 JointCount = 55;
constexpr int32 PoseStride = JointCount * 3;
constexpr int32 KeypointStride = 25 * 3;
constexpr int32 HipSamples = 2;
constexpr int32 SurfaceSamples = 16;
constexpr int32 Samples = HipSamples + SurfaceSamples;
constexpr int32 JointIds[HipSamples] = {1, 2};
constexpr int32 KeypointIds[HipSamples] = {12, 9};

FMat Exp(const FVec& V)
{
	const double Angle = V.norm();
	return Angle > 1.e-10 ? Eigen::AngleAxisd(Angle, V / Angle).toRotationMatrix() : FMat::Identity();
}

FVec Log(const FMat& R)
{
	const Eigen::AngleAxisd AA(R);
	return AA.axis() * AA.angle();
}

struct FFrame;
struct FSurfaceSlot
{
	FPelvisSurfaceObservation Observation;
	const FFrame* Reference = nullptr;
};

struct FFrame
{
	bool bValid = false;
	FMat RootRotation = FMat::Identity();
	FVec RootPosition = FVec::Zero();
	FVec Translation = FVec::Zero();
	FVec Position[HipSamples];
	FVec BranchOffset[HipSamples];
	Eigen::Vector2d Observed[HipSamples];
	Eigen::Vector2d RawObserved[HipSamples];
	TArray<FSurfaceSlot> Surface;
	FVec Prior = FVec(30.0, 14.0, 6.0);
	FVec TemporalScale = FVec::Ones();
	double Weight[Samples] = {};
	Eigen::Vector2d ObservedCenter = Eigen::Vector2d::Zero();
	FVec Delta = FVec::Zero();
};

using FResidual = Eigen::Matrix<double, Samples * 2, 1>;

FVec EigenVector(FVector P) { return FVec(P.X, P.Y, P.Z); }

bool HipProjection(const FFrame& Frame, const FVec& Delta, double Focal, FVector2f Center, Eigen::Vector2d* Points)
{
	const FMat Rotation = Frame.RootRotation * Exp(Delta);
	for (int32 I = 0; I < HipSamples; ++I)
	{
		const FVec P = Frame.Position[I] + (Rotation - Frame.RootRotation) * Frame.BranchOffset[I] + Frame.Translation;
		if (!P.allFinite() || P.z() <= 0.05)
		{
			return false;
		}
		Points[I] = Eigen::Vector2d(Focal * P.x() / P.z() + Center.X, Focal * P.y() / P.z() + Center.Y);
	}
	return true;
}

bool Residual(const FFrame& Frame, const FVec& Delta, double Focal, FVector2f Center, FResidual& Out, bool bZeroReferences = false)
{
	Out.setZero();
	Eigen::Vector2d Points[HipSamples];
	if (!HipProjection(Frame, Delta, Focal, Center, Points)) { return false; }
	const Eigen::Vector2d PredictedCenter = (Points[0] + Points[1]) * 0.5;
	for (int32 I = 0; I < HipSamples; ++I)
	{
		// Center-relative residuals cannot drive a correction merely because the
		// original solve placed the subject a few pixels to one side.
		Out.template segment<2>(I * 2) = (Points[I] - PredictedCenter) - (Frame.Observed[I] - Frame.ObservedCenter);
	}
	const FMat RotationDelta = Frame.RootRotation * Exp(Delta) - Frame.RootRotation;
	for (int32 I = 0; I < Frame.Surface.Num(); ++I)
	{
		const auto& Slot = Frame.Surface[I];
		const auto& Observation = Slot.Observation;
		const FFrame& Ref = *Slot.Reference;
		const FVec RefDelta = bZeroReferences ? FVec::Zero() : Ref.Delta;
		Eigen::Vector2d ReferenceHips[HipSamples];
		if (!HipProjection(Ref, RefDelta, Focal, Center, ReferenceHips)) { return false; }
		const FVec Current = EigenVector(Observation.Position) + RotationDelta * EigenVector(Observation.Lever) + Frame.Translation;
		const FVec Reference = EigenVector(Observation.ReferencePosition) +
			(Ref.RootRotation * Exp(RefDelta) - Ref.RootRotation) * EigenVector(Observation.ReferenceLever) + Ref.Translation;
		if (Current.z() <= 0.05 || Reference.z() <= 0.05 || !Current.allFinite() || !Reference.allFinite()) { return false; }
		const Eigen::Vector2d CurrentPixel(Focal * Current.x() / Current.z() + Center.X, Focal * Current.y() / Current.z() + Center.Y);
		const Eigen::Vector2d RefPixel(Focal * Reference.x() / Reference.z() + Center.X, Focal * Reference.y() / Reference.z() + Center.Y);
		Out.template segment<2>((HipSamples + I) * 2) = CurrentPixel - PredictedCenter -
			(RefPixel - (ReferenceHips[0] + ReferenceHips[1]) * 0.5) - Eigen::Vector2d(Observation.ObservedMotion.X, Observation.ObservedMotion.Y);
	}
	return Out.allFinite();
}

double DataCost(const FFrame& Frame, const FResidual& R)
{
	double Cost = 0;
	for (int32 I = 0; I < Samples; ++I)
	{
		const double Length = R.template segment<2>(I * 2).norm();
		// Huber loss, in pixels. Occluded/misidentified joints cannot pull the
		// pelvis without bound; confidence is applied separately.
		Cost += Frame.Weight[I] * (Length <= 8.0 ? Length * Length : 16.0 * Length - 64.0);
	}
	return Cost;
}

FVec Bound(FVec Value, const FPelvisRefinementSettings& Settings)
{
	const double Tilt = FMath::DegreesToRadians(FMath::Clamp(Settings.MaxTiltDegrees, 0.f, 45.f));
	const double Twist = FMath::DegreesToRadians(FMath::Clamp(Settings.MaxTwistDegrees, 0.f, 45.f));
	// SMPL-X local Y is the body's longitudinal (twist) axis. X/Z are tilt.
	Value.y() = FMath::Clamp(Value.y(), -Twist, Twist);
	const double TiltLength = FMath::Sqrt(Value.x() * Value.x() + Value.z() * Value.z());
	if (TiltLength > Tilt && TiltLength > 0)
	{
		Value.x() *= Tilt / TiltLength;
		Value.z() *= Tilt / TiltLength;
	}
	return Value;
}
}

FPelvisRefinementStats RefinePelvisOrientation(
	const TArray<TTuple<FString, int32, FTransform>>& RestHierarchy,
	TConstArrayView64<float> Keypoints, TConstArrayView64<bool> ValidFrames,
	TConstArrayView64<float> Translation, float FocalLength, FVector2f ImageCenter,
	float Fps, const FPelvisRefinementSettings& Settings, TArray64<float>& Pose, bool bDiagnostics,
	TConstArrayView<FPelvisSurfaceObservation> SurfaceObservations)
{
	using namespace PelvisRefinement;
	FPelvisRefinementStats Stats;
	const int32 Frames = ValidFrames.Num();
	if (Frames <= 0 || RestHierarchy.Num() != JointCount + 1 || Pose.Num() != int64(Frames) * PoseStride ||
		Translation.Num() != int64(Frames) * 3 || Keypoints.Num() != int64(Frames) * KeypointStride ||
		!FMath::IsFinite(FocalLength) || FocalLength <= 0 || !FMath::IsFinite(Fps) || Fps <= 0 ||
		!FMath::IsFinite(Settings.TiltStrength) || !FMath::IsFinite(Settings.TwistStrength) ||
		!FMath::IsFinite(Settings.PitchStrength) || !FMath::IsFinite(Settings.RollStrength) ||
		!FMath::IsFinite(Settings.Exaggeration) || !FMath::IsFinite(Settings.SurfaceWeight) || Settings.Exaggeration <= 0 ||
		!FMath::IsFinite(Settings.MaxTiltDegrees) || !FMath::IsFinite(Settings.MaxTwistDegrees) ||
		!FMath::IsFinite(Settings.MinConfidence) || !FMath::IsFinite(Settings.TemporalWeight) ||
		((Settings.TiltStrength <= 0 || (Settings.PitchStrength <= 0 && Settings.RollStrength <= 0)) && Settings.TwistStrength <= 0))
	{
		return Stats;
	}
	FVec Offsets[JointCount];
	int32 Parents[JointCount], Branches[JointCount];
	for (int32 J = 0; J < JointCount; ++J)
	{
		Parents[J] = RestHierarchy[J + 1].Get<1>() - 1;
		if ((J == 0 && Parents[J] != -1) || (J > 0 && (Parents[J] < 0 || Parents[J] >= J)))
		{
			return Stats;
		}
		const FVector P = RestHierarchy[J + 1].Get<2>().GetTranslation() / 100.0;
		Offsets[J] = FVec(P.X, P.Z, P.Y);
		Branches[J] = J == 0 ? 0 : (Parents[J] == 0 ? J : Branches[Parents[J]]);
	}
	TArray<FFrame> Data;
	Data.SetNum(Frames);
	for (int32 F = 0; F < Frames; ++F)
	{
		FFrame& Frame = Data[F];
		if (!ValidFrames[F]) { continue; }
		const int64 PBase = int64(F) * PoseStride;
		const int64 KBase = int64(F) * KeypointStride;
		const int64 TBase = int64(F) * 3;
		FMat Rotations[JointCount];
		FVec Positions[JointCount];
		bool bFinite = true;
		for (int32 J = 0; J < JointCount; ++J)
		{
			const FVec AA(Pose[PBase + J * 3], Pose[PBase + J * 3 + 1], Pose[PBase + J * 3 + 2]);
			bFinite &= AA.allFinite();
			if (!bFinite) { break; }
			const FMat Local = Exp(AA);
			if (J == 0) { Rotations[J] = Local; Positions[J] = Offsets[J]; }
			else
			{
				Rotations[J] = Rotations[Parents[J]] * Local;
				Positions[J] = Positions[Parents[J]] + Rotations[Parents[J]] * Offsets[J];
			}
		}
		if (!bFinite) { continue; }
		Frame.RootRotation = Rotations[0];
		Frame.RootPosition = Positions[0];
		Frame.Translation = FVec(Translation[TBase], Translation[TBase + 1], Translation[TBase + 2]);
		for (int32 I = 0; I < HipSamples; ++I)
		{
			const int64 K = KBase + KeypointIds[I] * 3;
			Frame.Position[I] = Positions[JointIds[I]];
			Frame.BranchOffset[I] = Offsets[Branches[JointIds[I]]];
			Frame.Observed[I] = Eigen::Vector2d(Keypoints[K], Keypoints[K + 1]);
			Frame.RawObserved[I] = Frame.Observed[I];
			const float Confidence = Keypoints[K + 2];
			Frame.Weight[I] = FMath::IsFinite(Confidence) && Confidence >= Settings.MinConfidence && Frame.Observed[I].allFinite()
				? FMath::Clamp(double(Confidence), 0.0, 1.0) : 0;
			// Invalid unused landmarks must not introduce 0 * NaN into the solve.
			if (!Frame.Observed[I].allFinite()) { Frame.Observed[I].setZero(); }
		}
		Frame.ObservedCenter = (Frame.Observed[0] + Frame.Observed[1]) * 0.5;
		FResidual R;
		Frame.bValid = Frame.Weight[0] > 0 && Frame.Weight[1] > 0 &&
			(Frame.Observed[0] - Frame.Observed[1]).norm() >= 6.0 && Residual(Frame, FVec::Zero(), FocalLength, ImageCenter, R);
		if (Frame.bValid) { ++Stats.EligibleFrames; }
	}
	for (const auto& Observation : SurfaceObservations)
	{
		if (!Data.IsValidIndex(Observation.Frame) || Observation.ReferenceFrame < 0 || Observation.ReferenceFrame >= Observation.Frame ||
			!Data[Observation.Frame].bValid || !Data[Observation.ReferenceFrame].bValid || !FMath::IsFinite(Observation.Confidence) ||
			Observation.Confidence <= 0 || Observation.Position.ContainsNaN() || Observation.ReferencePosition.ContainsNaN() ||
			Observation.Lever.ContainsNaN() || Observation.ReferenceLever.ContainsNaN() || !FMath::IsFinite(Observation.ObservedMotion.X) ||
			!FMath::IsFinite(Observation.ObservedMotion.Y) || Data[Observation.Frame].Surface.Num() >= SurfaceSamples)
		{
			continue;
		}
		Data[Observation.Frame].Surface.Add({Observation, &Data[Observation.ReferenceFrame]});
	}
	for (FFrame& Frame : Data)
	{
		if (Frame.Surface.Num() < 4 || Settings.SurfaceWeight <= 0) { Frame.Surface.Reset(); continue; }
		Frame.Prior = FVec(8.0, 10.0, 6.0);
		++Stats.SurfaceFrames;
		Stats.SurfaceObservations += Frame.Surface.Num();
		for (int32 I = 0; I < Frame.Surface.Num(); ++I)
		{
			Frame.Weight[HipSamples + I] = FMath::Clamp(double(Settings.SurfaceWeight), 0.0, 10.0) *
				FMath::Clamp(double(Frame.Surface[I].Observation.Confidence), 0.0, 1.0) / Frame.Surface.Num();
		}
	}
	// Brief feature dropouts must not snap pitch back to the landmark-only
	// prior. Bridge up to three valid hip frames around surface evidence, with
	// stronger temporal coupling on the less-observable pitch/yaw axes.
	if (Stats.SurfaceFrames > 0)
	{
		for (int32 F = 0; F < Frames; ++F)
		{
			if (!Data[F].bValid) { continue; }
			bool bNearSurface = !Data[F].Surface.IsEmpty();
			for (int32 Direction : {-1, 1})
			{
				for (int32 Offset = 1; Offset <= 3; ++Offset)
				{
					const int32 N = F + Direction * Offset;
					if (!Data.IsValidIndex(N) || !Data[N].bValid) { break; }
					bNearSurface |= !Data[N].Surface.IsEmpty();
				}
			}
			if (bNearSurface)
			{
				Data[F].Prior = FVec(8.0, 10.0, 6.0);
				Data[F].TemporalScale = FVec(3.0, 2.0, 1.0);
			}
		}
	}
	// Detector hip landmarks and SMPL-X joint centers differ anatomically. Fit
	// ONE robust width calibration for the entire sequence, never a scale per
	// frame. This preserves relative width changes (twist evidence) without
	// interpreting a consistently wider detector hip line as constant rotation.
	TArray<double> WidthRatios;
	for (const FFrame& Frame : Data)
	{
		if (!Frame.bValid) { continue; }
		FVec L = Frame.Position[0] + Frame.Translation;
		FVec R = Frame.Position[1] + Frame.Translation;
		const Eigen::Vector2d PL(FocalLength * L.x() / L.z(), FocalLength * L.y() / L.z());
		const Eigen::Vector2d PR(FocalLength * R.x() / R.z(), FocalLength * R.y() / R.z());
		const double Width = (PL - PR).norm();
		if (Width >= 6.0) { WidthRatios.Add((Frame.Observed[0] - Frame.Observed[1]).norm() / Width); }
	}
	if (!WidthRatios.IsEmpty())
	{
		WidthRatios.Sort();
		const int32 Mid = WidthRatios.Num() / 2;
		const double Median = WidthRatios.Num() % 2 ? WidthRatios[Mid] : (WidthRatios[Mid - 1] + WidthRatios[Mid]) * 0.5;
		Stats.LandmarkScale = FMath::Clamp(Median, 0.5, 2.0);
		for (FFrame& Frame : Data)
		{
			for (int32 I = 0; I < 2; ++I)
			{
				Frame.Observed[I] = Frame.ObservedCenter + (Frame.Observed[I] - Frame.ObservedCenter) / Stats.LandmarkScale;
			}
		}
	}
	// A prior is essential: monocular hip landmarks do not uniquely determine
	// twist. The stronger Y prior retains Hue's depth branch instead of inventing
	// a front/back flip from a narrower projected hip line.
	// Sagittal pitch (X) is particularly weakly observed by a hip line. Retain
	// the original pitch unless the image evidence actually constrains it.
	const double Temporal = FMath::Clamp(double(Settings.TemporalWeight), 0.0, 20.0) * FMath::Clamp(double(Fps) / 30.0, 0.1, 4.0);
	for (int32 Sweep = 0; Sweep < 12; ++Sweep)
	{
		for (int32 Step = 0; Step < Frames; ++Step)
		{
			const int32 F = Sweep % 2 ? Frames - 1 - Step : Step;
			FFrame& Frame = Data[F];
			if (!Frame.bValid) { continue; }
			const FVec& Prior = Frame.Prior;
			FResidual R;
			if (!Residual(Frame, Frame.Delta, FocalLength, ImageCenter, R)) { continue; }
			Eigen::Matrix<double, Samples * 2, 3> Jacobian;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				FVec D = Frame.Delta;
				D[Axis] += 1.e-4;
				FResidual Perturbed;
				if (!Residual(Frame, D, FocalLength, ImageCenter, Perturbed)) { Perturbed = R; }
				Jacobian.col(Axis) = (Perturbed - R) / 1.e-4;
			}
			FMat H = Prior.array().square().matrix().asDiagonal();
			FVec Gradient = Prior.array().square().matrix().cwiseProduct(Frame.Delta);
			for (int32 I = 0; I < Samples; ++I)
			{
				const double Length = R.template segment<2>(I * 2).norm();
				const double Weight = Frame.Weight[I] * (Length > 8.0 ? 8.0 / Length : 1.0);
				const auto J = Jacobian.template block<2, 3>(I * 2, 0);
				H += Weight * J.transpose() * J;
				Gradient += Weight * J.transpose() * R.template segment<2>(I * 2);
			}
			auto Cost = [&](const FVec& D, const FResidual& Error)
			{
				double Value = DataCost(Frame, Error) + Prior.cwiseProduct(D).squaredNorm();
				for (int32 N : {F - 1, F + 1})
				{
					if (Data.IsValidIndex(N) && Data[N].bValid)
					{
						const FVec Coupling = Frame.TemporalScale.cwiseMax(Data[N].TemporalScale) * Temporal;
						Value += Coupling.cwiseProduct(D - Data[N].Delta).squaredNorm();
					}
				}
				return Value;
			};
			for (int32 N : {F - 1, F + 1})
			{
				if (Data.IsValidIndex(N) && Data[N].bValid)
				{
					const FVec Coupling = Frame.TemporalScale.cwiseMax(Data[N].TemporalScale) * Temporal;
					H.diagonal() += Coupling.array().square().matrix();
					Gradient += Coupling.array().square().matrix().cwiseProduct(Frame.Delta - Data[N].Delta);
				}
			}
			const FVec Update = H.ldlt().solve(-Gradient);
			if (!Update.allFinite()) { continue; }
			const double OriginalCost = Cost(Frame.Delta, R);
			for (double Alpha : {1.0, 0.5, 0.25, 0.125})
			{
				const FVec Candidate = Bound(Frame.Delta + Alpha * Update, Settings);
				FResidual CandidateResidual;
				if (Residual(Frame, Candidate, FocalLength, ImageCenter, CandidateResidual) && Cost(Candidate, CandidateResidual) < OriginalCost)
				{
					Frame.Delta = Candidate;
					break;
				}
			}
		}
	}
	for (FFrame& Frame : Data)
	{
		Frame.Delta.x() *= FMath::Clamp(Settings.TiltStrength, 0.f, 1.f) * FMath::Clamp(Settings.PitchStrength, 0.f, 1.f);
		Frame.Delta.z() *= FMath::Clamp(Settings.TiltStrength, 0.f, 1.f) * FMath::Clamp(Settings.RollStrength, 0.f, 1.f);
		Frame.Delta.y() *= FMath::Clamp(Settings.TwistStrength, 0.f, 1.f);
		if (Stats.SurfaceFrames == 0 && Frame.bValid)
		{
			FResidual Before, After;
			Residual(Frame, FVec::Zero(), FocalLength, ImageCenter, Before);
			Residual(Frame, Frame.Delta, FocalLength, ImageCenter, After);
			if (DataCost(Frame, After) > DataCost(Frame, Before)) { Frame.Delta.setZero(); }
		}
	}
	auto TotalCost = [&](bool bZero)
	{
		double Cost = 0;
		for (const FFrame& Frame : Data)
		{
			if (!Frame.bValid) { continue; }
			FResidual R;
			if (!Residual(Frame, bZero ? FVec::Zero() : Frame.Delta, FocalLength, ImageCenter, R, bZero))
			{
				return TNumericLimits<double>::Max();
			}
			Cost += DataCost(Frame, R);
		}
		return Cost;
	};
	Stats.BeforeSquaredError = TotalCost(true);
	// Linked surface tracks are assessed as a sequence, with matching corrections
	// on both ends of each track. Backtrack strength blending if necessary.
	for (int32 Attempt = 0; Attempt < 8 && TotalCost(false) > Stats.BeforeSquaredError; ++Attempt)
	{
		for (FFrame& Frame : Data) { Frame.Delta *= Attempt == 7 ? 0.0 : 0.5; }
	}
	Stats.FittedSquaredError = TotalCost(false);
	TArray<FVec> Fitted;
	TArray<double> BeforeCosts, FittedCosts;
	Fitted.SetNum(Frames); BeforeCosts.SetNumZeroed(Frames); FittedCosts.SetNumZeroed(Frames);
	for (int32 F = 0; F < Frames; ++F)
	{
		FFrame& Frame = Data[F];
		Fitted[F] = Frame.Delta;
		if (!Frame.bValid) { continue; }
		FResidual Before, Fit;
		Residual(Frame, FVec::Zero(), FocalLength, ImageCenter, Before, true);
		Residual(Frame, Frame.Delta, FocalLength, ImageCenter, Fit);
		BeforeCosts[F] = DataCost(Frame, Before);
		FittedCosts[F] = DataCost(Frame, Fit);
	}
	// Exaggeration is an artistic post-fit operation. It is deliberately NOT
	// rejected for increasing reprojection error; final rotation caps still hold.
	for (FFrame& Frame : Data) { Frame.Delta = Bound(Frame.Delta * FMath::Clamp(Settings.Exaggeration, 0.f, 3.f), Settings); }
	TArray<FVec> Uneased;
	Uneased.Reserve(Frames);
	for (const FFrame& Frame : Data) { Uneased.Add(Frame.Delta); }
	const double Easing = FMath::Clamp(double(Settings.EasingStrength), 0.0, 1.0);
	if (Easing > 0 && FMath::IsFinite(Fps) && Fps > 0)
	{
		// Offline, symmetric Gaussian easing in the bounded local correction's
		// rotation-vector space. Read an immutable copy so scan direction cannot
		// add lag. Never filter Hue itself, cross invalid frames, or blend Euler
		// angles. Positive weights preserve constant corrections and angle bounds.
		const double Sigma = 0.080 * Easing * Fps;
		const int32 Radius = FMath::Min(Frames - 1, FMath::CeilToInt(FMath::Min(3.0 * Sigma, double(Frames))));
		for (int32 F = 0; F < Frames; ++F)
		{
			if (!Data[F].bValid) { continue; }
			FVec Sum = Uneased[F];
			double WeightSum = 1.0;
			for (int32 Direction : {-1, 1})
			{
				for (int32 Distance = 1; Distance <= Radius; ++Distance)
				{
					const int32 N = F + Direction * Distance;
					if (!Data.IsValidIndex(N) || !Data[N].bValid) { break; }
					const double Weight = FMath::Exp(-0.5 * FMath::Square(double(Distance) / Sigma));
					Sum += Uneased[N] * Weight;
					WeightSum += Weight;
				}
			}
			Data[F].Delta = Bound(Sum / WeightSum, Settings);
		}
	}
	Stats.AfterSquaredError = TotalCost(false);
	if (bDiagnostics)
	{
		Stats.DiagnosticCSV = TEXT("frame,eligible,hip_l_x,hip_l_y,hip_r_x,hip_r_y,confidence_l,confidence_r,tilt_x_deg,twist_y_deg,tilt_z_deg,before_cost,after_cost,landmark_scale,surface_points,fitted_pitch_deg,fitted_yaw_deg,fitted_roll_deg,fitted_cost,exaggeration,uneased_pitch_deg,uneased_yaw_deg,uneased_roll_deg,easing_strength\n");
	}
	for (int32 F = 0; F < Frames; ++F)
	{
		FFrame& Frame = Data[F];
		if (!Frame.bValid) { continue; }
		const FVec D = Frame.Delta;
		FResidual After;
		Residual(Frame, D, FocalLength, ImageCenter, After);
		const double AfterCost = DataCost(Frame, After);
		if (D.norm() > 1.e-6)
		{
			const int64 Base = int64(F) * PoseStride;
			const FMat NewRoot = Frame.RootRotation * Exp(D);
			const FVec AA = Log(NewRoot);
			for (int32 Axis = 0; Axis < 3; ++Axis) { Pose[Base + Axis] = AA[Axis]; }
			// Preserve torso and leg global orientations. Changing pelvis alone
			// would otherwise rotate the entire character with the new hip tilt.
			for (int32 J = 1; J < JointCount; ++J)
			{
				if (Parents[J] != 0) { continue; }
				const int64 Offset = Base + J * 3;
				const FMat Local = Exp(FVec(Pose[Offset], Pose[Offset + 1], Pose[Offset + 2]));
				const FVec Compensated = Log(NewRoot.transpose() * Frame.RootRotation * Local);
				for (int32 Axis = 0; Axis < 3; ++Axis) { Pose[Offset + Axis] = Compensated[Axis]; }
			}
			++Stats.RefinedFrames;
			Stats.MaxCorrectionDegrees = FMath::Max(Stats.MaxCorrectionDegrees, FMath::RadiansToDegrees(D.norm()));
		}
		if (bDiagnostics)
		{
			Stats.DiagnosticCSV += FString::Printf(TEXT("%d,1,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%d,%.5f,%.5f,%.5f,%.6f,%.3f,%.5f,%.5f,%.5f,%.3f\n"),
				F, Frame.RawObserved[0].x(), Frame.RawObserved[0].y(), Frame.RawObserved[1].x(), Frame.RawObserved[1].y(),
				Frame.Weight[0], Frame.Weight[1], FMath::RadiansToDegrees(D.x()), FMath::RadiansToDegrees(D.y()),
				FMath::RadiansToDegrees(D.z()), BeforeCosts[F], AfterCost, Stats.LandmarkScale, Frame.Surface.Num(),
				FMath::RadiansToDegrees(Fitted[F].x()), FMath::RadiansToDegrees(Fitted[F].y()), FMath::RadiansToDegrees(Fitted[F].z()),
				FittedCosts[F], Settings.Exaggeration, FMath::RadiansToDegrees(Uneased[F].x()),
				FMath::RadiansToDegrees(Uneased[F].y()), FMath::RadiansToDegrees(Uneased[F].z()), Easing);
		}
	}
	return Stats;
}
}
