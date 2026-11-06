#include "PelvisRefinement.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MetaHumanPerformance.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "SupressWarnings.h"
MH_DISABLE_EIGEN_WARNINGS
#include "Eigen/Dense"
MH_ENABLE_WARNINGS

namespace UE::MetaHuman::Pipeline
{
namespace PelvisRefinementTests
{
using FVec = Eigen::Vector3d;
using FMat = Eigen::Matrix3d;

struct FFixture
{
	TArray<TTuple<FString, int32, FTransform>> Rest;
	TArray64<float> Pose, Keypoints, Translation;
	TArray64<bool> Valid;
	FMat Base;
	static constexpr int32 Frames = 9;

	FFixture(bool bTwist, int32 Count = Frames)
	{
		int32 Parent[55];
		for (int32 J = 0; J < 55; ++J) { Parent[J] = J == 0 ? -1 : 3; }
		Parent[1] = Parent[2] = Parent[3] = 0;
		Parent[4] = 1; Parent[5] = 2;
		FVec Offset[55];
		for (auto& V : Offset) { V.setZero(); }
		Offset[1] = FVec(0.09, -0.06, 0.0);
		Offset[2] = FVec(-0.09, -0.06, 0.0);
		Offset[3] = FVec(0, 0.10, 0.0);
		Offset[4] = Offset[5] = FVec(0, -0.4, 0.0);
		Offset[16] = FVec(0.18, 0.4, 0.0);
		Offset[17] = FVec(-0.18, 0.4, 0.0);
		Rest.Add(MakeTuple(FString(TEXT("root")), -1, FTransform::Identity));
		for (int32 J = 0; J < 55; ++J)
		{
			Rest.Add(MakeTuple(FString::FromInt(J), Parent[J] + 1,
				FTransform(FVector(Offset[J].x() * 100, Offset[J].z() * 100, Offset[J].y() * 100))));
		}
		Base = Eigen::AngleAxisd(PI, FVec::UnitX()).toRotationMatrix();
		if (bTwist) { Base = Base * Eigen::AngleAxisd(0.6, FVec::UnitY()).toRotationMatrix(); }
		const Eigen::AngleAxisd BaseAA(Base);
		const FVec AA = BaseAA.axis() * BaseAA.angle();
		Pose.SetNumZeroed(Count * 165);
		Keypoints.SetNumZeroed(Count * 75);
		Translation.SetNumZeroed(Count * 3);
		Valid.Init(true, Count);
		for (int32 F = 0; F < Count; ++F)
		{
			const double Angle = bTwist ? 0.22 * FMath::Sin(2.0 * PI * F / (Count - 1)) : 0.22;
			const FMat Target = Base * Eigen::AngleAxisd(Angle, bTwist ? FVec::UnitY() : FVec::UnitZ()).toRotationMatrix();
			for (int32 A = 0; A < 3; ++A) { Pose[F * 165 + A] = AA[A]; }
			Translation[F * 3 + 2] = 3.f;
			const int32 Joints[] = {1, 2, 4, 5, 16, 17};
			const int32 Slots[] = {12, 9, 13, 10, 5, 2};
			for (int32 I = 0; I < 6; ++I)
			{
				const int32 J = Joints[I];
				const int32 Branch = J <= 2 ? J : (J == 4 ? 1 : (J == 5 ? 2 : 3));
				FVec P = Target * Offset[Branch];
				if (J != Branch) { P += Base * Offset[J]; }
				P.z() += 3.0;
				Keypoints[F * 75 + Slots[I] * 3] = 1000.0 * P.x() / P.z() + 360;
				Keypoints[F * 75 + Slots[I] * 3 + 1] = 1000.0 * P.y() / P.z() + 640;
				Keypoints[F * 75 + Slots[I] * 3 + 2] = 0.95f;
			}
		}
	}

	FPelvisRefinementStats Run(const FPelvisRefinementSettings& Settings, TConstArrayView<FPelvisSurfaceObservation> Surface = {})
	{
		return RefinePelvisOrientation(Rest, Keypoints, Valid, Translation, 1000, FVector2f(360, 640), 30, Settings, Pose, false, Surface);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisSyntheticTiltTest, "MetaHuman.BodyTracker.PelvisRefinement.SyntheticTilt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisSyntheticTiltTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Fixture(false);
	const auto Before = Fixture.Pose;
	Fixture.Valid[4] = false;
	FPelvisRefinementSettings Settings;
	Settings.TwistStrength = 0;
	const auto Stats = Fixture.Run(Settings);
	TestEqual(TEXT("Only valid frames are fitted"), Stats.RefinedFrames, 8);
	TestTrue(TEXT("Known tilt substantially reduces image error"), Stats.AfterSquaredError < Stats.BeforeSquaredError * 0.4);
	TestTrue(TEXT("A real bounded correction is recovered"), Stats.MaxCorrectionDegrees > 5 && Stats.MaxCorrectionDegrees <= Settings.MaxTiltDegrees + 1.e-3);
	for (int32 F = 0; F < FFixture::Frames; ++F)
	{
		for (int32 J = 0; J < 55; ++J)
		{
			if (F != 4 && J <= 3) { continue; }
			for (int32 A = 0; A < 3; ++A)
			{
				TestEqual(TEXT("Invalid frames and non-compensated joints are unchanged"), Fixture.Pose[F * 165 + J * 3 + A], Before[F * 165 + J * 3 + A]);
			}
		}
	}
	const FVec RootAA(Fixture.Pose[0], Fixture.Pose[1], Fixture.Pose[2]);
	const FMat Root = Eigen::AngleAxisd(RootAA.norm(), RootAA.normalized()).toRotationMatrix();
	for (int32 J : {1, 2, 3})
	{
		const FVec AA(Fixture.Pose[J * 3], Fixture.Pose[J * 3 + 1], Fixture.Pose[J * 3 + 2]);
		const FMat Local = Eigen::AngleAxisd(AA.norm(), AA.normalized()).toRotationMatrix();
		TestTrue(TEXT("Thigh/spine global orientation is preserved"), (Root * Local - Fixture.Base).norm() < 1.e-5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisSyntheticTwistTest, "MetaHuman.BodyTracker.PelvisRefinement.SyntheticTwist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisSyntheticTwistTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Fixture(true);
	FPelvisRefinementSettings Settings;
	Settings.MaxTiltDegrees = 0;
	Settings.TiltStrength = 0;
	Settings.TwistStrength = 1;
	const auto Stats = Fixture.Run(Settings);
	TestTrue(TEXT("Dynamic twist is refined"), Stats.RefinedFrames >= 6);
	TestTrue(TEXT("Oblique hip width supplies usable twist evidence"), Stats.AfterSquaredError < Stats.BeforeSquaredError * 0.95);
	TestTrue(TEXT("Twist remains bounded"), Stats.MaxCorrectionDegrees <= Settings.MaxTwistDegrees + 1.e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisConfidenceTest, "MetaHuman.BodyTracker.PelvisRefinement.ConfidenceAndDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisConfidenceTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Fixture(false);
	const auto Before = Fixture.Pose;
	FPelvisRefinementSettings Settings;
	Settings.TiltStrength = Settings.TwistStrength = 0;
	TestEqual(TEXT("Zero strength does not refine"), Fixture.Run(Settings).RefinedFrames, 0);
	TestTrue(TEXT("Zero strength is an exact bypass"), Fixture.Pose == Before);
	Settings.TiltStrength = 1;
	for (int32 F = 0; F < FFixture::Frames; ++F) { Fixture.Keypoints[F * 75 + 12 * 3 + 2] = 0.1f; }
	TestEqual(TEXT("One unreliable hip disables its frame"), Fixture.Run(Settings).EligibleFrames, 0);
	TestTrue(TEXT("Low-confidence fallback preserves the original pose"), Fixture.Pose == Before);
	Fixture.Keypoints.Reset();
	TestEqual(TEXT("Bad array lengths are safely rejected"), Fixture.Run(Settings).EligibleFrames, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisCalibrationTest, "MetaHuman.BodyTracker.PelvisRefinement.LandmarkCalibration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisCalibrationTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Fixture(false);
	for (int32 F = 0; F < FFixture::Frames; ++F)
	{
		const int32 L = F * 75 + 12 * 3;
		const int32 R = F * 75 + 9 * 3;
		for (int32 A = 0; A < 2; ++A)
		{
			const double Center = (Fixture.Keypoints[L + A] + Fixture.Keypoints[R + A]) * 0.5;
			Fixture.Keypoints[L + A] = Center + (Fixture.Keypoints[L + A] - Center) * 1.8;
			Fixture.Keypoints[R + A] = Center + (Fixture.Keypoints[R + A] - Center) * 1.8;
		}
	}
	FPelvisRefinementSettings Settings;
	Settings.TwistStrength = 1;
	const auto Stats = Fixture.Run(Settings);
	TestTrue(TEXT("Static landmark width bias is calibrated once"), FMath::Abs(Stats.LandmarkScale - 1.8) < 1.e-4);
	TestTrue(TEXT("Calibration retains genuine tilt evidence"), Stats.AfterSquaredError < Stats.BeforeSquaredError * 0.4);
	TestTrue(TEXT("Wider detector landmarks do not push the fit to its limit"), Stats.MaxCorrectionDegrees < 15.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisSurfacePitchTest, "MetaHuman.BodyTracker.PelvisRefinement.SurfacePitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisSurfacePitchTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Fixture(false);
	TArray<FPelvisSurfaceObservation> Observations;
	auto Project = [](const FVec& P) { return Eigen::Vector2d(1000 * P.x() / P.z() + 360, 1000 * P.y() / P.z() + 640); };
	auto HipCenter = [&](const FMat& R)
	{
		const FVec T(0, 0, 3);
		return Eigen::Vector2d((Project(R * FVec(0.09, -0.06, 0) + T) + Project(R * FVec(-0.09, -0.06, 0) + T)) * 0.5);
	};
	for (int32 F = 0; F < FFixture::Frames; ++F)
	{
		const FMat Target = Fixture.Base * Eigen::AngleAxisd(0.24 * F / (FFixture::Frames - 1), FVec::UnitX()).toRotationMatrix();
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const Eigen::Vector2d Pixel = Project(Target * FVec(Side ? -0.09 : 0.09, -0.06, 0) + FVec(0, 0, 3));
			const int32 Slot = Side ? 9 : 12;
			Fixture.Keypoints[F * 75 + Slot * 3] = Pixel.x();
			Fixture.Keypoints[F * 75 + Slot * 3 + 1] = Pixel.y();
		}
		if (F == 0) { continue; }
		for (int32 I = 0; I < 8; ++I)
		{
			const FVec Local((I & 1) ? 0.08 : -0.08, (I & 2) ? 0.07 : -0.07, (I & 4) ? 0.11 : 0.07);
			const FVec Baseline = Fixture.Base * Local;
			const Eigen::Vector2d Motion = (Project(Target * Local + FVec(0, 0, 3)) - HipCenter(Target)) -
				(Project(Baseline + FVec(0, 0, 3)) - HipCenter(Fixture.Base));
			FPelvisSurfaceObservation Observation;
			Observation.Frame = F; Observation.ReferenceFrame = 0;
			Observation.Position = Observation.ReferencePosition = FVector(Baseline.x(), Baseline.y(), Baseline.z());
			Observation.Lever = Observation.ReferenceLever = FVector(Local.x(), Local.y(), Local.z());
			Observation.ObservedMotion = FVector2f(Motion.x(), Motion.y());
			Observation.Confidence = 0.95;
			Observations.Add(Observation);
		}
	}
	FPelvisRefinementSettings Settings;
	Settings.TwistStrength = 0;
	Settings.RollStrength = 0;
	const auto Stats = Fixture.Run(Settings, Observations);
	TestEqual(TEXT("Tracked surface frames are used"), Stats.SurfaceFrames, 8);
	TestTrue(TEXT("Surface motion reduces the three-axis fitting loss"), Stats.FittedSquaredError < Stats.BeforeSquaredError * 0.6);
	TestTrue(TEXT("Surface points recover pitch that hips alone cannot constrain"), Stats.MaxCorrectionDegrees > 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisExaggerationTest, "MetaHuman.BodyTracker.PelvisRefinement.Exaggeration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisExaggerationTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	FFixture Normal(false), Exaggerated(false), Bypass(false);
	FPelvisRefinementSettings Settings;
	Settings.MaxTiltDegrees = 45;
	Settings.TwistStrength = 0;
	const auto NormalStats = Normal.Run(Settings);
	Settings.Exaggeration = 2;
	const auto ExaggeratedStats = Exaggerated.Run(Settings);
	TestTrue(TEXT("Post-fit gain doubles an uncapped rotation correction"),
		FMath::Abs(ExaggeratedStats.MaxCorrectionDegrees - 2 * NormalStats.MaxCorrectionDegrees) < 1.e-4);
	TestTrue(TEXT("Exaggeration does not change the underlying fitted estimate"),
		FMath::Abs(ExaggeratedStats.FittedSquaredError - NormalStats.FittedSquaredError) < 1.e-6);
	TestTrue(TEXT("Artistic overshoot is not rejected by the image-fit gate"), ExaggeratedStats.AfterSquaredError > ExaggeratedStats.FittedSquaredError);
	const auto Original = Bypass.Pose;
	Settings.Exaggeration = 0;
	TestEqual(TEXT("Zero gain bypasses refinement"), Bypass.Run(Settings).RefinedFrames, 0);
	TestTrue(TEXT("Zero gain preserves the original pose"), Bypass.Pose == Original);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisEasingTest, "MetaHuman.BodyTracker.PelvisRefinement.Easing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisEasingTest::RunTest(const FString&)
{
	using namespace PelvisRefinementTests;
	auto SetTilt = [](FFixture& Fixture, int32 F, double Angle)
	{
		const FMat Target = Fixture.Base * Eigen::AngleAxisd(Angle, FVec::UnitZ()).toRotationMatrix();
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FVec P = Target * FVec(Side ? -.09 : .09, -.06, 0) + FVec(0, 0, 3);
			const int32 Offset = F * 75 + (Side ? 9 : 12) * 3;
			Fixture.Keypoints[Offset] = 1000 * P.x() / P.z() + 360;
			Fixture.Keypoints[Offset + 1] = 1000 * P.y() / P.z() + 640;
		}
	};
	auto Correction = [](const FFixture& Fixture, int32 F) -> double
	{
		const FVec AA(Fixture.Pose[F * 165], Fixture.Pose[F * 165 + 1], Fixture.Pose[F * 165 + 2]);
		const Eigen::AngleAxisd Delta(Fixture.Base.transpose() * Eigen::AngleAxisd(AA.norm(), AA.normalized()).toRotationMatrix());
		return Delta.axis().z() * Delta.angle();
	};
	FFixture Raw(false, 61), Eased(false, 61), Repeat(false, 61);
	for (int32 F = 0; F < 61; ++F)
	{
		const double Time = F / 30.0;
		const double Angle = .12 + .05 * FMath::Sin(2 * PI * .7 * Time) + .025 * FMath::Sin(2 * PI * 12 * Time);
		SetTilt(Raw, F, Angle); SetTilt(Eased, F, Angle); SetTilt(Repeat, F, Angle);
	}
	FPelvisRefinementSettings Settings;
	Settings.TwistStrength = 0; Settings.PitchStrength = 0; Settings.TemporalWeight = 0;
	Settings.EasingStrength = 0;
	Raw.Run(Settings); Repeat.Run(Settings);
	TestTrue(TEXT("Zero easing reproduces the exact uneased solve"), Raw.Pose == Repeat.Pose);
	Settings.EasingStrength = .5;
	Eased.Run(Settings);
	double RawRoughness = 0, EasedRoughness = 0, MeanRaw = 0, MeanEased = 0;
	double LowFrequencyRaw = 0, LowFrequencyEased = 0;
	for (int32 F = 5; F < 56; ++F)
	{
		const double R = Correction(Raw, F), E = Correction(Eased, F);
		RawRoughness += FMath::Square(Correction(Raw, F - 1) - 2 * R + Correction(Raw, F + 1));
		EasedRoughness += FMath::Square(Correction(Eased, F - 1) - 2 * E + Correction(Eased, F + 1));
		MeanRaw += R; MeanEased += E;
		const double Wave = FMath::Sin(2 * PI * .7 * F / 30.0);
		LowFrequencyRaw += (R - .12) * Wave;
		LowFrequencyEased += (E - .12) * Wave;
	}
	TestTrue(TEXT("Easing suppresses high-frequency angular acceleration"), EasedRoughness < RawRoughness * .2);
	TestTrue(TEXT("Easing preserves the sustained pelvis correction"), FMath::Abs(MeanEased - MeanRaw) / 51 < .003);
	TestTrue(TEXT("Easing retains deliberate slow articulation"), LowFrequencyEased > LowFrequencyRaw * .9);
	FFixture ConstantRaw(false), ConstantEased(false), GapRaw(false), GapEased(false);
	GapRaw.Valid[4] = GapEased.Valid[4] = false;
	for (int32 F = 0; F < 9; ++F)
	{
		SetTilt(GapRaw, F, F < 4 ? .16 : -.16); SetTilt(GapEased, F, F < 4 ? .16 : -.16);
	}
	Settings.EasingStrength = 0;
	ConstantRaw.Run(Settings); GapRaw.Run(Settings);
	Settings.EasingStrength = 1;
	ConstantEased.Run(Settings); GapEased.Run(Settings);
	double ConstantError = 0, GapError = 0;
	for (int32 I = 0; I < ConstantRaw.Pose.Num(); ++I)
	{
		ConstantError = FMath::Max(ConstantError, double(FMath::Abs(ConstantRaw.Pose[I] - ConstantEased.Pose[I])));
		GapError = FMath::Max(GapError, double(FMath::Abs(GapRaw.Pose[I] - GapEased.Pose[I])));
	}
	TestTrue(TEXT("Constant correction is not attenuated, including sequence ends"), ConstantError < 1.e-5);
	TestTrue(TEXT("Easing does not leak rotations across an invalid frame"), GapError < 1.e-5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPelvisSavedSettingsTest, "MetaHuman.BodyTracker.PelvisRefinement.SavedSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPelvisSavedSettingsTest::RunTest(const FString&)
{
	TStrongObjectPtr<UMetaHumanPerformance> Performance(NewObject<UMetaHumanPerformance>());
	Performance->PelvisEasingStrength = .8f;
	Performance->PelvisTwistStrength = 1.f;
	Performance->PelvisPitchStrength = .7f;
	Performance->PelvisRollStrength = .6f;
	Performance->PelvisTiltStrength = .9f;
	Performance->PelvisExaggeration = 1.1f;
	Performance->PelvisImageWeight = 3.f;
	Performance->PelvisMaxTilt = 18.f;
	Performance->PelvisMaxTwist = 12.f;
	Performance->bPelvisImageTracking = false;
	Performance->bPelvisDumpDiagnostics = true;
	IConsoleVariable* Easing = IConsoleManager::Get().FindConsoleVariable(TEXT("mh.BodyTracker.PelvisRefinement.EasingStrength"));
	const float OldEasing = Easing->GetFloat();
	ON_SCOPE_EXIT { Easing->Set(OldEasing, ECVF_SetByConsole); };
	Easing->Set(.1f, ECVF_SetByConsole);
	const auto Options = ResolvePelvisRefinementOptions(Performance.Get());
	TestFalse(TEXT("Saved settings are authoritative by default"), Options.bUsingConsoleSettings);
	TestEqual(TEXT("Saved easing overrides session console"), Options.Fit.EasingStrength, .8f);
	TestEqual(TEXT("Saved twist is propagated"), Options.Fit.TwistStrength, 1.f);
	TestEqual(TEXT("Saved pitch is propagated"), Options.Fit.PitchStrength, .7f);
	TestEqual(TEXT("Saved roll is propagated"), Options.Fit.RollStrength, .6f);
	TestEqual(TEXT("Saved master tilt is propagated"), Options.Fit.TiltStrength, .9f);
	TestEqual(TEXT("Saved gain is propagated"), Options.Fit.Exaggeration, 1.1f);
	TestEqual(TEXT("Saved image weight is propagated"), Options.Fit.SurfaceWeight, 3.f);
	TestEqual(TEXT("Saved tilt cap is propagated"), Options.Fit.MaxTiltDegrees, 18.f);
	TestEqual(TEXT("Saved twist cap is propagated"), Options.Fit.MaxTwistDegrees, 12.f);
	TestFalse(TEXT("Saved image disable controls image collection"), Options.bImageTracking);
	TestTrue(TEXT("Saved diagnostics flag is propagated"), Options.bDumpDiagnostics);
	Performance->bEnablePelvisRefinement = false;
	TestFalse(TEXT("Saved refinement disable is honored"), ResolvePelvisRefinementOptions(Performance.Get()).bEnabled);
	Performance->PelvisEasingStrength = .2f;
	TestEqual(TEXT("An in-flight snapshot is independent of later asset edits"), Options.Fit.EasingStrength, .8f);
	Performance->bUsePelvisConsoleSettings = true;
	const auto ConsoleOptions = ResolvePelvisRefinementOptions(Performance.Get());
	TestTrue(TEXT("Console workflow is explicitly selectable"), ConsoleOptions.bUsingConsoleSettings);
	TestEqual(TEXT("Console mode uses session easing"), ConsoleOptions.Fit.EasingStrength, .1f);
	return true;
}
}
#endif
