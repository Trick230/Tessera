// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "exports_d3d12_gen.h"
#include "tessera/d3d12_capture_format.h"
#include "tessera/d3d12_raytracing.h"
#include "tessera/d3d12_stream.h"
#include "vtable.h"

extern "C" void* tessera_real[];

namespace {
using namespace tessera::d3d12_capture;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr GUID root_data = {0x18dd39b3, 0xe76e, 0x482f, {0x83, 0xbc, 0x6c, 0x24, 0x24, 0x1f, 0xff, 0x82}};
constexpr GUID state_data = {0x0902ebfa, 0xeed7, 0x40b9, {0xbd, 0x07, 0x39, 0x1c, 0x24, 0x5a, 0x47, 0x70}};
INIT_ONCE capture_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE runtime_once = INIT_ONCE_STATIC_INIT;
SRWLOCK capture_lock = SRWLOCK_INIT;
HANDLE capture = INVALID_HANDLE_VALUE;
uint64_t recorded_bytes = 0;
HMODULE runtime = nullptr;

BOOL CALLBACK load_runtime(PINIT_ONCE, void*, void**) {
    wchar_t path[MAX_PATH]{};
    const UINT size = GetSystemDirectoryW(path, MAX_PATH);
    if (size && size < MAX_PATH - 11) {
        wcscat_s(path, L"\\d3d12.dll");
        runtime = LoadLibraryW(path);
    }
    return TRUE;
}

bool write_bytes(HANDLE file, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) && written == size;
}

BOOL CALLBACK open_capture(PINIT_ONCE, void*, void**) {
    wchar_t path[32768];
    const DWORD length = GetEnvironmentVariableW(L"TESSERA_D3D12_CAPTURE", path, _countof(path));
    if (!length || length >= _countof(path)) return TRUE;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return TRUE;
    Header header{};
    std::memcpy(header.magic, magic, sizeof(magic));
    header.version = version;
    WIN32_FILE_ATTRIBUTE_DATA exe{};
    const DWORD exe_length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (exe_length && exe_length < _countof(path) && GetFileAttributesExW(path, GetFileExInfoStandard, &exe)) {
        header.exe_size = (uint64_t(exe.nFileSizeHigh) << 32) | exe.nFileSizeLow;
        header.exe_write_time = (uint64_t(exe.ftLastWriteTime.dwHighDateTime) << 32) | exe.ftLastWriteTime.dwLowDateTime;
    }
    if (write_bytes(file, &header, sizeof(header))) {
        capture = file;
        recorded_bytes = sizeof(header);
    } else {
        CloseHandle(file);
    }
    return TRUE;
}

bool capture_enabled() {
    InitOnceExecuteOnce(&capture_once, open_capture, nullptr, nullptr);
    // The handle remains valid until process exit. A failed write disables
    // further appends under capture_lock.
    return capture != INVALID_HANDLE_VALUE;
}

HRESULT STDMETHODCALLTYPE record_root(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
HRESULT STDMETHODCALLTYPE record_compute(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE record_graphics(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE record_stream(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE record_raytracing(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE record_addition(ID3D12Device7*, const D3D12_STATE_OBJECT_DESC*, ID3D12StateObject*, REFIID, void**);
HRESULT STDMETHODCALLTYPE factory_device(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
HRESULT STDMETHODCALLTYPE sdk_factory(ID3D12SDKConfiguration1*, UINT, LPCSTR, REFIID, void**);
tessera::VtHook root_hook{16, reinterpret_cast<void*>(&record_root)};
tessera::VtHook compute_hook{11, reinterpret_cast<void*>(&record_compute)};
tessera::VtHook graphics_hook{10, reinterpret_cast<void*>(&record_graphics)};
tessera::VtHook stream_hook{47, reinterpret_cast<void*>(&record_stream)};
tessera::VtHook raytracing_hook{62, reinterpret_cast<void*>(&record_raytracing)};
tessera::VtHook addition_hook{66, reinterpret_cast<void*>(&record_addition)};
tessera::VtHook factory_hook{9, reinterpret_cast<void*>(&factory_device)};
tessera::VtHook sdk_hook{4, reinterpret_cast<void*>(&sdk_factory)};

HRESULT STDMETHODCALLTYPE record_root(ID3D12Device* device, UINT nodes, const void* bytes, SIZE_T size,
                                      REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(root_hook, device))(device, nodes, bytes, size, iid, result);
    if (FAILED(hr) || !result || !*result || !bytes || size == 0 || size > max_root_bytes) return hr;
    try {
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&root)))) return hr;
        std::vector<uint8_t> signature(sizeof(nodes) + size);
        std::memcpy(signature.data(), &nodes, sizeof(nodes));
        std::memcpy(signature.data() + sizeof(nodes), bytes, size);
        root->SetPrivateData(root_data, static_cast<UINT>(signature.size()), signature.data());
    } catch (...) {
        // Capture failures must not change the game's result.
    }
    return hr;
}

bool read_root_signature(ID3D12RootSignature* root, std::vector<uint8_t>& signature, uint32_t& root_nodes) {
    if (!root) return true;
    UINT size = 0;
    root->GetPrivateData(root_data, &size, nullptr);
    if (size <= sizeof(root_nodes) || size > max_root_bytes + sizeof(root_nodes)) return false;
    signature.resize(size);
    if (FAILED(root->GetPrivateData(root_data, &size, signature.data())) || size != signature.size()) return false;
    std::memcpy(&root_nodes, signature.data(), sizeof(root_nodes));
    signature.erase(signature.begin(), signature.begin() + sizeof(root_nodes));
    return true;
}

void append_pipeline(const void* payload, uint32_t payload_size, ID3D12RootSignature* root,
                     UINT nodes, D3D12_PIPELINE_STATE_FLAGS flags, uint32_t kind, uint64_t create_ns) {
    std::vector<uint8_t> signature;
    uint32_t root_nodes = 0;
    if (!read_root_signature(root, signature, root_nodes)) return;
    const auto root_size = static_cast<uint32_t>(signature.size());
    const Record entry{payload_size, root_size, nodes, root_nodes, static_cast<uint32_t>(flags), kind, create_ns};
    const uint64_t size = sizeof(entry) + entry.payload_size + entry.root_size;
    AcquireSRWLockExclusive(&capture_lock);
    if (recorded_bytes <= max_capture_bytes - size) {
        const bool complete = write_bytes(capture, &entry, sizeof(entry))
            && write_bytes(capture, payload, entry.payload_size)
            && (!root_size || write_bytes(capture, signature.data(), root_size));
        recorded_bytes = complete ? recorded_bytes + size : max_capture_bytes;
    }
    ReleaseSRWLockExclusive(&capture_lock);
}

HRESULT STDMETHODCALLTYPE record_compute(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                         REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
    const auto start = Clock::now();
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(compute_hook, device))(device, desc, iid, result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (SUCCEEDED(hr) && desc && result && *result) {
        try {
            const auto& shader = desc->CS;
            if (shader.pShaderBytecode && shader.BytecodeLength && shader.BytecodeLength <= max_shader_bytes)
                append_pipeline(shader.pShaderBytecode, static_cast<uint32_t>(shader.BytecodeLength),
                                desc->pRootSignature, desc->NodeMask, desc->Flags, compute_kind, elapsed);
        } catch (...) {
            // The driver result is independent of the recording.
        }
    }
    return hr;
}

void append_payload(std::vector<uint8_t>& payload, const void* data, size_t size) {
    if (!size) return;
    const size_t from = payload.size();
    payload.resize(from + size);
    std::memcpy(payload.data() + from, data, size);
}

void append_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, uint64_t elapsed) {
    if (desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides
        || desc.InputLayout.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
        return;
    const D3D12_SHADER_BYTECODE shaders[] = {desc.VS, desc.PS, desc.DS, desc.HS, desc.GS};
    GraphicsState state{};
    state.input_elements = desc.InputLayout.NumElements;
    state.blend = desc.BlendState;
    state.sample_mask = desc.SampleMask;
    state.rasterizer = desc.RasterizerState;
    state.depth_stencil = desc.DepthStencilState;
    state.strip_cut = desc.IBStripCutValue;
    state.topology = desc.PrimitiveTopologyType;
    state.render_targets = desc.NumRenderTargets;
    std::memcpy(state.formats, desc.RTVFormats, sizeof(state.formats));
    state.depth_format = desc.DSVFormat;
    state.samples = desc.SampleDesc;
    size_t total = sizeof(state);
    for (size_t i = 0; i < graphics_stages; ++i) {
        const auto size = shaders[i].BytecodeLength;
        if (size > max_shader_bytes || size > max_graphics_bytes - total
            || (size && !shaders[i].pShaderBytecode)) return;
        state.code_sizes[i] = static_cast<uint32_t>(size);
        total += size;
    }
    std::vector<uint8_t> payload;
    payload.reserve(total);
    append_payload(payload, &state, sizeof(state));
    for (const auto& shader : shaders) append_payload(payload, shader.pShaderBytecode, shader.BytecodeLength);
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
        const auto& element = desc.InputLayout.pInputElementDescs[i];
        const size_t name_size = element.SemanticName ? strnlen_s(element.SemanticName, 256) + 1 : 0;
        if (name_size <= 1 || name_size > 256 || payload.size() + sizeof(InputElement) + name_size > max_graphics_bytes)
            return;
        const InputElement entry{static_cast<uint32_t>(name_size), element.SemanticIndex, element.Format,
            element.InputSlot, element.AlignedByteOffset, element.InputSlotClass, element.InstanceDataStepRate};
        append_payload(payload, &entry, sizeof(entry));
        append_payload(payload, element.SemanticName, name_size);
    }
    append_pipeline(payload.data(), static_cast<uint32_t>(payload.size()), desc.pRootSignature,
                    desc.NodeMask, desc.Flags, graphics_kind, elapsed);
}

HRESULT STDMETHODCALLTYPE record_graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                          REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    const auto start = Clock::now();
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(graphics_hook, device))(device, desc, iid, result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (SUCCEEDED(hr) && desc && result && *result) {
        try { append_graphics(*desc, elapsed); } catch (...) {}
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE record_stream(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                        REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
    const auto start = Clock::now();
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(stream_hook, device))(device, desc, iid, result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (SUCCEEDED(hr) && desc && result && *result && desc->pPipelineStateSubobjectStream
        && desc->SizeInBytes <= max_graphics_bytes - sizeof(StreamHeader)) {
        try {
            const StreamHeader header{static_cast<uint32_t>(desc->SizeInBytes), 0};
            std::vector<uint8_t> payload;
            append_payload(payload, &header, sizeof(header));
            append_payload(payload, desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
            ID3D12RootSignature* root = nullptr;
            if (relocate_stream(payload, true, root))
                append_pipeline(payload.data(), static_cast<uint32_t>(payload.size()), root, 0,
                    D3D12_PIPELINE_STATE_FLAG_NONE, stream_kind, elapsed);
        } catch (...) {}
    }
    return hr;
}

bool read_state(ID3D12StateObject* state, std::vector<uint8_t>& bytes) {
    if (!state) return false;
    UINT size = 0;
    state->GetPrivateData(state_data, &size, nullptr);
    if (size <= sizeof(uint32_t) || size > max_graphics_bytes) return false;
    bytes.resize(size);
    return SUCCEEDED(state->GetPrivateData(state_data, &size, bytes.data())) && size == bytes.size();
}

void remember_state(void* result, const std::vector<uint8_t>& payload, uint32_t kind) {
    if (payload.size() > max_graphics_bytes - sizeof(kind)) return;
    ComPtr<ID3D12StateObject> state;
    if (FAILED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&state)))) return;
    std::vector<uint8_t> stored;
    stored.reserve(sizeof(kind) + payload.size());
    append_payload(stored, &kind, sizeof(kind));
    append_payload(stored, payload.data(), payload.size());
    state->SetPrivateData(state_data, static_cast<UINT>(stored.size()), stored.data());
}

HRESULT STDMETHODCALLTYPE record_raytracing(ID3D12Device5* device, const D3D12_STATE_OBJECT_DESC* desc,
                                            REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
    const auto start = Clock::now();
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(raytracing_hook, device))(device, desc, iid, result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (SUCCEEDED(hr) && desc && result && *result) {
        try {
            D3D12_STATE_OBJECT_DESC unused{};
            std::vector<uint8_t> payload;
            if (relocate_raytracing<true>(payload, *desc, unused, read_root_signature, read_state)) {
                remember_state(*result, payload, raytracing_kind);
                if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
                    append_pipeline(payload.data(), static_cast<uint32_t>(payload.size()), nullptr, 0,
                        D3D12_PIPELINE_STATE_FLAG_NONE, raytracing_kind, elapsed);
            }
        } catch (...) {}
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE record_addition(ID3D12Device7* device, const D3D12_STATE_OBJECT_DESC* desc,
                                         ID3D12StateObject* parent, REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device7*, const D3D12_STATE_OBJECT_DESC*, ID3D12StateObject*, REFIID, void**);
    const auto start = Clock::now();
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(addition_hook, device))(device, desc, parent, iid, result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (SUCCEEDED(hr) && desc && result && *result) {
        try {
            D3D12_STATE_OBJECT_DESC unused{};
            std::vector<uint8_t> base, addition;
            // The parent's private data is its kind followed by its record payload.
            if (read_state(parent, base) && relocate_raytracing<true>(addition, *desc, unused, read_root_signature, read_state)
                && sizeof(RayAddition) + addition.size() <= max_graphics_bytes) {
                RayAddition header{};
                std::memcpy(&header.parent_kind, base.data(), sizeof(header.parent_kind));
                header.parent_size = static_cast<uint32_t>(base.size() - sizeof(header.parent_kind));
                header.parent_hash = ray_payload_hash(base.data() + sizeof(header.parent_kind), header.parent_size);
                std::vector<uint8_t> payload;
                append_payload(payload, &header, sizeof(header));
                append_payload(payload, addition.data(), addition.size());
                remember_state(*result, payload, raytracing_add_kind);
                if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
                    append_pipeline(payload.data(), static_cast<uint32_t>(payload.size()), nullptr, 0,
                        D3D12_PIPELINE_STATE_FLAG_NONE, raytracing_add_kind, elapsed);
            }
        } catch (...) {}
    }
    return hr;
}

bool arm_capture() {
    if (!capture_enabled()) return false;
    static const bool pinned = [] {
        HMODULE self{};
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&record_root), &self) != FALSE;
    }();
    return pinned;
}

void hook_device(void* result) {
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&device)))
        && tessera::vt_hook(root_hook, device.Get())) {
        tessera::vt_hook(compute_hook, device.Get());
        tessera::vt_hook(graphics_hook, device.Get());
    }
    ComPtr<ID3D12Device2> device2;
    if (SUCCEEDED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&device2)))) {
        tessera::vt_hook(root_hook, device2.Get());
        tessera::vt_hook(stream_hook, device2.Get());
    }
    ComPtr<ID3D12Device5> device5;
    if (SUCCEEDED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&device5)))) {
        tessera::vt_hook(root_hook, device5.Get());
        tessera::vt_hook(raytracing_hook, device5.Get());
    }
    ComPtr<ID3D12Device7> device7;
    if (SUCCEEDED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&device7)))) {
        tessera::vt_hook(root_hook, device7.Get());
        tessera::vt_hook(raytracing_hook, device7.Get());
        tessera::vt_hook(addition_hook, device7.Get());
    }
}

void hook_factory(void* result) {
    ComPtr<ID3D12DeviceFactory> factory;
    if (SUCCEEDED(static_cast<IUnknown*>(result)->QueryInterface(IID_PPV_ARGS(&factory))))
        tessera::vt_hook(factory_hook, factory.Get());
}

HRESULT STDMETHODCALLTYPE factory_device(ID3D12DeviceFactory* factory, IUnknown* adapter, D3D_FEATURE_LEVEL level,
                                        REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(factory_hook, factory))(factory, adapter, level, iid, result);
    if (SUCCEEDED(hr) && result && *result) hook_device(*result);
    return hr;
}

HRESULT STDMETHODCALLTYPE sdk_factory(ID3D12SDKConfiguration1* sdk, UINT sdk_version, LPCSTR path, REFIID iid, void** result) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D12SDKConfiguration1*, UINT, LPCSTR, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(tessera::vt_orig(sdk_hook, sdk))(sdk, sdk_version, path, iid, result);
    if (SUCCEEDED(hr) && result && *result) hook_factory(*result);
    return hr;
}

extern "C" HRESULT unavailable() { return E_NOTIMPL; }
}  // namespace

extern "C" void* tessera_resolve(unsigned index) {
    using namespace tessera::exports;
    InitOnceExecuteOnce(&runtime_once, load_runtime, nullptr, nullptr);
    if (index >= kCount || !runtime) return reinterpret_cast<void*>(&unavailable);
    const char* name = index == kOrdinal99 ? MAKEINTRESOURCEA(99) : kNames[index];
    void* target = reinterpret_cast<void*>(GetProcAddress(runtime, name));
    if (!target) target = reinterpret_cast<void*>(&unavailable);
    InterlockedExchangePointer(&tessera_real[index], target);
    return target;
}

extern "C" HRESULT WINAPI Tessera_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level,
                                                   REFIID iid, void** result) {
    const auto next = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(tessera_resolve(tessera::exports::kD3D12CreateDevice));
    const HRESULT hr = next(adapter, level, iid, result);
    if (SUCCEEDED(hr) && result && *result && arm_capture()) hook_device(*result);
    return hr;
}

extern "C" HRESULT WINAPI Tessera_D3D12GetInterface(REFCLSID clsid, REFIID iid, void** result) {
    const auto next = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(tessera_resolve(tessera::exports::kD3D12GetInterface));
    const HRESULT hr = next(clsid, iid, result);
    if (FAILED(hr) || !result || !*result || !arm_capture()) return hr;
    hook_factory(*result);
    ComPtr<ID3D12SDKConfiguration1> sdk;
    if (SUCCEEDED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&sdk))))
        tessera::vt_hook(sdk_hook, sdk.Get());
    return hr;
}
