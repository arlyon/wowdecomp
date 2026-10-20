#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SLANG_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SLANG_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GxVulkan {
class ShaderSourceFiles;
struct ShaderOverride;
struct ShaderBindings;
struct CompiledShader;
struct ShaderCompileContext {
    std::shared_ptr<ShaderSourceFiles> files;
    std::string filename = "gx-shader.slang";
};
// Owned by the renderer thread. Compilation sessions are released after each
// shader; the global compiler session retains the shared standard library.
class SlangCompiler {
public:
    SlangCompiler();
    ~SlangCompiler();
    std::vector<uint32_t> Compile(const std::string& source, bool vertex, const ShaderCompileContext& context = {});
    // Maintained source never invokes the bytecode frontend. Its compiled layout
    // must satisfy the explicit full-register binding interface.
    CompiledShader CompileMaintained(const ShaderOverride& source);
private:
    std::vector<uint32_t> CompileProgram(const std::string& source, bool vertex, const ShaderCompileContext& context, ShaderBindings* bindings);
    struct State;
    std::unique_ptr<State> m_state;
};
}
#endif
