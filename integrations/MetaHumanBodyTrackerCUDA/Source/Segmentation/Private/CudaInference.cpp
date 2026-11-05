#include "CudaInference.h"

#if PLATFORM_LINUX && WITH_EDITOR
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "NNEModelData.h"
#include "Serialization/MemoryReader.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

DEFINE_LOG_CATEGORY_STATIC(LogMetaHumanCuda, Log, All);

namespace UE::MetaHuman::Cuda
{
static TAutoConsoleVariable<int32> CVarEnabled(TEXT("mh.BodyTracker.CUDA"), 1,
	TEXT("Use CUDA for offline body inference on Linux. 0 retains the existing CPU path."));
constexpr uint64 Capacity = 256ull * 1024 * 1024;

// A private native process isolates ORT/libstdc++ allocation and CUDA provider loading from Unreal.
// Only small control messages use pipes; tensor data stays in a shared mmap (no disk or pipe copies).
struct FBuffer
{
	uint8* Data;
	uint64 Offset = 0;
	uint64 Limit = Capacity;
	bool bValid = true;
	void Write(const void* Source, uint64 Bytes)
	{
		if (!bValid || Bytes > Limit - Offset) { bValid = false; return; }
		if (Bytes) FMemory::Memcpy(Data + Offset, Source, Bytes);
		Offset += Bytes;
	}
	void Read(void* Target, uint64 Bytes)
	{
		if (!bValid || Bytes > Limit - Offset) { bValid = false; return; }
		if (Bytes) FMemory::Memcpy(Target, Data + Offset, Bytes);
		Offset += Bytes;
	}
	template<class T> void Put(T Value) { Write(&Value, sizeof(T)); }
	template<class T> T Get() { T Value{}; Read(&Value, sizeof(T)); return Value; }
	FString String()
	{
		uint32 Size = Get<uint32>();
		if (!bValid || Size > Limit - Offset) { bValid = false; return {}; }
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data + Offset), Size);
		FString Result(Converted.Length(), Converted.Get());
		Offset += Size;
		return Result;
	}
};

static ENNETensorDataType TensorType(uint32 Type)
{
	switch (Type)
	{
	case 1: return ENNETensorDataType::Float;
	case 2: return ENNETensorDataType::UInt8;
	case 3: return ENNETensorDataType::Int8;
	case 4: return ENNETensorDataType::UInt16;
	case 5: return ENNETensorDataType::Int16;
	case 6: return ENNETensorDataType::Int32;
	case 7: return ENNETensorDataType::Int64;
	case 9: return ENNETensorDataType::Boolean;
	case 10: return ENNETensorDataType::Half;
	case 11: return ENNETensorDataType::Double;
	case 12: return ENNETensorDataType::UInt32;
	case 13: return ENNETensorDataType::UInt64;
	case 16: return ENNETensorDataType::BFloat16;
	default: return ENNETensorDataType::None;
	}
}

class FInstance final : public NNE::IModelInstanceRunSync
{
	FProcHandle Process;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	uint8* Memory = nullptr;
	FString SharedName, TempDirectory, Name, Pending;
	TArray<NNE::FTensorDesc> InputDescs, OutputDescs;
	TArray<NNE::FTensorShape> InputShapes, OutputShapes;
	bool Send(const FString& Command)
	{
		if (!Process.IsValid() || !FPlatformProcess::IsProcRunning(Process)) return false;
		FTCHARToUTF8 Message(*(Command + TEXT("\n")));
		return FPlatformProcess::WritePipe(WritePipe, reinterpret_cast<const uint8*>(Message.Get()), Message.Length(), nullptr);
	}

	bool Wait(const TCHAR* Expected, uint64& OutSize)
	{
		const double Deadline = FPlatformTime::Seconds() + 300;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Pending += FPlatformProcess::ReadPipe(ReadPipe);
			int32 Newline;
			while (Pending.FindChar(TEXT('\n'), Newline))
			{
				FString Line = Pending.Left(Newline).TrimStartAndEnd();
				Pending.RightChopInline(Newline + 1);
				if (Line.StartsWith(Expected))
				{
					OutSize = FCString::Strtoui64(*Line.RightChop(FCString::Strlen(Expected)), nullptr, 10);
					return OutSize > 0 && OutSize <= Capacity;
				}
				if (!Line.IsEmpty()) UE_LOG(LogMetaHumanCuda, Warning, TEXT("%s: %s"), *Name, *Line);
				if (Line.StartsWith(TEXT("MHCUDA_ERROR"))) return false;
			}
			if (!FPlatformProcess::IsProcRunning(Process)) break;
			FPlatformProcess::Sleep(0.001f);
		}
		UE_LOG(LogMetaHumanCuda, Error, TEXT("%s: CUDA worker exited or timed out. %s"), *Name, *Pending);
		return false;
	}

	bool ReadDescs(FBuffer& Buffer, TArray<NNE::FTensorDesc>& Descs)
	{
		uint32 Count = Buffer.Get<uint32>();
		if (Count > 1024) return false;
		for (uint32 I = 0; I < Count; ++I)
		{
			FString TensorName = Buffer.String();
			ENNETensorDataType Type = TensorType(Buffer.Get<uint32>());
			uint32 Rank = Buffer.Get<uint32>();
			if (Rank > NNE::FSymbolicTensorShape::MaxRank || Type == ENNETensorDataType::None) return false;
			TArray<int32> Shape;
			for (uint32 D = 0; D < Rank; ++D)
			{
				int64 Dimension = Buffer.Get<int64>();
				if (Dimension > MAX_int32) return false;
				Shape.Add(Dimension < 0 ? -1 : static_cast<int32>(Dimension));
			}
			Descs.Add(NNE::FTensorDesc::Make(TensorName, NNE::FSymbolicTensorShape::Make(Shape), Type));
		}
		return Buffer.bValid;
	}

public:
	~FInstance() override
	{
		if (Process.IsValid())
		{
			Send(TEXT("QUIT"));
			const double Deadline = FPlatformTime::Seconds() + 5;
			while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.01f);
			if (FPlatformProcess::IsProcRunning(Process)) FPlatformProcess::TerminateProc(Process, true);
			FPlatformProcess::CloseProc(Process);
		}
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		if (Memory) munmap(Memory, Capacity);
		if (!SharedName.IsEmpty()) shm_unlink(TCHAR_TO_UTF8(*SharedName));
		if (!TempDirectory.IsEmpty()) IFileManager::Get().DeleteDirectory(*TempDirectory, false, true);
	}

	bool Init(UNNEModelData* Model)
	{
		Name = Model->GetName();
		FString LibraryDirectory = FPaths::ConvertRelativePathToFull(FPaths::GetPath(FModuleManager::Get().GetModuleFilename(TEXT("Segmentation"))) /
			TEXT("../ThirdParty/OnnxCuda/Linux"));
		FPaths::CollapseRelativeDirectories(LibraryDirectory);
		const FString Executable = LibraryDirectory / TEXT("MetaHumanCudaWorker");
		if (!FPaths::FileExists(Executable))
		{
			UE_LOG(LogMetaHumanCuda, Warning, TEXT("CUDA helper missing. Run Resources/SetupCuda.py."));
			return false;
		}
		const FString ID = FGuid::NewGuid().ToString();
		SharedName = TEXT("/mhcuda_") + ID;
		int FD = shm_open(TCHAR_TO_UTF8(*SharedName), O_CREAT | O_EXCL | O_RDWR, 0600);
		if (FD < 0) return false;
		bool bSized = ftruncate(FD, Capacity) == 0;
		void* Map = bSized ? mmap(nullptr, Capacity, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0) : MAP_FAILED;
		close(FD);
		if (Map == MAP_FAILED) return false;
		Memory = static_cast<uint8*>(Map);
		TempDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("MetaHumanCuda") / ID);
		const FString ModelFile = TempDirectory / TEXT("model.onnx");
		if (!FFileHelper::SaveArrayToFile(Model->GetFileData(), *ModelFile)) return false;
		const auto Descriptor = Model->GetAdditionalFileData(TEXT("OnnxExternalDataDescriptor"));
		if (!Descriptor.IsEmpty())
		{
			FMemoryReaderView Reader(Descriptor, true);
			TMap<FString, int64> Files;
			Reader << Files;
			const auto Bytes = Model->GetAdditionalFileData(TEXT("OnnxExternalDataBytes"));
			int64 Offset = 0;
			for (const auto& File : Files)
			{
				FString Path = FPaths::ConvertRelativePathToFull(TempDirectory / File.Key);
				FPaths::CollapseRelativeDirectories(Path);
				if (Reader.IsError() || File.Value < 0 || File.Value > Bytes.Num() - Offset ||
					!FPaths::IsUnderDirectory(Path, TempDirectory) ||
					!FFileHelper::SaveArrayToFile(MakeArrayView(Bytes.GetData() + Offset, File.Value), *Path)) return false;
				Offset += File.Value;
			}
		}
		void* ChildRead = nullptr;
		void* ChildWrite = nullptr;
		if (!FPlatformProcess::CreatePipe(ReadPipe, ChildWrite) || !FPlatformProcess::CreatePipe(ChildRead, WritePipe, true)) return false;
		FString ProfilePrefix = TEXT("-");
		if (FParse::Param(FCommandLine::Get(), TEXT("MetaHumanCudaProfile")))
		{
			ProfilePrefix = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CudaProfiles") / (Name + TEXT("_") + ID));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProfilePrefix), true);
		}
		// Set the library path only in the child; Unreal keeps its original ORT and CUDA environment.
		FString Args = FString::Printf(TEXT("\"LD_LIBRARY_PATH=%s\" \"%s\" \"%s\" \"%s\" \"%s\""),
			*LibraryDirectory, *Executable, *ModelFile, *SharedName, *ProfilePrefix);
		Process = FPlatformProcess::CreateProc(TEXT("/usr/bin/env"), *Args, false, true, true, nullptr, 0, nullptr, ChildWrite, ChildRead, ChildWrite);
		FPlatformProcess::ClosePipe(ChildRead, ChildWrite);
		if (!Process.IsValid()) return false;
		uint64 Size = 0;
		if (!Wait(TEXT("MHCUDA_READY "), Size)) return false;
		FBuffer Buffer{Memory, 0, Size};
		if (!ReadDescs(Buffer, InputDescs) || !ReadDescs(Buffer, OutputDescs)) return false;
		// The session has read its weights, so the temporary model copy can be removed now.
		IFileManager::Get().DeleteDirectory(*TempDirectory, false, true);
		TempDirectory.Reset();
		shm_unlink(TCHAR_TO_UTF8(*SharedName));
		UE_LOG(LogMetaHumanCuda, Display, TEXT("CUDA session ready (isolated worker, TF32 off): %s"), *Model->GetPathName());
		return true;
	}

	TConstArrayView<NNE::FTensorDesc> GetInputTensorDescs() const override { return InputDescs; }
	TConstArrayView<NNE::FTensorDesc> GetOutputTensorDescs() const override { return OutputDescs; }
	TConstArrayView<NNE::FTensorShape> GetInputTensorShapes() const override { return InputShapes; }
	TConstArrayView<NNE::FTensorShape> GetOutputTensorShapes() const override { return OutputShapes; }
	ESetInputTensorShapesStatus SetInputTensorShapes(TConstArrayView<NNE::FTensorShape> Shapes) override
	{
		InputShapes.Reset(); OutputShapes.Reset();
		if (Shapes.Num() != InputDescs.Num()) return ESetInputTensorShapesStatus::Fail;
		for (int32 I = 0; I < Shapes.Num(); ++I)
			if (!Shapes[I].IsCompatibleWith(InputDescs[I].GetShape())) return ESetInputTensorShapesStatus::Fail;
		InputShapes = Shapes;
		for (const auto& Desc : OutputDescs)
		{
			if (!Desc.GetShape().IsConcrete()) { OutputShapes.Reset(); break; }
			OutputShapes.Add(NNE::FTensorShape::MakeFromSymbolic(Desc.GetShape()));
		}
		return ESetInputTensorShapesStatus::Ok;
	}
	ERunSyncStatus RunSync(TConstArrayView<NNE::FTensorBindingCPU> Inputs, TConstArrayView<NNE::FTensorBindingCPU> Outputs) override
	{
		if (InputShapes.Num() != InputDescs.Num() || Inputs.Num() != InputDescs.Num() || Outputs.Num() != OutputDescs.Num()) return ERunSyncStatus::Fail;
		FBuffer Buffer{Memory};
		Buffer.Put<uint32>(Inputs.Num());
		for (int32 I = 0; I < Inputs.Num(); ++I)
		{
			uint64 Bytes = InputShapes[I].Volume() * InputDescs[I].GetElementByteSize();
			if (Inputs[I].SizeInBytes < Bytes || (Bytes && !Inputs[I].Data)) return ERunSyncStatus::Fail;
			Buffer.Put<uint32>(InputShapes[I].Rank());
			for (uint32 Dimension : InputShapes[I].GetData()) Buffer.Put<int64>(Dimension);
			Buffer.Put<uint64>(Bytes);
			Buffer.Write(Inputs[I].Data, Bytes);
		}
		if (!Buffer.bValid || !Send(FString::Printf(TEXT("RUN %llu"), Buffer.Offset))) return ERunSyncStatus::Fail;
		uint64 Size = 0;
		if (!Wait(TEXT("MHCUDA_DONE "), Size)) return ERunSyncStatus::Fail;
		Buffer = FBuffer{Memory, 0, Size};
		if (Buffer.Get<uint32>() != static_cast<uint32>(Outputs.Num())) return ERunSyncStatus::Fail;
		TArray<uint64> Offsets, Sizes;
		TArray<NNE::FTensorShape> Shapes;
		for (int32 I = 0; I < Outputs.Num(); ++I)
		{
			uint32 Rank = Buffer.Get<uint32>();
			if (Rank > NNE::FTensorShape::MaxRank) return ERunSyncStatus::Fail;
			TArray<uint32> Shape;
			for (uint32 D = 0; D < Rank; ++D)
			{
				int64 Dimension = Buffer.Get<int64>();
				if (Dimension < 0 || Dimension > MAX_uint32) return ERunSyncStatus::Fail;
				Shape.Add(Dimension);
			}
			uint64 Bytes = Buffer.Get<uint64>();
			Shapes.Add(NNE::FTensorShape::Make(Shape));
			if (!Buffer.bValid || Bytes > Buffer.Limit - Buffer.Offset || Bytes > Outputs[I].SizeInBytes ||
				Bytes != Shapes.Last().Volume() * OutputDescs[I].GetElementByteSize() || (Bytes && !Outputs[I].Data))
			{
				UE_LOG(LogMetaHumanCuda, Error, TEXT("%s: invalid CUDA output/buffer size at index %d"), *Name, I);
				return ERunSyncStatus::Fail;
			}
			Offsets.Add(Buffer.Offset); Sizes.Add(Bytes); Buffer.Offset += Bytes;
		}
		for (int32 I = 0; I < Outputs.Num(); ++I) if (Sizes[I]) FMemory::Memcpy(Outputs[I].Data, Memory + Offsets[I], Sizes[I]);
		OutputShapes = MoveTemp(Shapes);
		return ERunSyncStatus::Ok;
	}
};

TSharedPtr<NNE::IModelInstanceRunSync> CreateInstance(UNNEModelData* Model)
{
	if (!Model || !CVarEnabled.GetValueOnAnyThread() || FParse::Param(FCommandLine::Get(), TEXT("NoMetaHumanCuda")) ||
		Model->GetFileType() != TEXT("onnx") || Model->GetFileData().IsEmpty()) return nullptr;
	auto Result = MakeShared<FInstance>();
	if (Result->Init(Model)) return Result;
	UE_LOG(LogMetaHumanCuda, Warning, TEXT("CUDA could not initialize %s; trying existing runtime fallbacks."), *Model->GetPathName());
	return nullptr;
}
}
#else
namespace UE::MetaHuman::Cuda
{
TSharedPtr<NNE::IModelInstanceRunSync> CreateInstance(UNNEModelData*) { return nullptr; }
}
#endif
