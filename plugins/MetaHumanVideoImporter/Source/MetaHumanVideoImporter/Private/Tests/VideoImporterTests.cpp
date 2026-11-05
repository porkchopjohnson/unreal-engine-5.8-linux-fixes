#if WITH_DEV_AUTOMATION_TESTS

#include "VideoImportJob.h"
#include "CaptureData.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "ImgMediaSource.h"
#include "ImportedPerformanceUtils.h"
#include "MetaHumanPerformance.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoImporterEndToEndTest, "MetaHuman.VideoImporter.EndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVideoImporterEndToEndTest::RunTest(const FString&)
{
	const FString TestId = TEXT("ImporterTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString InputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TestId);
	IFileManager::Get().MakeDirectory(*InputDirectory, true);
	const FString Video = InputDirectory / (TestId + TEXT(".mp4"));
	int32 Code = -1;
	FString Out, Error;
	// A follow-up fixture also exercises spaces in Linux filenames.
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/ffmpeg"),
		*(TEXT("-hide_banner -loglevel error -nostdin -n -f lavfi -i testsrc2=size=128x128:rate=30000/1001 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1.001 -c:v mpeg4 -c:a aac \"") + Video + TEXT("\"")),
		&Code, &Out, &Error);
	if (!TestEqual(TEXT("Generate audio/video fixture: ") + Error, Code, 0)) { return false; }
	const FString AssetRoot = TEXT("/Game/") + TestId;
	auto RunJob = [this](FVideoImportJob& Job)
	{
		if (!Job.Start()) { AddError(Job.GetResult()); return false; }
		const double Deadline = FPlatformTime::Seconds() + 90;
		while (!Job.IsFinished() && FPlatformTime::Seconds() < Deadline)
		{
			Job.Tick();
			FPlatformProcess::Sleep(0.01f);
		}
		if (!Job.IsFinished()) { Job.Cancel(); AddError(TEXT("Import timed out")); return false; }
		return TestTrue(Job.GetResult(), Job.WasSuccessful());
	};
	FVideoImportJob Job(Video, AssetRoot);
	if (RunJob(Job))
	{
		UMetaHumanPerformance* Performance = Job.GetPerformance();
		TestNotNull(TEXT("Created performance"), Performance);
		if (Performance)
		{
			TestTrue(TEXT("Monocular input"), Performance->InputType == EDataInputType::MonoFootage);
			TestEqual(TEXT("Range starts at zero"), Performance->StartFrameToProcess, uint32(0));
			TestEqual(TEXT("Exclusive range is 30 frames"), Performance->EndFrameToProcess, uint32(30));
			TestEqual(TEXT("Default camera is selected"), Performance->Camera, FString(TEXT("Camera 0")));
			const TRange<FFrameNumber>& ProcessingRange = Performance->GetProcessingLimitFrameRange();
			TestEqual(TEXT("Detected processing range starts at zero"), ProcessingRange.GetLowerBoundValue().Value, 0);
			TestEqual(TEXT("Detected processing range includes all frames"), ProcessingRange.GetUpperBoundValue().Value, 30);
			if (FApp::CanEverRender())
			{
				TestTrue(TEXT("Performance is ready to process: ") + Performance->GetCannotProcessTooltipText().ToString(), Performance->CanProcess());
			}
			else if (!Performance->CanProcess())
			{
				AddInfo(TEXT("Headless processing restriction: ") + Performance->GetCannotProcessTooltipText().ToString());
			}
			UFootageCaptureData* Footage = Performance->FootageCaptureData;
			if (TestNotNull(TEXT("Footage assigned"), Footage))
			{
				TestEqual(TEXT("One audio track"), Footage->AudioTracks.Num(), 1);
				TestEqual(TEXT("One image sequence"), Footage->ImageSequences.Num(), 1);
				TestTrue(TEXT("Footage validates"), Footage->IsInitialized(ECaptureDataInitializedCheck::ImageSequencesOnly));
				if (!Footage->ImageSequences.IsEmpty())
				{
					TestTrue(TEXT("Rational rate preserved"), Footage->ImageSequences[0]->FrameRateOverride == FFrameRate(30000, 1001));
					const FStructProperty* PathProperty = FindFProperty<FStructProperty>(UImgMediaSource::StaticClass(), TEXT("SequencePath"));
					if (TestNotNull(TEXT("Sequence path property"), PathProperty))
					{
						const FDirectoryPath* StoredPath = PathProperty->ContainerPtrToValuePtr<FDirectoryPath>(Footage->ImageSequences[0]);
						TestTrue(TEXT("Portable project-relative path"), StoredPath->Path.StartsWith(TEXT("{project_dir}")));
					}
					TestTrue(TEXT("Expanded frame path exists"), IFileManager::Get().DirectoryExists(*Footage->ImageSequences[0]->GetFullPath()));
				}
			}
			TestFalse(TEXT("Performance package saved"), Performance->GetOutermost()->IsDirty());
		}
	}
	FVideoImportJob Duplicate(Video, AssetRoot);
	if (RunJob(Duplicate) && Duplicate.GetPerformance() && Job.GetPerformance())
	{
		TestNotEqual(TEXT("Reimport uses new asset"), Duplicate.GetPerformance()->GetPathName(), Job.GetPerformance()->GetPathName());
	}
	const FString SilentVideo = InputDirectory / (TestId + TEXT(" silent.mp4"));
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/ffmpeg"),
		*(TEXT("-hide_banner -loglevel error -nostdin -n -i \"") + Video + TEXT("\" -an -c:v copy \"") + SilentVideo + TEXT("\"")), &Code, &Out, &Error);
	TestEqual(TEXT("Generate silent fixture"), Code, 0);
	FVideoImportJob Silent(SilentVideo, AssetRoot);
	if (RunJob(Silent) && Silent.GetPerformance())
	{
		TestTrue(TEXT("Silent footage has no audio"), Silent.GetPerformance()->FootageCaptureData->AudioTracks.IsEmpty());
	}
	FVideoImportJob Cancelled(Video, AssetRoot);
	if (Cancelled.Start())
	{
		const double Deadline = FPlatformTime::Seconds() + 10;
		const FString NextOutput = FPaths::ProjectDir() / TEXT("SourceFootage/MetaHuman") / (TestId + TEXT("_002"));
		while (!IFileManager::Get().DirectoryExists(*NextOutput) && !Cancelled.IsFinished() && FPlatformTime::Seconds() < Deadline)
		{
			Cancelled.Tick();
			FPlatformProcess::Sleep(0.01f);
		}
		AddExpectedError(TEXT("Import cancelled"), EAutomationExpectedErrorFlags::Contains, 1);
		Cancelled.Cancel();
		TestFalse(TEXT("Cancellation removes unfinished output"), IFileManager::Get().DirectoryExists(*NextOutput));
		TestFalse(TEXT("Cancelled import not successful"), Cancelled.WasSuccessful());
	}
	AddExpectedError(TEXT("Video file does not exist"), EAutomationExpectedErrorFlags::Contains, 1);
	FVideoImportJob Missing(InputDirectory / TEXT("missing.mp4"), AssetRoot);
	TestFalse(TEXT("Missing input rejected"), Missing.Start());
	IFileManager::Get().DeleteDirectory(*InputDirectory, false, true);
	// Saved output is deliberately retained for inspection in this disposable test project.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoImporterAudioRangeBoundaryTest, "MetaHuman.VideoImporter.AudioRangeBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVideoImporterAudioRangeBoundaryTest::RunTest(const FString&)
{
	// 14.7 seconds is not exactly representable as a SoundWave float duration. Unreal
	// can therefore report 440 usable audio frames for a 441-frame, 30 fps video.
	const FString TestId = TEXT("AudioBoundary_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString InputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TestId);
	IFileManager::Get().MakeDirectory(*InputDirectory, true);
	const FString Video = InputDirectory / (TestId + TEXT(".mp4"));
	int32 Code = -1;
	FString Out, Error;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/ffmpeg"),
		*(TEXT("-hide_banner -loglevel error -nostdin -n -f lavfi -i testsrc2=size=32x32:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 14.7 -c:v mpeg4 -c:a aac \"") + Video + TEXT("\"")),
		&Code, &Out, &Error);
	if (!TestEqual(TEXT("Generate boundary fixture: ") + Error, Code, 0)) { return false; }
	FVideoImportJob Job(Video, TEXT("/Game/") + TestId);
	if (!Job.Start()) { AddError(Job.GetResult()); return false; }
	const double Deadline = FPlatformTime::Seconds() + 90;
	while (!Job.IsFinished() && FPlatformTime::Seconds() < Deadline)
	{
		Job.Tick();
		FPlatformProcess::Sleep(0.01f);
	}
	if (!Job.IsFinished()) { Job.Cancel(); AddError(TEXT("Boundary import timed out")); return false; }
	if (!TestTrue(Job.GetResult(), Job.WasSuccessful())) { return false; }
	UMetaHumanPerformance* Performance = Job.GetPerformance();
	if (!TestNotNull(TEXT("Boundary performance"), Performance)) { return false; }
	const int32 LimitUpper = Performance->GetProcessingLimitFrameRange().GetUpperBoundValue().Value;
	TestEqual(TEXT("Reproduces the rounded-down audio boundary"), LimitUpper, 440);
	TestTrue(TEXT("Imported range fits the usable media range"), Performance->EndFrameToProcess <= uint32(LimitUpper));
	TestTrue(TEXT("Every requested output index is valid"),
		Performance->EndFrameToProcess - Performance->StartFrameToProcess <= Performance->AnimationData.Num());
	TestFalse(TEXT("Valid new imports do not require migration"), UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(Performance));
	// Recreate the saved 1.0 asset state from the crash. Preserve body-only settings
	// and processed output while migrating its invalid end frame.
	Performance->bFaceTracking = false;
	Performance->bBodyTracking = true;
	Performance->EndFrameToProcess = 441;
	const int64 OutputFrames = Performance->AnimationData.Num();
	TestTrue(TEXT("Existing 1.0 import is repaired"), UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(Performance));
	TestEqual(TEXT("Repaired end frame fits output arrays"), Performance->EndFrameToProcess, uint32(LimitUpper));
	TestTrue(TEXT("Body-only settings retained"), !Performance->bFaceTracking && Performance->bBodyTracking);
	TestEqual(TEXT("Existing output retained"), Performance->AnimationData.Num(), OutputFrames);
	TestTrue(TEXT("Repair is marked for saving"), Performance->GetOutermost()->IsDirty());
	TestFalse(TEXT("Repair is idempotent"), UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(Performance));
	Performance->SetProcessingRange(10, 100);
	TestFalse(TEXT("Valid user subrange is preserved"), UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(Performance));
	TestEqual(TEXT("Subrange start retained"), Performance->StartFrameToProcess, uint32(10));
	TestEqual(TEXT("Subrange end retained"), Performance->EndFrameToProcess, uint32(100));
	Performance->StartFrameToProcess = 0;
	Performance->EndFrameToProcess = 441;
	if (TestNotNull(TEXT("Editor available for automatic repair test"), GEditor))
	{
		UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (TestTrue(TEXT("Open existing invalid imported performance"), Editors->OpenEditorForAsset(Performance)))
		{
			TestEqual(TEXT("Opening automatically repairs the old range"), Performance->EndFrameToProcess, uint32(LimitUpper));
			Editors->CloseAllEditorsForAsset(Performance);
		}
	}
	IFileManager::Get().DeleteDirectory(*InputDirectory, false, true);
	return true;
}

#endif
