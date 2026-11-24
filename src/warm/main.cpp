// SPDX-License-Identifier: GPL-3.0-or-later
// tessera_warm.exe (S5): compiles a game's stored shaders in the driver before
// the game runs, so its disk cache already has them. The app copies this exe
// under the game's exe name (the driver cache is keyed by it) and runs it
// while the game is not running.
//
//   <game exe name> <store base path>     base path without .tshd/.tshi
//
// D3D11: each shader is created, then used in a draw (or dispatch) whose count
// comes from an indirect buffer of zeros: the driver must build the pipeline
// without knowing that nothing will be drawn. Progress goes to <base>.twarm
// as it happens ("try", then "ok"/"fail" per hash), so a crash or device
// removal skips that shader on the next launch instead of hitting it again.
// The cache depends on the driver: a new driver version starts over.
//
// Exit codes: 0 done, 2 device removed (relaunch to continue), 1 error.
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tessera/d3d12_capture_format.h"
#include "tessera/d3d12_raytracing.h"
#include "tessera/d3d12_stream.h"
#include "tessera/shader_store_format.h"

namespace {
struct Shader {
    uint64_t hash;
    uint16_t stage, flags;
    const uint8_t* code;
    uint32_t size;
};

bool read_file(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const bool ok = out->empty() || fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

bool header_ok(const std::vector<uint8_t>& b, const char* magic, uint32_t entry) {
    if (b.size() < sizeof(TesseraShaderHeader)) return false;
    TesseraShaderHeader h;
    memcpy(&h, b.data(), sizeof h);
    return memcmp(h.magic, magic, 8) == 0 && h.version == TESSERA_SHADER_VERSION && h.header_size == sizeof h &&
           h.entry_size == entry;
}

// Whole entries named by the index, as the layer and the app read them.
std::vector<Shader> load_store(const std::vector<uint8_t>& store, const std::vector<uint8_t>& index) {
    std::vector<Shader> out;
    if (!header_ok(store, TESSERA_SHADER_MAGIC, sizeof(TesseraShaderEntry)) ||
        !header_ok(index, TESSERA_SHADER_INDEX_MAGIC, sizeof(TesseraShaderIndexEntry)))
        return out;
    uint64_t start = sizeof(TesseraShaderHeader);
    for (size_t at = sizeof(TesseraShaderHeader); at + sizeof(TesseraShaderIndexEntry) <= index.size();
         at += sizeof(TesseraShaderIndexEntry)) {
        TesseraShaderIndexEntry ie;
        memcpy(&ie, index.data() + at, sizeof ie);
        if (ie.end <= start || ie.end > store.size() || ie.end - start < sizeof(TesseraShaderEntry)) break;
        TesseraShaderEntry e;
        memcpy(&e, store.data() + start, sizeof e);
        if (e.hash != ie.hash || sizeof e + e.size > ie.end - start) break;
        out.push_back({e.hash, e.stage, e.flags, store.data() + start + sizeof e, e.size});
        start = ie.end;
    }
    return out;
}

// --- DXBC signatures ---------------------------------------------------------
struct SigElem {
    std::string name;
    uint32_t index, sv, type, reg;
    uint8_t mask;
};

// Parses ISGN/OSGN (24-byte elements) or ISG1/OSG1 (32-byte, with stream and
// min precision). Empty when the chunk is missing or malformed.
bool signature(const uint8_t* b, size_t n, const char* fourcc, const char* fourcc1, std::vector<SigElem>* out) {
    if (n < 32 || memcmp(b, "DXBC", 4) != 0) return false;
    uint32_t parts = 0;
    memcpy(&parts, b + 28, 4);
    if (parts > 64 || 32 + 4ull * parts > n) return false;
    for (uint32_t i = 0; i < parts; ++i) {
        uint32_t off = 0, size = 0;
        memcpy(&off, b + 32 + 4 * i, 4);
        if (off + 8ull > n) return false;
        memcpy(&size, b + off + 4, 4);
        const bool v1 = memcmp(b + off, fourcc1, 4) == 0;
        if (!v1 && memcmp(b + off, fourcc, 4) != 0) continue;
        const uint8_t* d = b + off + 8;
        if (off + 8ull + size > n || size < 8) return false;
        uint32_t count = 0;
        memcpy(&count, d, 4);
        const size_t stride = v1 ? 32 : 24, base = v1 ? 4 : 0;
        if (8 + count * stride > size) return false;
        for (uint32_t k = 0; k < count; ++k) {
            const uint8_t* e = d + 8 + k * stride + base;
            uint32_t f[5];
            memcpy(f, e, sizeof f);
            if (f[0] >= size) return false;
            const char* name = reinterpret_cast<const char*>(d + f[0]);
            const size_t len = strnlen(name, size - f[0]);
            if (len == size - f[0]) return false;
            out->push_back({std::string(name, len), f[1], f[2], f[3], f[4], e[20]});
        }
        return true;
    }
    return false;
}

// Width of a signature element: its mask spans lowest..highest component
// (TEXCOORD1 packed in .zw is a float2, not a float4).
int components(uint8_t mask) {
    mask &= 0xF;
    if (!mask) return 0;
    int lo = 0, hi = 3;
    while (!(mask >> lo & 1)) ++lo;
    while (!(mask >> hi & 1)) --hi;
    return hi - lo + 1;
}

// D3D_REGISTER_COMPONENT_TYPE: 1 uint32, 2 sint32, 3 float32.
const char* hlsl_type(uint32_t type) {
    return type == 1 ? "uint" : type == 2 ? "int" : "float";
}

DXGI_FORMAT format(uint32_t type, int n) {
    static const DXGI_FORMAT f[3][4] = {
        {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT},
        {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT},
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
         DXGI_FORMAT_R32G32B32A32_FLOAT}};
    const int t = type == 1 ? 0 : type == 2 ? 1 : 2;
    return f[t][n - 1];
}

// D3D_NAME values the rasterizer makes up: not written by a vertex shader.
bool system_generated(uint32_t sv) {
    return sv == 7 /*PRIMITIVE_ID*/ || sv == 9 /*IS_FRONT_FACE*/ || sv == 10 /*SAMPLE_INDEX*/ ||
           sv == 6 /*VERTEX_ID*/ || sv == 8 /*INSTANCE_ID*/ || sv >= 64 /*targets, coverage, depth*/;
}

// --- Warm-up log -------------------------------------------------------------
// Words: ok (created and used), made (created; no valid draw for it), fail,
// skip (not creatable in D3D11: DXIL). "try" precedes each risky call.
struct Log {
    FILE* f = nullptr;
    std::unordered_set<uint64_t> done;  // settled with this driver
    uint32_t ok = 0, failed = 0, skipped = 0;

    void line(const char* what, uint64_t h) {
        fprintf(f, "%s %016llx\n", what, static_cast<unsigned long long>(h));
        fflush(f);  // reaches the OS before the risky call: survives a crash of this process
    }
};

using Outcomes = std::unordered_map<uint64_t, char>;

Outcomes outcomes(const std::vector<uint8_t>& old, uint64_t* pending) {
    const std::string text(old.begin(), old.end());
    Outcomes result;
    *pending = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        unsigned long long hash = 0;
        if (sscanf_s(line.c_str(), "try %llx", &hash) == 1) {
            *pending = hash;
            continue;
        }
        if (sscanf_s(line.c_str(), "ok %llx", &hash) == 1 || sscanf_s(line.c_str(), "made %llx", &hash) == 1 ||
            sscanf_s(line.c_str(), "fail %llx", &hash) == 1 || sscanf_s(line.c_str(), "skip %llx", &hash) == 1) {
            result[hash] = line[0];
            if (*pending == hash) *pending = 0;
        }
    }
    return result;
}

bool read_matching_log(const std::wstring& path, const char* header, std::vector<uint8_t>& bytes) {
    const size_t n = strlen(header);
    return read_file(path, &bytes) && bytes.size() > n && memcmp(bytes.data(), header, n) == 0 && bytes[n] == '\n';
}

// Replays the latest result in each log. A peer can supply a successful result
// for a duplicate, but a refusal in one store does not block the other store.
bool open_log(const std::wstring& path, const wchar_t* peer, unsigned long long driver, bool retry,
              const std::unordered_set<uint64_t>& retry_created, Log* log) {
    char head[64];
    snprintf(head, sizeof head, "tessera-warm 1 driver %016llx", driver);
    std::vector<uint8_t> old;
    if (read_matching_log(path, head, old)) {
        uint64_t pending;
        for (const auto& [hash, status] : outcomes(old, &pending)) {
            if ((status == 'f' && retry) || (status == 'm' && retry_created.count(hash))) continue;
            log->done.insert(hash);
            ++*(status == 'f' ? &log->failed : status == 's' ? &log->skipped : &log->ok);
        }
        log->f = _wfopen(path.c_str(), L"ab");
        // A line cut short (power loss) must not swallow the next one.
        if (log->f && !old.empty() && old.back() != '\n') fputc('\n', log->f);
        if (log->f && pending) {  // the last run died on this one
            log->line("fail", pending);
            if (log->done.insert(pending).second) ++log->failed;
        }
    } else {
        log->f = _wfopen(path.c_str(), L"wb");
        if (log->f) fprintf(log->f, "%s\n", head);
    }
    if (peer && *peer && read_matching_log(std::wstring(peer) + L".twarm", head, old)) {
        uint64_t pending;
        for (const auto& [hash, status] : outcomes(old, &pending))
            if (hash != pending && (status == 'o' || (status == 'm' && !retry_created.count(hash))))
                log->done.insert(hash);
    }
    return log->f != nullptr;
}

// --- D3D11 -------------------------------------------------------------------
struct Warm {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Buffer* args = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Buffer> stream_target;
    std::unordered_map<std::string, ID3D11VertexShader*> feeders;  // PS input signature -> generated VS
    struct VertexSource {
        const Shader* shader;
        std::vector<SigElem> outputs;
    };
    std::vector<VertexSource> vertex_sources;

    void index_vertex_sources(const std::vector<Shader>& shaders) {
        for (const Shader& shader : shaders) {
            if (shader.stage != TESSERA_STAGE_VS || (shader.flags & TESSERA_SHADER_DXIL)) continue;
            VertexSource source{&shader, {}};
            if (!signature(shader.code, shader.size, "OSGN", "OSG1", &source.outputs)) continue;
            for (const SigElem& output : source.outputs) {
                if (output.sv != 1) continue;
                vertex_sources.push_back(std::move(source));
                break;
            }
        }
    }

    bool init(bool warp = false) {
        // The high-performance GPU: on hybrid laptops adapter 0 is the iGPU,
        // and the per-app GPU choice does not follow the copied exe.
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory6* f6 = nullptr;
        if (!warp && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f6)))) {
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            f6->Release();
        }
        const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP
            : adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(adapter, type, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (adapter) adapter->Release();
        if (FAILED(hr)) return false;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32;
        bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        const uint32_t zeros[8] = {};
        D3D11_SUBRESOURCE_DATA init = {zeros, 0, 0};
        bd.BindFlags = 0;
        if (FAILED(dev->CreateBuffer(&bd, &init, &args))) return false;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* rt = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) return false;
        const HRESULT r = dev->CreateRenderTargetView(rt, nullptr, &rtv);
        rt->Release();
        if (FAILED(r)) return false;
        D3D11_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        return true;
    }

    unsigned long long driver() const {
        unsigned long long v = 0;
        IDXGIDevice* d = nullptr;
        IDXGIAdapter* a = nullptr;
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d))) && SUCCEEDED(d->GetAdapter(&a)) &&
            SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            v = static_cast<unsigned long long>(umd.QuadPart);
        if (a) a->Release();
        if (d) d->Release();
        return v;
    }

    void draw() {
        ctx->DrawInstancedIndirect(args, 0);
        ctx->Flush();
    }

    // A VS writing what this PS reads, compiled once per signature. Elements
    // are declared in the PS's order and widths so fxc packs them the same;
    // the result is checked register by register, and a feeder that does not
    // link (packing split by interpolation modes, system values mid-way) is
    // not used: null, and the PS is only created.
    ID3D11VertexShader* feeder(const std::vector<SigElem>& in) {
        std::string body = "struct O {\n", sets;
        bool has_pos = false;
        for (size_t i = 0; i < in.size(); ++i) {
            const SigElem& e = in[i];
            if (system_generated(e.sv)) continue;
            const int n = components(e.mask);
            if (!n) continue;
            const bool pos = e.sv == 1;  // SV_Position is always float4
            has_pos |= pos;
            char line[160];
            snprintf(line, sizeof line, "  %s%s v%zu : %s%u;\n", pos ? "float" : hlsl_type(e.type),
                     pos ? "4" : n > 1 ? std::to_string(n).c_str() : "", i, e.name.c_str(), e.index);
            body += line;
            snprintf(line, sizeof line, "  o.v%zu = 0;\n", i);
            sets += line;
        }
        // The rasterizer needs a position; last, so it moves no other register.
        if (!has_pos) {
            body += "  float4 pos_ : SV_Position;\n";
            sets += "  o.pos_ = 0;\n";
        }
        const std::string src = body + "};\nO main() {\n  O o;\n" + sets + "  return o;\n}\n";
        auto it = feeders.find(src);
        if (it != feeders.end()) return it->second;
        ID3DBlob* blob = nullptr;
        ID3D11VertexShader* vs = nullptr;
        if (SUCCEEDED(D3DCompile(src.data(), src.size(), "feeder", nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob,
                                 nullptr))) {
            std::vector<SigElem> out;
            const uint8_t* b = static_cast<const uint8_t*>(blob->GetBufferPointer());
            if (signature(b, blob->GetBufferSize(), "OSGN", "OSG1", &out) && links(out, in))
                dev->CreateVertexShader(b, blob->GetBufferSize(), nullptr, &vs);
            blob->Release();
        }
        feeders.emplace(src, vs);
        return vs;
    }

    // Every PS input the VS must write sits in the same register, at the
    // same components, as a VS output with its semantic.
    static bool links(const std::vector<SigElem>& out, const std::vector<SigElem>& in) {
        for (const SigElem& e : in) {
            if (system_generated(e.sv) || !(e.mask & 0xF)) continue;
            bool ok = false;
            for (const SigElem& o : out)
                ok = ok || (o.index == e.index && o.reg == e.reg && o.type == e.type &&
                            (o.mask & e.mask & 0xF) == (e.mask & 0xF) &&
                            _stricmp(o.name.c_str(), e.name.c_str()) == 0);
            if (!ok) return false;
        }
        return true;
    }

    // Input layout for every non-system input of a VS, all in slot 0.
    ID3D11InputLayout* layout(const Shader& s, const std::vector<SigElem>& in) {
        D3D11_INPUT_ELEMENT_DESC e[32];
        UINT n = 0;
        for (const SigElem& x : in) {
            if (x.sv != 0 || n == 32 || !components(x.mask)) continue;
            e[n++] = {x.name.c_str(), x.index, format(x.type, components(x.mask)), 0, D3D11_APPEND_ALIGNED_ELEMENT,
                      D3D11_INPUT_PER_VERTEX_DATA, 0};
        }
        ID3D11InputLayout* il = nullptr;
        if (n) dev->CreateInputLayout(e, n, s.code, s.size, &il);
        return il;
    }

    // The generated feeder cannot reproduce every register packing. Reuse a
    // compatible game vertex shader when the store already contains one.
    bool draw_pixel_with_game_vertex(const std::vector<SigElem>& inputs, ID3D11PixelShader* ps) {
        for (const VertexSource& source : vertex_sources) {
            if (!links(source.outputs, inputs)) continue;
            const Shader& shader = *source.shader;
            std::vector<SigElem> vertex_inputs;
            if (!signature(shader.code, shader.size, "ISGN", "ISG1", &vertex_inputs)) continue;
            ID3D11InputLayout* il = layout(shader, vertex_inputs);
            if (!il) {
                bool needs_layout = false;
                for (const SigElem& input : vertex_inputs)
                    needs_layout = needs_layout || (input.sv == 0 && components(input.mask) != 0);
                if (needs_layout) continue;
            }
            ID3D11VertexShader* vs = nullptr;
            const HRESULT hr = dev->CreateVertexShader(shader.code, shader.size, nullptr, &vs);
            if (SUCCEEDED(hr)) {
                ctx->IASetInputLayout(il);
                ctx->VSSetShader(vs, nullptr, 0);
                ctx->PSSetShader(ps, nullptr, 0);
                draw();
                vs->Release();
            }
            if (il) il->Release();
            if (SUCCEEDED(hr)) return true;
        }
        return false;
    }

    bool exercise_geometry(const Shader& shader, ID3D11GeometryShader* raster) {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11ShaderReflection> reflection;
        D3D11_SHADER_DESC desc{};
        if (FAILED(D3DReflect(shader.code, shader.size, IID_PPV_ARGS(&reflection)))
            || FAILED(reflection->GetDesc(&desc))) return false;
        auto primitive = desc.InputPrimitive;
        // Stripped game bytecode can retain signatures and instructions but
        // omit STAT, leaving reflection's primitive undefined.
        if (primitive == D3D_PRIMITIVE_UNDEFINED) {
            ComPtr<ID3DBlob> assembly;
            if (FAILED(D3DDisassemble(shader.code, shader.size, 0, nullptr, &assembly))) return false;
            const std::string_view text(static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize());
            constexpr std::string_view declaration = "\ndcl_inputprimitive ";
            const auto at = text.find(declaration);
            if (at == std::string_view::npos) return false;
            const auto begin = at + declaration.size();
            const auto token = text.substr(begin, text.find_first_of(" \r\n\0", begin) - begin);
            if (token == "point") primitive = D3D_PRIMITIVE_POINT;
            else if (token == "line") primitive = D3D_PRIMITIVE_LINE;
            else if (token == "triangle") primitive = D3D_PRIMITIVE_TRIANGLE;
            else if (token == "lineadj") primitive = D3D_PRIMITIVE_LINE_ADJ;
            else if (token == "triangleadj") primitive = D3D_PRIMITIVE_TRIANGLE_ADJ;
        }
        D3D11_PRIMITIVE_TOPOLOGY topology;
        switch (primitive) {
            case D3D_PRIMITIVE_POINT: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
            case D3D_PRIMITIVE_LINE: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
            case D3D_PRIMITIVE_TRIANGLE: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
            case D3D_PRIMITIVE_LINE_ADJ: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; break;
            case D3D_PRIMITIVE_TRIANGLE_ADJ: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ; break;
            default: return false;
        }
        std::vector<SigElem> inputs;
        if (!signature(shader.code, shader.size, "ISGN", "ISG1", &inputs)) return false;
        auto* vs = feeder(inputs);
        if (!vs) return false;

        std::vector<D3D11_SO_DECLARATION_ENTRY> output;
        UINT stride = 0;
        bool position = false;
        for (UINT i = 0; i < desc.OutputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC element{};
            if (FAILED(reflection->GetOutputParameterDesc(i, &element)) || element.Stream != 0)
                return false;
            position |= element.SystemValueType == D3D_NAME_POSITION;
            const int count = components(element.Mask);
            if (!count) continue;
            // A stream-output declaration addresses components inside the
            // semantic, even if the compiler packed it into a wider register.
            output.push_back({0, element.SemanticName, element.SemanticIndex, 0, static_cast<BYTE>(count), 0});
            stride += static_cast<UINT>(count) * sizeof(uint32_t);
        }
        ComPtr<ID3D11GeometryShader> stream;
        const bool use_stream = !position || shader.stage == TESSERA_STAGE_GS_SO;
        if (use_stream) {
            if (!stride || !output.size()) return false;
            if (FAILED(dev->CreateGeometryShaderWithStreamOutput(shader.code, shader.size, output.data(),
                static_cast<UINT>(output.size()), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream)))
                return false;
            if (!stream_target) {
                D3D11_BUFFER_DESC buffer{};
                buffer.ByteWidth = 4096;
                buffer.BindFlags = D3D11_BIND_STREAM_OUTPUT;
                if (FAILED(dev->CreateBuffer(&buffer, nullptr, &stream_target))) return false;
            }
            auto* target = stream_target.Get();
            const UINT offset = 0;
            ctx->SOSetTargets(1, &target, &offset);
        } else if (!raster) {
            return false;
        }
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->GSSetShader(use_stream ? stream.Get() : raster, nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);
        draw();
        ctx->GSSetShader(nullptr, nullptr, 0);
        if (use_stream) ctx->SOSetTargets(0, nullptr, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        return true;
    }

    // The log word for one shader: "ok" created and used in a draw/dispatch,
    // "made" created only (no stage to draw it with), "fail" not created,
    // "skip" not a D3D11 shader.
    const char* warm(const Shader& s) {
        if (s.flags & TESSERA_SHADER_DXIL) return "skip";
        HRESULT hr = E_FAIL;
        bool used = false;
        switch (s.stage) {
            case TESSERA_STAGE_VS: {
                ID3D11VertexShader* vs = nullptr;
                hr = dev->CreateVertexShader(s.code, s.size, nullptr, &vs);
                if (FAILED(hr)) break;
                std::vector<SigElem> in, out;
                signature(s.code, s.size, "ISGN", "ISG1", &in);
                ID3D11InputLayout* il = layout(s, in);
                // Without SV_Position the VS only feeds later stages: created, not drawn.
                bool has_pos = false;
                if (signature(s.code, s.size, "OSGN", "OSG1", &out))
                    for (const SigElem& e : out) has_pos = has_pos || e.sv == 1;
                if (has_pos) {
                    ctx->IASetInputLayout(il);
                    ctx->VSSetShader(vs, nullptr, 0);
                    ctx->PSSetShader(nullptr, nullptr, 0);
                    draw();
                    used = true;
                }
                if (il) il->Release();
                vs->Release();
                break;
            }
            case TESSERA_STAGE_PS: {
                ID3D11PixelShader* ps = nullptr;
                hr = dev->CreatePixelShader(s.code, s.size, nullptr, &ps);
                if (FAILED(hr)) break;
                std::vector<SigElem> in;
                if (signature(s.code, s.size, "ISGN", "ISG1", &in)) {
                    if (ID3D11VertexShader* vs = feeder(in)) {
                        ctx->IASetInputLayout(nullptr);
                        ctx->VSSetShader(vs, nullptr, 0);
                        ctx->PSSetShader(ps, nullptr, 0);
                        draw();
                        used = true;
                    } else {
                        used = draw_pixel_with_game_vertex(in, ps);
                    }
                }
                ps->Release();
                break;
            }
            case TESSERA_STAGE_CS: {
                ID3D11ComputeShader* cs = nullptr;
                hr = dev->CreateComputeShader(s.code, s.size, nullptr, &cs);
                if (FAILED(hr)) break;
                ctx->CSSetShader(cs, nullptr, 0);
                ctx->DispatchIndirect(args, 0);
                ctx->Flush();
                used = true;
                cs->Release();
                break;
            }
            case TESSERA_STAGE_GS:
            case TESSERA_STAGE_GS_SO: {
                ID3D11GeometryShader* gs = nullptr;
                hr = dev->CreateGeometryShader(s.code, s.size, nullptr, &gs);
                used = exercise_geometry(s, gs);
                if (used) hr = S_OK;
                if (gs) gs->Release();
                break;
            }
            case TESSERA_STAGE_HS: {
                ID3D11HullShader* hs = nullptr;
                hr = dev->CreateHullShader(s.code, s.size, nullptr, &hs);
                if (hs) hs->Release();
                break;
            }
            case TESSERA_STAGE_DS: {
                ID3D11DomainShader* ds = nullptr;
                hr = dev->CreateDomainShader(s.code, s.size, nullptr, &ds);
                if (ds) ds->Release();
                break;
            }
            default:
                return "skip";
        }
        if (FAILED(hr))
            fprintf(stderr, "create failed %016llx stage %u hr %08lx\n", static_cast<unsigned long long>(s.hash),
                    static_cast<unsigned>(s.stage), static_cast<unsigned long>(hr));
        return FAILED(hr) ? "fail" : used ? "ok" : "made";
    }

    bool removed() const { return dev->GetDeviceRemovedReason() != S_OK; }
    void finish() const { ctx->Flush(); }
};

struct WarmD3d12 {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> dev;

    bool init(bool warp) {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        if (warp) {
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        } else if (FAILED(factory->EnumAdapterByGpuPreference(
                       0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            return false;
        }
        return SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    }

    unsigned long long driver() const {
        LARGE_INTEGER umd{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        return static_cast<unsigned long long>(umd.QuadPart);
    }

    const char* warm(const Shader& shader) const {
        if (shader.stage != TESSERA_STAGE_CS) return "skip";
        // A null pRootSignature uses the exact signature embedded in the
        // shader. The scan only adds compute shaders that carry one.
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.CS = {shader.code, shader.size};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) fprintf(stderr, "D3D12 compute pipeline %016llx: 0x%08lx\n",
                               static_cast<unsigned long long>(shader.hash), static_cast<unsigned long>(hr));
        return SUCCEEDED(hr) ? "ok" : "fail";
    }

    bool removed() const { return dev->GetDeviceRemovedReason() != S_OK; }
    void finish() const {}
};

template<typename T>
bool take_capture(std::span<const uint8_t>& data, T& object) {
    if (data.size() < sizeof(T)) return false;
    std::memcpy(&object, data.data(), sizeof(T));
    data = data.subspan(sizeof(T));
    return true;
}

HRESULT replay_graphics(ID3D12Device* device, ID3D12RootSignature* root,
                        const tessera::d3d12_capture::Record& entry, std::span<const uint8_t> data) {
    using namespace tessera::d3d12_capture;
    GraphicsState state{};
    if (!take_capture(data, state) || state.input_elements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
        || state.render_targets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    D3D12_SHADER_BYTECODE* shaders[] = {&desc.VS, &desc.PS, &desc.DS, &desc.HS, &desc.GS};
    for (size_t i = 0; i < graphics_stages; ++i) {
        const auto size = state.code_sizes[i];
        if (size > data.size() || size > max_shader_bytes) return E_INVALIDARG;
        if (size) *shaders[i] = {data.data(), size};
        data = data.subspan(size);
    }
    D3D12_INPUT_ELEMENT_DESC input[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT]{};
    for (uint32_t i = 0; i < state.input_elements; ++i) {
        InputElement element{};
        if (!take_capture(data, element) || element.name_size <= 1 || element.name_size > data.size()
            || std::memchr(data.data(), 0, element.name_size) != data.data() + element.name_size - 1)
            return E_INVALIDARG;
        input[i] = {reinterpret_cast<const char*>(data.data()), element.semantic_index, element.format,
            element.slot, element.offset, element.classification, element.step_rate};
        data = data.subspan(element.name_size);
    }
    if (!data.empty()) return E_INVALIDARG;
    desc.pRootSignature = root;
    desc.BlendState = state.blend;
    desc.SampleMask = state.sample_mask;
    desc.RasterizerState = state.rasterizer;
    desc.DepthStencilState = state.depth_stencil;
    desc.InputLayout = {state.input_elements ? input : nullptr, state.input_elements};
    desc.IBStripCutValue = state.strip_cut;
    desc.PrimitiveTopologyType = state.topology;
    desc.NumRenderTargets = state.render_targets;
    std::memcpy(desc.RTVFormats, state.formats, sizeof(state.formats));
    desc.DSVFormat = state.depth_format;
    desc.SampleDesc = state.samples;
    desc.NodeMask = entry.node_mask;
    desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(entry.flags);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
}

HRESULT replay_stream(ID3D12Device* device, ID3D12RootSignature* root, std::vector<uint8_t>& payload) {
    using namespace tessera::d3d12_capture;
    Microsoft::WRL::ComPtr<ID3D12Device2> device2;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2)))) return E_NOINTERFACE;
    if (!relocate_stream(payload, false, root)) return E_INVALIDARG;
    StreamHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{header.size, payload.data() + sizeof(header)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    return device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline));
}

HRESULT create_ray_state(ID3D12Device* device, uint32_t kind, std::span<const uint8_t> data,
                         unsigned depth, size_t& budget, Microsoft::WRL::ComPtr<ID3D12StateObject>& state) {
    using namespace tessera::d3d12_capture;
    using Microsoft::WRL::ComPtr;
    if (depth >= 16 || data.size() > budget) return E_INVALIDARG;
    budget -= data.size();
    ComPtr<ID3D12StateObject> parent;
    if (kind == raytracing_add_kind) {
        RayAddition header{};
        if (!take_capture(data, header) || header.parent_size > data.size()
            || header.addition_size != data.size() - header.parent_size) return E_INVALIDARG;
        auto base = data.first(header.parent_size);
        uint32_t parent_kind = 0;
        if (!take_capture(base, parent_kind)) return E_INVALIDARG;
        const HRESULT hr = create_ray_state(device, parent_kind, base, depth + 1, budget, parent);
        if (FAILED(hr)) return hr;
        data = data.subspan(header.parent_size);
    } else if (kind != raytracing_kind) {
        return E_INVALIDARG;
    }
    std::vector<uint8_t> payload(data.begin(), data.end());
    struct RootSlot {
        ComPtr<ID3D12RootSignature> signature;
        D3D12_GLOBAL_ROOT_SIGNATURE global{};
        D3D12_LOCAL_ROOT_SIGNATURE local{};
    };
    struct DependencySlot {
        ComPtr<ID3D12StateObject> state;
        D3D12_EXISTING_COLLECTION_DESC desc{};
    };
    std::deque<RootSlot> roots;
    std::deque<DependencySlot> dependencies;
    HRESULT dependent_result = S_OK;
    const auto create_root = [&](D3D12_STATE_SUBOBJECT& sub, const RayRoot& root) {
        auto& slot = roots.emplace_back();
        if (root.size) {
            dependent_result = device->CreateRootSignature(root.node_mask, root.bytes, root.size, IID_PPV_ARGS(&slot.signature));
            if (FAILED(dependent_result)) return false;
        }
        if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE) {
            slot.global.pGlobalRootSignature = slot.signature.Get();
            sub.pDesc = &slot.global;
        } else {
            slot.local.pLocalRootSignature = slot.signature.Get();
            sub.pDesc = &slot.local;
        }
        return true;
    };
    const auto create_dependency = [&](D3D12_STATE_SUBOBJECT& sub, const RayDependency& dependency) {
        std::span<const uint8_t> bytes(dependency.bytes, dependency.size);
        uint32_t dependency_kind = 0;
        if (!take_capture(bytes, dependency_kind)) return false;
        auto& slot = dependencies.emplace_back();
        dependent_result = create_ray_state(device, dependency_kind, bytes, depth + 1, budget, slot.state);
        if (FAILED(dependent_result)) return false;
        slot.desc = {slot.state.Get(), dependency.export_count, dependency.exports};
        sub.pDesc = &slot.desc;
        return true;
    };
    D3D12_STATE_OBJECT_DESC desc{};
    if (!relocate_raytracing<false>(payload, {}, desc, create_root, create_dependency))
        return FAILED(dependent_result) ? dependent_result : E_INVALIDARG;
    if (parent) {
        ComPtr<ID3D12Device7> device7;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device7)))) return E_NOINTERFACE;
        return device7->AddToStateObject(&desc, parent.Get(), IID_PPV_ARGS(&state));
    }
    ComPtr<ID3D12Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return E_NOINTERFACE;
    return device5->CreateStateObject(&desc, IID_PPV_ARGS(&state));
}

int replay_d3d12(const wchar_t* path, bool warp) {
    using namespace tessera::d3d12_capture;
    using Microsoft::WRL::ComPtr;
    std::unique_ptr<FILE, decltype(&fclose)> file(_wfopen(path, L"rb"), fclose);
    if (!file) {
        fprintf(stderr, "cannot open the D3D12 capture\n");
        return 1;
    }
    Header header{};
    if (fread(&header, 1, sizeof(header), file.get()) != sizeof(header)
        || std::memcmp(header.magic, magic, sizeof(magic)) != 0 || header.version != version || header.reserved) {
        fprintf(stderr, "invalid D3D12 capture header\n");
        return 1;
    }
    WarmD3d12 device;
    if (!device.init(warp)) {
        fprintf(stderr, "no D3D12 device\n");
        return 1;
    }
    std::vector<uint8_t> payload, root;
    uint64_t total_bytes = sizeof(header);
    size_t prepared[5]{}, failed = 0;
    const auto completed = [&] { return prepared[0] + prepared[1] + prepared[2] + prepared[3] + prepared[4] + failed; };
    while (true) {
        Record record{};
        const size_t received = fread(&record, 1, sizeof(record), file.get());
        if (received == 0 && feof(file.get())) break;
        const uint64_t size = sizeof(record) + uint64_t{record.payload_size} + record.root_size;
        const uint32_t max_payload = record.kind == compute_kind ? max_shader_bytes : max_graphics_bytes;
        if (received != sizeof(record) || record.kind > raytracing_add_kind || !record.payload_size || record.payload_size > max_payload
            || record.root_size > max_root_bytes || total_bytes > max_capture_bytes - size) {
            fprintf(stderr, "invalid D3D12 capture record after %zu pipelines\n", completed());
            return 1;
        }
        total_bytes += size;
        payload.resize(record.payload_size);
        root.resize(record.root_size);
        if (fread(payload.data(), 1, payload.size(), file.get()) != payload.size()
            || (!root.empty() && fread(root.data(), 1, root.size(), file.get()) != root.size())) {
            fprintf(stderr, "truncated D3D12 capture record after %zu pipelines\n", completed());
            return 1;
        }
        ComPtr<ID3D12RootSignature> signature;
        HRESULT hr = S_OK;
        if (!root.empty()) {
            hr = device.dev->CreateRootSignature(record.root_node_mask, root.data(), root.size(), IID_PPV_ARGS(&signature));
        }
        if (SUCCEEDED(hr) && record.kind == graphics_kind) {
            hr = replay_graphics(device.dev.Get(), signature.Get(), record, payload);
        } else if (SUCCEEDED(hr) && record.kind == stream_kind) {
            hr = replay_stream(device.dev.Get(), signature.Get(), payload);
        } else if (SUCCEEDED(hr) && (record.kind == raytracing_kind || record.kind == raytracing_add_kind)) {
            size_t budget = 128 << 20;
            Microsoft::WRL::ComPtr<ID3D12StateObject> state;
            hr = create_ray_state(device.dev.Get(), record.kind, payload, 0, budget, state);
        } else if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = signature.Get();
            desc.CS = {payload.data(), payload.size()};
            desc.NodeMask = record.node_mask;
            desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(record.flags);
            ComPtr<ID3D12PipelineState> pipeline;
            hr = device.dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        }
        if (SUCCEEDED(hr)) {
            ++prepared[record.kind];
        } else {
            ++failed;
            fprintf(stderr, "pipeline %zu: 0x%08lx\n", completed(), static_cast<unsigned long>(hr));
            if (device.removed()) break;
        }
    }
    printf("D3D12 pipelines prepared: %zu compute, %zu graphics, %zu streams, %zu ray tracing, %zu ray tracing additions, %zu failed%s\n",
           prepared[0], prepared[1], prepared[2], prepared[3], prepared[4], failed, warp ? " (WARP)" : "");
    return failed != 0 || completed() == 0 ? 1 : 0;
}

template<class Device>
int run(Device& w, const std::wstring& base, const std::vector<Shader>& shaders, const wchar_t* peer,
        bool retry, bool progress, const wchar_t* log_extension) {
    Log log;
    std::unordered_set<uint64_t> geometry;
    if (retry) {
        for (const auto& shader : shaders)
            if (shader.stage == TESSERA_STAGE_GS || shader.stage == TESSERA_STAGE_GS_SO)
                geometry.insert(shader.hash);
    }
    if (!open_log(base + log_extension, peer, w.driver(), retry, geometry, &log)) {
        fprintf(stderr, "cannot write the warm-up log\n");
        return 1;
    }
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG last_progress = t0;
    size_t processed = 0;
    auto report_progress = [&] {
        printf("tessera-progress %zu %zu\n", processed, shaders.size());
        fflush(stdout);
        last_progress = GetTickCount64();
    };
    if (progress) {
        for (const Shader& s : shaders) processed += log.done.count(s.hash);
        report_progress();
    }
    uint32_t now_ok = 0, now_failed = 0, now_skipped = 0;
    for (const Shader& s : shaders) {
        if (log.done.count(s.hash)) continue;
        log.line("try", s.hash);
        const char* result = w.warm(s);
        if (w.removed()) {
            log.line("fail", s.hash);
            fprintf(stderr, "device removed at %016llx; relaunch to continue\n",
                    static_cast<unsigned long long>(s.hash));
            fclose(log.f);
            return 2;
        }
        log.line(result, s.hash);
        log.done.insert(s.hash);
        if (progress) {
            ++processed;
            if (GetTickCount64() - last_progress >= 200) report_progress();
        }
        ++*(result[0] == 'f' ? &now_failed : result[0] == 's' ? &now_skipped : &now_ok);
    }
    w.finish();
    if (progress) report_progress();
    fclose(log.f);
    printf("stored %zu, compiled now %u, failed now %u, skipped now %u, total ok %u failed %u, %llu ms\n",
           shaders.size(), now_ok, now_failed, now_skipped, log.ok + now_ok, log.failed + now_failed,
           GetTickCount64() - t0);
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && wcscmp(argv[1], L"--replay-d3d12") == 0) {
        const bool warp = argc == 4 && wcscmp(argv[3], L"--warp") == 0;
        if (argc != 3 && !warp) return 1;
        return replay_d3d12(argv[2], warp);
    }
    if (argc < 2) {
        fwprintf(stderr, L"usage: %ls <store base path>\n", argv[0]);
        return 1;
    }
    const std::wstring base = argv[1];
    std::vector<uint8_t> store, index;
    if (!read_file(base + L".tshd", &store) || !read_file(base + L".tshi", &index)) {
        fwprintf(stderr, L"no shader store at %ls\n", base.c_str());
        return 1;
    }
    const std::vector<Shader> shaders = load_store(store, index);
    const wchar_t* peer = argc > 2 ? argv[2] : nullptr;
    const bool retry = argc > 3 && wcscmp(argv[3], L"--retry-failed") == 0;
    const bool progress = argc > 4 && wcscmp(argv[4], L"--progress") == 0;
    if (argc > 5 && wcscmp(argv[5], L"--d3d12") == 0) {
        const bool warp = argc > 6 && wcscmp(argv[6], L"--warp") == 0;
        WarmD3d12 device;
        if (!device.init(warp)) {
            fprintf(stderr, "no D3D12 device\n");
            return 1;
        }
        return run(device, base, shaders, nullptr, retry, progress, warp ? L".warp.twarm" : L".twarm");
    }
    Warm device;
    const bool warp = argc > 5 && wcscmp(argv[5], L"--d3d11-warp") == 0;
    if (!device.init(warp)) {
        fprintf(stderr, "no D3D11 device\n");
        return 1;
    }
    device.index_vertex_sources(shaders);
    return run(device, base, shaders, warp ? nullptr : peer, retry, progress, warp ? L".warp.twarm" : L".twarm");
}
