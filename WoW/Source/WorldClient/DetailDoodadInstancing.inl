// 15595's retained-mesh/instance-record mechanism, adapted to Wrath inputs.
// Keep mixed-model batches in their original order/path: splitting them could
// increase draw count and change equal-depth alpha-cutout ordering.
CDetailDoodad::InstancingStats CDetailDoodad::s_instancingStats;
void CDetailDoodad::FreeInstancedBuffer(CGxBuf*& buffer){
    if(!buffer)return;auto* pool=buffer->m_pool;
    g_theGxDevicePtr->BufDestroy(buffer);g_theGxDevicePtr->PoolDestroy(pool);
}
namespace {
CGxBuf* DetailBuffer(EGxPoolTarget target,uint32_t stride,uint32_t count){
    auto* gx=g_theGxDevicePtr;auto* pool=gx->PoolCreate(target,GxPoolUsage_Static,stride*count,static_cast<EGxPoolHintBits>(0),"Detail instancing");
    try{auto* buffer=gx->BufCreate(pool,stride,count,0);if(!buffer)throw std::runtime_error("Detail buffer allocation failed");return buffer;}
    catch(...){gx->PoolDestroy(pool);throw;}
}
bool PrepareDetailMesh(CDetailDoodadData& data){
    auto* shared=data.m_model->m_shared;auto* skin=shared->m_skinData;auto* model=shared->m_data;
    if(!skin || !model || !skin->m_vertices.m_count || !skin->m_indices.m_count || skin->m_vertices.m_count>65536)return false;
    const bool rebuilding=data.m_instanceSkin!=skin || !data.m_instanceVertexBuf || !data.m_instanceIndexBuf || !data.m_instanceVertexBuf->m_hasData || !data.m_instanceVertexBuf->m_valid || !data.m_instanceIndexBuf->m_hasData || !data.m_instanceIndexBuf->m_valid;
    const auto started=rebuilding?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    if(data.m_instanceSkin!=skin || !data.m_instanceVertexBuf || !data.m_instanceIndexBuf){
        CDetailDoodad::FreeInstancedBuffer(data.m_instanceVertexBuf);CDetailDoodad::FreeInstancedBuffer(data.m_instanceIndexBuf);data.m_instanceSkin=nullptr;
        try{data.m_instanceVertexBuf=DetailBuffer(GxPoolTarget_Vertex,sizeof(CGxVertexPNCT),skin->m_vertices.m_count);
            data.m_instanceIndexBuf=DetailBuffer(GxPoolTarget_Index,sizeof(uint16_t),skin->m_indices.m_count);data.m_instanceSkin=skin;}
        catch(...){CDetailDoodad::FreeInstancedBuffer(data.m_instanceVertexBuf);CDetailDoodad::FreeInstancedBuffer(data.m_instanceIndexBuf);throw;}
    }
    auto* gx=g_theGxDevicePtr;
    if(!data.m_instanceVertexBuf->m_hasData || !data.m_instanceVertexBuf->m_valid){
        auto* out=reinterpret_cast<CGxVertexPNCT*>(gx->BufLock(data.m_instanceVertexBuf));
        for(uint32_t i=0;i<skin->m_vertices.m_count;++i){
            const auto& source=model->m_vertices[skin->m_vertices[i]];
            out[i].m_position=source.m_position;out[i].m_normal=C3Vector(0,0,1);out[i].m_color=CImVector(255,255,255,255);out[i].m_texture=source.m_texcoord[0];
        }
        gx->BufUnlock(data.m_instanceVertexBuf,0);data.m_instanceVertexBuf->m_hasData=1;
        CDetailDoodad::s_instancingStats.meshBuiltBytes+=data.m_instanceVertexBuf->m_size;
    }
    if(!data.m_instanceIndexBuf->m_hasData || !data.m_instanceIndexBuf->m_valid){
        auto* out=reinterpret_cast<uint16_t*>(gx->BufLock(data.m_instanceIndexBuf));
        for(uint32_t i=0;i<skin->m_indices.m_count;++i)out[i]=skin->m_indices[i];
        gx->BufUnlock(data.m_instanceIndexBuf,0);data.m_instanceIndexBuf->m_hasData=1;
        CDetailDoodad::s_instancingStats.meshBuiltBytes+=data.m_instanceIndexBuf->m_size;
    }
    if(rebuilding)CDetailDoodad::s_instancingStats.meshBuildMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return true;
}
}
bool CDetailDoodadInstBatch::DrawInstanced(){
    auto* gx=g_theGxDevicePtr;
    if(!CDetailDoodad::s_bUseShaders || !gx->DetailDoodadInstancingSupported() || m_adds.Count()<2)return false;
    const auto id=m_adds[0].m_doodadId;
    for(uint32_t i=1;i<m_adds.Count();++i)if(m_adds[i].m_doodadId!=id)return false;
    auto* data=CDetailDoodad::s_doodadList[id];
    if(!data || !data->m_model || !(data->m_model->m_fFlags&1))return false;
    const auto* skin=data->m_model->m_shared->m_skinData;if(!skin)return false;
    const uint64_t meshBytes=uint64_t(skin->m_vertices.m_count)*sizeof(CGxVertexPNCT)+uint64_t(skin->m_indices.m_count)*2;
    if(meshBytes+uint64_t(m_adds.Count())*sizeof(GxDetailDoodadInstance)>=meshBytes*m_adds.Count() || !PrepareDetailMesh(*data))return false;
    if(!m_instanceBuf)m_instanceBuf=DetailBuffer(GxPoolTarget_Vertex,sizeof(GxDetailDoodadInstance),m_adds.Count());
    if(!m_instanceBuf->m_hasData || !m_instanceBuf->m_valid){
        const auto started=std::chrono::steady_clock::now();
        auto* out=reinterpret_cast<GxDetailDoodadInstance*>(gx->BufLock(m_instanceBuf));
        const C3Vector axis(0,0,1);C33Matrix rotation(1,0,0,0,1,0,0,0,1),matrices[4];uint32_t cell=0xFFFF;uint8_t built=0;
        for(uint32_t i=0;i<m_adds.Count();++i){
            const auto& add=m_adds[i];C33Matrix matrix(1,0,0,0,1,0,0,0,1);
            if(data->m_record->m_flags&1){
                if((add.m_triIndex&0xFC)!=cell){cell=add.m_triIndex&0xFC;built=0;}
                const auto fan=add.m_triIndex&3;
                if(!(built&(1<<fan))){
                    const auto& n=add.m_normal;const float inv=1.f/std::sqrt(n.y*n.y+n.z*n.z);
                    const C3Vector t(0, n.z*inv,-n.y*inv);
                    const C3Vector b(n.y*t.z-n.z*t.y,n.z*t.x-n.x*t.z,n.x*t.y-n.y*t.x);
                    const C33Matrix basis(b.x,b.y,b.z,t.x,t.y,t.z,n.x,n.y,n.z);
                    BuildInstanceMatrix(matrices[fan],rotation,basis,add.m_rotation,axis);built|=1<<fan;
                }
                matrix=matrices[fan];
            }else{const float c=std::cos(add.m_rotation),s=std::sin(add.m_rotation);matrix=C33Matrix(c,s,0,-s,c,0,0,0,1);}
            auto& instance=out[i];
            const float rows[3][4]={{matrix.a0,matrix.b0,matrix.c0,add.m_position.x},{matrix.a1,matrix.b1,matrix.c1,add.m_position.y},{matrix.a2,matrix.b2,matrix.c2,add.m_position.z}};
            std::memcpy(instance.rows,rows,sizeof(rows));
            const float normalScale[]={add.m_normal.x,add.m_normal.y,add.m_normal.z,add.m_scale};std::memcpy(instance.normalScale,normalScale,sizeof(normalScale));
            const auto c=add.m_color;const float colour[]={c.b/255.f,c.g/255.f,c.r/255.f,c.a/255.f};std::memcpy(instance.colour,colour,sizeof(colour));
        }
        gx->BufUnlock(m_instanceBuf,0);m_instanceBuf->m_hasData=1;
        CDetailDoodad::s_instancingStats.instanceBuildMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        CDetailDoodad::s_instancingStats.instanceBuiltBytes+=m_instanceBuf->m_size;
        CDetailDoodad::s_instancingStats.legacyEquivalentBytes+=uint64_t(m_vertexCount)*sizeof(CGxVertexPNCT)+uint64_t(m_indexCount)*2;
    }
    GxPrimVertexPtr(data->m_instanceVertexBuf,GxVBF_PNCT);gx->PrimIndexPtr(data->m_instanceIndexBuf);
    CGxBatch batch;batch.m_primType=GxPrim_Triangles;batch.m_start=0;batch.m_count=data->m_model->m_shared->m_skinData->m_indices.m_count;
    batch.m_minIndex=0;batch.m_maxIndex=data->m_model->m_shared->m_skinData->m_vertices.m_count-1;
    if(!gx->DrawDetailDoodadInstances(&batch,m_instanceBuf,m_adds.Count()))return false;
    ++CDetailDoodad::s_instancingStats.draws;CDetailDoodad::s_instancingStats.instances+=m_adds.Count();return true;
}
