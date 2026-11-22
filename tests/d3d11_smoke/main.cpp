// SPDX-License-Identifier: GPL-3.0-or-later
// Smoke test for the d3d11 layer.
//   d3d11_smoke            render through both creation paths, then exit cleanly
//   d3d11_smoke --crash    create a device and die without DLL detach
//   d3d11_smoke --tiled    mip residency (M3-M5), with mip_residency=1 and mip_force_evict=1
//   d3d11_smoke --pacing [driver]   measure recovery after a late frame or verify driver handoff
//   d3d11_smoke --verify <sessions dir> [fps]   check the newest session file;
//       with fps, also check that the frame limiter held that rate
#include <d3d11_3.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "tessera/session_format.h"
#include "tessera/shader_store_format.h"

namespace {
int fail(const char* what, HRESULT hr = S_OK) {
    std::printf("FAIL: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    return 1;
}

// S6: a VS (created twice: the second is already known) and a PS.
int make_shaders(ID3D11Device* dev) {
    static const char kSrc[] =
        "float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id & 1, id >> 1, 0, 1); }\n"
        "float4 ps() : SV_Target { return float4(1, 0.5, 0, 1); }\n";
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    HRESULT hr = D3DCompile(kSrc, sizeof kSrc - 1, "smoke", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, nullptr);
    if (SUCCEEDED(hr)) hr = D3DCompile(kSrc, sizeof kSrc - 1, "smoke", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, nullptr);
    if (FAILED(hr)) return fail("D3DCompile", hr);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        ID3D11VertexShader* v = nullptr;
        hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &v);
        if (v) v->Release();
    }
    ID3D11PixelShader* p = nullptr;
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p);
    if (p) p->Release();
    vs->Release();
    ps->Release();
    return FAILED(hr) ? fail("create shaders", hr) : 0;
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

HMODULE module_of(void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(p), &m);
    return m;
}

// What a module's import slot for `name` holds now (&func gives a thunk).
void* import_slot(HMODULE m, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                std::strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, name) == 0)
                return reinterpret_cast<void*>(iat->u1.Function);
    }
    return nullptr;
}

// NVAPI (NVIDIA only): resolved through our import, the memory it reports
// must be capped as well. 0 ok or no NVAPI; else the failure's exit code.
int nvapi_capped(unsigned long long cap, const char* when) {
    HMODULE nv = LoadLibraryW(L"nvapi64.dll");
    if (!nv) return 0;
    using Qi = void*(__cdecl*)(unsigned);
    const auto qi = reinterpret_cast<Qi>(GetProcAddress(nv, "nvapi_QueryInterface"));
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(qi), &owner);
    if (!qi || owner == nv) return fail("nvapi_QueryInterface not wrapped");
    using Init = int(__cdecl*)();
    using Enum = int(__cdecl*)(void**, unsigned*);
    using Mem = int(__cdecl*)(void*, unsigned*);
    const auto init = reinterpret_cast<Init>(qi(0x0150E828));
    const auto enum_gpus = reinterpret_cast<Enum>(qi(0xE5AC921F));
    const auto mem = reinterpret_cast<Mem>(qi(0x07F9B368));
    void* gpus[64] = {};
    unsigned n = 0;
    unsigned info[8] = {32 | (3 << 16)};
    if (init && enum_gpus && mem && init() == 0 && enum_gpus(gpus, &n) == 0 && n > 0 &&
        mem(gpus[0], info) == 0) {
        std::printf("NVAPI dedicated memory seen %s: %u MB\n", when, info[1] >> 10);
        if ((static_cast<unsigned long long>(info[1]) << 10) > cap) return fail("NVAPI memory cap not applied");
        // GetMemoryInfoEx (bytes): capped too; system and shared memory,
        // never capped, match the older call (the layout is right).
        using MemEx = int(__cdecl*)(void*, unsigned long long*);
        const auto mem_ex = reinterpret_cast<MemEx>(qi(0xC0599498));
        unsigned long long ex[10] = {80 | (1 << 16)};
        if (mem_ex && mem_ex(gpus[0], ex) == 0) {
            std::printf("NVAPI Ex dedicated memory seen %s: %llu MB (shared %llu MB)\n", when, ex[1] >> 20,
                        ex[4] >> 20);
            if (ex[1] > cap || ex[2] > cap) return fail("NVAPI Ex memory cap not applied");
            if ((ex[3] >> 10) != info[3] || (ex[4] >> 10) != info[4])
                return fail("NVAPI Ex layout: system/shared memory differ from GetMemoryInfo");
        } else {
            std::printf("NVAPI Ex memory query not available, skipped\n");
        }
    } else {
        std::printf("NVAPI memory query not available, skipped\n");
    }
    FreeLibrary(nv);
    return 0;
}

int run(bool crash) {
    wchar_t mod[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(L"d3d11.dll"), mod, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    std::printf("d3d11.dll = %ls\n", mod);
    if (_wcsnicmp(mod, exe, wcslen(exe)) != 0) return fail("layer not loaded from the exe folder");

    // M1 before any device (as Unreal sizes its texture pool): the adapter
    // from the exe's own CreateDXGIFactory1 import must already be capped.
    if (GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", nullptr, 0)) {
        wchar_t cap_s[16] = {};
        GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", cap_s, 16);
        const unsigned long long cap = static_cast<unsigned long long>(_wtoi(cap_s)) << 20;
        IDXGIFactory1* f = nullptr;
        IDXGIAdapter1* a = nullptr;
        DXGI_ADAPTER_DESC1 d = {};
        HRESULT er = CreateDXGIFactory1(IID_PPV_ARGS(&f));
        if (FAILED(er)) return fail("CreateDXGIFactory1", er);
        er = f->EnumAdapters1(0, &a);
        if (SUCCEEDED(er)) er = a->GetDesc1(&d);
        if (a) a->Release();
        f->Release();
        std::printf("dedicated memory seen before the device: %zu MB\n", d.DedicatedVideoMemory >> 20);
        if (FAILED(er) || d.DedicatedVideoMemory > cap) return fail("dedicated memory cap not applied before the device", er);
        // Decisive on any GPU (a small one is below the cap anyway): the
        // factory the exe calls is the layer's.
        if (module_of(import_slot(GetModuleHandleW(nullptr), "CreateDXGIFactory1")) != GetModuleHandleW(L"d3d11.dll"))
            return fail("the exe's CreateDXGIFactory1 import is not the layer's");
        if (const int r = nvapi_capped(cap, "before the device")) return r;
    }

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
    // M1: with TESSERA_SMOKE_CAP_MB set, the budget the app sees must be capped.
    if (GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", nullptr, 0)) {
        wchar_t cap_s[16] = {};
        GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", cap_s, 16);
        IDXGIAdapter3* a3 = nullptr;
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&a3)))) return fail("IDXGIAdapter3");
        DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
        hr = a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
        a3->Release();
        const unsigned long long cap = static_cast<unsigned long long>(_wtoi(cap_s)) << 20;
        std::printf("budget seen by the app: %llu MB\n", mi.Budget >> 20);
        if (FAILED(hr) || mi.Budget > cap) return fail("budget cap not applied", hr);
        // Adapter descriptions too, through every version of GetDesc.
        DXGI_ADAPTER_DESC d0 = {};
        DXGI_ADAPTER_DESC1 dd1 = {};
        DXGI_ADAPTER_DESC2 dd2 = {};
        IDXGIAdapter2* a2 = nullptr;
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&a2)))) return fail("IDXGIAdapter2");
        const bool got = SUCCEEDED(adapter->GetDesc(&d0)) && SUCCEEDED(a2->GetDesc1(&dd1)) && SUCCEEDED(a2->GetDesc2(&dd2));
        a2->Release();
        std::printf("dedicated memory seen by the app: %zu / %zu / %zu MB\n", d0.DedicatedVideoMemory >> 20,
                    dd1.DedicatedVideoMemory >> 20, dd2.DedicatedVideoMemory >> 20);
        if (!got || d0.DedicatedVideoMemory > cap || dd1.DedicatedVideoMemory > cap || dd2.DedicatedVideoMemory > cap)
            return fail("dedicated memory cap not applied");
        if (const int r = nvapi_capped(cap, "by the app")) return r;
    }

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

    // Composition swap chain (no window, as used by UWP/XAML/DirectComposition hosts).
    DXGI_SWAP_CHAIN_DESC1 dc = d1;
    dc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    dc.Scaling = DXGI_SCALING_STRETCH;
    dc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1* sc_comp = nullptr;
    hr = factory->CreateSwapChainForComposition(dev, &dc, nullptr, &sc_comp);
    if (FAILED(hr)) return fail("CreateSwapChainForComposition", hr);
    render(dev, ctx, sc_comp, 30, false);
    sc_comp->Release();

    // 10 BC7 1024^2 textures with full mip chains, 5 buffers, one SRV,
    // one render target (gets raised eviction priority, M2).
    {
        D3D11_TEXTURE2D_DESC rd = {};
        rd.Width = rd.Height = 256;
        rd.MipLevels = rd.ArraySize = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* rt = nullptr;
        hr = dev->CreateTexture2D(&rd, nullptr, &rt);
        if (FAILED(hr)) return fail("CreateTexture2D rt", hr);
        rt->Release();
    }
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
    // M11: bind the first texture on two stages for 1.5 s, so a working-set
    // tick sees exactly one of the ten as used.
    for (int i = 0; i < 150; ++i) {
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->CSSetShaderResources(3, 1, &srv);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        sc1->Present(0, 0);
        Sleep(10);
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 64 * 1024;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    for (int i = 0; i < 5; ++i) {
        ID3D11Buffer* b = nullptr;
        hr = dev->CreateBuffer(&bd, nullptr, &b);
        if (FAILED(hr)) return fail("CreateBuffer", hr);
        b->Release();
    }
    if (make_shaders(dev)) return 1;

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
    // A resize must keep working when the layer made this swapchain flip model (P2).
    hr = sc->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return fail("ResizeBuffers", hr);
    render(dev, ctx, sc, 10, false);
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(wnd);
    std::printf("OK: rendered 210 frames\n");
    return 0;
}

int verify(const wchar_t* dir, int fps) {
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
    int frames = 0, tex = 0, bc7_full = 0, buffers = 0, srvs = 0, devices = 0, swapchains = 0, sc_comp = 0,
        end = 0;
    uint32_t dropped = 0, max_frame_us = 0;
    int prio_buffers = 0, prio_tex = 0, foreign = 0, vram = 0, flipped = 0, shaders = 0, shaders_new = 0;
    uint32_t budget_mb = 0, usage_mb = 0, cap_mb = 0;
    int workset = 0;
    uint32_t used_kb = 0, alive = 0, untracked = 0, tiled = 0;
    std::vector<uint32_t> paced;  // frame times of frames the limiter waited for
    TesseraRecord r;
    while (fread(&r, sizeof r, 1, f) == 1) {
        switch (r.kind) {
            case TESSERA_REC_FRAME:
                ++frames;
                if (r.a > max_frame_us) max_frame_us = r.a;
                if (r.b && r.a) paced.push_back(r.a);
                break;
            case TESSERA_REC_TEXTURE2D:
                ++tex;
                if (r.bytes == 1398128) ++bc7_full;
                if (r.flags & TESSERA_FLAG_PRIORITY) ++prio_tex;
                if (r.flags & TESSERA_FLAG_FOREIGN) ++foreign;
                break;
            case TESSERA_REC_BUFFER:
                ++buffers;
                if (r.flags & TESSERA_FLAG_PRIORITY) ++prio_buffers;
                if (r.flags & TESSERA_FLAG_FOREIGN) ++foreign;
                break;
            case TESSERA_REC_SRV: ++srvs; break;
            case TESSERA_REC_DEVICE: ++devices; tiled = r.b; break;
            case TESSERA_REC_WORKSET:
                ++workset;
                if (r.a > used_kb) used_kb = r.a;
                if (r.c > alive) alive = r.c;
                untracked = r.d;
                break;
            case TESSERA_REC_SWAPCHAIN:
                ++swapchains;
                if (r.d & 0x100) ++flipped;
                if (r.d == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ++sc_comp;
                break;
            case TESSERA_REC_SHADER:
                if (!(r.d & TESSERA_SHADER_DXBC) || r.c == 0 || r.bytes == 0) return fail("shader record fields");
                // Overlays or the runtime may create their own in this process: only the exe's count.
                if (r.flags & TESSERA_FLAG_FOREIGN) break;
                ++shaders;
                if (r.d & TESSERA_SHADER_NEW) ++shaders_new;
                break;
            case TESSERA_REC_VRAM: ++vram; budget_mb = r.a; usage_mb = r.b; cap_mb = r.d; break;
            case TESSERA_REC_END: ++end; dropped = r.a; break;
        }
    }
    fclose(f);
    std::printf("%ls\nframes %d (max %u us) tex2d %d (bc7 1024 full-mip %d) buffers %d srv %d devices %d "
                "swapchains %d (composition %d) end %d dropped %u\n",
                path, frames, max_frame_us, tex, bc7_full, buffers, srvs, devices, swapchains, sc_comp, end, dropped);
    const bool ok = frames >= 210 && bc7_full == 10 && buffers >= 5 && srvs >= 1 && devices == 2 &&
                    swapchains >= 2 && sc_comp == 1 && end == 1 && dropped == 0;
    if (!ok) return fail("unexpected counts");
    // M2: the 5 DEFAULT buffers and the render target, not the sampled BC7
    // textures. C3: everything here is created by the exe, nothing foreign.
    std::printf("priority buffers %d tex %d, foreign %d, paced frames %zu\n", prio_buffers, prio_tex, foreign,
                paced.size());
    if (prio_buffers < 5 || prio_tex != 1 || foreign != 0) return fail("residency / module flags");
    std::printf("vram samples %d, budget %u MB, usage %u MB, cap %u MB\n", vram, budget_mb, usage_mb, cap_mb);
    if (vram < 1 || budget_mb == 0) return fail("no VRAM budget samples");
    if (fps > 0 && cap_mb != 2048) return fail("budget cap not in the log");
    // P2: the limited run also sets flip_model=1; only the DISCARD swapchain qualifies.
    std::printf("swapchains converted to flip model %d\n", flipped);
    if (flipped != (fps > 0 ? 1 : 0)) return fail("flip model conversion");
    // S6: the first run stores the VS and the PS; the limited run finds both in the store.
    std::printf("shaders %d, new %d\n", shaders, shaders_new);
    if (shaders != 3 || shaders_new != (fps > 0 ? 0 : 2)) return fail("shader records");
    if (fps > 0) {
        if (paced.size() < 100) return fail("limiter did not wait");
        std::sort(paced.begin(), paced.end());
        const double median = paced[paced.size() / 2], target = 1e6 / fps;
        std::printf("limiter: median %.0f us, target %.0f us\n", median, target);
        if (median < target * 0.95 || median > target * 1.05) return fail("limiter off target");
    } else if (!paced.empty()) {
        return fail("limiter waited with no fps_limit");
    }
    // M11: 1398128 bytes (one BC7 1024^2 full chain) = 1365 KB bound; the
    // render target and the swapchain buffers are not sampled textures.
    std::printf("workset samples %d, max used %u KB, max alive %u, untracked %u, tiled tier %u\n", workset, used_kb,
                alive, untracked, tiled);
    if (workset < 1 || used_kb != 1365 || alive != 10 || untracked != 0) return fail("working set");
    std::printf("OK: session verified\n");
    return 0;
}
int pacing_driver(ID3D11Device* dev, IDXGISwapChain* sc, int64_t freq) {
    using QueryInterface = void*(__cdecl*)(unsigned);
    using SleepMode = int(__cdecl*)(IUnknown*, const void*);
    const HMODULE probe = LoadLibraryW(L"load_probe.dll");
    const auto qi = probe ? reinterpret_cast<QueryInterface>(GetProcAddress(probe, "nvapi_QueryInterface")) : nullptr;
    const auto set_mode = qi ? reinterpret_cast<SleepMode>(qi(0xAC1CA9E0)) : nullptr;
    if (!set_mode) return fail("could not resolve the probe's sleep mode");
    if (!LoadLibraryW(L"sl.reflex.dll")) return fail("load the idle Reflex SDK fixture");
    struct {
        uint32_t version = 0x1002C;
        bool low_latency = false, boost = false;
        uint32_t interval = 0;
        bool reject = false;
        uint8_t padding[31] = {};
    } params;
    static_assert(sizeof params == 44);
    const auto measure = [&](bool expect_cap) {
        sc->Present(0, 0);
        LARGE_INTEGER start, stop;
        QueryPerformanceCounter(&start);
        for (int i = 0; i < 10; ++i) sc->Present(0, 0);
        QueryPerformanceCounter(&stop);
        const double average_ms = (stop.QuadPart - start.QuadPart) * 100.0 / freq;
        std::printf("NVAPI probe: %s, %.2f ms/frame\n", expect_cap ? "Tessera cap" : "delegated", average_ms);
        return expect_cap ? average_ms >= 15.0 : average_ms < 10.0;
    };

    Sleep(2100);  // Let the background poll see the loaded, inactive SDK.
    if (!measure(true)) return fail("idle Reflex SDK must leave Tessera's cap active");
    ID3D11Device* other = nullptr;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &other, nullptr, nullptr);
    if (FAILED(hr)) return fail("create secondary pacing device", hr);
    params.low_latency = true;
    const int secondary_status = set_mode(other, &params);
    other->Release();
    if (secondary_status != 0 || !measure(true)) return fail("NVAPI mode on a secondary device must leave the cap active");
    if (set_mode(dev, &params) != 0 || !measure(false)) return fail("handoff with Reflex enabled");
    params.low_latency = false;
    params.reject = true;
    if (set_mode(dev, &params) == 0 || !measure(false)) return fail("failed NVAPI change must leave the cap delegated");
    params.reject = false;
    if (set_mode(dev, &params) != 0 || !measure(true)) return fail("cap resumes after Reflex is disabled");
    params.interval = 20000;
    if (set_mode(dev, &params) != 0 || !measure(false)) return fail("handoff to an explicit NVAPI cap");
    params.interval = 0;
    if (set_mode(dev, &params) != 0 || !measure(true)) return fail("cap resumes after NVAPI cap is disabled");
    return 0;
}

int pacing_mode(bool driver) {
    const HWND wnd = make_window();
    if (!wnd) return fail("make pacing window");
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc = {64, 64, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = wnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &desc, &sc, &dev, nullptr, nullptr);
    if (FAILED(hr)) return fail("create pacing swap chain", hr);

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    if (driver) {
        const int result = pacing_driver(dev, sc, freq.QuadPart);
        sc->Release();
        dev->Release();
        DestroyWindow(wnd);
        return result;
    }
    // At a 60 FPS cap, inject an arrival about 3 ms overdue every tenth frame.
    // Sample CPU-side Present returns; this does not measure display latency.
    constexpr int frames = 160;
    constexpr int delay_every = 10;
    std::vector<double> gaps, recovery;
    gaps.reserve(frames);
    recovery.reserve(frames / delay_every);
    if (FAILED(sc->Present(0, 0))) return fail("initial pacing present");
    QueryPerformanceCounter(&last);
    for (int i = 1; i <= frames; ++i) {
        if (i % delay_every == 1) {
            const int64_t end = last.QuadPart + freq.QuadPart / 60 + freq.QuadPart * 3 / 1000;
            do { QueryPerformanceCounter(&now); } while (now.QuadPart < end);
        }
        const HRESULT presented = sc->Present(0, 0);
        if (FAILED(presented)) return fail("pacing present", presented);
        QueryPerformanceCounter(&now);
        const double gap = 1000.0 * (now.QuadPart - last.QuadPart) / freq.QuadPart;
        gaps.push_back(gap);
        if (i % delay_every == 2) recovery.push_back(gap);
        last = now;
    }
    std::sort(gaps.begin(), gaps.end());
    std::sort(recovery.begin(), recovery.end());
    const auto short_recovery = std::count_if(recovery.begin(), recovery.end(), [](double gap) { return gap < 15.5; });
    std::printf("pacing: median=%.3f ms, recovery median=%.3f ms, recovery <15.5 ms=%td/%zu\n",
                gaps[gaps.size() / 2], recovery[recovery.size() / 2], short_recovery, recovery.size());
    sc->Release();
    dev->Release();
    DestroyWindow(wnd);
    return 0;
}

int d3d11_calls_mode(bool bench) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("create diagnostics device", hr);
    D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query* query = nullptr;
    if (FAILED(dev->CreateQuery(&query_desc, &query))) return fail("create event query");

    if (!bench) {
        D3D11_BUFFER_DESC desc = {16, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
        ID3D11Buffer* buffer = nullptr;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, &buffer))) return fail("create default buffer");
        const UINT data[4] = {8, 6, 4, 2};
        ctx->UpdateSubresource(buffer, 0, nullptr, data, 0, 0);
        ID3D11DeviceContext1* c1 = nullptr;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c1)))) return fail("get context1");
        c1->UpdateSubresource1(buffer, 0, nullptr, data, 0, 0, 0);
        c1->Release();
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, &staging))) return fail("create readback buffer");
        ctx->CopyResource(staging, buffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return fail("read back updated buffer");
        const bool data_ok = std::memcmp(mapped.pData, data, sizeof data) == 0;
        ctx->Unmap(staging, 0);
        staging->Release();
        buffer->Release();
        if (!data_ok) return fail("UpdateSubresource and Map data changed");
    }

    ctx->End(query);
    ctx->Flush();
    if (!bench) {
        ID3D11DeviceContext3* c3 = nullptr;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c3)))) return fail("get context3");
        c3->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        c3->Release();
    }
    const HRESULT ready = ctx->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (FAILED(ready)) return fail("GetData event poll", ready);
    if (bench) {
        LARGE_INTEGER freq, start, stop;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        for (unsigned i = 0; i < 2000000; ++i)
            if (FAILED(ctx->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH))) return fail("GetData probe");
        QueryPerformanceCounter(&stop);
        std::printf("GetData %.1f ns/call\n", (stop.QuadPart - start.QuadPart) * 500.0 / freq.QuadPart);
    }
    query->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
}  // namespace

int stream() {
    ID3D11Device* probe = nullptr;
    constexpr UINT probe_flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, probe_flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &probe, nullptr, nullptr)))
        return fail("stream probe device");
    const HWND wnd = make_window();
    if (!wnd) return fail("stream window");
    DXGI_SWAP_CHAIN_DESC swap = {};
    swap.BufferDesc = {64, 64, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    swap.SampleDesc.Count = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 2;
    swap.OutputWindow = wnd;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &swap, &sc, &dev, nullptr, &ctx)))
        return fail("stream device and swap chain");
    wchar_t mode[2] = {};
    GetEnvironmentVariableW(L"TESSERA_SMOKE_PREDICT", mode, _countof(mode));
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"TESSERA_SMOKE_ASSET", path, MAX_PATH)) return fail("stream input path");
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail("stream open asset");
    std::vector<BYTE> pixels(65536);
    const auto read_asset = [&](unsigned block, DWORD bytes) {
        LARGE_INTEGER offset = {};
        offset.QuadPart = 65536ull * block;
        DWORD count = 0;
        return SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
               ReadFile(file, pixels.data(), bytes, &count, nullptr) && count == bytes && pixels[0] == 0x53;
    };
    const bool learning = mode[0] == L'1';
    if (learning) {
        // Fill the old trace before the first material bind without a large fixture.
        for (unsigned i = 0; i < 4096; ++i)
            if (!read_asset(1 + i % 32, i < 32 ? 65536 : 4096)) return fail("stream startup reads");
    }
    if (mode[0]) {
        if (FAILED(sc->Present(0, 0))) return fail("stream present");
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = 64;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint32_t> texels(64 * 64, 0x76543210);
        ID3D11ShaderResourceView* views[2] = {};
        for (auto& view : views) {
            ID3D11Texture2D* tex = nullptr;
            const D3D11_SUBRESOURCE_DATA init = {texels.data(), 64 * 4, 0};
            if (FAILED(dev->CreateTexture2D(&d, &init, &tex)) ||
                FAILED(dev->CreateShaderResourceView(tex, nullptr, &view)))
                return fail("stream material");
            tex->Release();
            texels.assign(texels.size(), 0x87654321);
        }
        ctx->PSSetShaderResources(0, _countof(views), views);
        for (auto* view : views) view->Release();
    }
    if (!read_asset(learning ? 33 : 1, 65536)) return fail("stream asset read");
    CloseHandle(file);
    Sleep(650);  // the learning and prefetch workers run asynchronously
    ctx->Release();
    sc->Release();
    dev->Release();
    probe->Release();
    DestroyWindow(wnd);
    return 0;
}

int dedup() {
    ID3D11Device* dev = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &dev, nullptr, nullptr)))
        return fail("dedup device");
    constexpr UINT size = 256, pitch = size * 4 + 16;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = size;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<BYTE> pixels(size * pitch, 0x41);
    D3D11_SUBRESOURCE_DATA init = {pixels.data(), pitch, 0};
    ID3D11Texture2D *first = nullptr, *second = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, &init, &first))) return fail("dedup first");
    pixels[size * 4] ^= 0xff;  // different row padding, same pixels
    if (FAILED(dev->CreateTexture2D(&d, &init, &second))) return fail("dedup second");
    const bool shared = first == second;
    if (shared != (GetEnvironmentVariableW(L"TESSERA_SMOKE_DEDUP", nullptr, 0) != 0))
        return fail("dedup identical pixels");
    second->Release();
    pixels[0] ^= 0xff;  // same sampled signature, different pixels
    if (FAILED(dev->CreateTexture2D(&d, &init, &second)) || first == second) return fail("dedup distinct pixels");
    second->Release();
    pixels[0] ^= 0xff;
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    constexpr int count = 512;
    for (int i = 0; i < count; ++i) {
        if (FAILED(dev->CreateTexture2D(&d, &init, &second))) return fail("dedup loop");
        second->Release();
    }
    QueryPerformanceCounter(&end);
    std::printf("dedup %s: %.2f us per repeated immutable create\n", shared ? "on" : "off",
                (end.QuadPart - start.QuadPart) * 1e6 / freq.QuadPart / count);
    first->Release();
    dev->Release();
    return 0;
}

// Cost of 16-view binds across a large table and for repeated material batches.
int bench(bool repeated) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   nullptr, &ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Many live views bound in random order on tables that already saw churn
    // (tombstones): the cache-missing case of a real game, not a hot loop.
    constexpr int kViews = 24576, kChurn = 8192;
    std::vector<ID3D11Texture2D*> t(kViews);
    std::vector<ID3D11ShaderResourceView*> v(kViews);
    for (int pass = 0; pass < 2; ++pass) {
        const int n = pass == 0 ? kChurn : kViews;
        for (int i = 0; i < n; ++i) {
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &t[i])) ||
                FAILED(dev->CreateShaderResourceView(t[i], nullptr, &v[i])))
                return fail("bench resources");
        }
        if (pass == 0)
            for (int i = 0; i < n; ++i) {
                v[i]->Release();
                t[i]->Release();
            }
    }
    std::vector<ID3D11ShaderResourceView*> order(v);
    uint32_t seed = 12345;
    for (int i = kViews - 1; i > 0; --i) {
        seed = seed * 1664525u + 1013904223u;
        std::swap(order[i], order[seed % (i + 1)]);
    }
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    constexpr int kCalls = 2000000;
    for (int i = 0; i < kCalls; ++i) {
        const size_t batch = repeated ? static_cast<size_t>(i / 8) : static_cast<size_t>(i);
        ctx->PSSetShaderResources(0, 16, &order[batch * 16 * 61 % (kViews - 16)]);
    }
    QueryPerformanceCounter(&b);
    std::printf("bench %s: %.1f ns per 16-view bind\n", repeated ? "repeated" : "random",
                (b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / kCalls);
    // ClearState also changes all slots without calling our hook. An identical
    // batch must still reach D3D11 after the clear.
    ctx->PSSetShaderResources(0, 16, order.data());
    ctx->PSSetShaderResources(0, 16, order.data());
    ctx->ClearState();
    ctx->PSSetShaderResources(0, 16, order.data());
    ID3D11ShaderResourceView* bound = nullptr;
    ctx->PSGetShaderResources(0, 1, &bound);
    if (bound != order[0]) return fail("repeated bind missing after ClearState");
    bound->Release();
    for (int i = 0; i < kViews; ++i) {
        v[i]->Release();
        t[i]->Release();
    }
    ctx->Release();
    dev->Release();
    return 0;
}

// M3-M5 (run with mip_residency=1 and mip_force_evict=1 in tessera.ini):
// large immutable textures are managed, their top mip leaves VRAM once they
// are not bound, and every way of reading them (bind on the immediate or a
// deferred context, copy) sees the full texture again.
struct Tiled {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIAdapter3* adapter = nullptr;
    IDXGISwapChain1* sc = nullptr;
    ID3D11Texture2D* rt = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* readback = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
};

constexpr UINT kTiledSize = 2048, kTiledMips = 12, kTiledCount = 8, kProbe = 64;

uint32_t texel(UINT t, UINT m, UINT x, UINT y) {
    const uint32_t r = (x + t * 17) & 255, g = y & 255, b = ((x >> 8) & 15) | (((y >> 8) & 15) << 4);
    return r | g << 8 | b << 16 | (255u - m * 8) << 24;
}

uint64_t local_usage(IDXGIAdapter3* a) {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
    return mi.CurrentUsage;
}

void present_frames(Tiled& g, int n) {
    for (int i = 0; i < n; ++i) {
        DXGI_PRESENT_PARAMETERS pp = {};
        g.sc->Present1(0, 0, &pp);
    }
}

// Lets the GPU finish, so freed memory shows in the usage.
void settle(Tiled& g) {
    g.ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g.ctx->Map(g.readback, 0, D3D11_MAP_READ, 0, &map))) g.ctx->Unmap(g.readback, 0);
    Sleep(200);
}

// Draws mip m of the view into the probe target on ctx (the immediate one
// or a deferred one, then executed) and compares it with what was uploaded.
bool probe(Tiled& g, ID3D11ShaderResourceView* v, UINT t, UINT m, bool deferred) {
    const UINT size = kTiledSize >> m;
    const UINT step = size >= kProbe ? size / kProbe : 1, off = size >= kProbe ? 7 % step : 0;
    const int p[4] = {static_cast<int>(step), static_cast<int>(off), static_cast<int>(off), static_cast<int>(m)};
    ID3D11DeviceContext* c = g.ctx;
    if (deferred && FAILED(g.dev->CreateDeferredContext(0, &c))) return false;
    c->UpdateSubresource(g.cb, 0, nullptr, p, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, float(kProbe), float(kProbe), 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &g.rtv, nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.vs, nullptr, 0);
    c->PSSetShader(g.ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g.cb);
    c->PSSetShaderResources(0, 1, &v);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    if (deferred) {
        ID3D11CommandList* cl = nullptr;
        const HRESULT hr = c->FinishCommandList(FALSE, &cl);
        c->Release();
        if (FAILED(hr)) return false;
        g.ctx->ExecuteCommandList(cl, FALSE);
        cl->Release();
    }
    g.ctx->CopyResource(g.readback, g.rt);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.ctx->Map(g.readback, 0, D3D11_MAP_READ, 0, &map))) return false;
    bool ok = true;
    for (UINT y = 0; y < kProbe && ok; ++y)
        for (UINT x = 0; x < kProbe && ok; ++x) {
            const UINT sx = x * step + off, sy = y * step + off;
            if (sx >= size || sy >= size) continue;
            const uint32_t got = static_cast<const uint32_t*>(map.pData)[y * map.RowPitch / 4 + x];
            if (got != texel(t, m, sx, sy)) {
                std::printf("texture %u mip %u at %u,%u: 0x%08x, want 0x%08x\n", t, m, sx, sy, got, texel(t, m, sx, sy));
                ok = false;
            }
        }
    g.ctx->Unmap(g.readback, 0);
    return ok;
}

bool probe_all_mips(Tiled& g, ID3D11ShaderResourceView* v, UINT t, bool deferred = false) {
    return probe(g, v, t, 0, deferred) && probe(g, v, t, 1, deferred) && probe(g, v, t, kTiledMips - 1, deferred);
}

int tiled() {
    Tiled g;
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};  // tier 2 needs 11_1
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &g.dev,
                                   nullptr, &g.ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
    g.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
    if (o1.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2) {
        std::printf("tiled: skipped (tiled resources tier %d)\n", static_cast<int>(o1.TiledResourcesTier));
        return 0;
    }
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* a = nullptr;
    IDXGIFactory2* factory = nullptr;
    g.dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    dxgi->GetAdapter(&a);
    a->QueryInterface(IID_PPV_ARGS(&g.adapter));
    a->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(g.dev, make_window(), &sd, nullptr, nullptr, &g.sc);
    factory->Release();
    a->Release();
    dxgi->Release();  // a reference to the device, which must go at the end
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);

    static const char kSrc[] =
        "Texture2D<float4> tx : register(t0);\n"
        "cbuffer c : register(b0) { int4 p; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps(float4 pos : SV_Position) : SV_Target { return tx.Load(int3(int2(pos.xy) * p.x + p.yz, p.w)); }\n";
    ID3DBlob* vb = nullptr;
    ID3DBlob* pb = nullptr;
    hr = D3DCompile(kSrc, sizeof kSrc - 1, "tiled", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, nullptr);
    if (SUCCEEDED(hr)) hr = D3DCompile(kSrc, sizeof kSrc - 1, "tiled", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, nullptr);
    if (SUCCEEDED(hr)) hr = g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g.vs);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g.ps);
    if (FAILED(hr)) return fail("probe shaders", hr);
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = rd.Height = kProbe;
    rd.MipLevels = rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    if (SUCCEEDED(hr)) hr = g.dev->CreateTexture2D(&rd, nullptr, &g.rt);
    if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(g.rt, nullptr, &g.rtv);
    rd.BindFlags = 0;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = g.dev->CreateTexture2D(&rd, nullptr, &g.readback);
    if (FAILED(hr)) return fail("probe target", hr);

    // The textures: full chains, a different pattern per texture and mip.
    std::vector<uint32_t> data;
    for (UINT m = 0; m < kTiledMips; ++m) data.resize(data.size() + (kTiledSize >> m) * (kTiledSize >> m));
    D3D11_SUBRESOURCE_DATA init[kTiledMips];
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kTiledSize;
    td.MipLevels = 0;  // the full chain
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex[kTiledCount] = {};
    ID3D11ShaderResourceView* srv[kTiledCount] = {};
    const bool stream = GetEnvironmentVariableW(L"TESSERA_SMOKE_MIP_STREAM", nullptr, 0) != 0;
    const bool predict = GetEnvironmentVariableW(L"TESSERA_SMOKE_MIP_PREDICT", nullptr, 0) != 0;
    for (UINT t = 0; t < kTiledCount; ++t) {
        size_t at = 0;
        for (UINT m = 0; m < kTiledMips; ++m) {
            const UINT s = kTiledSize >> m;
            for (UINT y = 0; y < s; ++y)
                for (UINT x = 0; x < s; ++x) data[at + y * s + x] = texel(t, m, x, y);
            init[m] = {&data[at], s * 4, 0};
            at += size_t{s} * s;
        }
        hr = g.dev->CreateTexture2D(&td, init, &tex[t]);
        if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(tex[t], nullptr, &srv[t]);
        if (FAILED(hr)) return fail("tiled texture", hr);
        if (stream && t == 0) Sleep(550);  // refill an exact-size spare before the next create
    }
    D3D11_TEXTURE2D_DESC seen = {};
    tex[0]->GetDesc(&seen);
    std::printf("tiled: desc usage %d misc 0x%x mips %u\n", seen.Usage, seen.MiscFlags, seen.MipLevels);
    if (seen.Usage != D3D11_USAGE_IMMUTABLE || seen.MiscFlags != 0 || seen.MipLevels != kTiledMips)
        return fail("the game must see the texture it asked for");

    if (stream) {
        LARGE_INTEGER f, start, end;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&start);
        present_frames(g, 1);
        QueryPerformanceCounter(&end);
        std::printf("tiled: first upload Present %.0f us\n", (end.QuadPart - start.QuadPart) * 1e6 / f.QuadPart);
        if (!probe_all_mips(g, srv[kTiledCount - 1], kTiledCount - 1)) return fail("content after a partial upload");
    }
    // The first bind must finish any outstanding copy.
    for (UINT t = 0; t < kTiledCount; ++t)
        if (!probe_all_mips(g, srv[t], t)) return fail("content at the first bind");
    if (predict) {
        present_frames(g, 1);  // start a fresh sampling window
        for (unsigned i = 0; i < 4; ++i) g.ctx->PSSetShaderResources(0, 2, srv);
        Sleep(300);
        ID3D11ShaderResourceView* empty[2] = {};
        g.ctx->PSSetShaderResources(0, 2, empty);
        for (UINT t = 0; t < kTiledCount; ++t)
            if (!probe_all_mips(g, srv[t], t)) return fail("content during prediction training");
    }
    settle(g);
    const uint64_t before = local_usage(g.adapter);
    // Nothing is bound now: all evicted, and the freed memory trimmed a few
    // frames later.
    present_frames(g, 6);
    settle(g);
    const uint64_t evicted = local_usage(g.adapter);
    std::printf("tiled: local usage %llu MB -> %llu MB after eviction\n", before >> 20, evicted >> 20);
    if (evicted + (96ull << 20) > before) return fail("top mips must leave VRAM");
    if (g.ctx->GetResourceMinLOD(tex[3]) != 0.0f) return fail("the game's min LOD must stay 0");
    if (predict) {
        g.ctx->PSSetShaderResources(0, 1, srv);  // the learned partner remains unbound
        Sleep(300);
        present_frames(g, 1);
        if (stream) {
            settle(g);  // let a deferred prediction retry after the preceding upload
            present_frames(g, 1);
        }
        if (!probe_all_mips(g, srv[1], 1)) return fail("content of the predicted texture");
        Sleep(1050);  // let the writer publish the prediction counter
    }

    // M5: binding restores before the draw, immediate and deferred. The bind
    // itself carries the restore: its CPU cost is what the frame pays.
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        double sum = 0, worst = 0;
        for (UINT t = 0; t < kTiledCount; ++t) {
            QueryPerformanceCounter(&t0);
            g.ctx->PSSetShaderResources(0, 1, &srv[t]);
            QueryPerformanceCounter(&t1);
            const double us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(f.QuadPart);
            sum += us;
            if (us > worst) worst = us;
        }
        ID3D11ShaderResourceView* none = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &none);
        std::printf("tiled: restore on bind %.0f us mean, %.0f us worst (%u textures)\n", sum / kTiledCount, worst,
                    kTiledCount);
    }
    for (UINT t = 0; t < kTiledCount; ++t)
        if (!probe_all_mips(g, srv[t], t)) return fail("content after restore");
    present_frames(g, 3);
    // Games create deferred contexts all session long, each with a vtable of
    // its own: the hooks must keep working past many of them.
    for (int round = 0; round < 2; ++round) {
        ID3D11DeviceContext* c[40] = {};
        ID3D11Device1* dev1 = nullptr;
        if (FAILED(g.dev->QueryInterface(IID_PPV_ARGS(&dev1)))) return fail("ID3D11Device1");
        for (int k = 0; k < 40; ++k) {
            // Round 1 through CreateDeferredContext1, which may reach the plain hook too.
            HRESULT chr = S_OK;
            if (round == 0) {
                chr = g.dev->CreateDeferredContext(0, &c[k]);
            } else {
                ID3D11DeviceContext1* c1 = nullptr;
                chr = dev1->CreateDeferredContext1(0, &c1);
                c[k] = c1;
            }
            if (FAILED(chr)) return fail("deferred context", chr);
            c[k]->PSSetShaderResources(0, 1, &srv[k % kTiledCount]);
        }
        for (ID3D11DeviceContext* x : c) x->Release();
        dev1->Release();
    }
    if (!probe_all_mips(g, srv[2], 2, true)) return fail("content through a deferred context");
    present_frames(g, 6);
    settle(g);
    const uint64_t again = local_usage(g.adapter);
    std::printf("tiled: local usage %llu MB after eviction past 80 deferred contexts\n", again >> 20);
    if (again + (96ull << 20) > before) return fail("evictions must go on past many deferred contexts");
    // A copy from an evicted texture copies the whole texture.
    D3D11_TEXTURE2D_DESC cd = td;
    cd.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D* copy = nullptr;
    ID3D11ShaderResourceView* copy_srv = nullptr;
    hr = g.dev->CreateTexture2D(&cd, nullptr, &copy);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(copy, nullptr, &copy_srv);
    if (FAILED(hr)) return fail("copy target", hr);

    g.ctx->CopyResource(copy, tex[5]);
    if (!probe_all_mips(g, copy_srv, 5)) return fail("content of a copy from an evicted texture");
    copy_srv->Release();
    copy->Release();

    // A new device while these textures live (a device the game replaces):
    // it takes over once they are gone (the log shows it, run_smoke checks).
    ID3D11Device* next = nullptr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &next, nullptr,
                           nullptr);
    if (FAILED(hr)) return fail("second device", hr);
    // A third one (a video decoder's, say) comes after it: the second still takes over.
    ID3D11Device* other = nullptr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &other, nullptr,
                           nullptr);
    if (FAILED(hr)) return fail("third device", hr);

    // Released by the game: our reference goes within a few seconds.
    for (UINT t = 0; t < kTiledCount; ++t) {
        srv[t]->Release();
        tex[t]->Release();
    }
    const uint64_t held = local_usage(g.adapter);
    uint64_t now = held;
    for (int i = 0; i < 40 && now + (32ull << 20) > held; ++i) {
        Sleep(100);
        present_frames(g, 1);
        now = local_usage(g.adapter);
    }
    std::printf("tiled: local usage %llu MB -> %llu MB after release\n", held >> 20, now >> 20);
    if (now + (32ull << 20) > held) return fail("released textures must be freed");
    present_frames(g, 10);  // the rest of the drops
    // The managed device is still alive: the second one must not take over.
    ID3D11Texture2D* on_next = nullptr;
    hr = next->CreateTexture2D(&td, init, &on_next);
    if (FAILED(hr)) return fail("texture on the second device", hr);
    ID3D11Device2* next2 = nullptr;
    if (FAILED(next->QueryInterface(IID_PPV_ARGS(&next2)))) return fail("ID3D11Device2");
    // Managed textures are tiled: tiles to report.
    auto managed = [next2](ID3D11Texture2D* t) {
        UINT total = 0, n_sub = 0;
        next2->GetResourceTiling(t, &total, nullptr, nullptr, &n_sub, 0, nullptr);
        return total != 0;
    };
    if (managed(on_next)) return fail("the second device took over while the first lives");
    on_next->Release();
    // The game lets go of the first device: now the second one takes over.
    g.ctx->ClearState();
    if (stream || predict) {
        // Let queued uploads and readbacks finish, then give the writer its
        // next pass to enqueue releases no longer held by GPU commands.
        settle(g);
        Sleep(1050);
        present_frames(g, 2);
        settle(g);
    }
    g.ctx->Flush();
    for (IUnknown* u : {static_cast<IUnknown*>(g.ps), static_cast<IUnknown*>(g.vs), static_cast<IUnknown*>(g.cb),
                        static_cast<IUnknown*>(g.readback), static_cast<IUnknown*>(g.rtv), static_cast<IUnknown*>(g.rt),
                        static_cast<IUnknown*>(g.sc), static_cast<IUnknown*>(g.ctx)})
        if (u) u->Release();
    if (g.dev->Release() != 0) return fail("the first device is still referenced");
    hr = next->CreateTexture2D(&td, init, &on_next);
    if (FAILED(hr)) return fail("texture on the second device after the first is gone", hr);
    const bool took_over = managed(on_next);
    on_next->Release();
    next2->Release();
    next->Release();
    other->Release();
    if (!took_over) return fail("the second device must take over once the first is gone");
    std::printf("tiled: ok\n");
    return 0;
}

// --nvapi: is nvapi_QueryInterface handed back wrapped, both to the exe and
// to a game DLL loaded later (nvapi_probe.dll)? 0 both, 3 neither (safe
// mode), 1 mixed, 4 no NVAPI on this machine.
int nvapi_mode() {
    HMODULE probe = LoadLibraryW(L"nvapi_probe.dll");
    if (!probe) return fail("nvapi_probe.dll");
    using Probe = void*(*)();
    const auto probe_qi = reinterpret_cast<Probe>(GetProcAddress(probe, "probe_qi"));
    void* const from_dll = probe_qi ? probe_qi() : nullptr;
    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (!nv) {
        std::printf("no NVAPI, skipped\n");
        return 4;
    }
    void* const from_exe = reinterpret_cast<void*>(GetProcAddress(nv, "nvapi_QueryInterface"));
    auto wrapped = [nv](void* p) {
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(p), &owner);
        return p && owner != nv;
    };
    const bool e = wrapped(from_exe), d = wrapped(from_dll);
    std::printf("nvapi_QueryInterface wrapped: exe %d, game DLL %d\n", e, d);
    return e && d ? 0 : !e && !d ? 3 : 1;
}

// --imports: CreateDXGIFactory1 as the exe and a game DLL get it, through
// their import tables and GetProcAddress, must be the layer's: 0 all, 3 none
// (safe mode), 1 mixed. With TESSERA_SMOKE_CAP_MB: capped before any device,
// and left alone once the game creates a D3D12 device instead (2 if not).
int imports_mode() {
    HMODULE probe = LoadLibraryW(L"nvapi_probe.dll");
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!probe || !dxgi) return fail("nvapi_probe.dll / dxgi.dll");
    // The loader announces a DLL before binding its imports: the layer patches
    // them on the next load (as games load many) or the session's writer tick.
    // A DLL of ours, so it is surely a new load.
    if (!LoadLibraryW(L"load_probe.dll")) return fail("load_probe.dll");
    using Probe = void*(*)();
    // probe_factory_import only makes the DLL import CreateDXGIFactory1; its slot is read directly.
    const auto by_name = reinterpret_cast<Probe>(GetProcAddress(probe, "probe_factory"));
    if (!by_name) return fail("probe exports");
    void* const p[4] = {import_slot(GetModuleHandleW(nullptr), "CreateDXGIFactory1"),
                        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")),
                        import_slot(probe, "CreateDXGIFactory1"), by_name()};
    int wrapped = 0;
    for (void* q : p) wrapped += q && module_of(q) != dxgi;
    std::printf("CreateDXGIFactory1 wrapped: exe import %d, exe GetProcAddress %d, DLL import %d, DLL GetProcAddress %d\n",
                p[0] && module_of(p[0]) != dxgi, p[1] && module_of(p[1]) != dxgi, p[2] && module_of(p[2]) != dxgi,
                p[3] && module_of(p[3]) != dxgi);
    if (wrapped == 0) return 3;
    if (wrapped != 4) return 1;
    if (!GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", nullptr, 0)) return 0;
    wchar_t cap_s[16] = {};
    GetEnvironmentVariableW(L"TESSERA_SMOKE_CAP_MB", cap_s, 16);
    const unsigned long long cap = static_cast<unsigned long long>(_wtoi(cap_s)) << 20;
    auto dedicated = []() -> unsigned long long {
        IDXGIFactory1* f = nullptr;
        IDXGIAdapter1* a = nullptr;
        DXGI_ADAPTER_DESC1 d = {};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 0;
        if (SUCCEEDED(f->EnumAdapters1(0, &a))) {
            a->GetDesc1(&d);
            a->Release();
        }
        f->Release();
        return d.DedicatedVideoMemory;
    };
    const unsigned long long before = dedicated();
    std::printf("dedicated memory before any device: %llu MB\n", before >> 20);
    if (!before || before > cap) return fail("dedicated memory cap not applied before the device");
    HMODULE d12 = LoadLibraryW(L"d3d12.dll");
    using Create12 = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create12 = d12 ? reinterpret_cast<Create12>(GetProcAddress(d12, "D3D12CreateDevice")) : nullptr;
    ID3D12Device* dev12 = nullptr;
    if (!create12 || FAILED(create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12)))) {
        std::printf("no D3D12 device, skipped\n");
        return 0;
    }
    const unsigned long long after = dedicated();
    dev12->Release();
    std::printf("dedicated memory after a D3D12 device: %llu MB\n", after >> 20);
    if (before < cap) {
        std::printf("adapter below the cap: not decisive\n");
        return 0;
    }
    if (after <= cap) {
        std::printf("FAIL: still capped after the game created a D3D12 device\n");
        return 2;
    }
    return 0;
}

// --recycle (rt_recycle=1): a released big render target comes back on the
// next create with the same description; one still held, or a create with
// initial data, gets a new texture. 0 ok, 3 nothing reused (layer off).
int recycle_mode() {
    ID3D11Device* dev = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   nullptr, nullptr);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4096;  // 64 MB
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    static const GUID kTag = {0x5e1a7c3b, 0x2d4f, 0x4c9a, {0x8b, 0x61, 0x0f, 0x3e, 0x9d, 0x27, 0xa4, 0x15}};
    auto tagged = [](ID3D11Texture2D* t) {
        UINT v = 0, size = sizeof v;
        return SUCCEEDED(t->GetPrivateData(kTag, &size, &v)) && v == 1;
    };
    ID3D11Texture2D *a = nullptr, *b = nullptr, *c = nullptr;
    if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &a))) return fail("CreateTexture2D a", hr);
    const UINT one = 1;
    a->SetPrivateData(kTag, sizeof one, &one);
    ID3D11RenderTargetView* v = nullptr;
    dev->CreateRenderTargetView(a, nullptr, &v);
    // Held by the game: never handed out.
    if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &b))) return fail("CreateTexture2D b", hr);
    if (tagged(b)) return fail("a texture still held was handed out");
    b->Release();
    a->Release();  // the view still holds it
    if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &b))) return fail("CreateTexture2D b2", hr);
    if (tagged(b)) return fail("a texture with a live view was handed out");
    b->Release();
    v->Release();  // now released for good
    const bool reused = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &c)) && tagged(c);
    std::printf("released render target reused: %d\n", reused);
    if (!reused) {
        if (c) c->Release();
        dev->Release();
        return 3;
    }
    c->Release();
    // Initial data: always a new texture.
    std::vector<uint32_t> px(4096 * 4096);
    const D3D11_SUBRESOURCE_DATA init = {px.data(), 4096 * 4, 0};
    if (FAILED(hr = dev->CreateTexture2D(&td, &init, &c))) return fail("CreateTexture2D init", hr);
    if (tagged(c)) return fail("a create with initial data got a recycled texture");
    c->Release();
    dev->Release();
    return 0;
}

// U1: fake Unreal console variables laid out as the engine's, set by the layer
// from cvar.* lines this mode writes to tessera.ini (the RVAs are only known here).
struct FakeAutoCvar {
    const void* vtable;  // FAutoConsoleObject vtable, in .rdata
    void* target;        // IConsoleObject* (heap object whose vtable is in .rdata)
    void* ref;           // TConsoleVariableData<T>*: T[2] on the heap
};
extern const void* const kFakeVtable[2];
const void* const kFakeVtable[2] = {&kFakeVtable[0], &kFakeVtable[1]};
FakeAutoCvar g_cvar_int, g_cvar_float, g_cvar_bad;
float g_cvar_ref = 0.25f;

int uecvars_mode(bool stale) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return fail("VirtualAlloc");
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto set_up = [&](FakeAutoCvar& c, size_t at, const void* object_vtable) {
        c.vtable = kFakeVtable;
        c.target = page + at;
        *reinterpret_cast<const void**>(page + at) = object_vtable;
        c.ref = page + at + 64;
    };
    set_up(g_cvar_int, 0, kFakeVtable);
    set_up(g_cvar_float, 128, kFakeVtable);
    set_up(g_cvar_bad, 256, page);  // target's vtable on the heap: not a console variable
    int* vi = static_cast<int*>(g_cvar_int.ref);
    float* vf = static_cast<float*>(g_cvar_float.ref);
    int* vb = static_cast<int*>(g_cvar_bad.ref);
    vi[0] = vi[1] = -1;
    vf[0] = vf[1] = 1.0f;
    vb[0] = vb[1] = -1;
    auto rva = [&](const void* p) { return static_cast<unsigned long>(static_cast<const uint8_t*>(p) - exe); };
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + dos->e_lfanew);
    wchar_t ini[MAX_PATH];
    GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wcscpy_s(wcsrchr(ini, L'\\') + 1, 32, L"tessera.ini");
    FILE* f = nullptr;
    if (_wfopen_s(&f, ini, L"w") || !f) return fail("write tessera.ini");
    std::fprintf(f, "[tessera]\nue_image=%lx %lx\n", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
                 static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage) + (stale ? 0x1000ul : 0ul));
    std::fprintf(f, "cvar.r.Test.Int=2850 i p %lx\n", rva(&g_cvar_int.ref));
    std::fprintf(f, "cvar.r.Test.Float=0.5 f p %lx\n", rva(&g_cvar_float.ref));
    std::fprintf(f, "cvar.s.Test.Ref=8.5 f d %lx\n", rva(&g_cvar_ref));
    std::fprintf(f, "cvar.r.Test.Bad=7 i p %lx\n", rva(&g_cvar_bad.ref));
    std::fprintf(f, "cvar.r.Test.ReadOnly=7 i d %lx\n", rva(&kFakeVtable[0]));
    std::fprintf(f, "cvar.r.Test.Garbage=x i p %lx\n", rva(&g_cvar_int.ref));
    std::fclose(f);

    HWND wnd = make_window();
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 320;
    sd.BufferDesc.Height = 180;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("D3D11CreateDeviceAndSwapChain", hr);
    int rc = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) rc = fail(what);
    };
    const bool untouched_bad = vb[0] == -1 && vb[1] == -1 && kFakeVtable[0] == &kFakeVtable[0];
    if (stale) {
        expect(vi[0] == -1 && vi[1] == -1 && vf[0] == 1.0f && g_cvar_ref == 0.25f, "stale image: values changed");
    } else {
        expect(vi[0] == 2850 && vi[1] == 2850, "int variable not set in both copies");
        expect(vf[0] == 0.5f && vf[1] == 0.5f, "float variable not set in both copies");
        expect(g_cvar_ref == 8.5f, "referenced variable not set");
        expect(untouched_bad, "a slot that failed the checks was written");
        // The game sets them again (scalability, settings menu): restored within about a second.
        vi[0] = 5;
        vf[1] = 2.0f;
        g_cvar_ref = 1.0f;
        const ULONGLONG until = GetTickCount64() + 1500;
        while (GetTickCount64() < until) render(dev, ctx, sc, 5, false);
        expect(vi[0] == 2850 && vf[1] == 0.5f && g_cvar_ref == 8.5f, "changed values not restored");
        expect(vb[0] == -1, "bad slot written later");
        // The engine frees or replaces a variable (shutdown): the old memory is left alone.
        g_cvar_float.ref = page + 128 + 96;
        vf[0] = 3.0f;
        const ULONGLONG until2 = GetTickCount64() + 1500;
        while (GetTickCount64() < until2) render(dev, ctx, sc, 5, false);
        expect(vf[0] == 3.0f, "a moved variable's old memory was written");
    }
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(wnd);
    std::printf("console variables (%s): %s\n", stale ? "stale image" : "applied", rc ? "FAIL" : "ok");
    return rc;
}

int upload_flight_probe();

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--upload-flight") == 0) return upload_flight_probe();
    if (argc >= 2 && wcscmp(argv[1], L"--uecvars") == 0) return uecvars_mode(argc >= 3 && wcscmp(argv[2], L"stale") == 0);
    if (argc >= 2 && wcscmp(argv[1], L"--bench") == 0)
        return bench(argc >= 3 && wcscmp(argv[2], L"repeated") == 0);
    if (argc >= 2 && wcscmp(argv[1], L"--dedup") == 0) return dedup();
    if (argc >= 2 && wcscmp(argv[1], L"--stream") == 0) return stream();
    if (argc >= 2 && wcscmp(argv[1], L"--imports") == 0) return imports_mode();
    if (argc >= 2 && wcscmp(argv[1], L"--recycle") == 0) return recycle_mode();
    if (argc >= 2 && wcscmp(argv[1], L"--tiled") == 0) return tiled();
    if (argc >= 2 && wcscmp(argv[1], L"--nvapi") == 0) return nvapi_mode();
    if (argc >= 2 && wcscmp(argv[1], L"--pacing") == 0) return pacing_mode(argc >= 3 && wcscmp(argv[2], L"driver") == 0);
    if (argc >= 2 && wcscmp(argv[1], L"--d3d11-calls") == 0)
        return d3d11_calls_mode(argc >= 3 && wcscmp(argv[2], L"bench") == 0);
    if (argc >= 3 && wcscmp(argv[1], L"--verify") == 0) return verify(argv[2], argc >= 4 ? _wtoi(argv[3]) : 0);
    return run(argc >= 2 && wcscmp(argv[1], L"--crash") == 0);
}
