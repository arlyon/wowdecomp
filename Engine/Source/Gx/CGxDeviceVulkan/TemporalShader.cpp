#include "Shader.h"
#include <regex>
#include <stdexcept>

namespace GxVulkan {
namespace {
void AddFields(std::string& source, const char* structure, const std::string& fields) {
    const auto start = source.find(std::string("struct ") + structure + "\n{");
    const auto end = source.find("};", start);
    if (start == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Missing recovered temporal shader interface");
    const auto existing = source.substr(start,end-start);
    if (fields.find("gx_currentClip") != std::string::npos &&
        (std::regex_search(existing,std::regex(R"(vk::location\((11|12)\)|:\s*TEXCOORD[89]\s*;)"))))
        throw std::runtime_error("Legacy varying collides with temporal motion interface");
    source.insert(end, fields);
}
constexpr const char* motionInputs =
    "    [[vk::location(11)]] float4 gx_currentClip : TEXCOORD8;\n"
    "    [[vk::location(12)]] float4 gx_previousClip : TEXCOORD9;\n";

std::string TemporalVertex(std::string source) {
    std::smatch position;
    if (!std::regex_search(source, position, std::regex(R"(\bfloat4\s+([A-Za-z_]\w*)\s*:\s*SV_Position\s*;)")))
        throw std::runtime_error("Temporal vertex shader needs a clip-position output");
    const auto positionField = position[1].str();
    AddFields(source, "main_Output", motionInputs);
    const std::regex signature(R"(main_Output main\((main_Input input)?\))");
    std::smatch entry;
    if (!std::regex_search(source, entry, signature))
        throw std::runtime_error("Unsupported recovered temporal vertex entry point");
    const std::string arguments = entry[1].matched ? "input" : "";
    const auto offset = static_cast<size_t>(entry.position());
    std::string current = source.substr(offset), previous = current;
    current.replace(0, entry.length(), "main_Output gx_currentVertex(" + entry[1].str() + ")");
    previous.replace(0, entry.length(), "main_Output gx_previousVertex(" + entry[1].str() + ")");
    std::string previousConstants;
    const std::regex buffer(R"(\[\[vk::binding\(0, 1\)\]\] cbuffer ([A-Za-z_]\w*)\s*\{([\s\S]*?)\};)");
    std::smatch constants;
    if (std::regex_search(source, constants, buffer)) {
        previousConstants = "[[vk::binding(1, 1)]] cbuffer gx_PreviousUniforms\n{" + constants[2].str() + "};\n";
        const std::regex field(R"(\b(?:float|int|uint|bool)[1-4]?\s+([A-Za-z_]\w*)\s*(?:\[|;))");
        const auto body = constants[2].str();
        for (std::sregex_iterator i(body.begin(), body.end(), field), end; i != end; ++i) {
            const auto name = (*i)[1].str();
            const std::regex token("\\b" + name + "\\b");
            previousConstants = std::regex_replace(previousConstants, token, "gx_previous_" + name);
            previous = std::regex_replace(previous, token, "gx_previous_" + name);
        }
    }
    // Evaluate the original vertex program twice, not just its world matrix.
    // Previous bone palettes and all other packed registers retain their ABI.
    return source.substr(0, offset) + previousConstants +
        "[[vk::binding(2, 1)]] cbuffer GxTemporalJitter { float4 gx_temporalJitter; };\n" +
        current + "\n" + previous + "\nmain_Output main(" + entry[1].str() + ")\n{\n"
        "    main_Output output = gx_currentVertex(" + arguments + ");\n"
        "    main_Output previous = gx_previousVertex(" + arguments + ");\n"
        "    output.gx_currentClip = output." + positionField + ";\n"
        "    output.gx_previousClip = previous." + positionField + ";\n"
        "    output." + positionField + ".xy += gx_temporalJitter.xy * output." + positionField + ".w;\n"
        "    return output;\n}\n";
}
std::string TemporalPixel(std::string source) {
    if (std::regex_search(source, std::regex(R"(SV_Target[1-9])")))
        throw std::runtime_error("Temporal output requires a single legacy colour target");
    if (source.find("struct main_Input\n{") == std::string::npos) {
        const std::string signature = "main_Output main()";
        const auto entry = source.find(signature);
        if (entry == std::string::npos)
            throw std::runtime_error("Unsupported recovered temporal pixel entry point");
        source.replace(entry, signature.size(), "main_Output main(main_Input input)");
        source = "struct main_Input\n{\n};\n" + source;
    }
    AddFields(source, "main_Input", motionInputs);
    AddFields(source, "main_Output", "    float4 gx_motion : SV_Target1;\n");
    const auto end = source.rfind("return output;");
    if (end == std::string::npos) throw std::runtime_error("Missing recovered temporal pixel return");
    // xy: current minus previous, in top-down UV units, excluding jitter.
    // z: previous device depth in [0,1], or -1 for invalid history; w: reactivity.
    // Reprojected previous depth, not current depth, is needed when cameras or
    // actors move. Invalid history never asserts zero-velocity geometry.
    source.insert(end,
        "output.gx_motion = float4(0, 0, -1, saturate(gx_temporalOptions.y));\n"
        "    if (gx_temporalOptions.x > 0 && input.gx_currentClip.w > 0.00001 && input.gx_previousClip.w > 0.00001 &&\n"
        "        all(isfinite(input.gx_currentClip)) && all(isfinite(input.gx_previousClip)))\n"
        "    {\n"
        "        float2 delta = input.gx_currentClip.xy / input.gx_currentClip.w - input.gx_previousClip.xy / input.gx_previousClip.w;\n"
        "        float previousDepth = lerp(gx_temporalOptions.z, gx_temporalOptions.w, input.gx_previousClip.z / input.gx_previousClip.w);\n"
        "        if (all(isfinite(delta)) && all(abs(delta) <= 4) && isfinite(previousDepth) && previousDepth >= 0 && previousDepth <= 1)\n"
        "            output.gx_motion = float4(delta * float2(0.5, -0.5), previousDepth, saturate(gx_temporalOptions.y));\n"
        "    }\n    ");
    return "[[vk::binding(2, 3)]] cbuffer GxTemporalOptions { float4 gx_temporalOptions; };\n" + source;
}
}
RecoveredShader RecoverTemporalSlangShader(const void* bytecode, size_t size) {
    auto recovered = RecoverSlangShader(bytecode, size);
    recovered.source = recovered.reflection->shader_type == MOJOSHADER_TYPE_VERTEX ?
        TemporalVertex(std::move(recovered.source)) : TemporalPixel(std::move(recovered.source));
    return recovered;
}
}
