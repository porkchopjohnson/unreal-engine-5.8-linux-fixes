#include "VideoImportJob.h"
#include "ImportedPerformanceUtils.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "CaptureData.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImgMediaSource.h"
#include "MetaHumanPerformance.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMetaHumanVideoImporter, Log, All);

bool UE::MetaHumanVideoImporter::RepairImportedPerformanceRange(UMetaHumanPerformance* Performance)
{
	if (!Performance || Performance->InputType != EDataInputType::MonoFootage || !Performance->FootageCaptureData)
	{
		return false;
	}
	FString ImportRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("SourceFootage/MetaHuman"));
	FPaths::RemoveDuplicateSlashes(ImportRoot);
	bool bImportedFootage = false;
	for (const UImgMediaSource* Images : Performance->FootageCaptureData->ImageSequences)
	{
		if (!Images)
		{
			continue;
		}
		FString ImagePath = Images->GetFullPath();
		// Expanding {project_dir}/ can introduce // because ProjectDir ends in /.
		FPaths::RemoveDuplicateSlashes(ImagePath);
		if (FPaths::IsUnderDirectory(ImagePath, ImportRoot))
		{
			bImportedFootage = true;
			break;
		}
	}
	if (!bImportedFootage)
	{
		return false;
	}
	const TRange<FFrameNumber>& Limits = Performance->GetProcessingLimitFrameRange();
	const int32 Lower = Limits.GetLowerBoundValue().Value;
	const int32 Upper = Limits.GetUpperBoundValue().Value;
	if (Lower < 0 || Upper <= Lower)
	{
		return false;
	}
	const uint32 OldStart = Performance->StartFrameToProcess;
	const uint32 OldEnd = Performance->EndFrameToProcess;
	if (OldStart >= uint32(Lower) && OldStart <= OldEnd && OldEnd <= uint32(Upper))
	{
		return false;
	}
	Performance->Modify();
	Performance->SetProcessingRange(int32(FMath::Min(OldStart, uint32(MAX_int32))), int32(FMath::Min(OldEnd, uint32(MAX_int32))));
	Performance->MarkPackageDirty();
	UE_LOG(LogMetaHumanVideoImporter, Display, TEXT("Repaired imported Performance range %s: [%u, %u) -> [%u, %u). Save the asset to keep the repair."),
		*Performance->GetPathName(), OldStart, OldEnd, Performance->StartFrameToProcess, Performance->EndFrameToProcess);
	return true;
}

namespace
{
// FPlatformProcess parses arguments without a shell. Reject embedded quotes rather than
// accidentally splitting a Linux filename into extra ffmpeg arguments.
FString Quote(const FString& Value)
{
	return TEXT("\"") + Value + TEXT("\"");
}

bool SaveAsset(UObject* Asset)
{
	Asset->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Asset->GetOutermost(), Asset,
		*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
}

template<typename T>
T* NewAsset(const FString& Folder, const FString& Name)
{
	UPackage* Package = CreatePackage(*(Folder / Name));
	T* Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}
}

FVideoImportJob::FVideoImportJob(const FString& InFilename, const FString& InAssetRoot)
	: Filename(FPaths::ConvertRelativePathToFull(InFilename)), AssetRoot(InAssetRoot)
{
}

FVideoImportJob::~FVideoImportJob()
{
	Cancel();
}

UMetaHumanPerformance* FVideoImportJob::GetPerformance() const
{
	return Performance.Get();
}

bool FVideoImportJob::Start()
{
	if (Filename.Contains(TEXT("\"")) || Filename.Contains(TEXT("\n")) || Filename.Contains(TEXT("\r")) ||
		FPaths::ProjectDir().Contains(TEXT("\"")) || FPaths::ProjectDir().Contains(TEXT("\n")) || FPaths::ProjectDir().Contains(TEXT("\r")))
	{
		Fail(TEXT("Video and project paths must not contain double quotes or newlines."));
		return false;
	}
	FText Reason;
	if (!(AssetRoot == TEXT("/Game") || AssetRoot.StartsWith(TEXT("/Game/"))) ||
		!FPackageName::IsValidLongPackageName(AssetRoot / TEXT("Validation"), false, &Reason))
	{
		Fail(TEXT("Choose an asset folder under /Game, for example /Game/MetaHumanFootage. ") + Reason.ToString());
		return false;
	}
	if (!IFileManager::Get().FileExists(*Filename))
	{
		Fail(TEXT("Video file does not exist: ") + Filename);
		return false;
	}
	return Launch(TEXT("/usr/bin/ffprobe"), TEXT("-v error -show_streams -show_format -of json ") + Quote(Filename));
}

bool FVideoImportJob::Launch(const FString& Executable, const FString& Arguments)
{
	ProcessOutput.Reset();
	ProcessSeconds = 0;
	if (!FPaths::FileExists(Executable) || !FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
	{
		Fail(TEXT("Cannot run ") + Executable + TEXT(". Install ffmpeg (including ffprobe) to /usr/bin."));
		return false;
	}
	Process = FPlatformProcess::CreateProc(*Executable, *Arguments, false, true, true, nullptr, 0, nullptr, WritePipe, nullptr, WritePipe);
	if (!Process.IsValid())
	{
		Fail(TEXT("Could not start ") + Executable);
		return false;
	}
	return true;
}

void FVideoImportJob::CloseProcess()
{
	if (Process.IsValid())
	{
		FPlatformProcess::CloseProc(Process);
		Process.Reset();
	}
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = WritePipe = nullptr;
	}
}

void FVideoImportJob::Cancel()
{
	if (Process.IsValid())
	{
		FPlatformProcess::TerminateProc(Process, true);
		FPlatformProcess::WaitForProc(Process);
	}
	CloseProcess();
	if (!bFinished)
	{
		Fail(TEXT("Import cancelled."));
	}
}

void FVideoImportJob::Fail(const FString& Message)
{
	CloseProcess();
	if (bOwnsOutput && !bKeepOutput)
	{
		IFileManager::Get().DeleteDirectory(*OutputDirectory, false, true);
		bOwnsOutput = false;
	}
	bFinished = true;
	Result = FPaths::GetCleanFilename(Filename) + TEXT(": ") + Message;
	UE_LOG(LogMetaHumanVideoImporter, Error, TEXT("%s"), *Result);
}

void FVideoImportJob::Tick()
{
	if (bFinished || !Process.IsValid())
	{
		return;
	}
	ProcessOutput += FPlatformProcess::ReadPipe(ReadPipe);
	// Progress and diagnostics are bounded even for very long clips.
	if (ProcessOutput.Len() > 262144)
	{
		ProcessOutput.RightInline(131072);
	}
	const int32 ProgressIndex = ProcessOutput.Find(TEXT("out_time_us="), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ProgressIndex != INDEX_NONE)
	{
		ProcessSeconds = FCString::Atod(*ProcessOutput.Mid(ProgressIndex + 12)) / 1000000.0;
	}
	if (FPlatformProcess::IsProcRunning(Process))
	{
		return;
	}
	ProcessOutput += FPlatformProcess::ReadPipe(ReadPipe);
	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
	CloseProcess();
	if (ReturnCode != 0)
	{
		Fail(FString::Printf(TEXT("Conversion failed (exit %d):\n%s"), ReturnCode, *ProcessOutput.Right(4000)));
		return;
	}
	if (Stage == EStage::Probe)
	{
		if (ReadMetadata())
		{
			StartFrames();
		}
	}
	else if (Stage == EStage::Frames)
	{
		TArray<FString> Frames;
		IFileManager::Get().FindFiles(Frames, *(OutputDirectory / TEXT("Frames/*.png")), true, false);
		FrameCount = Frames.Num();
		if (FrameCount == 0)
		{
			Fail(TEXT("No frames were decoded."));
			return;
		}
		StartAudioOrAssets();
	}
	else if (Stage == EStage::Audio)
	{
		Stage = EStage::Assets;
		CreateAssets();
	}
}

bool FVideoImportJob::ReadMetadata()
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ProcessOutput), Root) || !Root.IsValid())
	{
		Fail(TEXT("ffprobe returned invalid metadata."));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Streams = nullptr;
	if (!Root->TryGetArrayField(TEXT("streams"), Streams))
	{
		Fail(TEXT("No media streams were found."));
		return false;
	}
	bool bFoundVideo = false;
	double VideoStart = 0, AudioStart = 0;
	for (const TSharedPtr<FJsonValue>& StreamValue : *Streams)
	{
		const TSharedPtr<FJsonObject> Stream = StreamValue->AsObject();
		FString Type;
		if (!Stream.IsValid() || !Stream->TryGetStringField(TEXT("codec_type"), Type))
		{
			continue;
		}
		if (Type == TEXT("audio") && !bHasAudio)
		{
			FString StartTime;
			Stream->TryGetStringField(TEXT("start_time"), StartTime);
			AudioStart = FCString::Atod(*StartTime);
			bHasAudio = true;
		}
		if (Type != TEXT("video") || bFoundVideo)
		{
			continue;
		}
		// ffmpeg's V:0 selector excludes cover art / attached pictures.
		const TSharedPtr<FJsonObject>* Disposition = nullptr;
		if (Stream->TryGetObjectField(TEXT("disposition"), Disposition) && (*Disposition)->GetIntegerField(TEXT("attached_pic")) != 0)
		{
			continue;
		}
		FString Rate;
		Stream->TryGetStringField(TEXT("avg_frame_rate"), Rate);
		auto ParseRate = [this](const FString& InRate)
		{
			FString Numerator, Denominator;
			int32 N = 0, D = 0;
			if (InRate.Split(TEXT("/"), &Numerator, &Denominator) && LexTryParseString(N, *Numerator) &&
				LexTryParseString(D, *Denominator) && N > 0 && D > 0 && double(N) / D <= 240)
			{
				FrameRate = FFrameRate(N, D);
				return true;
			}
			return false;
		};
		if (!ParseRate(Rate))
		{
			Stream->TryGetStringField(TEXT("r_frame_rate"), Rate);
			if (!ParseRate(Rate))
			{
				Fail(TEXT("Cannot determine a video frame rate between 0 and 240 fps."));
				return false;
			}
		}
		FString DurationString;
		FString StartTime;
		Stream->TryGetStringField(TEXT("start_time"), StartTime);
		VideoStart = FCString::Atod(*StartTime);
		Stream->TryGetStringField(TEXT("duration"), DurationString);
		Duration = FCString::Atod(*DurationString);
		bFoundVideo = true;
	}
	AudioOffsetSeconds = AudioStart - VideoStart;
	if (!bFoundVideo)
	{
		Fail(TEXT("No usable video stream was found."));
		return false;
	}
	if (Duration <= 0)
	{
		const TSharedPtr<FJsonObject>* Format = nullptr;
		if (Root->TryGetObjectField(TEXT("format"), Format))
		{
			FString DurationString;
			(*Format)->TryGetStringField(TEXT("duration"), DurationString);
			Duration = FCString::Atod(*DurationString);
		}
	}
	return true;
}

void FVideoImportJob::StartFrames()
{
	FString BaseName = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(Filename));
	if (BaseName.IsEmpty())
	{
		BaseName = TEXT("Take");
	}
	BaseName.LeftInline(80);
	// Reserve a fresh take: never overwrite existing assets or footage, including on reimport.
	for (int32 Suffix = 0; ; ++Suffix)
	{
		TakeName = Suffix == 0 ? BaseName : FString::Printf(TEXT("%s_%03d"), *BaseName, Suffix);
		OutputRelative = TEXT("SourceFootage/MetaHuman") / TakeName;
		OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / OutputRelative);
		const FString AssetFolder = AssetRoot / TakeName;
		if (!IFileManager::Get().DirectoryExists(*OutputDirectory) &&
			!IFileManager::Get().DirectoryExists(*FPackageName::LongPackageNameToFilename(AssetFolder)) &&
			!FindObject<UPackage>(nullptr, *(AssetFolder / (TEXT("Performance_") + TakeName))))
		{
			break;
		}
	}
	if (!IFileManager::Get().MakeDirectory(*(OutputDirectory / TEXT("Frames")), true))
	{
		Fail(TEXT("Cannot create footage directory: ") + OutputDirectory);
		return;
	}
	bOwnsOutput = true;
	Stage = EStage::Frames;
	const FString Rate = FString::Printf(TEXT("%d/%d"), FrameRate.Numerator, FrameRate.Denominator);
	// CFR resampling preserves duration for variable-frame-rate phone videos; -autorotate is
	// ffmpeg's default. Audio is aligned to the first video timestamp in StartAudioOrAssets.
	Launch(TEXT("/usr/bin/ffmpeg"), TEXT("-hide_banner -loglevel error -nostdin -n -i ") + Quote(Filename) +
		TEXT(" -map 0:V:0 -an -vf ") + Quote(TEXT("setpts=PTS-STARTPTS,fps=") + Rate) +
		TEXT(" -pix_fmt rgb24 -start_number 0 -progress pipe:1 -nostats ") + Quote(OutputDirectory / TEXT("Frames/%08d.png")));
}

void FVideoImportJob::StartAudioOrAssets()
{
	if (bHasAudio)
	{
		Stage = EStage::Audio;
		const int64 OffsetSamples = FMath::RoundToInt64(AudioOffsetSeconds * 48000);
		const FString Filter = FString::Printf(
			TEXT("aresample=48000:async=1,atrim=start_sample=%lld,asetpts=PTS-STARTPTS,adelay=%lldS:all=1,apad"),
			FMath::Max<int64>(0, -OffsetSamples), FMath::Max<int64>(0, OffsetSamples));
		const FString OutputDuration = FString::Printf(TEXT("%.9f"), FrameCount / FrameRate.AsDecimal());
		Launch(TEXT("/usr/bin/ffmpeg"), TEXT("-hide_banner -loglevel error -nostdin -n -i ") + Quote(Filename) +
			TEXT(" -map 0:a:0 -vn -ar 48000 -af ") + Quote(Filter) + TEXT(" -t ") + OutputDuration +
			TEXT(" -ac 1 -c:a pcm_s16le -progress pipe:1 -nostats ") +
			Quote(OutputDirectory / TEXT("Audio.wav")));
	}
	else
	{
		Stage = EStage::Assets;
		CreateAssets();
	}
}

bool FVideoImportJob::CreateAssets()
{
	// Once assets reference these files, preserve them even if a package cannot be saved.
	bKeepOutput = true;
	const FString Folder = AssetRoot / TakeName;
	TStrongObjectPtr<UImgMediaSource> Images(NewAsset<UImgMediaSource>(Folder, TEXT("Images_") + TakeName));
	Images->SetTokenizedSequencePath(TEXT("{project_dir}/") + OutputRelative / TEXT("Frames"));
	Images->FrameRateOverride = FrameRate;
	Images->StartTimecode = FTimecode(0, 0, 0, 0, false);
	TStrongObjectPtr<UFootageCaptureData> Footage(NewAsset<UFootageCaptureData>(Folder, TEXT("Footage_") + TakeName));
	Footage->ImageSequences.Add(Images.Get());
	Footage->Metadata.FrameRate = FrameRate.AsDecimal();
	Footage->Metadata.DeviceModelName = TEXT("Imported monocular video");
	if (bHasAudio)
	{
		TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
		Task->Filename = OutputDirectory / TEXT("Audio.wav");
		Task->DestinationPath = Folder;
		Task->DestinationName = TEXT("Audio_") + TakeName;
		Task->bAutomated = true;
		Task->bSave = false;
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task.Get()});
		for (UObject* Object : Task->GetObjects())
		{
			if (USoundWave* Sound = Cast<USoundWave>(Object))
			{
				Footage->AudioTracks.Add(Sound);
			}
		}
		if (Footage->AudioTracks.IsEmpty())
		{
			Fail(TEXT("Frames converted, but WAV import failed. Footage retained at ") + OutputDirectory);
			return false;
		}
	}
	const UFootageCaptureData::FVerifyResult Verified = Footage->VerifyData(ECaptureDataInitializedCheck::ImageSequencesOnly);
	if (Verified.HasError())
	{
		Fail(TEXT("Generated footage failed validation: ") + Verified.GetError());
		return false;
	}
	TStrongObjectPtr<UMetaHumanPerformance> NewPerformance(NewAsset<UMetaHumanPerformance>(Folder, TEXT("Performance_") + TakeName));
	NewPerformance->SetInputType(EDataInputType::MonoFootage);
	NewPerformance->SetFootageCaptureData(Footage.Get());
	NewPerformance->TimecodeAlignment = ETimecodeAlignment::Relative;
	// MetaHuman intersects image and audio ranges. SoundWave's float duration can
	// round down by a frame, so the decoded image count is not a safe processing end.
	NewPerformance->SetProcessingRange(0, FrameCount);
	if (NewPerformance->FootageCaptureData != Footage.Get())
	{
		Fail(TEXT("MetaHuman rejected the generated footage. Files retained at ") + OutputDirectory);
		return false;
	}
	TArray<UObject*> Assets = {Images.Get(), Footage.Get(), NewPerformance.Get()};
	for (USoundWave* Sound : Footage->AudioTracks)
	{
		Assets.Add(Sound);
	}
	for (UObject* Asset : Assets)
	{
		if (!SaveAsset(Asset))
		{
			Fail(TEXT("Could not save ") + Asset->GetPathName() + TEXT(". Assets remain in the editor; footage retained at ") + OutputDirectory);
			return false;
		}
	}
	FString Metadata;
	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("source"), Filename);
	Manifest->SetStringField(TEXT("performance"), NewPerformance->GetPathName());
	Manifest->SetNumberField(TEXT("frame_rate_numerator"), FrameRate.Numerator);
	Manifest->SetNumberField(TEXT("frame_rate_denominator"), FrameRate.Denominator);
	Manifest->SetNumberField(TEXT("frames"), FrameCount);
	Manifest->SetBoolField(TEXT("has_audio"), bHasAudio);
	Manifest->SetNumberField(TEXT("source_audio_offset_seconds"), AudioOffsetSeconds);
	Manifest->SetStringField(TEXT("timing"), TEXT("CFR resampled; relative start, no embedded source timecode"));
	FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Metadata));
	if (!FFileHelper::SaveStringToFile(Metadata, *(OutputDirectory / TEXT("Import.json"))))
	{
		UE_LOG(LogMetaHumanVideoImporter, Warning, TEXT("Could not write import metadata in %s"), *OutputDirectory);
	}
	Performance = NewPerformance.Get();
	bSuccessful = bFinished = true;
	Result = FString::Printf(TEXT("%s: imported %d frames at %.3f fps%s → %s"),
		*FPaths::GetCleanFilename(Filename), FrameCount, FrameRate.AsDecimal(), bHasAudio ? TEXT(" with audio") : TEXT(" (silent)"), *NewPerformance->GetPathName());
	UE_LOG(LogMetaHumanVideoImporter, Display, TEXT("%s"), *Result);
	return true;
}

float FVideoImportJob::GetProgress() const
{
	if (bFinished) { return 1; }
	const float MediaProgress = Duration > 0 ? FMath::Clamp(ProcessSeconds / Duration, 0.0, 1.0) : 0;
	switch (Stage)
	{
	case EStage::Probe: return 0;
	case EStage::Frames: return 0.05f + 0.8f * MediaProgress;
	case EStage::Audio: return 0.85f + 0.1f * MediaProgress;
	default: return 0.95f;
	}
}

FString FVideoImportJob::GetStatus() const
{
	const TCHAR* StageText = Stage == EStage::Probe ? TEXT("Reading metadata") : Stage == EStage::Frames ?
		TEXT("Converting frames") : Stage == EStage::Audio ? TEXT("Converting audio") : TEXT("Creating assets");
	return FString::Printf(TEXT("%s — %s"), *FPaths::GetCleanFilename(Filename), StageText);
}
