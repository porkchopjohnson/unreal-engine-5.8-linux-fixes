// Standalone system-C++ process. Never link this into Unreal (allocator/ORT isolation).
#include "onnxruntime_cxx_api.h"
#include <sys/mman.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

constexpr size_t Capacity = 256ull * 1024 * 1024;
struct Buffer {
    unsigned char* data;
    size_t offset = 0, limit = Capacity;
    void check(size_t bytes) { if (bytes > limit - offset) throw std::runtime_error("Shared tensor capacity exceeded"); }
    template<class T> T get() { T v; check(sizeof(v)); std::memcpy(&v, data + offset, sizeof(v)); offset += sizeof(v); return v; }
    void put(const void* p, size_t bytes) { check(bytes); if (bytes) std::memcpy(data + offset, p, bytes); offset += bytes; }
    template<class T> void put(T v) { put(&v, sizeof(v)); }
    void string(const char* s) { put<uint32_t>(std::strlen(s)); put(s, std::strlen(s)); }
};

static void descriptors(Ort::Session& session, bool input, Buffer& out) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = input ? session.GetInputCount() : session.GetOutputCount();
    out.put<uint32_t>(count);
    for (size_t i = 0; i < count; ++i) {
        auto name = input ? session.GetInputNameAllocated(i, allocator) : session.GetOutputNameAllocated(i, allocator);
        auto info = input ? session.GetInputTypeInfo(i) : session.GetOutputTypeInfo(i);
        auto tensor = info.GetTensorTypeAndShapeInfo();
        auto shape = tensor.GetShape();
        out.string(name.get());
        out.put<uint32_t>(tensor.GetElementType());
        out.put<uint32_t>(shape.size());
        for (int64_t dim : shape) out.put<int64_t>(dim);
    }
}

static size_t element_size(ONNXTensorElementDataType type) {
    switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32: return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64: return 8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return 2;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return 1;
    default: throw std::runtime_error("Unsupported tensor data type");
    }
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Expected model, shared memory name, profile prefix");
        const pid_t parent = getppid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) return 1;
        int fd = shm_open(argv[2], O_RDWR, 0);
        if (fd < 0) throw std::runtime_error("Cannot open shared tensor memory");
        void* map = mmap(nullptr, Capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (map == MAP_FAILED) throw std::runtime_error("Cannot map shared tensor memory");
        auto* memory = static_cast<unsigned char*>(map);
        std::unique_ptr<unsigned char[]> scratch(new unsigned char[Capacity]);
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "MetaHumanCUDA");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        OrtCUDAProviderOptionsV2* cuda = nullptr;
        Ort::ThrowOnError(Ort::GetApi().CreateCUDAProviderOptions(&cuda));
        const char* keys[] = {"device_id", "use_tf32", "cudnn_conv_algo_search", "arena_extend_strategy"};
        const char* values[] = {"0", "0", "HEURISTIC", "kSameAsRequested"};
        Ort::ThrowOnError(Ort::GetApi().UpdateCUDAProviderOptions(cuda, keys, values, 4));
        options.AppendExecutionProvider_CUDA_V2(*cuda);
        Ort::GetApi().ReleaseCUDAProviderOptions(cuda);
        const bool profile = std::string(argv[3]) != "-";
        if (profile) options.EnableProfiling(argv[3]);
        Ort::Session session(env, argv[1], options);
        Ort::AllocatorWithDefaultOptions allocator;
        auto cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<std::string> input_names, output_names;
        std::vector<const char*> inputs, outputs;
        std::vector<ONNXTensorElementDataType> types;
        for (size_t i = 0; i < session.GetInputCount(); ++i) {
            input_names.emplace_back(session.GetInputNameAllocated(i, allocator).get());
            types.push_back(session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetElementType());
        }
        for (size_t i = 0; i < session.GetOutputCount(); ++i) output_names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
        for (auto& n : input_names) inputs.push_back(n.c_str());
        for (auto& n : output_names) outputs.push_back(n.c_str());
        Buffer meta{memory};
        descriptors(session, true, meta); descriptors(session, false, meta);
        std::cout << "MHCUDA_READY " << meta.offset << std::endl;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "QUIT") break;
            if (command.rfind("RUN ", 0) != 0) throw std::runtime_error("Invalid worker command");
            size_t length = std::stoull(command.substr(4));
            if (length > Capacity) throw std::runtime_error("Invalid request length");
            Buffer in{memory, 0, length};
            if (in.get<uint32_t>() != inputs.size()) throw std::runtime_error("Input count mismatch");
            std::vector<Ort::Value> tensors;
            for (size_t i = 0; i < inputs.size(); ++i) {
                uint32_t rank = in.get<uint32_t>();
                if (rank > 8) throw std::runtime_error("Invalid tensor rank");
                std::vector<int64_t> shape(rank);
                for (auto& dim : shape) dim = in.get<int64_t>();
                uint64_t bytes = in.get<uint64_t>();
                in.check(bytes);
                tensors.push_back(Ort::Value::CreateTensor(cpu, memory + in.offset, bytes, shape.data(), shape.size(), types[i]));
                in.offset += bytes;
            }
            auto results = session.Run(Ort::RunOptions{nullptr}, inputs.data(), tensors.data(), tensors.size(), outputs.data(), outputs.size());
            Buffer out{scratch.get()};
            out.put<uint32_t>(results.size());
            for (auto& result : results) {
                auto info = result.GetTensorTypeAndShapeInfo();
                auto shape = info.GetShape();
                out.put<uint32_t>(shape.size());
                for (int64_t dim : shape) out.put<int64_t>(dim);
                size_t bytes = info.GetElementCount() * element_size(info.GetElementType());
                out.put<uint64_t>(bytes);
                out.put(result.GetTensorRawData(), bytes);
            }
            std::memcpy(memory, scratch.get(), out.offset);
            std::cout << "MHCUDA_DONE " << out.offset << std::endl;
        }
        if (profile) {
            auto path = session.EndProfilingAllocated(allocator);
            std::cerr << "CUDA profile: " << path.get() << std::endl;
        }
        munmap(memory, Capacity);
    } catch (const std::exception& error) {
        std::cout << "MHCUDA_ERROR " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
