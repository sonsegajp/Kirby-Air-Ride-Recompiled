// The host's graphics memory, for sizing what the renderer may keep resident (the HD texture
// cache): the largest dedicated video memory of the system's adapters.
#include "runtime/runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>

#pragma comment(lib, "dxgi.lib")

namespace rt {

u64 hostGpuMemory() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return 0;
    u64 best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best)
            best = d.DedicatedVideoMemory;
        a->Release();
    }
    f->Release();
    return best;
}

}  // namespace rt
