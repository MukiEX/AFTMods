// D3D11CreateDeviceAndSwapChain and D3DCompile, loaded at first use from the system directory.
//
// Importing d3d11.dll / d3dcompiler_47.dll normally makes Windows resolve them when PD Loader
// loads this plugin, searching the game folder first. A copy of either in the game folder that
// is 32-bit (or otherwise unsuitable) then stops the whole plugin from loading with
// STATUS_INVALID_IMAGE_FORMAT (0xc000007b). Loading them by full System32 path avoids that,
// and lets the launcher show a readable error instead. Under Proton/Wine, System32 is where
// DXVK's d3d11.dll lives, so this picks it up as well.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static HMODULE load_system_dll(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 1 + wcslen(name) >= MAX_PATH)
        return nullptr;
    path[n] = L'\\';
    wcscpy_s(path + n + 1, MAX_PATH - n - 1, name);
    return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* got_level, ID3D11DeviceContext** context) {
    typedef HRESULT(WINAPI* PFN)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
        const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    static PFN fn = []() -> PFN {
        HMODULE h = load_system_dll(L"d3d11.dll");
        return h ? (PFN)GetProcAddress(h, "D3D11CreateDeviceAndSwapChain") : nullptr;
    }();
    if (!fn)
        return E_NOINTERFACE;
    return fn(adapter, driver_type, software, flags, levels, level_count, sdk_version, desc, swapchain, device, got_level, context);
}

extern "C" HRESULT WINAPI D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
    ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    typedef HRESULT(WINAPI* PFN)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
        UINT, UINT, ID3DBlob**, ID3DBlob**);
    static PFN fn = []() -> PFN {
        const wchar_t* names[] = { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" };
        for (const wchar_t* n : names)
            if (HMODULE h = load_system_dll(n))
                if (PFN f = (PFN)GetProcAddress(h, "D3DCompile"))
                    return f;
        return nullptr;
    }();
    if (!fn)
        return E_NOINTERFACE;
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}
