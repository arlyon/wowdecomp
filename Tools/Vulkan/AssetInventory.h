#ifndef TOOLS_VULKAN_ASSET_INVENTORY_H
#define TOOLS_VULKAN_ASSET_INVENTORY_H
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace AssetInventory {
// The original runtime uses 260-byte path buffers. Reject rather than truncate
// names that cannot be faithfully addressed, including an unterminated last
// row.
inline std::set<std::string> ListNames(std::string_view bytes) {
    std::set<std::string> names;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find_first_of("\r\n", start);
        if (end == std::string_view::npos)
            end = bytes.size();
        if (end != start) {
            if (end - start > 259)
                throw std::runtime_error("Archive list name exceeds runtime path capacity");
            std::string name(bytes.substr(start, end - start));
            for (char &c : name) {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 32 || byte == 127 || c == ':')
                    throw std::runtime_error("Invalid archive list name");
                if (c == '/')
                    c = '\\';
                if (c >= 'a' && c <= 'z')
                    c -= ('a' - 'A');
            }
            size_t component = 0;
            for (size_t i = 0; i <= name.size(); ++i)
                if (i == name.size() || name[i] == '\\') {
                    const auto part = name.substr(component, i - component);
                    if (part.empty() || part == "." || part == "..")
                        throw std::runtime_error("Unsafe archive list path");
                    component = i + 1;
                }
            names.insert(std::move(name));
        }
        start = end == bytes.size() ? end : end + 1;
    }
    return names;
}
inline uint32_t Word(std::string_view bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated shader record");
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
struct ShaderRecord {
    uint32_t permutation;
    size_t offset, size;
    uint32_t version;
};
inline std::vector<ShaderRecord> ShaderRecords(std::string_view bytes) {
    if (Word(bytes, 0) != 0x47585348 || Word(bytes, 4) != 0x10003)
        throw std::runtime_error("Unsupported GXSH container revision");
    const uint32_t count = Word(bytes, 8);
    if (count > (bytes.size() - 12) / 16)
        throw std::runtime_error("Truncated shader permutation census");
    std::vector<ShaderRecord> records;
    size_t offset = 12;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t size = Word(bytes, offset + 12);
        offset += 16;
        const size_t padded = (size_t(size) + 3) & ~size_t(3);
        if (offset > bytes.size() || padded > bytes.size() - offset)
            throw std::runtime_error("Invalid shader program extent");
        // Containers also hold text/non-native profiles. Preserve every record;
        // the subsequent native-profile/interface gate validates its semantics.
        const uint32_t version = size >= 4 ? Word(bytes, offset) : 0;
        records.push_back({i, offset, size, version});
        offset += padded;
    }
    if (offset != bytes.size())
        throw std::runtime_error("Trailing bytes outside shader permutation census");
    return records;
}
} // namespace AssetInventory
#endif
