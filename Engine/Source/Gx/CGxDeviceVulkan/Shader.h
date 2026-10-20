#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SHADER_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SHADER_H

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
// Optional motion variant: previous packed vertex registers at set1/binding1,
// clip-space jitter at set1/binding2; pixel options at set3/binding2 are
// (previous-pose validity, reactivity, previous viewport minDepth/maxDepth).
// Motion target: xy=current-minus-previous top-down UV, z=previous device depth
// ([0,1], -1 invalid), w=reactivity. Clear uncovered motion pixels to z=-1.
// Preserves the raster colour/alpha-test ABI and writes motion/validity to target1.
// Caller must match the same instance/geometry across frames. Unsnapshotted CPU
// deformation or vertex textures require invalid history, not reused constants.
RecoveredShader RecoverTemporalSlangShader(const void* bytecode, size_t size);

struct ShaderPair {
    std::vector<uint32_t> vertex;
    std::vector<uint32_t> pixel;
};

// Linking is layout-specific. Copy the frontend output before patching it so
// another vertex layout cannot mutate an already-created pipeline's inputs.
ShaderPair LinkShaders(const MOJOSHADER_parseData& vertex,
    const MOJOSHADER_parseData& pixel,
    const std::vector<MOJOSHADER_vertexAttribute>& attributes);

// Pack the sparse legacy register file into the frontend's uniform block order.
std::vector<float> ShaderUniforms(const MOJOSHADER_parseData& shader,
    const float* registers, size_t registerCount);

}
#endif
