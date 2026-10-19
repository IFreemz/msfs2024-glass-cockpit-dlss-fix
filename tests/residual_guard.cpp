// GPU regression of the production composite residual block with controlled input samples.
// GPL-3.0-only with the additional terms in NOTICE.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 operation failed"); }
int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) return 2;
    std::ifstream file(std::filesystem::path(argv[1]), std::ios::binary);
    const std::vector<char> shader((std::istreambuf_iterator<char>(file)), {});
    if (shader.empty()) throw std::runtime_error("Missing test shader");
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    ComPtr<ID3D12RootSignature> signature; check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = signature.Get(); pipeline.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pso; check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)));
    auto buffer = [&](D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = heap_type;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 64;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    };
    auto output = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    list->SetComputeRootSignature(signature.Get()); list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier); list->CopyResource(readback.Get(), output.Get()); check(list->Close());
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent failed");
    const HRESULT scheduled = fence->SetEventOnCompletion(1, event);
    const DWORD waited = SUCCEEDED(scheduled) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) { std::fputs("GPU completion failed\n", stderr); ExitProcess(3); }
    void* mapped = nullptr; check(readback->Map(0, nullptr, &mapped));
    std::array<std::array<float, 4>, 4> values{}; memcpy(values.data(), mapped, sizeof(values)); readback->Unmap(0, nullptr);
    const char* names[]{"empty", "outline-only", "regular", "mixed"};
    bool passed = true;
    for (unsigned i = 0; i < values.size(); ++i) {
        const std::array<float, 4> expected = i == 0 ? std::array<float, 4>{0.8f, 0.1f, 0.8f, 1} : std::array<float, 4>{0.25f, 0.30f, 0.35f, 1};
        bool ok = true;
        for (unsigned c = 0; c < 4; ++c) ok &= std::isfinite(values[i][c]) && std::abs(values[i][c] - expected[c]) < 0.0001f;
        std::printf("residual %s: %.6f %.6f %.6f %s\n", names[i], values[i][0], values[i][1], values[i][2], ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    return passed ? 0 : 1;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
