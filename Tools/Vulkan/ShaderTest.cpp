#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
#include "Engine/Source/Gx/CGxDeviceVulkan/Slang.h"
#endif
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(WOWDECOMP_VALIDATE_SPIRV)
#include <spirv-tools/libspirv.h>
#endif

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void Reject(F&& function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "Invalid shader input was accepted");
}
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
// Synthetic SM2 programs: position passthrough and constant colour. No game data.
constexpr uint32_t vertex[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000,
    0x02000001, 0xC00F0000, 0x90E40000, 0x0000FFFF
};
constexpr uint32_t pixel[] = {
    0xFFFF0200, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF
};
constexpr uint32_t colourPointVertex[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000,
    0x0200001F, 0x8000000A, 0x900F0001,
    0x02000001, 0xC00F0000, 0x90E40000,
    0x02000001, 0xD00F0000, 0x90E40001,
    0x02000001, 0xC0010002, 0xA0000000, 0x0000FFFF
};
constexpr uint32_t relativeVertex[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000,
    0x0200002E, 0xB0010000, 0x90000000,
    0x03000001, 0xC00F0000, 0xA0E42000, 0xB0000000, 0x0000FFFF
};
}
int main() {
    try {
        using namespace GxVulkan;
        auto v = TranslateShader(vertex, sizeof(vertex));
        auto p = TranslateShader(pixel, sizeof(pixel));
        const auto recovered = RecoverSlangSource(pixel, sizeof(pixel));
        Check(recovered.find("[[vk::binding(0, 3)]] cbuffer") != std::string::npos,
            "Generated Slang lost the pixel uniform binding");
        const auto colourPointSource = RecoverSlangSource(colourPointVertex, sizeof(colourPointVertex));
        Check(colourPointSource.find("#define v1 input.m_v1.bgra") != std::string::npos,
            "Generated Slang did not decode the Gx BGRA vertex colour ABI");
        Check(colourPointSource.find("SV_PointSize") != std::string::npos,
            "Generated Slang lost the Vulkan point-size builtin");
        Check(colourPointSource.find("m_gx_varying9 : TEXCOORD7;") != std::string::npos,
            "Generated vertex interface omitted an unwritten legacy varying");
        Check(colourPointSource.find("m_gx_varying0") == std::string::npos,
            "Generated vertex interface duplicated a real colour output");
        const auto recoveredRelative = RecoverSlangSource(relativeVertex, sizeof(relativeVertex));
        Check(recoveredRelative.find("uniforms_float4[256]") != std::string::npos,
            "Generated Slang lost the relative register array");
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
        SlangCompiler compiler;
        Validate(compiler.Compile(recovered, false));
        Validate(compiler.Compile(RecoverSlangSource(vertex, sizeof(vertex)), true));
        Validate(compiler.Compile(recoveredRelative, true));
        Validate(compiler.Compile(colourPointSource, true));
        Reject([&] { compiler.Compile("this is not a shader", true); });
#endif
#if defined(WOWDECOMP_SLANG_TEST_DIR)
        for (const char* name : {"fixed.vert.spv", "fixed.frag.spv"}) {
            std::ifstream input(std::string(WOWDECOMP_SLANG_TEST_DIR) + "/" + name, std::ios::binary | std::ios::ate);
            const std::streamoff size = input.tellg();
            Check(input && size >= 20 && size % 4 == 0, "Slang shader output is missing or truncated");
            std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
            input.seekg(0);
            input.read(reinterpret_cast<char*>(words.data()), size);
            Check(static_cast<bool>(input), "Cannot read Slang shader output");
            Validate(words);
        }
#endif
        std::vector<char> original(v->output, v->output + v->output_len);
        const std::vector<MOJOSHADER_vertexAttribute> attributes{
            {MOJOSHADER_USAGE_POSITION, MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR3, 0}
        };
        const auto first = LinkShaders(*v, *p, attributes);
        const auto second = LinkShaders(*v, *p, attributes);
        Validate(first.vertex);
        Validate(first.pixel);
        Check(first.vertex == second.vertex && first.pixel == second.pixel, "Link is not repeatable");
        Check(std::memcmp(original.data(), v->output, original.size()) == 0, "Link mutated cached frontend output");
        Check(first.vertex.front() == 0x07230203 && first.pixel.front() == 0x07230203, "Missing SPIR-V header");
        Check(first.vertex.size() * 4 < static_cast<size_t>(v->output_len), "Patch table leaked into shader module");
        std::array<float, 256 * 4> registers{};
        registers[0] = 0.25f; registers[1] = 0.5f; registers[2] = 0.75f; registers[3] = 1.0f;
        const auto uniforms = ShaderUniforms(*p, registers.data(), 256);
        Check(uniforms == std::vector<float>(registers.begin(), registers.begin() + 4), "Uniform packing changed register values");
        auto relative = TranslateShader(relativeVertex, sizeof(relativeVertex));
        Check(relative->uniform_count > 0, "Relative registers lost their uniform storage");
        const auto array = ShaderUniforms(*relative, registers.data(), 256);
        Check(array.size() == 256 * 4, "Relative register array was truncated");
        std::vector<uint32_t> withConstant(std::begin(relativeVertex), std::end(relativeVertex));
        withConstant.insert(withConstant.begin() + 4, {0x05000051, 0xA00F0001,
            0x3F800000, 0x40000000, 0x40400000, 0x40800000});
        auto literal = TranslateShader(withConstant.data(), withConstant.size() * 4);
        const auto literalUniforms = ShaderUniforms(*literal, registers.data(), 256);
        Check(literalUniforms[4] == 1 && literalUniforms[5] == 2 && literalUniforms[6] == 3 && literalUniforms[7] == 4,
            "Application registers overrode a shader DEF constant through relative addressing");
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
        const auto literalSource = RecoverSlangShader(withConstant.data(), withConstant.size() * 4);
        Validate(compiler.Compile(literalSource.source, true));
        const auto hlslUniforms = ShaderUniforms(*literalSource.reflection, registers.data(), 256);
        Check(hlslUniforms[4] == 1 && hlslUniforms[5] == 2 && hlslUniforms[6] == 3 && hlslUniforms[7] == 4,
            "Generated Slang uniform packing lost immutable constants");
#endif
        const auto relativePair = LinkShaders(*relative, *p, attributes);
        Validate(relativePair.vertex);
        Validate(relativePair.pixel);
        Reject([&] { TranslateShader(nullptr, 0); });
        Reject([&] { TranslateShader(pixel, sizeof(pixel) - 1); });
        Reject([&] { TranslateShader(pixel, sizeof(pixel) - 4); });
        Reject([&] { LinkShaders(*p, *v, attributes); });
        Reject([&] { LinkShaders(*v, *p, {{MOJOSHADER_USAGE_POSITION, MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR3, 16}}); });
        Reject([&] { ShaderUniforms(*p, registers.data(), 0); });
        Reject([&] { ShaderUniforms(*relative, registers.data(), 255); });
        std::cout << "Vulkan shader translation, relative registers, linking and uniform packing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
