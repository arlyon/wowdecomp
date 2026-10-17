#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
#include "Engine/Source/Gx/CGxDeviceVulkan/Slang.h"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#if defined(WOWDECOMP_VALIDATE_SPIRV)
#include <spirv-tools/libspirv.h>
#endif

namespace {
constexpr uint32_t vertex[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000,
    0x02000001, 0xC00F0000, 0x90E40000, 0x0000FFFF
};
constexpr uint32_t pixel[] = {
    0xFFFF0200, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF
};
void Validate(const std::vector<uint32_t>& words) {
#if defined(WOWDECOMP_VALIDATE_SPIRV)
    const auto context = spvContextCreate(words.at(1) > 0x00010000 ? SPV_ENV_VULKAN_1_1 : SPV_ENV_VULKAN_1_0);
    spv_diagnostic diagnostic = nullptr;
    const auto result = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
    const std::string error = diagnostic ? diagnostic->error : "SPIR-V validation failed";
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (result != SPV_SUCCESS) throw std::runtime_error(error);
#else
    (void)words;
#endif
}
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--slang")) {
        std::cerr << "Usage: VulkanShaderCorpus <extracted-programs-directory> [--slang]\n";
        return 2;
    }
    try {
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
        std::unique_ptr<GxVulkan::SlangCompiler> compiler;
        if (argc == 3) compiler = std::make_unique<GxVulkan::SlangCompiler>();
#else
        if (argc == 3) throw std::runtime_error("Slang compiler support was not configured");
#endif
        auto defaultVertex = GxVulkan::TranslateShader(vertex, sizeof(vertex));
        auto defaultPixel = GxVulkan::TranslateShader(pixel, sizeof(pixel));
        std::set<std::vector<uint8_t>> unique;
        size_t passed = 0, failed = 0, ps14 = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
            if (entry.path().extension() != ".bin") continue;
            const std::string profile = entry.path().parent_path().parent_path().filename().string();
            if (profile.rfind("vs_", 0) && profile.rfind("ps_", 0)) continue;
            std::ifstream stream(entry.path(), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
            if (!unique.insert(bytes).second) continue;
            try {
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
                if (compiler) {
                    auto recovered = GxVulkan::RecoverSlangShader(bytes.data(), bytes.size());
                    const auto code = compiler->Compile(recovered.source,
                        recovered.reflection->shader_type == MOJOSHADER_TYPE_VERTEX);
                    Validate(code);
                    ++passed;
                    continue;
                }
#endif
                auto shader = GxVulkan::TranslateShader(bytes.data(), bytes.size());
                const auto& v = shader->shader_type == MOJOSHADER_TYPE_VERTEX ? *shader : *defaultVertex;
                const auto& p = shader->shader_type == MOJOSHADER_TYPE_PIXEL ? *shader : *defaultPixel;
                std::vector<MOJOSHADER_vertexAttribute> attributes;
                for (int i = 0; i < v.attribute_count; ++i)
                    attributes.push_back({v.attributes[i].usage, MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR4, v.attributes[i].index});
                auto linked = GxVulkan::LinkShaders(v, p, attributes);
                Validate(linked.vertex);
                Validate(linked.pixel);
                ++passed;
            } catch (const std::exception& error) {
                if (profile == "ps_1_4") {
                    ++ps14;
                } else {
                    ++failed;
                    const std::string diagnostic = error.what();
                    std::cerr << entry.path() << ": " << diagnostic.substr(0, 1200);
                    if (diagnostic.size() > 1200) std::cerr << "\n[remaining diagnostics truncated]";
                    std::cerr << '\n';
                }
            }
        }
        std::cout << "Unique shaders: " << unique.size() << "; " << (argc == 3 ? "recovered/Slang-compiled" : "translated/linked");
#if defined(WOWDECOMP_VALIDATE_SPIRV)
        std::cout << "/validated";
#endif
        std::cout << ": " << passed << "; unsupported ps_1_4: " << ps14 << "; unexpected failures: " << failed << '\n';
        return failed || passed == 0 ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
