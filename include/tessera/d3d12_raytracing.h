// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tessera/d3d12_stream.h"

namespace tessera::d3d12_capture {
struct RayHeader {
    uint32_t subobjects;
    uint32_t collection;
};

// An AddToStateObject record names its parent, an earlier raytracing record of the same
// capture, by its payload size and hash, so a chain of additions stays linear in size.
struct RayAddition {
    uint32_t parent_kind;
    uint32_t parent_size;
    uint64_t parent_hash;
};

inline uint64_t ray_payload_hash(const uint8_t* bytes, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

struct RayDependency {
    const uint8_t* bytes;
    uint32_t size;
    uint32_t export_count;
    const D3D12_EXPORT_DESC* exports;
};

struct RayRoot {
    const uint8_t* bytes;
    uint32_t size;
    uint32_t node_mask;
};

inline constexpr uint32_t max_ray_subobjects = 4096;
inline constexpr uint32_t max_ray_exports = 4096;
inline constexpr size_t max_ray_name = 4096;

// Ray tracing subobject descriptions use the same checked offsets as
// pipeline streams. An association points to a subobject index in this record.
template<bool Capture, class Root, class Dependency>
bool relocate_raytracing(std::vector<uint8_t>& payload, const D3D12_STATE_OBJECT_DESC& source,
                         D3D12_STATE_OBJECT_DESC& result, Root&& update_root, Dependency&& update_dependency) {
    if constexpr (sizeof(void*) != 8) return false;
    RayHeader header{};
    if constexpr (Capture) {
        if ((source.Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE && source.Type != D3D12_STATE_OBJECT_TYPE_COLLECTION)
            || !source.NumSubobjects || source.NumSubobjects > max_ray_subobjects || !source.pSubobjects)
            return false;
        header.subobjects = source.NumSubobjects;
        header.collection = source.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION;
        payload.resize(sizeof(header) + header.subobjects * sizeof(D3D12_STATE_SUBOBJECT));
        std::memcpy(payload.data(), &header, sizeof(header));
    } else {
        if (payload.size() < sizeof(header)) return false;
        std::memcpy(&header, payload.data(), sizeof(header));
        if (header.collection > 1 || !header.subobjects || header.subobjects > max_ray_subobjects) return false;
    }
    const size_t table_end = sizeof(header) + header.subobjects * sizeof(D3D12_STATE_SUBOBJECT);
    if (table_end > payload.size()) return false;
    StreamRelocator relocator(payload, table_end, Capture);
    const auto wide = [&](LPCWSTR& name) { return relocator.name(name, max_ray_name); };
    const auto exports = [&](LPCWSTR*& array, UINT count) {
        const LPCWSTR* entries = array;
        if (!relocator.mapped_array(entries, count, max_ray_exports, wide)) return false;
        array = const_cast<LPCWSTR*>(entries);
        return true;
    };
    const auto definitions = [&](const D3D12_EXPORT_DESC*& names, UINT count) {
        return relocator.mapped_array(names, count, max_ray_exports,
            [&](D3D12_EXPORT_DESC& name) { return wide(name.Name) && wide(name.ExportToRename); });
    };
    for (uint32_t i = 0; i < header.subobjects; ++i) {
        const size_t at = sizeof(header) + i * sizeof(D3D12_STATE_SUBOBJECT);
        D3D12_STATE_SUBOBJECT item{};
        if constexpr (Capture) {
            item.Type = source.pSubobjects[i].Type;
            item.pDesc = source.pSubobjects[i].pDesc;
        } else {
            std::memcpy(&item, payload.data() + at, sizeof(item));
        }
        if (!item.pDesc) return false;
        const auto field = [&]<class T>(auto&& transform) {
            const auto* ptr = static_cast<const T*>(item.pDesc);
            if (!relocator.mapped_array(ptr, 1, 1, transform)) return false;
            item.pDesc = ptr;
            return true;
        };
        const auto scalar = [&]<class T>() { return field.template operator()<T>([](auto&) { return true; }); };
        bool ok = false;
        switch (item.Type) {
            case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
                ok = scalar.template operator()<D3D12_STATE_OBJECT_CONFIG>();
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
                ok = scalar.template operator()<D3D12_NODE_MASK>();
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
                ok = scalar.template operator()<D3D12_RAYTRACING_SHADER_CONFIG>();
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
                ok = scalar.template operator()<D3D12_RAYTRACING_PIPELINE_CONFIG>();
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
                ok = scalar.template operator()<D3D12_RAYTRACING_PIPELINE_CONFIG1>();
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
                if constexpr (Capture) {
                    auto* signature = item.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE
                        ? static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(item.pDesc)->pGlobalRootSignature
                        : static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(item.pDesc)->pLocalRootSignature;
                    std::vector<uint8_t> bytes;
                    uint32_t node_mask = 0;
                    if (!update_root(signature, bytes, node_mask)) return false;
                    const RayRoot wire{bytes.data(), static_cast<uint32_t>(bytes.size()), node_mask};
                    item.pDesc = &wire;
                    ok = field.template operator()<RayRoot>([&](auto& root) {
                        return relocator.array(root.bytes, root.size, max_root_bytes);
                    });
                } else {
                    RayRoot decoded{};
                    ok = field.template operator()<RayRoot>([&](auto& root) {
                        if (!relocator.array(root.bytes, root.size, max_root_bytes)) return false;
                        decoded = root;
                        return true;
                    }) && update_root(item, decoded);
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
                ok = field.template operator()<D3D12_DXIL_LIBRARY_DESC>([&](auto& library) {
                    auto* bytes = static_cast<const uint8_t*>(library.DXILLibrary.pShaderBytecode);
                    if (!relocator.array(bytes, library.DXILLibrary.BytecodeLength, max_shader_bytes)) return false;
                    library.DXILLibrary.pShaderBytecode = bytes;
                    return definitions(library.pExports, library.NumExports);
                });
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
                const auto dependency = [&](RayDependency& value) {
                    return value.size && relocator.array(value.bytes, value.size, max_graphics_bytes)
                        && definitions(value.exports, value.export_count);
                };
                if constexpr (Capture) {
                    const auto& desc = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(item.pDesc);
                    std::vector<uint8_t> bytes;
                    if (!update_dependency(desc.pExistingCollection, bytes)) return false;
                    const RayDependency wire{bytes.data(), static_cast<uint32_t>(bytes.size()), desc.NumExports, desc.pExports};
                    item.pDesc = &wire;
                    ok = field.template operator()<RayDependency>(dependency);
                } else {
                    RayDependency decoded{};
                    ok = field.template operator()<RayDependency>([&](auto& value) {
                        if (!dependency(value)) return false;
                        decoded = value;
                        return true;
                    }) && update_dependency(item, decoded);
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
                ok = field.template operator()<D3D12_HIT_GROUP_DESC>([&](auto& group) {
                    return wide(group.HitGroupExport) && wide(group.AnyHitShaderImport)
                        && wide(group.ClosestHitShaderImport) && wide(group.IntersectionShaderImport);
                });
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
                ok = field.template operator()<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION>([&](auto& association) {
                    if constexpr (Capture) {
                        uint32_t target = 0;
                        while (target < header.subobjects && association.pSubobjectToAssociate != source.pSubobjects + target)
                            ++target;
                        if (target == header.subobjects) return false;
                        association.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(uintptr_t{target} + 1);
                    } else {
                        const auto index = reinterpret_cast<uintptr_t>(association.pSubobjectToAssociate);
                        if (!index || index > header.subobjects) return false;
                        association.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(
                            payload.data() + sizeof(header)) + index - 1;
                    }
                    return exports(association.pExports, association.NumExports);
                });
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
                ok = field.template operator()<D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION>([&](auto& association) {
                    return wide(association.SubobjectToAssociate) && exports(association.pExports, association.NumExports);
                });
                break;
            default: return false;
        }
        if (!ok) return false;
        std::memcpy(payload.data() + at, &item, sizeof(item));
    }
    if constexpr (!Capture) {
        result = {header.collection ? D3D12_STATE_OBJECT_TYPE_COLLECTION : D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, header.subobjects,
            reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(payload.data() + sizeof(header))};
    }
    return true;
}
}  // namespace tessera::d3d12_capture
