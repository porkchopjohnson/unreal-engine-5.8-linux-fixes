#include "CudaInference.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LINUX && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBodyCudaBindingsTest, "MetaHuman.BodyTracker.CUDA.DynamicBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBodyCudaBindingsTest::RunTest(const FString& Parameters)
{
	// ONNX: float X[N] -> Add(X,X)=Y[N], Identity(X)=Z[N]. The identity can alias input storage.
	const uint8 ModelBytes[] = {
		8, 8, 58, 103, 10, 14, 10, 1, 88, 10, 1, 88, 18, 1, 89, 34, 3, 65, 100, 100,
		10, 16, 10, 1, 88, 18, 1, 90, 34, 8, 73, 100, 101, 110, 116, 105, 116, 121,
		18, 13, 100, 121, 110, 97, 109, 105, 99, 95, 97, 108, 105, 97, 115,
		90, 16, 10, 1, 88, 18, 11, 10, 9, 8, 1, 18, 5, 10, 3, 18, 1, 78,
		98, 16, 10, 1, 89, 18, 11, 10, 9, 8, 1, 18, 5, 10, 3, 18, 1, 78,
		98, 16, 10, 1, 90, 18, 11, 10, 9, 8, 1, 18, 5, 10, 3, 18, 1, 78, 66, 2, 16, 17
	};
	auto* Data = NewObject<UNNEModelData>();
	Data->Init(TEXT("onnx"), MakeArrayView(ModelBytes));
	auto GPU = UE::MetaHuman::Cuda::CreateInstance(Data);
	if (!TestTrue(TEXT("CUDA test model initialized"), GPU.IsValid())) return false;
	TArray<uint32> Dimensions{4};
	TArray<UE::NNE::FTensorShape> Shapes{UE::NNE::FTensorShape::Make(Dimensions)};
	TestTrue(TEXT("Dynamic shape accepted"), GPU->SetInputTensorShapes(Shapes) == UE::NNE::EResultStatus::Ok);
	float Values[] = {1, 2, 3, 4}, Identity[4]{};
	TArray<UE::NNE::FTensorBindingCPU> Inputs{{Values, sizeof(Values)}};
	TArray<UE::NNE::FTensorBindingCPU> Outputs{{Values, sizeof(Values)}, {Identity, sizeof(Identity)}};
	if (!TestTrue(TEXT("Aliased output inference succeeds"), GPU->RunSync(Inputs, Outputs) == UE::NNE::EResultStatus::Ok)) return false;
	for (int32 I = 0; I < 4; ++I)
	{
		TestEqual(TEXT("Add output"), Values[I], float((I + 1) * 2));
		TestEqual(TEXT("Identity preserves original input"), Identity[I], float(I + 1));
	}
	TestEqual(TEXT("Dynamic output size resolved"), GPU->GetOutputTensorShapes()[0].Volume(), uint64(4));
	Outputs[1].SizeInBytes = sizeof(float);
	AddExpectedError(TEXT("invalid CUDA output/buffer size"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Undersized output rejected"), GPU->RunSync(Inputs, Outputs) == UE::NNE::EResultStatus::Fail);
	TestEqual(TEXT("Rejected run does not overwrite caller output"), Values[0], 2.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBodyCudaModelsTest, "MetaHuman.BodyTracker.CUDA.ModelParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBodyCudaModelsTest::RunTest(const FString& Parameters)
{
	auto Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
	if (!TestTrue(TEXT("CPU reference runtime"), Runtime.IsValid())) return false;
	for (const TCHAR* ModelName : { TEXT("ViTPose"), TEXT("chmr_backbone") })
	{
		const FString Path = FString::Printf(TEXT("/MetaHumanBodyTracker/Models/Offline/%s.%s"), ModelName, ModelName);
		auto* Data = LoadObject<UNNEModelData>(nullptr, *Path);
		if (!TestNotNull(Path, Data)) return false;
		auto GPU = UE::MetaHuman::Cuda::CreateInstance(Data);
		if (!TestTrue(FString(ModelName) + TEXT(" CUDA session"), GPU.IsValid())) return false;
		auto Model = Runtime->CreateModelCPU(Data);
		if (!TestTrue(TEXT("CPU model"), Model.IsValid())) return false;
		auto CPU = Model->CreateModelInstanceCPU();
		if (!TestTrue(TEXT("CPU instance"), CPU.IsValid())) return false;
		TArray<UE::NNE::FTensorShape> Shapes;
		TArray<TArray<float>> Inputs, CPUOutputs, GPUOutputs;
		TArray<UE::NNE::FTensorBindingCPU> InputBindings, CPUBindings, GPUBindings;
		for (const auto& Desc : GPU->GetInputTensorDescs())
		{
			if (!TestTrue(TEXT("float model input"), Desc.GetDataType() == ENNETensorDataType::Float)) return false;
			Shapes.Add(UE::NNE::FTensorShape::MakeFromSymbolic(Desc.GetShape()));
			TArray<float>& Values = Inputs.AddDefaulted_GetRef();
			Values.SetNumUninitialized(Shapes.Last().Volume());
			for (int32 I = 0; I < Values.Num(); ++I) Values[I] = FMath::Sin(I * 0.013f) * 0.5f;
		}
		for (auto& Input : Inputs) InputBindings.Add({Input.GetData(), uint64(Input.Num()) * sizeof(float)});
		if (!TestTrue(TEXT("CUDA shapes"), GPU->SetInputTensorShapes(Shapes) == UE::NNE::EResultStatus::Ok) ||
			!TestTrue(TEXT("CPU shapes"), CPU->SetInputTensorShapes(Shapes) == UE::NNE::EResultStatus::Ok)) return false;
		if (!TestEqual(TEXT("concrete output shapes"), CPU->GetOutputTensorShapes().Num(), CPU->GetOutputTensorDescs().Num())) return false;
		for (const auto& Shape : CPU->GetOutputTensorShapes())
		{
			CPUOutputs.AddDefaulted_GetRef().SetNumZeroed(Shape.Volume());
			GPUOutputs.AddDefaulted_GetRef().SetNumZeroed(Shape.Volume());
		}
		for (auto& Output : CPUOutputs) CPUBindings.Add({Output.GetData(), uint64(Output.Num()) * sizeof(float)});
		for (auto& Output : GPUOutputs) GPUBindings.Add({Output.GetData(), uint64(Output.Num()) * sizeof(float)});
		double Start = FPlatformTime::Seconds();
		if (!TestTrue(TEXT("CPU inference"), CPU->RunSync(InputBindings, CPUBindings) == UE::NNE::EResultStatus::Ok)) return false;
		const double CPUSeconds = FPlatformTime::Seconds() - Start;
		if (!TestTrue(TEXT("CUDA warmup"), GPU->RunSync(InputBindings, GPUBindings) == UE::NNE::EResultStatus::Ok)) return false;
		Start = FPlatformTime::Seconds();
		if (!TestTrue(TEXT("CUDA inference"), GPU->RunSync(InputBindings, GPUBindings) == UE::NNE::EResultStatus::Ok)) return false;
		const double GPUSeconds = FPlatformTime::Seconds() - Start;
		double Error2 = 0, Reference2 = 0, MaxError = 0;
		int64 Count = 0;
		for (int32 Output = 0; Output < CPUOutputs.Num(); ++Output)
		{
			for (int32 I = 0; I < CPUOutputs[Output].Num(); ++I)
			{
				const float A = CPUOutputs[Output][I], B = GPUOutputs[Output][I];
				if (!FMath::IsFinite(A) || !FMath::IsFinite(B)) { AddError(TEXT("Non-finite inference output")); return false; }
				const double Error = double(A) - B;
				Error2 += Error * Error; Reference2 += double(A) * A;
				MaxError = FMath::Max(MaxError, FMath::Abs(Error)); ++Count;
			}
		}
		const double RelativeError = FMath::Sqrt(Error2 / FMath::Max(Reference2, 1.e-20));
		AddInfo(FString::Printf(TEXT("%s: CPU %.3fs, CUDA %.3fs (warm), relative L2 %.8f, max abs %.8f, %lld outputs"),
			ModelName, CPUSeconds, GPUSeconds, RelativeError, MaxError, Count));
		// CHMR's original graph uses FP16 internal layers. CPU promotes unsupported FP16
		// kernels to FP32, while CUDA executes native FP16; their roundoff is not bit-identical.
		const double Tolerance = FString(ModelName) == TEXT("chmr_backbone") ? 0.005 : 0.001;
		TestTrue(FString::Printf(TEXT("CUDA/CPU relative L2 below %.3f"), Tolerance), RelativeError < Tolerance);
	}
	return true;
}
#endif
