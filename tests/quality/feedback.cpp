// SPDX-License-Identifier: GPL-3.0-or-later
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return fail("usage: tessera_test_feedback <original.dxbc> <instrumented.dxbc>");
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("create D3D11 device", hr);
    ComPtr<ID3D11PixelShader> original, instrumented;
    hr = pixel_shader(dev.Get(), argv[1], &original);
    if (FAILED(hr)) return fail("create original shader", hr);
    hr = pixel_shader(dev.Get(), argv[2], &instrumented);
    if (FAILED(hr)) return fail("create instrumented shader", hr);

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
    std::array<std::array<UINT, 256>, 5> data = {};
    D3D11_SUBRESOURCE_DATA init[5] = {};
    for (UINT mip = 0; mip < 5; ++mip) {
        data[mip].fill(colors[mip]);
        init[mip] = {data[mip].data(), (16u >> mip) * sizeof(UINT), 0};
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = 16;
    d.MipLevels = 5;
    d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture, target, image_readback;
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = dev->CreateTexture2D(&d, init, &texture);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) return fail("create mip chain", hr);

    d.Width = d.Height = 4;
    d.MipLevels = 1;
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
        ctx->Draw(3, 0);
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
