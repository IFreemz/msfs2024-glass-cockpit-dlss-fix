// GPL-3.0-only with the additional terms in NOTICE.md.
// Offline startup-only reproduction. Does not access an MSFS installation.
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <array>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, const char* label) {
    printf("%s: %08lx\n", label, hr);
    if (FAILED(hr)) throw std::runtime_error(label);
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    try {
        HMODULE dll = LoadLibraryExW(std::filesystem::absolute(argv[1]).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        printf("loaded=%p error=%lu mode=%s\n", dll, GetLastError(), argv[2]);
        if (!dll) return 3;
        auto create = reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dll,"CreateDXGIFactory1"));
        if (!create) return 4;
        ComPtr<IDXGIFactory4> factory;
        check(create(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        check(factory->EnumAdapters1(0, &adapter), "adapter");
        ComPtr<ID3D12Device> device;
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC q{};
        ComPtr<ID3D12CommandQueue> queue;
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"GlassStartupProbe";
        RegisterClassW(&wc);
        HWND window=CreateWindowW(wc.lpszClassName,L"Offline startup probe",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=128; desc.Height=128; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&chain),"swapchain");
        auto object=reinterpret_cast<void***>(chain.Get());
        void** original=*object;
        // Model an overlay making a private copy of the swapchain's method table.
        // All function entries and the object itself stay unchanged.
        std::array<void*,23> snapshot{};
        memcpy(snapshot.data(),original,sizeof(snapshot));
        if (!strncmp(argv[2],"clone",5)) *object=snapshot.data();
        puts("before Present(TEST), no cockpit or upscaler work");
        DXGI_PRESENT_PARAMETERS parameters{};
        HRESULT result=strstr(argv[2],"present1") ? chain->Present1(0,DXGI_PRESENT_TEST,&parameters) : chain->Present(0,DXGI_PRESENT_TEST);
        *object=original;
        check(result,"Present(TEST)");
        puts("STARTUP PROBE PASSED");
        // No GPU work was submitted; let process exit clean up pinned hook modules.
        ExitProcess(0);
    } catch(const std::exception& ex) { printf("FAILED: %s\n",ex.what()); return 1; }
}
