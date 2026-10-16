#include "Slang.h"
#include <slang.h>
#include <slang-com-ptr.h>
#include <cstring>
#include <stdexcept>

namespace GxVulkan {
namespace {
void Check(SlangResult result, slang::IBlob* diagnostics, const char* operation) {
    if (SLANG_FAILED(result)) {
        std::string error = operation;
        if (diagnostics && diagnostics->getBufferSize())
            error += ": " + std::string(static_cast<const char*>(diagnostics->getBufferPointer()), diagnostics->getBufferSize());
        throw std::runtime_error(error);
    }
}
}
struct SlangCompiler::State {
    Slang::ComPtr<slang::IGlobalSession> global;
};
SlangCompiler::SlangCompiler() : m_state(std::make_unique<State>()) {
    Check(slang::createGlobalSession(m_state->global.writeRef()), nullptr, "Create Slang compiler");
}
SlangCompiler::~SlangCompiler() = default;
std::vector<uint32_t> SlangCompiler::Compile(const std::string& source, bool vertex) {
    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = m_state->global->findProfile("spirv_1_3");
    slang::SessionDesc description{};
    description.targets = &target; description.targetCount = 1;
    description.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    Slang::ComPtr<slang::ISession> session;
    Check(m_state->global->createSession(description, session.writeRef()), nullptr, "Create Slang compilation session");
    Slang::ComPtr<slang::IBlob> diagnostics;
    auto* module = session->loadModuleFromSourceString("gx-shader", "gx-shader.slang", source.c_str(), diagnostics.writeRef());
    if (!module) Check(SLANG_FAIL, diagnostics, "Load Slang shader");
    Slang::ComPtr<slang::IEntryPoint> entry;
    const auto result = module->findAndCheckEntryPoint("main", vertex ? SLANG_STAGE_VERTEX : SLANG_STAGE_FRAGMENT,
        entry.writeRef(), diagnostics.writeRef());
    Check(result, diagnostics, "Find Slang shader entry point");
    slang::IComponentType* components[] = {module, entry};
    Slang::ComPtr<slang::IComponentType> program, linked;
    auto status = session->createCompositeComponentType(components, 2, program.writeRef(), diagnostics.writeRef());
    Check(status, diagnostics, "Compose Slang shader");
    status = program->link(linked.writeRef(), diagnostics.writeRef());
    Check(status, diagnostics, "Link Slang shader");
    Slang::ComPtr<slang::IBlob> code;
    status = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
    Check(status, diagnostics, "Compile Slang shader");
    if (!code || code->getBufferSize() < 20 || code->getBufferSize() % 4)
        throw std::runtime_error("Slang produced invalid SPIR-V size");
    std::vector<uint32_t> words(code->getBufferSize() / 4);
    std::memcpy(words.data(), code->getBufferPointer(), code->getBufferSize());
    if (words[0] != 0x07230203) throw std::runtime_error("Slang produced invalid SPIR-V magic");
    return words;
}
}
