#include "ShaderOverride.h"
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace GxVulkan {
namespace {
constexpr size_t FileLimit=4*1024*1024, SnapshotLimit=32*1024*1024;
class Fd {
public:
    explicit Fd(int value=-1):value(value){}
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    int Get()const{return value;}
    void Reset(int next){if(value>=0)close(value);value=next;}
private:int value;
};
[[noreturn]] void Fail(const char* operation,const std::filesystem::path& path) {
    throw std::system_error(errno,std::generic_category(),std::string(operation)+": "+path.generic_string());
}
void Component(const std::string& text) {
    if(text.empty() || text=="." || text=="..")throw std::invalid_argument("Empty or relative shader identity component");
    for(unsigned char c:text)if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))
        throw std::invalid_argument("Shader identity must use readable filename components");
}
}
std::optional<ShaderIdentity> IdentifyShader(ShaderStage stage,const char* key,const void* bytecode,size_t bytes) {
    if(!key || !*key)return std::nullopt; // Explicitly anonymous compatibility fixtures.
    if(!bytecode || bytes<4)throw std::invalid_argument("Shader identity requires its loaded version token");
    uint32_t token=0;std::memcpy(&token,bytecode,4);
    const bool vertex=stage==ShaderStage::Vertex;
    if((!vertex && stage!=ShaderStage::Pixel) || (token>>16)!=(vertex?0xfffeu:0xffffu))
        throw std::invalid_argument("Shader identity stage does not match the loaded record");
    const auto major=(token>>8)&255u,minor=token&255u;
    if(major<1 || major>3)throw std::invalid_argument("Unsupported loaded shader profile");
    ShaderIdentity id{stage,key,(vertex?"vs_":"ps_")+std::to_string(major)+"_"+std::to_string(minor),0};
    const auto colon=id.name.rfind(':');
    if(colon!=std::string::npos) {
        const auto first=id.name.data()+colon+1,last=id.name.data()+id.name.size();
        const auto parsed=std::from_chars(first,last,id.permutation);
        if(parsed.ec!=std::errc{} || parsed.ptr!=last || first==last)throw std::invalid_argument("Invalid Gx shader permutation key");
        id.name.resize(colon);
    }
    Component(id.name);return id;
}
struct ShaderSourceFiles::State {
    std::filesystem::path root;
    Fd directory;
    size_t bytes=0;
    std::mutex mutex;
    std::map<std::filesystem::path,std::string> loaded;
    explicit State(const std::filesystem::path& path):root(std::filesystem::weakly_canonical(std::filesystem::absolute(path))) {
        const int fd=open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0 && errno!=ENOENT)Fail("Open shader root",root);
        directory.Reset(fd);
    }
};
ShaderSourceFiles::ShaderSourceFiles(const std::filesystem::path& root):state(std::make_unique<State>(root)){}
ShaderSourceFiles::~ShaderSourceFiles()=default;
const std::filesystem::path& ShaderSourceFiles::Root()const{return state->root;}
std::optional<std::string> ShaderSourceFiles::Read(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto relative=(path.is_absolute()?path.lexically_normal().lexically_relative(state->root):path.lexically_normal());
    if(relative.empty() || relative==".")throw std::invalid_argument("Shader path names a directory");
    for(const auto& part:relative)if(part=="..")throw std::invalid_argument("Shader path escapes its configured root");
    const auto found=state->loaded.find(relative);
    if(found!=state->loaded.end())return found->second;
    if(state->directory.Get()<0)return std::nullopt;
    Fd parent(fcntl(state->directory.Get(),F_DUPFD_CLOEXEC,0));
    if(parent.Get()<0)Fail("Duplicate shader root",state->root);
    auto last=relative.end();--last;
    for(auto part=relative.begin();part!=last;++part) {
        const int next=openat(parent.Get(),part->c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(next<0){if(errno==ENOENT)return std::nullopt;Fail("Open shader directory",relative);}
        parent.Reset(next);
    }
    // NONBLOCK makes a hostile FIFO fail the regular-file check rather than hang.
    Fd file(openat(parent.Get(),last->c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    if(file.Get()<0){if(errno==ENOENT)return std::nullopt;Fail("Open shader source",relative);}
    struct stat before{},after{};
    if(fstat(file.Get(),&before)!=0)Fail("Stat shader source",relative);
    if(!S_ISREG(before.st_mode) || before.st_size<=0 || uint64_t(before.st_size)>FileLimit)
        throw std::runtime_error("Shader source must be a nonempty regular file of at most 4 MiB: "+relative.generic_string());
    const size_t size=static_cast<size_t>(before.st_size);
    if(size>SnapshotLimit-state->bytes)throw std::runtime_error("Shader include snapshot exceeds 32 MiB");
    std::string contents(size,'\0');size_t offset=0;
    while(offset<size) {
        const auto count=read(file.Get(),contents.data()+offset,size-offset);
        if(count<0){if(errno==EINTR)continue;Fail("Read shader source",relative);}
        if(!count)throw std::runtime_error("Shader source changed while reading: "+relative.generic_string());
        offset+=static_cast<size_t>(count);
    }
    if(fstat(file.Get(),&after)!=0)Fail("Restat shader source",relative);
    if(before.st_size!=after.st_size || before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec)
        throw std::runtime_error("Shader source changed while reading: "+relative.generic_string());
    if(contents.find('\0')!=std::string::npos)throw std::runtime_error("Shader source contains NUL bytes: "+relative.generic_string());
    state->loaded.emplace(relative,contents);state->bytes+=size;return contents;
}
std::optional<ShaderOverride> ResolveShaderOverride(const std::filesystem::path& root,const ShaderIdentity& identity) {
    Component(identity.name);Component(identity.profile);
    const char* stage=nullptr;
    switch(identity.stage) {
        case ShaderStage::Vertex:stage="vertex";break;
        case ShaderStage::Pixel:stage="pixel";break;
    }
    if(!stage)throw std::invalid_argument("Unsupported shader stage");
    auto files=std::make_shared<ShaderSourceFiles>(root);
    const auto filename=std::filesystem::path(stage)/identity.profile/identity.name/(std::to_string(identity.permutation)+".slang");
    auto source=files->Read(filename);
    if(!source)return std::nullopt;
    return ShaderOverride{identity,files->Root()/filename,std::move(*source),std::move(files)};
}
}
