#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
uint32_t Word(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated BLS header");
    uint32_t result;
    std::memcpy(&result, bytes.data() + offset, 4);
    return result;
}
}
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: VulkanShaderSource <current-GXSH-container.bls> <output-directory>\n";
        return 2;
    }
    try {
        std::ifstream stream(argv[1], std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open shader container");
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
        if (Word(bytes, 0) != 0x47585348 || Word(bytes, 4) != 0x10003)
            throw std::runtime_error("Expected current GXSH BLS version 0x10003");
        const auto count = Word(bytes, 8);
        if (count > (bytes.size() - 12) / 16) throw std::runtime_error("Invalid BLS permutation count");
        std::filesystem::create_directories(argv[2]);
        size_t offset = 12, passed = 0, failed = 0;
        for (uint32_t permutation = 0; permutation < count; ++permutation) {
            const uint32_t size = Word(bytes, offset + 12);
            offset += 16;
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("Truncated BLS program");
            try {
                auto source = GxVulkan::RecoverSlangSource(bytes.data() + offset, size);
                source = "// Container: " + std::filesystem::path(argv[1]).filename().string() +
                    "; permutation: " + std::to_string(permutation) + "\n" + source;
                auto destination = std::filesystem::path(argv[2]) / (std::to_string(permutation) + ".slang");
                if (std::filesystem::exists(destination)) {
                    std::ifstream old(destination);
                    const std::string previous((std::istreambuf_iterator<char>(old)), {});
                    if (source != previous) throw std::runtime_error("Refusing to overwrite differing source: " + destination.string());
                } else {
                    std::ofstream output(destination);
                    output << source;
                    if (!output) throw std::runtime_error("Cannot write generated shader source");
                }
                ++passed;
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "Permutation " << permutation << ": " << error.what() << '\n';
            }
            offset += (static_cast<size_t>(size) + 3) & ~size_t{3};
        }
        if (offset != bytes.size()) throw std::runtime_error("Trailing or missing BLS bytes");
        std::cout << "Recovered " << passed << '/' << count << " Slang shaders; failures: " << failed << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
