// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the helper's real GS paths, including stripped game shaders.
#define wmain tessera_warm_main
#include "../src/warm/main.cpp"
#undef wmain

int wmain() {
    Warm device;
    if (!device.init(true)) return 1;
    constexpr const char* sources[]{
        "struct V { float4 p : SV_Position; }; [maxvertexcount(1)] void main(point V p[1], inout PointStream<V> o) { o.Append(p[0]); }",
        "struct V { float4 p : SV_Position; }; [maxvertexcount(2)] void main(line V p[2], inout LineStream<V> o) { o.Append(p[0]); o.Append(p[1]); }",
        "struct V { float4 p : SV_Position; }; [maxvertexcount(3)] void main(triangle V p[3], inout TriangleStream<V> o) { o.Append(p[0]); o.Append(p[1]); o.Append(p[2]); }",
        "struct V { float4 p : SV_Position; }; [maxvertexcount(2)] void main(lineadj V p[4], inout LineStream<V> o) { o.Append(p[1]); o.Append(p[2]); }",
        "struct V { float4 p : SV_Position; }; [maxvertexcount(3)] void main(triangleadj V p[6], inout TriangleStream<V> o) { o.Append(p[0]); o.Append(p[2]); o.Append(p[4]); }",
        "struct V { float4 value : DATA; }; [maxvertexcount(1)] void main(point V p[1], inout PointStream<V> o) { o.Append(p[0]); }",
    };
    size_t passed = 0;
    for (const auto* source : sources) {
        Microsoft::WRL::ComPtr<ID3DBlob> compiled, errors, stripped;
        if (FAILED(D3DCompile(source, strlen(source), "geometry-test", nullptr, nullptr,
                "main", "gs_5_0", 0, 0, &compiled, &errors))) {
            if (errors) fprintf(stderr, "%s", static_cast<const char*>(errors->GetBufferPointer()));
            return 2;
        }
        if (FAILED(D3DStripShader(compiled->GetBufferPointer(), compiled->GetBufferSize(),
                D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped))) return 3;
        for (auto* blob : {compiled.Get(), stripped.Get()}) {
            for (const uint16_t stage : {uint16_t(TESSERA_STAGE_GS), uint16_t(TESSERA_STAGE_GS_SO)}) {
                const Shader shader{0, stage, TESSERA_SHADER_DXBC,
                    static_cast<const uint8_t*>(blob->GetBufferPointer()), static_cast<uint32_t>(blob->GetBufferSize())};
                const auto* result = device.warm(shader);
                if (strcmp(result, "ok") || device.removed()) {
                    fprintf(stderr, "geometry case %zu stage %u: %s\n", passed, stage, result);
                    Microsoft::WRL::ComPtr<ID3DBlob> assembly;
                    if (SUCCEEDED(D3DDisassemble(shader.code, shader.size, 0, nullptr, &assembly))) {
                        const auto* declaration = strstr(static_cast<const char*>(assembly->GetBufferPointer()), "dcl_inputprimitive");
                        if (declaration) fprintf(stderr, "%.70s\n", declaration);
                    }
                    return 4;
                }
                ++passed;
            }
        }
    }
    printf("geometry warm-up: %zu raster/stream-output and stripped cases passed\n", passed);
    return 0;
}
