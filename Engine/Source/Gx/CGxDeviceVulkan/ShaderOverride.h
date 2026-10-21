#ifndef GX_VULKAN_SHADER_OVERRIDE_H
#define GX_VULKAN_SHADER_OVERRIDE_H
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace GxVulkan {
enum class ShaderStage { Vertex, Pixel };
enum class MaintainedShaderVariant { Raster, Directional };
struct ShaderIdentity {
    ShaderStage stage;
    std::string name, profile;
    uint32_t permutation = 0;
};
// Existing Gx keys are name or name:permutation; profile comes from the actual
// loaded SM1–3 record, not a material hash or a guessed lighting identity.
std::optional<ShaderIdentity> IdentifyShader(ShaderStage stage, const char* key, const void* bytecode, size_t bytes);
// The Linux Vulkan port pins a directory descriptor and opens every component
// without following symlinks. The same confined reader serves entry/includes.
// Loaded bytes are immutable for this shader generation; reload creates a new
// snapshot rather than mixing an edited include with an existing pipeline.
class ShaderSourceFiles {
public:
    explicit ShaderSourceFiles(const std::filesystem::path& root);
    ~ShaderSourceFiles();
    ShaderSourceFiles(const ShaderSourceFiles&) = delete;
    ShaderSourceFiles& operator=(const ShaderSourceFiles&) = delete;
    const std::filesystem::path& Root() const;
    std::optional<std::string> Read(const std::filesystem::path& path);
private:
    struct State;
    std::unique_ptr<State> state;
};
struct ShaderOverride {
    ShaderIdentity identity;
    std::filesystem::path filename;
    std::string source;
    std::shared_ptr<ShaderSourceFiles> files;
};
// Only absence returns nullopt. Present invalid/unreadable files, symlinks,
// escaping paths and excessive inputs are errors, never generation permission.
std::optional<ShaderOverride> ResolveShaderOverride(const std::filesystem::path& root, const ShaderIdentity& identity,
    MaintainedShaderVariant variant = MaintainedShaderVariant::Raster);
}
#endif
