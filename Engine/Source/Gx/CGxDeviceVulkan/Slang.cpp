#include "Slang.h"
#include "ShaderOverride.h"
#include "Shader.h"
#include "SpirvNonUniform.h"
#include <slang.h>
#include <slang-com-ptr.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <set>
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace GxVulkan {
namespace {
bool Guid(const SlangUUID& a,const SlangUUID& b) { return std::memcmp(&a,&b,sizeof(a))==0; }
class SourceBlob final : public ISlangBlob {
    std::atomic<uint32_t> references{1};
    std::string bytes;
public:
    explicit SourceBlob(std::string value):bytes(std::move(value)){}
    SlangResult SLANG_MCALL queryInterface(const SlangUUID& id,void** out) override {
        if(Guid(id,ISlangBlob::getTypeGuid()) || Guid(id,ISlangUnknown::getTypeGuid())) {*out=static_cast<ISlangBlob*>(this);addRef();return SLANG_OK;}
        *out=nullptr;return SLANG_E_NO_INTERFACE;
    }
    uint32_t SLANG_MCALL addRef() override {return ++references;}
    uint32_t SLANG_MCALL release() override {const auto n=--references;if(!n)delete this;return n;}
    const void* SLANG_MCALL getBufferPointer() override {return bytes.data();}
    size_t SLANG_MCALL getBufferSize() override {return bytes.size();}
};
class SourceFileSystem final : public ISlangFileSystem {
    std::atomic<uint32_t> references{1};
    std::shared_ptr<ShaderSourceFiles> files;
    std::mutex mutex;
    std::atomic<bool> failed{false};
    std::string failure;
public:
    explicit SourceFileSystem(std::shared_ptr<ShaderSourceFiles> files):files(std::move(files)){}
    void* SLANG_MCALL castAs(const SlangUUID& id) override {
        if(Guid(id,ISlangFileSystem::getTypeGuid()) || Guid(id,ISlangCastable::getTypeGuid()) || Guid(id,ISlangUnknown::getTypeGuid()))return static_cast<ISlangFileSystem*>(this);
        return nullptr;
    }
    SlangResult SLANG_MCALL queryInterface(const SlangUUID& id,void** out) override {
        *out=castAs(id);if(!*out)return SLANG_E_NO_INTERFACE;addRef();return SLANG_OK;
    }
    uint32_t SLANG_MCALL addRef() override {return ++references;}
    uint32_t SLANG_MCALL release() override {const auto n=--references;if(!n)delete this;return n;}
    SlangResult SLANG_MCALL loadFile(const char* path,ISlangBlob** out) override {
        *out=nullptr;
        try {
            auto source=files->Read(path);
            if(!source)return SLANG_E_NOT_FOUND;
            *out=new SourceBlob(std::move(*source));return SLANG_OK;
        }catch(const std::exception& error) {
            failed=true;
            // Never allow exceptions (including diagnostic allocation failure)
            // across the compiler's COM boundary, or certify that access worked.
            try {std::lock_guard<std::mutex> lock(mutex);if(failure.empty())failure=error.what();}catch(...) {}
            return SLANG_FAIL;
        }catch(...) {failed=true;return SLANG_FAIL;}
    }
    void CheckAccess() {
        std::lock_guard<std::mutex> lock(mutex);
        if(failed)throw std::runtime_error("Confined shader source access: "+(failure.empty()?std::string("allocation failure"):failure));
    }
};
bool FloatVector(slang::TypeLayoutReflection* type,unsigned count=0) {
    return type && type->getKind()==slang::TypeReflection::Kind::Vector &&
        type->getScalarType()==slang::TypeReflection::ScalarType::Float32 &&
        type->getColumnCount()>=1 && type->getColumnCount()<=4 && (!count || type->getColumnCount()==count);
}
void RequireInterface(bool value,const char* reason) {
    if(!value)throw std::runtime_error(std::string("Maintained shader interface: ")+reason);
}
bool RegisterBlock(slang::TypeLayoutReflection* type,bool full) {
    auto* block=type->getElementTypeLayout();
    if(!block || block->getKind()!=slang::TypeReflection::Kind::Struct || block->getFieldCount()!=1)return false;
    auto* field=block->getFieldByIndex(0);auto* value=field->getTypeLayout();
    if(field->getOffset()!=0 || block->getSize()!=(full?4096u:16u))return false;
    if(!full)return FloatVector(value,4);
    return value->getKind()==slang::TypeReflection::Kind::Array && value->getElementCount()==256 && FloatVector(value->getElementTypeLayout(),4);
}
bool ShadowFrameBlock(slang::TypeLayoutReflection* type) {
    auto* block=type->getElementTypeLayout();
    if(!block||block->getKind()!=slang::TypeReflection::Kind::Struct||block->getFieldCount()!=1||block->getSize()!=112)return false;
    auto* field=block->getFieldByIndex(0);auto* value=field->getTypeLayout();
    return field->getOffset()==0&&value->getKind()==slang::TypeReflection::Kind::Array&&value->getElementCount()==7&&FloatVector(value->getElementTypeLayout(),4);
}
bool ShadowTriangles(slang::TypeLayoutReflection* type) {
    auto* element=type->getElementTypeLayout();
    if(!element||element->getKind()!=slang::TypeReflection::Kind::Struct||element->getFieldCount()!=3||element->getSize()!=48)return false;
    for(unsigned i=0;i<3;++i){auto* field=element->getFieldByIndex(i);auto* value=field->getTypeLayout();
        if(field->getOffset()!=i*16||value->getKind()!=slang::TypeReflection::Kind::Vector||value->getColumnCount()!=4||
            value->getScalarType()!=(i==2?slang::TypeReflection::ScalarType::UInt32:slang::TypeReflection::ScalarType::Float32))return false;
    }
    return true;
}
ShaderBindings MaintainedBindings(slang::ProgramLayout* layout,bool vertex,MaintainedShaderVariant variant) {
    RequireInterface(layout && layout->getEntryPointCount()==1,"one graphics entry point is required");
    ShaderBindings result;
    result.interfaceVersion=variant==MaintainedShaderVariant::Directional?2:1;
    bool alpha=false,position=false,colour=false,classic=false;
    unsigned shadowBindings=0;
    std::set<std::pair<unsigned,unsigned>> descriptors;
    for(unsigned i=0;i<layout->getParameterCount();++i) {
        auto* p=layout->getParameterByIndex(i);auto* type=p->getTypeLayout();
        const unsigned set=p->getBindingSpace(),binding=p->getBindingIndex();
        RequireInterface(descriptors.emplace(set,binding).second,"duplicate descriptor binding");
        if(set==4){
            RequireInterface(variant==MaintainedShaderVariant::Directional,"shadow resources require the directional source variant");
            RequireInterface(binding<4&&(!vertex||binding==2),"resource outside the directional stage interface");
            switch(binding){
            case 0:RequireInterface(type->getKind()==slang::TypeReflection::Kind::Resource&&type->getResourceShape()==SLANG_ACCELERATION_STRUCTURE,"expected shared triangle acceleration structure");break;
            case 1:RequireInterface((type->getResourceShape()&SLANG_RESOURCE_BASE_SHAPE_MASK)==SLANG_STRUCTURED_BUFFER&&type->getResourceAccess()==SLANG_RESOURCE_ACCESS_READ&&ShadowTriangles(type),"expected read-only 48-byte shared triangle coverage");break;
            case 2:RequireInterface(type->getKind()==slang::TypeReflection::Kind::ConstantBuffer&&ShadowFrameBlock(type),"expected explicit seven-float4 directional frame");result.shadowFrame=true;break;
            case 3:{
                // The pinned compiler reports unsized getElementCount() as zero
                // (specialized count INT_MAX); its descriptor binding range is -1.
                RequireInterface(type->getKind()==slang::TypeReflection::Kind::Array&&
                    type->getBindingRangeCount()==1&&type->getBindingRangeBindingCount(0)==-1,
                    "expected runtime coverage texture array");
                auto* element=type->getElementTypeLayout();
                RequireInterface(element->getKind()==slang::TypeReflection::Kind::Resource&&element->getResourceShape()==(SLANG_TEXTURE_2D|SLANG_TEXTURE_COMBINED_FLAG)&&element->getResourceAccess()==SLANG_RESOURCE_ACCESS_READ,"expected original read-only combined 2D coverage samplers");break;
            }
            }
            shadowBindings|=1u<<binding;continue;
        }
        if(type->getKind()==slang::TypeReflection::Kind::ConstantBuffer) {
            RequireInterface(set==(vertex?1u:3u) && binding<=2,"constant buffer binding is outside the Gx stage interface");
            const bool full=binding==0 || (vertex && binding==1);
            RequireInterface(RegisterBlock(type,full),"expected float4[256] register file or one reserved float4 block");
            if(binding==0)result.registers.push_back({0,256});
            if(vertex && binding==1)result.previousRegisters=true;
            if(binding==2)result.motionParameters=true;
            if(!vertex && binding==1)alpha=true;
        }else if(type->getKind()==slang::TypeReflection::Kind::Resource) {
            const auto shape=type->getResourceShape();const auto base=shape&SLANG_RESOURCE_BASE_SHAPE_MASK;
            RequireInterface(set==(vertex?0u:2u) && binding<(vertex?4u:16u),"sampler binding is outside the Gx stage interface");
            RequireInterface((shape&~SLANG_RESOURCE_BASE_SHAPE_MASK)==SLANG_TEXTURE_COMBINED_FLAG &&
                (base==SLANG_TEXTURE_2D || base==SLANG_TEXTURE_3D || base==SLANG_TEXTURE_CUBE) &&
                type->getResourceAccess()==SLANG_RESOURCE_ACCESS_READ,"only ordinary combined Gx texture samplers are supported");
            result.samplers.push_back(static_cast<int>(binding));
        }else RequireInterface(false,"unsupported global resource or implicit uniform");
    }
    if(variant==MaintainedShaderVariant::Directional){
        RequireInterface(shadowBindings==(vertex?4u:15u),"directional source must declare its complete shared query interface");
        result.directionalVisibility=!vertex;
    }
    auto* entry=layout->getEntryPointByIndex(0);
    std::function<void(slang::VariableLayoutReflection*,size_t)> input=[&](auto* value,size_t base) {
        auto* type=value->getTypeLayout();const size_t offset=value->getOffset(slang::ParameterCategory::VaryingInput);
        RequireInterface(offset<=14 && base<=14-offset,"vertex input location overflow");base+=offset;
        if(type->getKind()==slang::TypeReflection::Kind::Struct) {
            for(unsigned f=0;f<type->getFieldCount();++f)input(type->getFieldByIndex(f),base);
        }else {
            const char* semantic=value->getSemanticName();
            if(semantic && !std::strncmp(semantic,"SV_",3))return;
            RequireInterface(FloatVector(type) && base<14,"vertex inputs must be float vectors at Gx locations 0..13");
            RequireInterface(std::find(result.attributes.begin(),result.attributes.end(),int(base))==result.attributes.end(),"duplicate vertex input location");
            result.attributes.push_back(static_cast<int>(base));
        }
    };
    if(vertex)for(unsigned i=0;i<entry->getParameterCount();++i)input(entry->getParameterByIndex(i),0);
    std::function<void(slang::VariableLayoutReflection*)> output=[&](auto* value) {
        RequireInterface(value!=nullptr,"entry point must return graphics output");auto* type=value->getTypeLayout();
        if(type->getKind()==slang::TypeReflection::Kind::Struct) {
            for(unsigned f=0;f<type->getFieldCount();++f)output(type->getFieldByIndex(f));
        }else {
            const char* semantic=value->getSemanticName();
            if(vertex && semantic && !std::strcmp(semantic,"SV_POSITION")) {
                RequireInterface(FloatVector(type,4),"SV_Position must be float4");position=true;
            }else if(!vertex) {
                RequireInterface(semantic && !std::strcmp(semantic,"SV_TARGET") && value->getSemanticIndex()<3 && FloatVector(type,4),
                    "pixel output must be float4 at maintained Gx colour slots 0..2");
                colour|=value->getSemanticIndex()==0;
                classic|=value->getSemanticIndex()==2;
            }
        }
    };
    output(entry->getResultVarLayout());
    if(vertex)RequireInterface(position && std::find(result.attributes.begin(),result.attributes.end(),0)!=result.attributes.end(),
        "position input and SV_Position output are required");
    else {
        RequireInterface(colour && alpha,"C0 output and the reserved alpha-test block are required");
        if(variant==MaintainedShaderVariant::Directional)RequireInterface(classic,"directional source must preserve the Classic C2 companion");
    }
    return result;
}
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
std::vector<uint32_t> SlangCompiler::Compile(const std::string& source, bool vertex, const ShaderCompileContext& context) {
    return CompileProgram(source,vertex,context,nullptr);
}
CompiledShader SlangCompiler::CompileMaintained(const ShaderOverride& source,MaintainedShaderVariant variant) {
    CompiledShader result;
    RequireInterface(bool(source.files),"maintained source must carry its confined file snapshot");
    const bool vertex=source.identity.stage==ShaderStage::Vertex;
    RequireInterface(vertex || source.identity.stage==ShaderStage::Pixel,"unsupported stage");
    RequireInterface(variant==MaintainedShaderVariant::Raster||variant==MaintainedShaderVariant::Directional,"unknown maintained source variant");
    result.code=CompileProgram(source.source,vertex,{source.files,source.filename.generic_string(),variant},&result.bindings);
    return result;
}
std::vector<uint32_t> SlangCompiler::CompileProgram(const std::string& source,bool vertex,const ShaderCompileContext& context,ShaderBindings* bindings) {
    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = m_state->global->findProfile(context.variant==MaintainedShaderVariant::Directional?"spirv_1_5":"spirv_1_3");
    slang::SessionDesc description{};
    description.targets = &target; description.targetCount = 1;
    description.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    const slang::PreprocessorMacroDesc macros[]={{"GX_DIRECTIONAL_SHADOWS",context.variant==MaintainedShaderVariant::Directional?"1":"0"},{"GX_SHADER_VERTEX",vertex?"1":"0"}};
    description.preprocessorMacros=macros;description.preprocessorMacroCount=2;
    Slang::ComPtr<SourceFileSystem> fileSystem;
    std::string root;
    const char* searchPaths[1]{};
    if(context.files) {
        fileSystem.attach(new SourceFileSystem(context.files));
        description.fileSystem=fileSystem;
        root=context.files->Root().generic_string();searchPaths[0]=root.c_str();
        description.searchPaths=searchPaths;description.searchPathCount=1;
    }
    Slang::ComPtr<slang::ISession> session;
    Check(m_state->global->createSession(description, session.writeRef()), nullptr, "Create Slang compilation session");
    Slang::ComPtr<slang::IBlob> diagnostics;
    auto* module = session->loadModuleFromSourceString("gx-shader", context.filename.c_str(), source.c_str(), diagnostics.writeRef());
    if(fileSystem)fileSystem->CheckAccess();
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
    if(fileSystem)fileSystem->CheckAccess();
    Check(status, diagnostics, "Compile Slang shader");
    if (!code || code->getBufferSize() < 20 || code->getBufferSize() % 4)
        throw std::runtime_error("Slang produced invalid SPIR-V size");
    std::vector<uint32_t> words(code->getBufferSize() / 4);
    std::memcpy(words.data(), code->getBufferPointer(), code->getBufferSize());
    if (words[0] != 0x07230203) throw std::runtime_error("Slang produced invalid SPIR-V magic");
    if(bindings)*bindings=MaintainedBindings(linked->getLayout(),vertex,context.variant);
    return QualifyNonUniformResourceLoads(words);
}
}
