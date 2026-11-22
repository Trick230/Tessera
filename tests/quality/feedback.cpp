// SPDX-License-Identifier: GPL-3.0-or-later
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
using Microsoft::WRL::ComPtr;

int fail(const char* check, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "FAIL %s (0x%08lx)\n", check, static_cast<unsigned long>(hr));
    return 1;
}

HRESULT pixel_shader(ID3D11Device* dev, const wchar_t* path, ID3D11PixelShader** out) {
    ComPtr<ID3DBlob> code;
    HRESULT hr = D3DReadFileToBlob(path, &code);
    if (FAILED(hr)) return hr;
    return dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
}

HRESULT make_swapchain(ID3D11Device* dev, IDXGISwapChain1** out) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    const HWND window = CreateWindowExW(0, L"STATIC", L"Tessera feedback test", WS_OVERLAPPEDWINDOW,
                                         0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return HRESULT_FROM_WIN32(GetLastError());
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = d.Height = 4;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferCount = 2;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return factory->CreateSwapChainForHwnd(dev, window, &d, nullptr, nullptr, out);
}

bool verify_copies(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* source,
                   const D3D11_TEXTURE2D_DESC& expected, const D3D11_SUBRESOURCE_DATA* initial) {
    D3D11_TEXTURE2D_DESC visible = {};
    source->GetDesc(&visible);
    if (visible.Format != expected.Format || visible.MipLevels != expected.MipLevels ||
        visible.Usage != D3D11_USAGE_IMMUTABLE) {
        fail("visible texture descriptor");
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = expected.Format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = expected.MipLevels;
    ComPtr<ID3D11ShaderResourceView> explicit_view;
    if (FAILED(dev->CreateShaderResourceView(source, &desc, &explicit_view))) {
        fail("explicit view creation");
        return false;
    }
    explicit_view->GetDesc(&desc);
    if (desc.Format != expected.Format) {
        fail("visible view descriptor");
        return false;
    }
    ComPtr<ID3D11Device3> dev3;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3)))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC1 d = {};
        d.Format = expected.Format;
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipLevels = expected.MipLevels;
        ComPtr<ID3D11ShaderResourceView1> view;
        if (FAILED(dev3->CreateShaderResourceView1(source, &d, &view))) {
            fail("explicit view 1 creation");
            return false;
        }
        view->GetDesc1(&d);
        if (d.Format != expected.Format) {
            fail("visible view 1 descriptor");
            return false;
        }
    }

    auto staging = expected;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.BindFlags = 0;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(dev->CreateTexture2D(&staging, nullptr, &readback))) return false;
    ctx->CopyResource(readback.Get(), source);
    for (UINT mip = 0; mip < expected.MipLevels; ++mip) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(readback.Get(), mip, D3D11_MAP_READ, 0, &m))) return false;
        const auto layout = expected.Height >> mip;
        bool same = true;
        for (UINT y = 0; y < layout; ++y) {
            same &= std::memcmp(static_cast<const char*>(m.pData) + y * m.RowPitch,
                                static_cast<const char*>(initial[mip].pSysMem) + y * initial[mip].SysMemPitch,
                                initial[mip].SysMemPitch) == 0;
        }
        ctx->Unmap(readback.Get(), mip);
        if (!same) {
            std::fprintf(stderr, "FAIL CopyResource content at mip %u\n", mip);
            return false;
        }
    }

    ComPtr<ID3D11DeviceContext> deferred;
    if (FAILED(dev->CreateDeferredContext(0, &deferred))) return false;
    // Unaligned on a BC block. This must be copied from the original pixels.
    ComPtr<ID3D11Texture2D> partial;
    if (FAILED(dev->CreateTexture2D(&staging, nullptr, &partial))) return false;
    const D3D11_BOX box = {1, 1, 0, 3, 3, 1};
    deferred->CopySubresourceRegion(partial.Get(), 0, 0, 0, 0, source, 0, &box);
    ComPtr<ID3D11CommandList> list;
    if (FAILED(deferred->FinishCommandList(FALSE, &list))) return false;
    ctx->ExecuteCommandList(list.Get(), TRUE);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(partial.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const auto pixel = initial[0].SysMemPitch / expected.Width;
    const auto* row = static_cast<const char*>(initial[0].pSysMem) + initial[0].SysMemPitch + pixel;
    const bool same = std::memcmp(m.pData, row, 2 * pixel) == 0 &&
                      std::memcmp(static_cast<const char*>(m.pData) + m.RowPitch,
                                  row + initial[0].SysMemPitch, 2 * pixel) == 0;
    ctx->Unmap(partial.Get(), 0);
    if (!same) fail("deferred CopySubresourceRegion content");
    return same;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool foreign = argc == 4 && std::wcscmp(argv[1], L"--layer-foreign") == 0;
    if (argc != 3 && !foreign) return fail("usage: tessera_test_feedback <original.dxbc> <instrumented.dxbc> | --layer <original.dxbc>");
    const bool changed = std::wcscmp(argv[1], L"--textures-changed") == 0;
    const bool hdr = std::wcscmp(argv[1], L"--textures-hdr") == 0;
    const bool occupied = std::wcscmp(argv[1], L"--layer-occupied") == 0;
    const bool indirect = std::wcscmp(argv[1], L"--layer-indirect") == 0;
    const bool textures = changed || hdr || std::wcscmp(argv[1], L"--textures") == 0;
    const bool layer = textures || occupied || indirect || foreign || std::wcscmp(argv[1], L"--layer") == 0;
    using ForeignDraw = unsigned (WINAPI*)(ID3D11DeviceContext*);
    ForeignDraw foreign_draw = nullptr;
    if (foreign) {
        const HMODULE module = LoadLibraryW(argv[3]);
        if (module) foreign_draw = reinterpret_cast<ForeignDraw>(GetProcAddress(module, "draw_foreign"));
        if (!foreign_draw) return fail("load foreign draw helper");
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("create D3D11 device", hr);
    using IndirectDraw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    const auto draw_indirect = reinterpret_cast<IndirectDraw>((*reinterpret_cast<void***>(ctx.Get()))[40]);
    ComPtr<ID3D11PixelShader> original, instrumented;
    hr = pixel_shader(dev.Get(), argv[layer ? 2 : 1], &original);
    if (FAILED(hr)) return fail("create original shader", hr);
    if (!layer) {
        hr = pixel_shader(dev.Get(), argv[2], &instrumented);
        if (FAILED(hr)) return fail("create instrumented shader", hr);
    }

    constexpr char source[] = "float4 main(uint id : SV_VertexID) : SV_Position {"
                              "float2 p = float2((id << 1) & 2, id & 2);"
                              "return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }";
    ComPtr<ID3DBlob> code;
    hr = D3DCompile(source, sizeof source - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
                    &code, nullptr);
    if (FAILED(hr)) return fail("compile test vertex shader", hr);
    ComPtr<ID3D11VertexShader> vs;
    hr = dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) return fail("create test vertex shader", hr);

    constexpr UINT colors[] = {0xff3b8dde, 0xffbb1133, 0xff2211bb, 0xff467382, 0xff987652};
    std::array<std::array<UINT, 512>, 5> data = {};
    D3D11_SUBRESOURCE_DATA init[5] = {};
    for (UINT mip = 0; mip < 5; ++mip) {
        data[mip].fill(hdr ? 0x3c003c00 : (textures ? 0xffffffff : colors[mip]));
        if (changed && mip == 1) data[mip][0] ^= 1;
        init[mip] = {data[mip].data(), (16u >> mip) * (hdr ? 8u : 4u), 0};
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = 16;
    d.MipLevels = 5;
    d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture, target, image_readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = dev->CreateTexture2D(&d, init, &texture);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) return fail("create mip chain", hr);
    if (textures) {
        if (!verify_copies(dev.Get(), ctx.Get(), texture.Get(), d, init))
            return fail("texture descriptors or copies changed");
        std::puts("PASS texture descriptors and immediate and deferred copies");
    }

    d.Width = d.Height = 4;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = dev->CreateTexture2D(&d, nullptr, &target);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(target.Get(), nullptr, &rtv);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&d, nullptr, &image_readback);
    if (FAILED(hr)) return fail("create output textures", hr);

    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = 128 * sizeof(UINT);
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> feedback, feedback_readback;
    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = 128;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = dev->CreateBuffer(&b, nullptr, &feedback);
    if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(feedback.Get(), &u, &uav);
    b.Usage = D3D11_USAGE_STAGING;
    b.BindFlags = b.MiscFlags = 0;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&b, nullptr, &feedback_readback);
    if (FAILED(hr)) return fail("create feedback resources", hr);

    // Clamp the actual sample to mip 0. Feedback must still report the
    // unclamped mip 2 for a 16x16 texture across a 4x4 viewport.
    D3D11_SAMPLER_DESC s = {};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxAnisotropy = 1;
    ComPtr<ID3D11SamplerState> sampler;
    hr = dev->CreateSamplerState(&s, &sampler);
    if (FAILED(hr)) return fail("create sampler", hr);
    ComPtr<ID3D11Buffer> args;
    if (indirect) {
        const D3D11_DRAW_INSTANCED_INDIRECT_ARGS draw_args = {3, 1, 0, 0};
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof draw_args;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        const D3D11_SUBRESOURCE_DATA initial = {&draw_args, 0, 0};
        if (FAILED(dev->CreateBuffer(&desc, &initial, &args))) return fail("create indirect draw arguments");
    }
    const auto render = [&](ID3D11PixelShader* ps, bool record, std::array<UINT, 16>& pixels) {
        const UINT sentinel[4] = {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
        constexpr float clear[4] = {};
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        auto* out = rtv.Get();
        if (record) {
            ctx->ClearUnorderedAccessViewUint(uav.Get(), sentinel);
            auto* buffer = uav.Get();
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &out, nullptr, 7, 1, &buffer, nullptr);
        } else {
            ctx->OMSetRenderTargets(1, &out, nullptr);
        }
        const D3D11_VIEWPORT viewport = {0, 0, 4, 4, 0, 1};
        ctx->RSSetViewports(1, &viewport);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        auto* view = srv.Get();
        auto* filter = sampler.Get();
        ctx->PSSetShaderResources(3, 1, &view);
        ctx->PSSetSamplers(1, 1, &filter);
        if (foreign_draw) {
            ctx->PSSetShader(nullptr, nullptr, 0);
            if (foreign_draw(ctx.Get()) != 8) return false;
            ctx->PSSetShader(ps, nullptr, 0);
        }
        if (indirect) draw_indirect(ctx.Get(), args.Get(), 0);
        else ctx->Draw(3, 0);
        if (layer) {
            ComPtr<ID3D11PixelShader> actual;
            ctx->PSGetShader(&actual, nullptr, nullptr);
            ComPtr<ID3D11UnorderedAccessView> actual_uav;
            ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 7, 1, &actual_uav);
            if (actual.Get() != ps || actual_uav.Get() != (record ? uav.Get() : nullptr)) return false;
        }
        ctx->ClearState();
        ctx->CopyResource(image_readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(image_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        for (size_t y = 0; y < 4; ++y)
            std::memcpy(pixels.data() + y * 4, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch,
                        4 * sizeof(UINT));
        ctx->Unmap(image_readback.Get(), 0);
        return true;
    };
    std::array<UINT, 16> before = {}, after = {};
    if (layer) {
        ComPtr<IDXGISwapChain1> sc;
        hr = make_swapchain(dev.Get(), &sc);
        if (FAILED(hr)) return fail("create smoke swapchain", hr);
        sc->Present(0, 0);
        if (indirect) sc->Present(0, 0);  // no suitable draw in the first frame of the window
        const UINT expected = textures ? 0xffffffff : colors[0];
        if (!render(original.Get(), occupied, before) ||
            !std::all_of(before.begin(), before.end(), [expected](UINT color) { return color == expected; }))
            return fail("layer feedback draw changed output or bindings");
        sc->Present(0, 0);  // the synchronous RT readback above already finished this test draw
        sc->Present(0, 0);
        if (textures) Sleep(350);  // leave the writer a tick to save any captures
        std::puts("PASS layer draw preserved output, pixel shader and UAV state");
        return 0;
    }
    if (!render(original.Get(), false, before) || !render(instrumented.Get(), true, after) ||
        before != after || !std::all_of(before.begin(), before.end(), [&](UINT color) { return color == colors[0]; }))
        return fail("instrumentation changed the rendered pixels");
    ctx->CopyResource(feedback_readback.Get(), feedback.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(feedback_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return fail("read feedback", hr);
    auto* values = static_cast<const UINT*>(mapped.pData);
    bool ok = true;
    for (UINT i = 0; i < 128; ++i) ok &= values[i] == (i == 3 ? 2u : UINT_MAX);
    std::printf("Feedback t3: mip %u\n", values[3]);
    ctx->Unmap(feedback_readback.Get(), 0);
    if (!ok) return fail("unclamped mip or feedback slot");
    std::puts("PASS unchanged output and correct mip feedback");
    return 0;
}
