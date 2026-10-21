#ifndef WOW_SOURCE_WORLDCLIENT_CDETAILDOODADDATA_H
#define WOW_SOURCE_WORLDCLIENT_CDETAILDOODADDATA_H

#include <cstddef>
#include <cstdint>

class CM2Model;
struct CGxBuf;
class CTexture;
class GroundEffectDoodadRec;

class CDetailDoodadData {
    public:
    static void ModelLoadedCallback(CM2Model* model, void* arg);

    /* 0000 */ GroundEffectDoodadRec* m_record;
    /* 0004 */ CM2Model* m_model;
    /* 0008 */ CTexture* m_texture;
    CGxBuf* m_instanceVertexBuf=nullptr;
    CGxBuf* m_instanceIndexBuf=nullptr;
    const void* m_instanceSkin=nullptr;

    int32_t Load();
};

#endif
