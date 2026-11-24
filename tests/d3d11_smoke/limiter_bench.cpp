// SPDX-License-Identifier: GPL-3.0-or-later
// Frame limiter benchmark: a synthetic game whose CPU work varies, with
// heavier frames, stutters and a sustained load step. The app writes when
// each frame started (input sampled: its previous Present returned) and when
// its work ended; the layer's session log has when each Present was released.
//   d3d11_smoke --limiter-bench <fps> <csv> [d3d12]   (d3d12: run with the layer as dxgi.dll)
//   d3d11_smoke --limiter-report <sessions dir> <csv> <fps>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "tessera/session_format.h"

namespace {
int bench_fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}

enum Kind : int { kNormal = 0, kHeavy = 1, kStutter = 2, kStep = 3 };
constexpr int kFrames = 480;

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Deterministic load in fractions of the frame interval.
Kind frame_kind(int i) {
    if (i % 120 == 60) return kStutter;
    if (i >= 240 && i < 300) return kStep;
    if (i % 25 == 7) return kHeavy;
    return kNormal;
}
double frame_work(int i, uint32_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    const double noise = (rng >> 8) / double(1u << 24);  // [0, 1)
    switch (frame_kind(i)) {
        case kStutter: return 2.7;
        case kStep: return 0.60 + 0.10 * noise;
        case kHeavy: return 0.66;
        default: return 0.42 + 0.12 * noise;
    }
}

struct Row {
    int64_t start, end;
    int kind;
};

// D3D12: a queue and a flip swap chain from a DXGI factory, the path a D3D12
// game takes (the layer sees it as dxgi.dll). Nothing is rendered: the back
// buffers stay in the present state.
IDXGISwapChain* d3d12_swapchain(HWND wnd, IUnknown** keep) {
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ID3D12CommandQueue* queue = nullptr;
    const HRESULT qhr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    dev->Release();
    if (FAILED(qhr)) return nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = d.Height = 64;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        factory->CreateSwapChainForHwnd(queue, wnd, &d, nullptr, nullptr, &sc);
        factory->Release();
    }
    if (!sc) {
        queue->Release();
        return nullptr;
    }
    *keep = queue;
    return sc;
}

IDXGISwapChain* d3d11_swapchain(HWND wnd, IUnknown** keep) {
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
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &desc, &sc, &dev, nullptr, nullptr)))
        return nullptr;
    *keep = dev;
    return sc;
}
}  // namespace

int limiter_bench(int fps, const wchar_t* csv, bool d3d12) {
    if (fps < 20) return bench_fail("fps");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"tessera_limiter_bench";
    RegisterClassW(&wc);
    const HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"limiter bench", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180,
                                     nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return bench_fail("window");
    IUnknown* keep = nullptr;
    IDXGISwapChain* sc = d3d12 ? d3d12_swapchain(wnd, &keep) : d3d11_swapchain(wnd, &keep);
    if (!sc) return bench_fail("device");
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double interval = double(freq.QuadPart) / fps;
    std::vector<Row> rows;
    rows.reserve(kFrames);
    uint32_t rng = 12345;
    if (FAILED(sc->Present(0, 0))) return bench_fail("initial present");
    for (int i = 0; i < kFrames; ++i) {
        const int64_t start = qpc();
        const int64_t end = start + static_cast<int64_t>(frame_work(i, rng) * interval);
        while (qpc() < end) YieldProcessor();
        rows.push_back({start, qpc(), frame_kind(i)});
        if (FAILED(sc->Present(0, 0))) return bench_fail("present");
    }
    sc->Release();
    keep->Release();
    DestroyWindow(wnd);
    FILE* f = nullptr;
    if (_wfopen_s(&f, csv, L"w") || !f) return bench_fail("open csv");
    std::fprintf(f, "start_qpc,work_end_qpc,kind\n");
    for (const auto& r : rows) std::fprintf(f, "%lld,%lld,%d\n", r.start, r.end, r.kind);
    std::fclose(f);
    return 0;
}

int limiter_report(const wchar_t* dir, const wchar_t* csv, int fps) {
    std::vector<Row> rows;
    FILE* f = nullptr;
    if (_wfopen_s(&f, csv, L"r") || !f) return bench_fail("open csv");
    char line[128];
    std::fgets(line, sizeof line, f);
    Row r{};
    while (fscanf_s(f, "%lld,%lld,%d", &r.start, &r.end, &r.kind) == 3) rows.push_back(r);
    std::fclose(f);

    wchar_t pattern[MAX_PATH], path[MAX_PATH] = {};
    swprintf_s(pattern, L"%s\\*.tsess", dir);
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return bench_fail("no session file");
    FILETIME newest = {};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
            newest = fd.ftLastWriteTime;
            swprintf_s(path, L"%s\\%s", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (_wfopen_s(&f, path, L"rb") || !f) return bench_fail("open session");
    TesseraSessionHeader hdr;
    if (std::fread(&hdr, sizeof hdr, 1, f) != 1 || std::memcmp(hdr.magic, TESSERA_SESSION_MAGIC, 8) != 0)
        return bench_fail("session header");
    std::vector<int64_t> released;
    TesseraRecord rec;
    while (std::fread(&rec, sizeof rec, 1, f) == 1)
        if (rec.kind == TESSERA_REC_FRAME) released.push_back(static_cast<int64_t>(rec.qpc));
    std::fclose(f);
    if (rows.size() < 100 || released.size() < rows.size()) return bench_fail("frame count");
    released.erase(released.begin(), released.end() - rows.size());  // drop the initial Present

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double ms = 1000.0 / freq.QuadPart, interval = 1000.0 / fps;
    std::vector<double> latency, after_stutter, deviation;
    int short_after = 0, misses = 0;
    double latency_sum = 0;
    for (size_t i = 1; i < rows.size(); ++i) {
        const double lat = (released[i] - rows[i].start) * ms;
        const double gap = (released[i] - released[i - 1]) * ms;
        const bool stutter = rows[i].kind == kStutter;
        const bool recovering = rows[i - 1].kind == kStutter || (i >= 2 && rows[i - 2].kind == kStutter) ||
                                (i >= 3 && rows[i - 3].kind == kStutter);
        if (!stutter) {
            latency.push_back(lat);
            latency_sum += lat;
            deviation.push_back(std::fabs(gap - interval));
            if (gap > interval * 1.1) ++misses;
        }
        if (recovering) {
            after_stutter.push_back(lat);
            if (gap < interval * 0.9) ++short_after;
        }
    }
    const auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
    };
    double sd = 0, mean_dev = 0;
    for (double d : deviation) mean_dev += d;
    mean_dev /= deviation.size();
    for (double d : deviation) sd += d * d;
    sd = std::sqrt(sd / deviation.size());
    std::printf("limiter: latency mean=%.2f p50=%.2f p99=%.2f ms, after stutter max=%.2f ms, "
                "cadence rms=%.3f p99=%.3f ms, short after stutter=%d, misses=%d/%zu\n",
                latency_sum / latency.size(), pct(latency, 0.5), pct(latency, 0.99),
                *std::max_element(after_stutter.begin(), after_stutter.end()), sd, pct(deviation, 0.99),
                short_after, misses, deviation.size());
    return 0;
}
