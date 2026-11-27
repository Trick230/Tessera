// SPDX-License-Identifier: GPL-3.0-or-later
// Build and test tool: compiles one HLSL entry point with the system d3dcompiler_47.dll, in place
// of fxc.exe (not in the toolchain).
//   tessera_hlsl_header <file.hlsl> <entry> <profile> <name> <out.inc> [DEFINE ...]
//     C header (`const BYTE <name>[] = {...};`) like DirectXTex's CompileShaders.cmd
//     (fxc /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug /Fh /Vn), for its DirectCompute shaders.
//   tessera_hlsl_header --object <file.hlsl> <entry> <profile> <out.dxbc> [DEFINE ...]
//     the shader object as `fxc /O3 /Fo` writes it (tests/run_quality.ps1).
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s.push_back(static_cast<char>(*w));  // ASCII names and defines
    return s;
}

// Written beside and renamed, so a failed run never leaves an output that looks up to date.
bool write_file(const wchar_t* path, const void* data, size_t size) {
    const std::wstring tmp = std::wstring(path) + L".tmp";
    FILE* out = nullptr;
    if (_wfopen_s(&out, tmp.c_str(), L"wb")) out = nullptr;
    bool ok = out && std::fwrite(data, 1, size, out) == size;
    if (out) ok = std::fclose(out) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        std::fputs("cannot write the output file\n", stderr);
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool object = argc > 1 && std::wstring(argv[1]) == L"--object";
    if (object) {
        --argc;
        ++argv;
    }
    const int fixed = object ? 5 : 6;  // arguments before the defines
    if (argc < fixed) {
        std::fputs("usage: tessera_hlsl_header <file.hlsl> <entry> <profile> <name> <out.inc> [DEFINE ...]\n"
                   "       tessera_hlsl_header --object <file.hlsl> <entry> <profile> <out.dxbc> [DEFINE ...]\n",
                   stderr);
        return 1;
    }
    const std::string entry = narrow(argv[2]), profile = narrow(argv[3]);
    const wchar_t* out_path = argv[fixed - 1];
    std::vector<std::string> names;
    for (int i = fixed; i < argc; ++i) names.push_back(narrow(argv[i]));
    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto& d : names) defines.push_back({d.c_str(), "1"});
    defines.push_back({nullptr, nullptr});

    const UINT flags = object ? D3DCOMPILE_OPTIMIZATION_LEVEL3
                              : D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG |
                                    D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompileFromFile(argv[1], defines.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.c_str(),
                                    profile.c_str(), flags, 0, &code, &errors);
    if (errors) {
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        errors->Release();
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "compile failed (0x%08lx): %s\n", static_cast<unsigned long>(hr), entry.c_str());
        return 1;
    }
    if (object) {
        const bool ok = write_file(out_path, code->GetBufferPointer(), code->GetBufferSize());
        code->Release();
        return ok ? 0 : 1;
    }
    ID3DBlob* stripped = nullptr;
    hr = D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                        D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped);
    code->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "strip failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 1;
    }

    std::string text = "const BYTE " + narrow(argv[4]) + "[] =\n{\n";
    const auto* bytes = static_cast<const unsigned char*>(stripped->GetBufferPointer());
    for (size_t i = 0; i < stripped->GetBufferSize(); ++i) {
        text += std::to_string(bytes[i]);
        text += (i + 1) % 16 ? ", " : ",\n";
    }
    text += "\n};\n";
    stripped->Release();
    return write_file(out_path, text.data(), text.size()) ? 0 : 1;
}
