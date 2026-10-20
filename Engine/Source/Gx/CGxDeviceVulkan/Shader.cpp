#include "Shader.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace GxVulkan {
namespace {

uint32_t ReadWord(const void* data) {
    uint32_t word;
    std::memcpy(&word, data, sizeof(word));
    return word;
}

std::string Diagnostics(const MOJOSHADER_parseData& shader) {
    std::string result;
    for (int i = 0; i < shader.error_count; ++i) {
        if (!result.empty()) result += "; ";
        result += shader.errors[i].error;
    }
    return result;
}

std::vector<uint32_t> VertexRegisterTable(const void* data, size_t size) {
    // The archive strips CTAB debug/reflection metadata, but relative addressing
    // still indexes the D3D9 vertex float register file c[0..255]. Describe that
    // actual layout rather than guessing the source's named arrays. DEF constants
    // remain in the original instruction stream and are handled by the frontend.
    const uint32_t version = ReadWord(data);
    const uint32_t table[] = {
        0x42415443, // CTAB fourcc
        28, 64, version, 1, 28, 0, 0, // constant table header
        68, 2, 256, 48, 0, // name, FLOAT4/register 0, count, type, default value
        0x00030001, 0x00040001, 256, 0, // VECTOR/FLOAT, rows/columns, elements, members
        0x00007847, 0x00000063 // "Gx", "c"
    };
    static_assert(sizeof(table) == 76, "CTAB offsets must match the packed D3D layout");
    std::vector<uint32_t> result(size / 4 + 1 + sizeof(table) / 4);
    result[0] = version;
    result[1] = (uint32_t(sizeof(table) / 4) << 16) | 0xFFFE;
    std::memcpy(result.data() + 2, table, sizeof(table));
    std::memcpy(result.data() + 2 + sizeof(table) / 4,
        static_cast<const uint8_t*>(data) + 4, size - 4);
    return result;
}

std::vector<uint32_t> CopyOutput(const MOJOSHADER_parseData& shader) {
    if (shader.error_count || !shader.output || shader.output_len <= 0 || shader.output_len % 4)
        throw std::runtime_error("Invalid translated shader output");
    std::vector<uint32_t> words(static_cast<size_t>(shader.output_len) / 4);
    std::memcpy(words.data(), shader.output, shader.output_len);
    return words;
}

}

void ShaderDeleter::operator()(const MOJOSHADER_parseData* shader) const {
    MOJOSHADER_freeParseData(shader);
}

static Shader Translate(const void* bytecode, size_t size, const char* profile) {
    if (!bytecode || size < 8 || size % 4 || size > std::numeric_limits<unsigned int>::max())
        throw std::invalid_argument("Expected aligned SM1-3 shader bytecode");
    const uint32_t version = ReadWord(bytecode);
    if ((version >> 16 != 0xFFFE && version >> 16 != 0xFFFF) ||
        ((version >> 8) & 255) < 1 || ((version >> 8) & 255) > 3 ||
        ReadWord(static_cast<const uint8_t*>(bytecode) + size - 4) != 0xFFFF)
        throw std::invalid_argument("Expected SM1-3 shader version and END token");
    const auto parse = [profile](const void* data, size_t length) {
        return Shader(MOJOSHADER_parse(profile, "main",
            static_cast<const unsigned char*>(data), static_cast<unsigned int>(length),
            nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr));
    };
    Shader shader = parse(bytecode, size);
    if (!shader) throw std::bad_alloc();
    if (shader->error_count && version >> 16 == 0xFFFE &&
        Diagnostics(*shader).find("relative addressing unsupported without a CTAB") != std::string::npos) {
        const auto described = VertexRegisterTable(bytecode, size);
        shader = parse(described.data(), described.size() * 4);
        if (!shader) throw std::bad_alloc();
    }
    if (shader->error_count)
        throw std::runtime_error("Shader translation failed: " + Diagnostics(*shader));
    return shader;
}

Shader TranslateShader(const void* bytecode, size_t size) {
    return Translate(bytecode, size, MOJOSHADER_PROFILE_SPIRV);
}

RecoveredShader RecoverSlangShader(const void* bytecode, size_t size) {
    auto shader = Translate(bytecode, size, MOJOSHADER_PROFILE_HLSL);
    const bool vertex = shader->shader_type == MOJOSHADER_TYPE_VERTEX;
    std::string source(shader->output, static_cast<size_t>(shader->output_len));
    if (vertex) {
        // Legacy stages are compiled independently. D3D9 permits a pixel
        // declaration for an unwritten vertex register (its value is undefined),
        // whereas Vulkan requires a complete matching stage interface. Expose
        // the small Gx varying register file and initialise unwritten values.
        const auto begin = source.find("struct main_Output\n{");
        const auto end = source.find("};", begin);
        if (begin == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Missing generated vertex output interface");
        const auto fields = source.substr(begin, end - begin);
        std::string extra;
        for (int i = 0; i < 10; ++i) {
            const std::string semantic = i < 2 ? "COLOR" + std::to_string(i) : "TEXCOORD" + std::to_string(i - 2);
            if (fields.find(": " + semantic + ";") == std::string::npos)
                extra += "\tfloat4 m_gx_varying" + std::to_string(i) + " : " + semantic + ";\n";
        }
        if (fields.find(": FOG;") == std::string::npos) extra += "\tfloat m_gx_fog : FOG;\n";
        source.insert(end, extra);
    }
    std::istringstream lines(source);
    std::string result = "// Mechanically recovered from SM1-3 bytecode; not original source.\n";
    const std::regex texture(R"(^Texture(2D|3D|Cube) (s[0-9]+)_texture : register\(t([0-9]+)\);$)");
    const std::regex sampler(R"(^SamplerState s[0-9]+ : register\(s[0-9]+\);$)");
    const std::regex constantBuffer(R"(^cbuffer ([A-Za-z0-9_]+) : register\(b0\)$)");
    const std::regex semantic(R"(^(\s*(?:float|int|uint)[1-4]?\s+[A-Za-z0-9_]+)\s*:\s*(POSITION|BLENDWEIGHT|BLENDINDICES|NORMAL|COLOR|TEXCOORD|FOG)([0-9]*);$)");
    enum { NoInterface, InputInterface, OutputInterface } interface = NoInterface;
    for (std::string line; std::getline(lines, line);) {
        // MojoShader's HLSL array aliases include a trailing statement semicolon;
        // as expression macros that makes otherwise valid operands unparseable.
        if (line.find("#define ") != std::string::npos && !line.empty() && line.back() == ';') line.pop_back();
        if (line == "struct main_Input") interface = InputInterface;
        else if (line == "struct main_Output") interface = OutputInterface;
        else if (line == "};") interface = NoInterface;
        if (vertex && interface == OutputInterface)
            line = std::regex_replace(line, std::regex(R"(: PSIZE[0-9]*;)"), ": SV_PointSize;");
        std::smatch match;
        if (interface != NoInterface && std::regex_match(line, match, semantic)) {
            const auto usage = match[2].str();
            const int index = match[3].str().empty() ? 0 : std::stoi(match[3].str());
            int location = -1;
            if (vertex && interface == InputInterface) {
                if (usage == "POSITION") location = 0;
                else if (usage == "BLENDWEIGHT") location = 1;
                else if (usage == "BLENDINDICES") location = 2;
                else if (usage == "NORMAL") location = 3;
                else if (usage == "COLOR") location = 4 + index;
                else if (usage == "TEXCOORD") location = 6 + index;
            } else {
                if (usage == "COLOR") location = index;
                else if (usage == "TEXCOORD") location = 2 + index;
                else if (usage == "FOG") location = 10;
            }
            if (location < 0 || location >= 16) throw std::runtime_error("Unsupported shader interface semantic");
            result += "    [[vk::location(" + std::to_string(location) + ")]] " + match[1].str() +
                " : " + usage + match[3].str() + ";\n";
        } else if (std::regex_match(line, match, texture)) {
            result += "[[vk::binding(" + match[3].str() + ", " + (vertex ? "0" : "2") + ")]] Sampler" +
                match[1].str() + "<float4> " + match[2].str() + ";\n";
        } else if (std::regex_match(line, sampler)) {
            // Slang's combined sampler represents both original declarations.
        } else if (std::regex_match(line, match, constantBuffer)) {
            result += "[[vk::binding(0, " + std::string(vertex ? "1" : "3") + ")]] cbuffer " + match[1].str() + "\n";
        } else {
            result += line + '\n';
        }
    }
    // The frontend emits the sampler as the first argument to each texture
    // operation. Slang's combined sampler supplies that argument implicitly.
    result = std::regex_replace(result,
        std::regex(R"((s[0-9]+)_texture\.(Sample|SampleLevel|SampleBias|SampleGrad)\(\1, )"), "$1.$2(");
    if (vertex) {
        // Gx advertises its original ARGB colour packing (BGRA byte order).
        // Vulkan's portable R8G8B8A8 vertex fetch needs this semantic swizzle.
        for (int i = 0; i < shader->attribute_count; ++i) {
            const auto& attribute = shader->attributes[i];
            if (attribute.usage != MOJOSHADER_USAGE_COLOR || !attribute.name) continue;
            const std::string alias = "#define " + std::string(attribute.name) + " input.m_" + attribute.name;
            const auto position = result.find(alias);
            if (position != std::string::npos) result.insert(position + alias.size(), ".bgra");
        }
    }
    if (!vertex && result.find("m_oC0 : SV_Target0") != std::string::npos) {
        // D3D9's alpha test lives outside shader bytecode. Preserve that raster
        // contract explicitly instead of silently losing cut-out materials.
        result = "[[vk::binding(1, 3)]] cbuffer GxAlphaTest { float4 gxAlphaTest; };\n" + result;
        const auto position = result.rfind("return output;");
        if (position == std::string::npos) throw std::runtime_error("Missing generated pixel shader return");
        result.insert(position, "clip(output.m_oC0.a - gxAlphaTest.x);\n\t");
    }
    return {std::move(shader), std::move(result)};
}

std::string RecoverSlangSource(const void* bytecode, size_t size) {
    return RecoverSlangShader(bytecode, size).source;
}

ShaderPair LinkShaders(const MOJOSHADER_parseData& vertex,
    const MOJOSHADER_parseData& pixel,
    const std::vector<MOJOSHADER_vertexAttribute>& attributes) {
    if (vertex.shader_type != MOJOSHADER_TYPE_VERTEX || pixel.shader_type != MOJOSHADER_TYPE_PIXEL)
        throw std::invalid_argument("Shader pair requires vertex and pixel stages");
    // MojoShader's patch table has 16 entries per semantic. Validate before it
    // indexes the table using vertex input metadata from the Gx declaration.
    for (const auto& attribute : attributes)
        if (attribute.usage < MOJOSHADER_USAGE_POSITION || attribute.usage >= MOJOSHADER_USAGE_TOTAL ||
            attribute.usageIndex < 0 || attribute.usageIndex >= 16)
            throw std::invalid_argument("Invalid vertex input semantic");
    ShaderPair result{CopyOutput(vertex), CopyOutput(pixel)};
    MOJOSHADER_parseData v = vertex, p = pixel;
    v.output = reinterpret_cast<const char*>(result.vertex.data());
    p.output = reinterpret_cast<const char*>(result.pixel.data());
    const int patchBytes = MOJOSHADER_linkSPIRVShaders(&v, &p, attributes.data(), static_cast<int>(attributes.size()));
    if (patchBytes <= 0 || patchBytes % 4 || patchBytes >= v.output_len || patchBytes >= p.output_len)
        throw std::runtime_error("Invalid SPIR-V link patch table");
    result.vertex.resize(result.vertex.size() - patchBytes / 4);
    result.pixel.resize(result.pixel.size() - patchBytes / 4);
    return result;
}

static bool ShaderUsesLocation(const std::vector<uint32_t>& code,uint32_t location,uint32_t storageClass) {
    if(code.size()<5 || code[0]!=0x07230203u) throw std::runtime_error("Invalid SPIR-V interface header");
    std::vector<uint32_t> inputs,decorated;
    for(size_t i=5;i<code.size();) {
        const auto words=code[i]>>16,op=code[i]&0xffffu;
        if(!words || words>code.size()-i) throw std::runtime_error("Invalid SPIR-V interface instruction");
        // Decoration groups/alternate decoration encodings are not resolved
        // here. Unknown interface provenance must retain the export, not turn
        // an unfamiliar module into a false absence certificate.
        if(op==73 || op==74 || op==75 || op==332 || op==5632 || op==5633) return true;
        if(op==59 && words>=4 && code[i+3]==storageClass) inputs.push_back(code[i+2]); // OpVariable
        if(op==71 && words>=4 && code[i+2]==30 && code[i+3]==location) decorated.push_back(code[i+1]);
        // Do not guess the storage class of an unfamiliar interface block.
        if(op==72 && words>=5 && code[i+3]==30 && code[i+4]==location) return true;
        i+=words;
    }
    for(auto id:inputs) if(std::find(decorated.begin(),decorated.end(),id)!=decorated.end()) return true;
    return false;
}
bool ShaderUsesInputLocation(const std::vector<uint32_t>& code,uint32_t location) { return ShaderUsesLocation(code,location,1); }
bool ShaderUsesOutputLocation(const std::vector<uint32_t>& code,uint32_t location) { return ShaderUsesLocation(code,location,3); }
std::string PruneVertexFogOutput(std::string source) {
    const std::regex field(R"(\[\[vk::location\(10\)\]\]\s*float\s+(\w+)\s*:\s*FOG\s*;)");
    std::smatch match;
    if(!std::regex_search(source,match,field)) throw std::runtime_error("Unrecognised unused vertex FOG ABI");
    const auto name=match[1].str();source=std::regex_replace(source,field,"");
    if(name=="m_gx_fog") {
        // Our generated zero-filled compatibility padding has no bytecode alias.
        if(source.find("output."+name)!=std::string::npos) throw std::runtime_error("Unexpected padded FOG reference");
        return source;
    }
    if(name.rfind("m_",0)!=0) throw std::runtime_error("Unrecognised vertex FOG field name");
    const auto alias="#define "+name.substr(2)+" output."+name;
    const auto offset=source.find(alias);
    if(offset==std::string::npos || source.find(alias,offset+alias.size())!=std::string::npos)
        throw std::runtime_error("Unrecognised unused vertex FOG alias");
    source.replace(offset,alias.size(),"float gx_unusedFog;\n#define "+name.substr(2)+" gx_unusedFog");
    if(source.find("output."+name)!=std::string::npos) throw std::runtime_error("Unexpected vertex FOG reference");
    return source;
}

std::string ShaderOutputBoundary(std::string source, bool decode) {
    // Wrap only the generated entry's final colour. Do not inspect or alter its
    // lighting arithmetic, original clips, alpha or resource bindings.
    const std::regex entry(R"(main_Output main\((main_Input input)?\))");
    std::smatch match;
    size_t offset = std::string::npos;
    std::string args;
    for (std::sregex_iterator i(source.begin(), source.end(), entry), end; i != end; ++i) {
        offset = size_t(i->position());
        args = (*i)[1].str();
    }
    if (offset == std::string::npos) throw std::runtime_error("Unsupported HDR pixel entry ABI");
    const auto structure = source.rfind("struct main_Output", offset);
    const auto end = source.find("};", structure);
    if (structure == std::string::npos || end > offset) throw std::runtime_error("Unsupported HDR output structure");
    const auto fields = source.substr(structure, end - structure);
    const std::regex colour(R"(\bfloat4\s+(\w+)\s*:\s*SV_Target0\s*;)");
    if (!std::regex_search(fields, match, colour)) throw std::runtime_error("HDR requires float4 colour at target0");
    const auto field = match[1].str();
    if (std::regex_search(fields, std::regex(R"(SV_Target(?:[2-9]|[0-9]{2,})\b)")))
        throw std::runtime_error("HDR companion target2 collision or unsupported output");
    std::string explicitFields;
    const std::regex target(R"((\[\[vk::location\([01]\)\]\]\s*)?(float4\s+\w+\s*:\s*SV_Target([01])\s*;))");
    size_t cursor = 0;
    for (std::sregex_iterator i(fields.begin(), fields.end(), target), finish; i != finish; ++i) {
        explicitFields += fields.substr(cursor, size_t(i->position()) - cursor);
        if (!(*i)[1].matched) explicitFields += "[[vk::location(" + (*i)[3].str() + ")]] ";
        explicitFields += i->str();
        cursor = size_t(i->position() + i->length());
    }
    explicitFields += fields.substr(cursor);
    explicitFields += "[[vk::location(2)]] float4 gx_classicColour : SV_Target2;\n";
    source.replace(structure, end - structure, explicitFields);
    offset += explicitFields.size() - fields.size();
    source.replace(offset, std::string("main_Output main").size(), "main_Output gx_hdrLegacyOutput");
    source += "\nfloat gx_hdrDecode(float c) { if (!isfinite(c)) return 0; c=max(c,0); return c<=.04045 ? c/12.92 : pow((c+.055)/1.055,2.4); }\n"
        "main_Output main(" + args + ") { main_Output output=gx_hdrLegacyOutput(" + (args.empty() ? "" : "input") + ");\n"
        "output.gx_classicColour=output." + field + ";\n" +
        (decode ? "output." + field + ".rgb=float3(gx_hdrDecode(output." + field + ".r),gx_hdrDecode(output." + field + ".g),gx_hdrDecode(output." + field + ".b));\n" : "") + "return output; }\n";
    return source;
}

std::vector<float> ShaderUniforms(const MOJOSHADER_parseData& shader,
    const float* registers, size_t registerCount) {
    std::vector<float> result;
    for (int i = 0; i < shader.uniform_count; ++i) {
        const auto& uniform = shader.uniforms[i];
        if (uniform.constant) continue; // Literal arrays are embedded in the generated shader.
        const size_t count = static_cast<size_t>(std::max(uniform.array_count, 1));
        if (uniform.type != MOJOSHADER_UNIFORM_FLOAT)
            throw std::runtime_error("Unsupported integer/bool application shader constants");
        if (!registers || uniform.index < 0 || static_cast<size_t>(uniform.index) > registerCount ||
            count > registerCount - static_cast<size_t>(uniform.index))
            throw std::out_of_range("Shader uniform exceeds the Gx register file");
        const size_t packedOffset = result.size();
        result.insert(result.end(), registers + uniform.index * 4, registers + (uniform.index + count) * 4);
        // Relative arrays can span DEF instructions. D3D's immutable constants
        // override the application's register file even through dynamic indexing.
        for (int c = 0; c < shader.constant_count; ++c) {
            const auto& constant = shader.constants[c];
            if (constant.type == MOJOSHADER_UNIFORM_FLOAT && constant.index >= uniform.index &&
                static_cast<size_t>(constant.index - uniform.index) < count)
                std::copy_n(constant.value.f, 4, result.begin() + packedOffset + (constant.index - uniform.index) * 4);
        }
    }
    return result;
}

}
