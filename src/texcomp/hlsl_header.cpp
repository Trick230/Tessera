// SPDX-License-Identifier: GPL-3.0-or-later
// Build-time tool: compiles one HLSL entry point with the system d3dcompiler_47.dll and writes
// it as a C header (`const BYTE <name>[] = {...};`), like `fxc /Fh /Vn`. It replaces fxc.exe,
// which the portable toolchain does not have, for DirectXTex's DirectCompute BC6H/BC7 shaders.
//   tessera_hlsl_header <file.hlsl> <entry> <profile> <name> <out.inc> [DEFINE ...]
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        std::fputs("usage: tessera_hlsl_header <file.hlsl> <entry> <profile> <name> <out.inc> [DEFINE ...]\n", stderr);
        return 1;
    }
    auto narrow = [](const wchar_t* w) {
        std::string s;
        for (; *w; ++w) s.push_back(static_cast<char>(*w));  // ASCII names and defines
        return s;
    };
    const std::string entry = narrow(argv[2]), profile = narrow(argv[3]), name = narrow(argv[4]);
    std::vector<std::string> names;
    for (int i = 6; i < argc; ++i) names.push_back(narrow(argv[i]));
    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto& d : names) defines.push_back({d.c_str(), "1"});
    defines.push_back({nullptr, nullptr});

    // The flags DirectXTex's CompileShaders.cmd passes to fxc (/WX /Ges /Zi /Zpc), then
    // /Qstrip_reflect /Qstrip_debug.
    const UINT flags = D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG |
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
    ID3DBlob* stripped = nullptr;
    hr = D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                        D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped);
    code->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "strip failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 1;
    }

    std::string text = "const BYTE " + name + "[] =\n{\n";
    const auto* bytes = static_cast<const unsigned char*>(stripped->GetBufferPointer());
    for (size_t i = 0; i < stripped->GetBufferSize(); ++i) {
        text += std::to_string(bytes[i]);
        text += (i + 1) % 16 ? ", " : ",\n";
    }
    text += "\n};\n";
    stripped->Release();

    // Written beside and renamed, so a failed run never leaves a header that looks up to date.
    const std::wstring tmp = std::wstring(argv[5]) + L".tmp";
    FILE* out = nullptr;
    if (_wfopen_s(&out, tmp.c_str(), L"wb")) out = nullptr;
    bool ok = out && std::fwrite(text.data(), 1, text.size(), out) == text.size();
    if (out) ok = std::fclose(out) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), argv[5], MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        std::fputs("cannot write the output header\n", stderr);
        return 1;
    }
    return 0;
}
