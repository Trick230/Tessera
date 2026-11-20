// SPDX-License-Identifier: GPL-3.0-or-later
// Smoke test for the d3d11 layer.
//   d3d11_smoke            render through both creation paths, then exit cleanly
//   d3d11_smoke --crash    create a device and die without DLL detach
//   d3d11_smoke --verify <sessions dir>   check the newest session file
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "tessera/session_format.h"

namespace {
int fail(const char* what, HRESULT hr = S_OK) {
    std::printf("FAIL: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    return 1;
}

HWND make_window() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"tessera_smoke";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"tessera smoke", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr,
                           nullptr, wc.hInstance, nullptr);
}

void render(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc, int frames, bool present1) {
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    IDXGISwapChain1* sc1 = nullptr;
    if (present1) sc->QueryInterface(IID_PPV_ARGS(&sc1));
    for (int i = 0; i < frames; ++i) {
        const float c[4] = {i / float(frames), 0.2f, 0.3f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, c);
        if (sc1) {
            DXGI_PRESENT_PARAMETERS pp = {};
            sc1->Present1(0, 0, &pp);
        } else {
            sc->Present(0, 0);
        }
    }
    if (sc1) sc1->Release();
    rtv->Release();
    bb->Release();
}

int run(bool crash) {
    wchar_t mod[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(L"d3d11.dll"), mod, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    std::printf("d3d11.dll = %ls\n", mod);
    if (_wcsnicmp(mod, exe, wcslen(exe)) != 0) return fail("layer not loaded from the exe folder");

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    if (crash) TerminateProcess(GetCurrentProcess(), 3);

    HWND wnd = make_window();
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    dxgi->GetAdapter(&adapter);
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("IDXGIFactory2", hr);

    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    d1.Width = 640;
    d1.Height = 360;
    d1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d1.SampleDesc.Count = 1;
    d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d1.BufferCount = 2;
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(dev, wnd, &d1, nullptr, nullptr, &sc1);
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);
    render(dev, ctx, sc1, 60, false);
    render(dev, ctx, sc1, 60, true);

    // 10 BC7 1024^2 textures with full mip chains, 5 buffers, one SRV.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1024;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC7_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<ID3D11Texture2D*> texs;
    for (int i = 0; i < 10; ++i) {
        ID3D11Texture2D* t = nullptr;
        hr = dev->CreateTexture2D(&td, nullptr, &t);
        if (FAILED(hr)) return fail("CreateTexture2D", hr);
        texs.push_back(t);
    }
    ID3D11ShaderResourceView* srv = nullptr;
    hr = dev->CreateShaderResourceView(texs[0], nullptr, &srv);
    if (FAILED(hr)) return fail("CreateShaderResourceView", hr);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 64 * 1024;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    for (int i = 0; i < 5; ++i) {
        ID3D11Buffer* b = nullptr;
        hr = dev->CreateBuffer(&bd, nullptr, &b);
        if (FAILED(hr)) return fail("CreateBuffer", hr);
        b->Release();
    }

    // A plain forwarded export (asm stub -> resolve thunk -> system d3d11).
    using PFN_FromDxgi = HRESULT(WINAPI*)(IDXGIDevice*, IUnknown**);
    auto from_dxgi =
        reinterpret_cast<PFN_FromDxgi>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "CreateDirect3D11DeviceFromDXGIDevice"));
    IUnknown* winrt_dev = nullptr;
    hr = from_dxgi ? from_dxgi(dxgi, &winrt_dev) : E_POINTER;
    if (FAILED(hr)) return fail("CreateDirect3D11DeviceFromDXGIDevice", hr);
    winrt_dev->Release();

    srv->Release();
    for (auto* t : texs) t->Release();
    sc1->Release();
    factory->Release();
    adapter->Release();
    dxgi->Release();
    ctx->Release();
    dev->Release();

    // Second path: device and legacy swap chain in one call.
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("D3D11CreateDeviceAndSwapChain", hr);
    render(dev, ctx, sc, 60, false);
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(wnd);
    std::printf("OK: rendered 180 frames\n");
    return 0;
}

int verify(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH], path[MAX_PATH] = {};
    swprintf_s(pattern, L"%s\\*.tsess", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return fail("no session file");
    FILETIME newest = {};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
            newest = fd.ftLastWriteTime;
            swprintf_s(path, L"%s\\%s", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return fail("open session");
    TesseraSessionHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || memcmp(hdr.magic, TESSERA_SESSION_MAGIC, 8) != 0 ||
        hdr.version != TESSERA_SESSION_VERSION || hdr.record_size != sizeof(TesseraRecord))
        return fail("bad header");
    int frames = 0, tex = 0, bc7_full = 0, buffers = 0, srvs = 0, devices = 0, swapchains = 0, end = 0;
    uint32_t dropped = 0, max_frame_us = 0;
    TesseraRecord r;
    while (fread(&r, sizeof r, 1, f) == 1) {
        switch (r.kind) {
            case TESSERA_REC_FRAME: ++frames; if (r.a > max_frame_us) max_frame_us = r.a; break;
            case TESSERA_REC_TEXTURE2D: ++tex; if (r.bytes == 1398128) ++bc7_full; break;
            case TESSERA_REC_BUFFER: ++buffers; break;
            case TESSERA_REC_SRV: ++srvs; break;
            case TESSERA_REC_DEVICE: ++devices; break;
            case TESSERA_REC_SWAPCHAIN: ++swapchains; break;
            case TESSERA_REC_END: ++end; dropped = r.a; break;
        }
    }
    fclose(f);
    std::printf("%ls\nframes %d (max %u us) tex2d %d (bc7 1024 full-mip %d) buffers %d srv %d devices %d "
                "swapchains %d end %d dropped %u\n",
                path, frames, max_frame_us, tex, bc7_full, buffers, srvs, devices, swapchains, end, dropped);
    const bool ok = frames >= 180 && bc7_full == 10 && buffers >= 5 && srvs >= 1 && devices == 2 &&
                    swapchains >= 1 && end == 1 && dropped == 0;
    if (!ok) return fail("unexpected counts");
    std::printf("OK: session verified\n");
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && wcscmp(argv[1], L"--verify") == 0) return verify(argv[2]);
    return run(argc >= 2 && wcscmp(argv[1], L"--crash") == 0);
}
