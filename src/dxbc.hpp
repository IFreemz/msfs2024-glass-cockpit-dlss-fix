#pragma once
// What the fix reads from a pixel shader's DXBC container (the bytecode the game passes to pipeline creation): its input
// signature (ISG1) and which texture registers it declares (PSV0). Anything not understood gives an empty result.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace glass {
struct SignatureElement { std::string semantic; uint32_t index = 0, reg = 0; };

// The data of the container part `fourcc` (e.g. "ISG1"), or null.
inline const uint8_t* dxbc_part(const void* bytecode, size_t size, const char* fourcc, uint32_t& part_size) {
    const auto bytes = static_cast<const uint8_t*>(bytecode);
    if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4)) return nullptr;
    uint32_t total = 0, parts = 0;
    std::memcpy(&total, bytes + 24, 4);
    std::memcpy(&parts, bytes + 28, 4);
    if (total > size || parts > 64 || 32 + size_t(parts) * 4 > total) return nullptr;
    for (uint32_t i = 0; i < parts; ++i) {
        uint32_t offset = 0;
        std::memcpy(&offset, bytes + 32 + i * 4, 4);
        if (size_t(offset) + 8 > total) return nullptr;
        uint32_t length = 0;
        std::memcpy(&length, bytes + offset + 4, 4);
        if (size_t(offset) + 8 + length > total) return nullptr;
        if (!std::memcmp(bytes + offset, fourcc, 4)) { part_size = length; return bytes + offset + 8; }
    }
    return nullptr;
}

// The input signature in declaration order (semantic name, index, register).
inline bool input_signature(const void* bytecode, size_t size, std::vector<SignatureElement>& elements) {
    elements.clear();
    uint32_t length = 0;
    const auto part = dxbc_part(bytecode, size, "ISG1", length);
    if (!part || length < 8) return false;
    uint32_t count = 0, first = 0;
    std::memcpy(&count, part, 4);
    std::memcpy(&first, part + 4, 4);
    if (count > 32 || size_t(first) + size_t(count) * 32 > length) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto element = part + first + i * 32; // stream, name offset, index, system value, type, register, masks, precision
        uint32_t name = 0;
        SignatureElement e;
        std::memcpy(&name, element + 4, 4);
        std::memcpy(&e.index, element + 8, 4);
        std::memcpy(&e.reg, element + 20, 4);
        if (name >= length) return false;
        const auto end = static_cast<const uint8_t*>(std::memchr(part + name, 0, length - name));
        if (!end) return false;
        e.semantic.assign(reinterpret_cast<const char*>(part + name), end - (part + name));
        elements.push_back(std::move(e));
    }
    return true;
}

// The shader's resource bindings from PSV0: calls `use(type, space, lower, upper, kind)` for each (kind 0 when the
// bytecode doesn't say it). Types and kinds are PSVResourceType and PSVResourceKind.
template <class Use> inline void resource_bindings(const void* bytecode, size_t size, Use&& use) {
    uint32_t length = 0;
    const auto part = dxbc_part(bytecode, size, "PSV0", length);
    if (!part || length < 4) return;
    uint32_t info = 0;
    std::memcpy(&info, part, 4);
    size_t at = 4 + size_t(info);
    uint32_t count = 0, stride = 0;
    if (at + 4 > length) return;
    std::memcpy(&count, part + at, 4);
    at += 4;
    if (!count) return;
    if (at + 4 > length) return;
    std::memcpy(&stride, part + at, 4);
    at += 4;
    if (stride < 16 || stride > 64 || at + size_t(count) * stride > length) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t binding[5]{}; // type, space, lower bound, upper bound, kind (bindings of 24 bytes and more)
        std::memcpy(binding, part + at + size_t(i) * stride, stride >= 20 ? 20 : 16);
        use(binding[0], binding[1], binding[2], binding[3], binding[4]);
    }
}

// Bit n set: the shader declares a 2D texture (or, when the bytecode doesn't say the kind, a typed SRV) at register tn in
// space 0, for n < 32.
inline uint32_t declared_textures(const void* bytecode, size_t size) {
    constexpr uint32_t srv_typed = 3, kind_texture_2d = 2; // PSVResourceType, PSVResourceKind
    uint32_t mask = 0;
    resource_bindings(bytecode, size, [&](uint32_t type, uint32_t space, uint32_t lower, uint32_t upper, uint32_t kind) {
        if (type != srv_typed || space != 0 || (kind && kind != kind_texture_2d)) return;
        for (uint32_t reg = lower; reg <= upper && reg < 32; ++reg) mask |= 1u << reg;
    });
    return mask;
}

// Does the shader declare an unordered-access view (it writes memory, e.g. the sim's decals blended into the G-buffer
// through rasterizer-ordered views, with no render target)?
inline bool declares_uavs(const void* bytecode, size_t size) {
    constexpr uint32_t uav_first = 6, uav_last = 11; // PSVResourceType: UAVTyped .. UAVStructuredWithCounter and feedback
    bool any = false;
    resource_bindings(bytecode, size, [&](uint32_t type, uint32_t, uint32_t, uint32_t, uint32_t) { any |= type >= uav_first && type <= uav_last; });
    return any;
}
}
