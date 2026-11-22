// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <DirectXTex.h>
#include <DirectXPackedVector.h>
#include <FLIP.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "tessera/quality_format.h"

namespace {
using namespace DirectX;
constexpr size_t kMaxDimension = 2048;
constexpr float kMaxMeanFlip = 0.005f, kMaxFlip = 0.025f;

DXGI_FORMAT target_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_BC6H_UF16;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool supported(const TexMetadata& m) {
    return m.dimension == TEX_DIMENSION_TEXTURE2D && m.arraySize == 1 && !m.IsCubemap() &&
           m.width >= 4 && m.height >= 4 && m.width <= kMaxDimension && m.height <= kMaxDimension &&
           m.width % 4 == 0 && m.height % 4 == 0 && m.mipLevels && m.mipLevels <= 12 &&
           target_format(m.format) != DXGI_FORMAT_UNKNOWN;
}

bool is_hdr(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT; }

float linear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float channel(const uint8_t* row, size_t i, bool hdr) {
    if (!hdr) return row[i] / 255.0f;
    uint16_t half;
    std::memcpy(&half, row + i * sizeof half, sizeof half);
    return PackedVector::XMConvertHalfToFloat(half);
}

bool opaque_nonnegative(const Image& img) {
    const bool hdr = is_hdr(img.format);
    for (size_t y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels + y * img.rowPitch;
        for (size_t x = 0; x < img.width; ++x) {
            if (channel(row, x * 4 + 3, hdr) != 1.0f) return false;
            if (hdr) {
                for (unsigned c = 0; c < 3; ++c) {
                    const float value = channel(row, x * 4 + c, true);
                    if (!std::isfinite(value) || value < 0.0f) return false;
                }
            }
        }
    }
    return true;
}

struct Quality {
    float worst = 0.0f, worst_mean = 0.0f;
};

bool compare_mip(const Image& ref, const Image& decoded, Quality& quality) {
    if (ref.format != decoded.format || ref.width != decoded.width || ref.height != decoded.height ||
        !opaque_nonnegative(decoded))
        return false;
    const size_t pixels = ref.width * ref.height;
    std::vector<float> a(pixels * 3), b(a.size());
    const bool hdr = is_hdr(ref.format), srgb = IsSRGB(ref.format);
    bool exact = true;
    for (size_t y = 0; y < ref.height; ++y) {
        const auto* row = ref.pixels + y * ref.rowPitch;
        const auto* other = decoded.pixels + y * decoded.rowPitch;
        for (size_t x = 0; x < ref.width; ++x) {
            for (size_t c = 0; c < 3; ++c) {
                const float v = channel(row, x * 4 + c, hdr), w = channel(other, x * 4 + c, hdr);
                const float limit = hdr ? std::max(0.00001f, v * 0.01f) : 2.0f / 255.0f + 0.000001f;
                if (std::abs(v - w) > limit) return false;
                exact &= v == w;
                const size_t i = (y * ref.width + x) * 3 + c;
                a[i] = srgb ? linear(v) : v;
                b[i] = srgb ? linear(w) : w;
            }
        }
    }
    if (exact) return true;

    FLIP::Parameters parameters;
    if (hdr) {
        // Bounded exposures. The channel guard above protects data outside them.
        parameters.startExposure = -8.0f;
        parameters.stopExposure = 8.0f;
        parameters.numExposures = 17;
    }
    float average = 0.0f;
    float* errors = nullptr;
    FLIP::evaluate(a.data(), b.data(), static_cast<int>(ref.width), static_cast<int>(ref.height), hdr,
                   parameters, false, true, average, &errors);
    const std::unique_ptr<float[]> map(errors);
    if (!map || !std::isfinite(average)) return false;
    float peak = 0.0f;
    for (size_t i = 0; i < pixels; ++i) {
        if (!std::isfinite(map[i])) return false;
        peak = std::max(peak, map[i]);
    }
    quality.worst_mean = std::max(quality.worst_mean, average);
    quality.worst = std::max(quality.worst, peak);
    return average <= kMaxMeanFlip && peak <= kMaxFlip;
}

enum Result { accepted = 0, failed = 1, rejected = 2 };

Result try_compress(const ScratchImage& input, ScratchImage& output, Quality& quality) {
    const auto& m = input.GetMetadata();
    if (!supported(m)) return rejected;
    const auto* images = input.GetImages();
    const size_t count = input.GetImageCount();
    if (!std::all_of(images, images + count, opaque_nonnegative)) return rejected;
    HRESULT hr = Compress(images, count, m, target_format(m.format), TEX_COMPRESS_DEFAULT,
                          TEX_THRESHOLD_DEFAULT, output);
    if (FAILED(hr)) {
        std::fprintf(stderr, "Compress failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        return failed;
    }
    ScratchImage decoded;
    hr = Decompress(output.GetImages(), output.GetImageCount(), output.GetMetadata(), m.format, decoded);
    if (FAILED(hr) || decoded.GetImageCount() != count) {
        std::fprintf(stderr, "Decompress failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        return failed;
    }
    for (size_t i = 0; i < count; ++i)
        if (!compare_mip(images[i], decoded.GetImages()[i], quality)) return rejected;
    return output.GetPixelsSize() < input.GetPixelsSize() ? accepted : rejected;
}

bool save_new_bytes(const void* bytes, size_t length, const wchar_t* path) {
    if (length > MAXDWORD) return false;
    const std::wstring tmp = std::wstring(path) + L".tessera-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto size = static_cast<DWORD>(length);
    const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (ok && MoveFileExW(tmp.c_str(), path, 0)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

bool save_new(const ScratchImage& output, const wchar_t* path) {
    Blob bytes;
    return SUCCEEDED(SaveToDDSMemory(output.GetImages(), output.GetImageCount(), output.GetMetadata(),
                                    DDS_FLAGS_FORCE_DX10_EXT, bytes)) &&
           save_new_bytes(bytes.GetBufferPointer(), bytes.GetBufferSize(), path);
}

Result pack_texture(const std::filesystem::path& source, const std::filesystem::path& output) {
    std::ifstream file(source, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    TesseraQualityHeader h = {};
    file.seekg(0);
    if (length < 0 || !file.read(reinterpret_cast<char*>(&h), sizeof h) ||
        !tessera::quality_header(h, static_cast<uint64_t>(length), false))
        return rejected;
    h.compressed = tessera::quality_compressed_format(h.format);
    h.compressed_bytes = tessera::quality_bytes(h, true);
    if (h.compressed_bytes >= h.original_bytes) return rejected;
    std::vector<uint8_t> encoded(sizeof h + h.original_bytes + h.compressed_bytes);
    std::memcpy(encoded.data(), &h, sizeof h);
    auto* pixels = encoded.data() + sizeof h;
    if (!file.read(reinterpret_cast<char*>(pixels), h.original_bytes)) return failed;

    ScratchImage input, compressed;
    if (FAILED(input.Initialize2D(static_cast<DXGI_FORMAT>(h.format), h.width, h.height, 1, h.mips)))
        return failed;
    for (unsigned m = 0; m < h.mips; ++m) {
        const auto& img = input.GetImages()[m];
        const auto layout = tessera::quality_mip(h, m, false);
        for (unsigned y = 0; y < layout.rows; ++y, pixels += layout.pitch)
            std::memcpy(img.pixels + y * img.rowPitch, pixels, layout.pitch);
    }
    Quality quality;
    const Result result = try_compress(input, compressed, quality);
    if (result != accepted) return result;
    for (unsigned m = 0; m < h.mips; ++m) {
        const auto& img = compressed.GetImages()[m];
        const auto layout = tessera::quality_mip(h, m, true);
        for (unsigned y = 0; y < layout.rows; ++y, pixels += layout.pitch)
            std::memcpy(pixels, img.pixels + y * img.rowPitch, layout.pitch);
    }
    return save_new_bytes(encoded.data(), encoded.size(), output.c_str()) ? accepted : failed;
}

int batch(const wchar_t* raw, const wchar_t* cache) {
    namespace fs = std::filesystem;
    fs::create_directories(cache);
    unsigned saved = 0, existing = 0, skipped = 0, errors = 0;
    for (const auto& input : fs::directory_iterator(raw)) {
        if (input.path().extension() != L".txr" || !input.is_regular_file()) continue;
        auto output = fs::path(cache) / input.path().filename();
        output.replace_extension(L".tbc");
        if (fs::exists(output)) {
            ++existing;
            continue;
        }
        const Result result = pack_texture(input.path(), output);
        if (result == accepted) ++saved;
        else if (result == rejected) ++skipped;
        else ++errors;
    }
    std::printf("Texture cache: %u added, %u existing, %u rejected, %u errors\n", saved, existing, skipped, errors);
    return errors ? failed : accepted;
}

int selftest() {
    for (auto f : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        ScratchImage input, output;
        if (FAILED(input.Initialize2D(f, 16, 16, 1, 5))) return failed;
        if (is_hdr(f)) {
            const uint16_t white[] = {0x3c00, 0x3c00, 0x3c00, 0x3c00};
            for (size_t i = 0; i < input.GetPixelsSize(); i += sizeof white)
                std::memcpy(input.GetPixels() + i, white, sizeof white);
        } else {
            std::memset(input.GetPixels(), 255, input.GetPixelsSize());
        }
        Quality quality;
        if (try_compress(input, output, quality) != accepted || output.GetMetadata().format != target_format(f) ||
            output.GetImageCount() != 5)
            return failed;
        std::printf("PASS %s five-mip recompression (peak FLIP %.6f)\n", is_hdr(f) ? "BC6H" : "BC7", quality.worst);
        auto* last = input.GetImages()[4].pixels;
        last[is_hdr(f) ? 7 : 3] = 0;  // alpha loss even on the smallest mip
        if (try_compress(input, output, quality) != rejected) return failed;
    }
    ScratchImage original, changed;
    if (FAILED(original.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1)) ||
        FAILED(changed.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1)))
        return failed;
    std::memset(original.GetPixels(), 255, original.GetPixelsSize());
    std::memset(changed.GetPixels(), 255, changed.GetPixelsSize());
    changed.GetPixels()[0] = 254;  // exercises FLIP, inside the channel guard
    Quality quality;
    if (!compare_mip(*original.GetImages(), *changed.GetImages(), quality) || quality.worst <= 0.0f) return failed;
    std::puts("PASS FLIP comparison and alpha rejection");
    return accepted;
}
}  // namespace

int wmain(int argc, wchar_t** argv) try {
    if (argc == 2 && std::wstring(argv[1]) == L"--selftest") return selftest();
    if (argc == 4 && std::wstring(argv[1]) == L"--batch") return batch(argv[2], argv[3]);
    if (argc != 3) {
        std::fputs("usage: tessera_texcomp <source.dds> <new-output.dds> | --batch <capture dir> <cache dir>\n",
                   stderr);
        return failed;
    }
    TexMetadata meta = {};
    if (FAILED(GetMetadataFromDDSFile(argv[1], DDS_FLAGS_NONE, meta))) {
        std::fputs("Cannot read the source DDS header.\n", stderr);
        return failed;
    }
    if (!supported(meta)) {
        std::fputs("Unsupported DDS dimensions or format.\n", stderr);
        return rejected;
    }
    ScratchImage input, output;
    if (FAILED(LoadFromDDSFile(argv[1], DDS_FLAGS_NONE, &meta, input))) {
        std::fputs("Cannot load the source DDS.\n", stderr);
        return failed;
    }
    Quality quality;
    const Result result = try_compress(input, output, quality);
    if (result == rejected) std::fputs("Texture rejected by the quality or format checks.\n", stderr);
    if (result != accepted) return result;
    if (!save_new(output, argv[2])) {
        std::fputs("Cannot write a new output DDS.\n", stderr);
        return failed;
    }
    std::printf("Accepted %zu -> %zu bytes, peak FLIP %.6f, worst mip mean %.6f\n",
                input.GetPixelsSize(), output.GetPixelsSize(), quality.worst, quality.worst_mean);
    return accepted;
} catch (const std::exception& e) {
    std::fprintf(stderr, "Texture processing failed: %s\n", e.what());
    return failed;
}
