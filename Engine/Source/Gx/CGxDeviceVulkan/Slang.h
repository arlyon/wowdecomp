#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SLANG_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_SLANG_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GxVulkan {
// Owned by the renderer thread. Compilation sessions are released after each
// shader; the global compiler session retains the shared standard library.
class SlangCompiler {
public:
    SlangCompiler();
    ~SlangCompiler();
    std::vector<uint32_t> Compile(const std::string& source, bool vertex);
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
#endif
