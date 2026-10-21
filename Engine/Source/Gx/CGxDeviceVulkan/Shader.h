#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SHADER_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SHADER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <mojoshader.h>

namespace GxVulkan {

struct ShaderDeleter {
    void operator()(const MOJOSHADER_parseData* shader) const;
};
using Shader = std::unique_ptr<const MOJOSHADER_parseData, ShaderDeleter>;

// The client stores stripped SM1-3 bytecode, not high-level shader sources.
// Translation failures throw with the frontend diagnostics; never substitute a shader.
Shader TranslateShader(const void* bytecode, size_t size);

// Offline migration path: recover HLSL-like code and mechanically adapt resource
// declarations to Slang. Names and source structure are generated, not original.
struct RecoveredShader {
    Shader reflection;
    std::string source;
};
RecoveredShader RecoverSlangShader(const void* bytecode, size_t size);
std::string RecoverSlangSource(const void* bytecode, size_t size);
// Preserve encoded C0 at target2 while optionally decoding final RGB into float C0.
// This is an output transport boundary, not reconstruction of material lighting.
std::string ShaderOutputBoundary(std::string source, bool decode = true);
// Pair linking may omit an unused scalar FOG export, without changing arithmetic.
bool ShaderUsesInputLocation(const std::vector<uint32_t>& code, uint32_t location);
bool ShaderUsesOutputLocation(const std::vector<uint32_t>& code, uint32_t location);
std::string PruneVertexFogOutput(std::string source);
// Prepend instance placement to the recovered Wrath detail-doodad program.
// Instance records use GxDetailDoodadInstance at set1/binding3.
std::string DetailDoodadInstancedSource(std::string source);
RecoveredShader RecoverFoglessSlangShader(const void* bytecode, size_t size, bool temporal);
// Optional motion variant: previous packed vertex registers at set1/binding1,
// clip-space jitter at set1/binding2; pixel options at set3/binding2 are
// (previous-pose validity, reactivity, previous viewport minDepth/maxDepth).
// Motion target: xy=current-minus-previous top-down UV, z=previous device depth
// ([0,1], -1 invalid), w=reactivity. Clear uncovered motion pixels to z=-1.
// Preserves the raster colour/alpha-test ABI and writes motion/validity to target1.
// Caller must match the same instance/geometry across frames. Unsnapshotted CPU
// deformation or vertex textures require invalid history, not reused constants.
RecoveredShader RecoverTemporalSlangShader(const void* bytecode, size_t size);
// Source-origin-neutral adaptation; never reselects/re-recovers a program.
std::string TemporalShaderSource(std::string source, bool vertex);

struct ShaderPair {
    std::vector<uint32_t> vertex;
    std::vector<uint32_t> pixel;
};

// Linking is layout-specific. Copy the frontend output before patching it so
// another vertex layout cannot mutate an already-created pipeline's inputs.
ShaderPair LinkShaders(const MOJOSHADER_parseData& vertex,
    const MOJOSHADER_parseData& pixel,
    const std::vector<MOJOSHADER_vertexAttribute>& attributes);

// A single backend binding/packing description, independent of source origin.
// Compatibility translation supplies sparse ranges; maintained source can
// explicitly expose the complete legacy float register file at the same binding.
struct ShaderRegisterRange { int first = 0; size_t count = 0; };
struct ShaderRegisterLiteral { int index = 0; std::array<float,4> value{}; };
struct ShaderBindings {
    uint32_t interfaceVersion = 1;
    std::vector<int> attributes;
    std::vector<int> samplers;
    std::vector<ShaderRegisterRange> registers;
    std::vector<ShaderRegisterLiteral> literals;
    bool previousRegisters = false, motionParameters = false;
    bool directionalVisibility = false, shadowFrame = false;
};
struct CompiledShader {
    std::vector<uint32_t> code;
    ShaderBindings bindings;
};
ShaderBindings LegacyShaderBindings(const MOJOSHADER_parseData& shader);
std::vector<float> ShaderUniforms(const ShaderBindings& bindings,
    const float* registers, size_t registerCount);

// Compatibility adapter; both source origins use the packing implementation above.
std::vector<float> ShaderUniforms(const MOJOSHADER_parseData& shader,
    const float* registers, size_t registerCount);

}
#endif
