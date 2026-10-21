#ifndef ENGINE_SOURCE_GX_HDRSETTINGS_H
#define ENGINE_SOURCE_GX_HDRSETTINGS_H
#include <cstddef>
#include <cstring>

// The empty path means None. Paths are local file names, never URLs or markup.
inline bool GxHdrLutPathValid(const char* path) {
    if(!path) return false;
    size_t length=0;
    for(;path[length];++length) {
        const unsigned char c=path[length];
        if(length>=1024 || c<32 || c==127) return false;
    }
    return !length || (length>=5 && std::strcmp(path+length-5,".cube")==0);
}
#endif
