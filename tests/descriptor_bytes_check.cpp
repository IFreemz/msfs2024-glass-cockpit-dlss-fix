// Checks that an SRV descriptor's bytes are identical in a CPU-only heap and after CopyDescriptorsSimple
// into a shader-visible heap (the overlay identifies display textures by comparing these bytes).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory6> factory; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter; factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return 2;
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> cpu, gpu; device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpu));
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu));
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 768, 768, 1, 5, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> a, b;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&a));
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b));
    auto c0 = cpu->GetCPUDescriptorHandleForHeapStart(), g0 = gpu->GetCPUDescriptorHandleForHeapStart();
    auto at = [inc](D3D12_CPU_DESCRIPTOR_HANDLE h, UINT i) { h.ptr += SIZE_T(i) * inc; return h; };
    device->CreateShaderResourceView(a.Get(), nullptr, at(c0, 0));
    device->CreateShaderResourceView(b.Get(), nullptr, at(c0, 1));
    device->CreateShaderResourceView(a.Get(), nullptr, at(c0, 2)); // second view of the same resource
    device->CopyDescriptorsSimple(1, at(g0, 5), at(c0, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CopyDescriptorsSimple(1, at(g0, 6), at(c0, 1), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(a.Get(), nullptr, at(g0, 7)); // created directly in the shader-visible heap
    auto bytes = [](D3D12_CPU_DESCRIPTOR_HANDLE h) { return reinterpret_cast<const unsigned char*>(h.ptr); };
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    printf("adapter=%ls increment=%u\n", ad.Description, inc);
    const bool copy_a = !memcmp(bytes(at(c0, 0)), bytes(at(g0, 5)), inc);
    const bool copy_b = !memcmp(bytes(at(c0, 1)), bytes(at(g0, 6)), inc);
    const bool distinct = memcmp(bytes(at(c0, 0)), bytes(at(c0, 1)), inc) != 0;
    const bool same_view = !memcmp(bytes(at(c0, 0)), bytes(at(c0, 2)), inc);
    const bool direct = !memcmp(bytes(at(c0, 0)), bytes(at(g0, 7)), inc);
    printf("copy_a=%d copy_b=%d distinct=%d same_resource_new_view=%d direct_in_gpu_heap=%d\n", copy_a, copy_b, distinct, same_view, direct);
    return copy_a && copy_b && distinct ? 0 : 1;
}
