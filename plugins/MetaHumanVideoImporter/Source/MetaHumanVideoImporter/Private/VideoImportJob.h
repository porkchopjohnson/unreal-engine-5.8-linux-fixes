#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FrameRate.h"
#include "UObject/WeakObjectPtr.h"

/** Polled on the game thread. Media conversion runs in child processes, never in the editor thread. */
class FVideoImportJob
{
public:
	FVideoImportJob(const FString& InFilename, const FString& InAssetRoot);
	~FVideoImportJob();
	bool Start();
	void Tick();
	void Cancel();
	bool IsFinished() const { return bFinished; }
	bool WasSuccessful() const { return bSuccessful; }
	float GetProgress() const;
	FString GetStatus() const;
	const FString& GetResult() const { return Result; }
	class UMetaHumanPerformance* GetPerformance() const;

private:
	enum class EStage { Probe, Frames, Audio, Assets };
	bool Launch(const FString& Executable, const FString& Arguments);
	void CloseProcess();
	void Fail(const FString& Message);
	bool ReadMetadata();
	bool CreateAssets();
	void StartFrames();
	void StartAudioOrAssets();

	FString Filename;
	FString AssetRoot;
	FString TakeName;
	FString OutputDirectory;
	FString OutputRelative;
	FString ProcessOutput;
	FString Result;
	FProcHandle Process;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	EStage Stage = EStage::Probe;
	FFrameRate FrameRate = FFrameRate(30, 1);
	double Duration = 0;
	double AudioOffsetSeconds = 0;
	double ProcessSeconds = 0;
	int32 FrameCount = 0;
	bool bHasAudio = false;
	bool bFinished = false;
	bool bSuccessful = false;
	bool bOwnsOutput = false;
	bool bKeepOutput = false;
	TWeakObjectPtr<class UMetaHumanPerformance> Performance;
};
