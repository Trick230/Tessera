// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "exports_d3d12_gen.h"
#include "tessera/d3d12_capture_format.h"
#include "vtable.h"

extern "C" void* tessera_real[];

namespace {
using namespace tessera::d3d12_capture;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr GUID root_data = {0x18dd39b3, 0xe76e, 0x482f, {0x83, 0xbc, 0x6c, 0x24, 0x24, 0x1f, 0xff, 0x82}};
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
tessera::VtHook root_hook{16, reinterpret_cast<void*>(&record_root)};
tessera::VtHook compute_hook{11, reinterpret_cast<void*>(&record_compute)};

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

void append_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, uint64_t create_ns) {
    if (!desc.CS.pShaderBytecode || !desc.CS.BytecodeLength || desc.CS.BytecodeLength > max_shader_bytes) return;
    std::vector<uint8_t> signature;
    uint32_t root_nodes = 0;
    if (desc.pRootSignature) {
        UINT size = 0;
        desc.pRootSignature->GetPrivateData(root_data, &size, nullptr);
        if (size <= sizeof(root_nodes) || size > max_root_bytes + sizeof(root_nodes)) return;
        signature.resize(size);
        if (FAILED(desc.pRootSignature->GetPrivateData(root_data, &size, signature.data()))
            || size != signature.size()) return;
        std::memcpy(&root_nodes, signature.data(), sizeof(root_nodes));
    }
    const auto root_size = static_cast<uint32_t>(signature.empty() ? 0 : signature.size() - sizeof(root_nodes));
    const ComputeRecord entry{static_cast<uint32_t>(desc.CS.BytecodeLength), root_size, desc.NodeMask,
        root_nodes, static_cast<uint32_t>(desc.Flags), 0, create_ns};
    const uint64_t size = sizeof(entry) + entry.shader_size + entry.root_size;
    AcquireSRWLockExclusive(&capture_lock);
    if (recorded_bytes <= max_capture_bytes - size) {
        const bool complete = write_bytes(capture, &entry, sizeof(entry))
            && write_bytes(capture, desc.CS.pShaderBytecode, entry.shader_size)
            && (!root_size || write_bytes(capture, signature.data() + sizeof(root_nodes), root_size));
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
            append_compute(*desc, static_cast<uint64_t>(elapsed));
        } catch (...) {
            // The driver result is independent of the recording.
        }
    }
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
    if (FAILED(hr) || !result || !*result || !capture_enabled()) return hr;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&device)))) {
        HMODULE pinned;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Tessera_D3D12CreateDevice), &pinned)
            && tessera::vt_hook(root_hook, device.Get())) {
            tessera::vt_hook(compute_hook, device.Get());
        }
    }
    return hr;
}
