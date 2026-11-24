// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tessera {
// Store pointer-backed fields as validated, non-overlapping packet offsets.
template<size_t Maximum>
class PacketOffsets {
public:
    PacketOffsets(std::vector<uint8_t>& bytes, size_t data_start, bool capture)
        : bytes_(bytes), start_(data_start), capture_(capture) {}

    template<class T>
    bool array(const T*& pointer, size_t count, size_t limit) {
        if (!count) {
            pointer = nullptr;
            return true;
        }
        if (!pointer || count > limit) return false;
        const size_t size = count * sizeof(T);
        if (capture_) {
            const size_t offset = (bytes_.size() + alignof(T) - 1) & ~(alignof(T) - 1);
            if (offset > Maximum || size > Maximum - offset) return false;
            bytes_.resize(offset + size);
            std::memcpy(bytes_.data() + offset, pointer, size);
            pointer = reinterpret_cast<const T*>(offset);
        } else {
            const auto offset = reinterpret_cast<uintptr_t>(pointer);
            if (offset < start_ || offset > bytes_.size() || offset % alignof(T) || size > bytes_.size() - offset)
                return false;
            pointer = reinterpret_cast<const T*>(bytes_.data() + offset);
            start_ = offset + size;
        }
        return true;
    }

    template<class Char>
    bool name(const Char*& pointer, size_t limit = 256) {
        if (!pointer) return true;
        if (capture_) {
            size_t length = 0;
            while (length < limit && pointer[length]) ++length;
            return length < limit && array(pointer, length + 1, limit);
        }
        const auto offset = reinterpret_cast<uintptr_t>(pointer);
        if (offset < start_ || offset >= bytes_.size() || offset % alignof(Char)) return false;
        const auto available = (bytes_.size() - offset) / sizeof(Char);
        const auto* value = reinterpret_cast<const Char*>(bytes_.data() + offset);
        for (size_t i = 0; i < available && i < limit; ++i) {
            if (value[i]) continue;
            pointer = value;
            start_ = offset + (i + 1) * sizeof(Char);
            return true;
        }
        return false;
    }

    template<class T, class Transform>
    bool mapped_array(const T*& pointer, size_t count, size_t limit, Transform transform) {
        const T* original = pointer;
        if (!array(pointer, count, limit)) return false;
        if (!count) return true;
        if (!capture_) original = pointer;
        const size_t offset = capture_ ? reinterpret_cast<uintptr_t>(pointer)
            : static_cast<size_t>(reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(bytes_.data()));
        for (size_t i = 0; i < count; ++i) {
            T item = original[i];
            if (!transform(item)) return false;
            std::memcpy(bytes_.data() + offset + i * sizeof(T), &item, sizeof(T));
        }
        return true;
    }

    template<class T>
    bool named_array(const T*& pointer, size_t count, size_t limit) {
        return mapped_array(pointer, count, limit, [&](T& item) { return name(item.SemanticName); });
    }

private:
    std::vector<uint8_t>& bytes_;
    size_t start_;
    bool capture_;
};

}  // namespace tessera
