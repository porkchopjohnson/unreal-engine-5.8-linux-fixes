#pragma once

#include "CoreMinimal.h"
#include "NNERuntimeRunSync.h"

class UNNEModelData;

namespace UE::MetaHuman::Cuda
{
// Editor-only ONNX CUDA path. Returns null when unavailable so callers retain CPU fallback.
SEGMENTATION_API TSharedPtr<NNE::IModelInstanceRunSync> CreateInstance(UNNEModelData* ModelData);
}
