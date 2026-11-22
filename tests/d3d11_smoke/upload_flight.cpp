// SPDX-License-Identifier: GPL-3.0-or-later
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdio>

#include "upload_flight.h"

int upload_flight_probe() {
    using Microsoft::WRL::ComPtr;
    using tessera::UploadFlight;
    constexpr uint64_t mib = 1ull << 20;
    UploadFlight flight;
    flight.submitted = 15 * mib / 2;
    if (flight.budget(2 * mib) != mib / 2 || flight.can_restore()) return 1;
    flight.submitted += 6 * mib;  // on demand can exceed the speculative window
    if (flight.budget(2 * mib) != 0) return 2;
    flight.observe(5 * mib);
    if (flight.budget(2 * mib) != 0) return 3;
    flight.observe(6 * mib);
    flight.observe(4 * mib);  // a different swap chain can report an older value
    flight.observe(UINT64_MAX);
    if (flight.budget(2 * mib) != mib / 2) return 4;
    flight.observe(flight.submitted);
    if (flight.budget(2 * mib) != 2 * mib || !flight.can_restore()) return 5;
    std::puts("PASS upload accounting, backpressure, stale fence values and recovery");

    // Exercise an actual GPU completion even on hardware with tiled resources
    // Tier 1. Only the smoke process explicitly flushes and waits.
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 6;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Fence> fence;
    if (FAILED(dev.As(&dev5)) || FAILED(ctx.As(&ctx4)) ||
        FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        std::puts("SKIP native D3D11 fence (unsupported)");
        return 0;
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(UploadFlight::limit);
    ComPtr<ID3D11Buffer> src, dst;
    if (FAILED(dev->CreateBuffer(&desc, nullptr, &src)) || FAILED(dev->CreateBuffer(&desc, nullptr, &dst))) return 7;
    flight = {};
    ctx->CopyResource(dst.Get(), src.Get());
    flight.submitted = desc.ByteWidth;
    flight.observe(fence->GetCompletedValue());
    if (flight.budget(2 * mib) != 0 || FAILED(ctx4->Signal(fence.Get(), flight.submitted))) return 8;
    const HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) return 9;
    const HRESULT hr = fence->SetEventOnCompletion(flight.submitted, completed);
    ctx->Flush();
    const DWORD timeout = SUCCEEDED(hr) ? WaitForSingleObject(completed, 5000) : WAIT_FAILED;
    CloseHandle(completed);
    flight.observe(fence->GetCompletedValue());
    if (timeout != WAIT_OBJECT_0 || flight.bytes() != 0 || flight.budget(2 * mib) != 2 * mib) return 10;
    std::puts("PASS native D3D11 copy completes the byte-count fence");
    return 0;
}
