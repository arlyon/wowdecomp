#include "FarAdtPrepared.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace FarAdt {
namespace {
uint32_t U32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
bool Span(const Tile& t, uint32_t offset, uint32_t bytes) { return offset<=t.encodedBytes && bytes<=t.encodedBytes-offset; }
uint16_t Corner(unsigned r,unsigned c) { return uint16_t(r*17+c); }
}
PrepareResult PrepareGeometry(const Tile& t, PreparedTile& out) {
    if (!t.encoded || !t.textureCount) return PrepareResult::Malformed;
    out.mapFlags=t.mapFlags; out.textureCount=out.indexCount=out.materialBytes=0;
    std::array<uint32_t,UsedTextures> ids{};
    for (unsigned ci=0;ci<256;++ci) {
        const auto& c=t.chunks[ci]; auto& d=out.chunks[ci];
        if (!c.layerCount || c.layerCount>4 || !Span(t,c.alphaOffset,c.alphaBytes) ||
            !Span(t,c.vertexShadingOffset,c.vertexShadingBytes) || !Span(t,c.shadowOffset,c.shadowBytes) ||
            (c.vertexShadingBytes && c.vertexShadingBytes!=580) || (c.shadowBytes && c.shadowBytes!=512)) return PrepareResult::Malformed;
        d.areaId=c.areaId; d.flags=c.flags; d.holes=c.holes; d.layerCount=c.layerCount; d.layers=c.layers;
        auto vertex=[&](unsigned index,unsigned hr,unsigned hc) {
            auto& v=out.vertices[ci*145+index];
            v.position[0]=c.position[0]-hr*(TileSize/256.f);
            v.position[1]=c.position[1]-hc*(TileSize/256.f);
            v.position[2]=c.position[2]+c.heights[index];
            for(unsigned a=0;a<3;++a) v.normal[a]=c.normals[index*3+a]/127.f;
            v.halfColumn=uint16_t(hc); v.halfRow=uint16_t(hr);
            if(c.vertexShadingBytes) { const auto* p=t.encoded.get()+c.vertexShadingOffset+index*4;
                v.shading[0]=p[2];v.shading[1]=p[1];v.shading[2]=p[0];v.shading[3]=p[3];
            } else {v.shading[0]=v.shading[1]=v.shading[2]=127;v.shading[3]=255;}
        };
        for(unsigned r=0;r<9;++r) {
            for(unsigned col=0;col<9;++col) vertex(r*17+col,r*2,col*2);
            if(r<8) for(unsigned col=0;col<8;++col) vertex(r*17+9+col,r*2+1,col*2+1);
        }
        for(unsigned i=0;i<9;++i) {
            d.edges[0][i]=Corner(0,i);d.edges[1][i]=Corner(i,8);
            d.edges[2][i]=Corner(8,i);d.edges[3][i]=Corner(i,0);
        }
        d.coarseError=0;
        auto tri=[&](uint16_t a,uint16_t b,uint16_t c) {
            const uint16_t base=uint16_t(ci*145);
            out.indices[out.indexCount++]=base+a;out.indices[out.indexCount++]=base+b;out.indices[out.indexCount++]=base+c;
        };
        for(unsigned lod=0;lod<2;++lod) {
            d.firstIndex[lod]=out.indexCount;
            for(unsigned r=0;r<8;++r) for(unsigned col=0;col<8;++col) {
                if(c.holes & (1u<<((r/2)*4+col/2))) continue;
                const uint16_t tl=Corner(r,col),tr=tl+1,bl=tl+17,br=tl+18,mid=tl+9;
                if(!lod) {tri(mid,tl,bl);tri(mid,tr,tl);tri(mid,br,tr);tri(mid,bl,br);}
                else {tri(tl,bl,br);tri(tl,br,tr);
                    d.coarseError=std::max(d.coarseError,std::abs(c.heights[mid]-(c.heights[tl]+c.heights[br])*0.5f));}
            }
            d.indexCount[lod]=out.indexCount-d.firstIndex[lod];
        }
        d.lodError[0]=0;d.lodError[1]=d.coarseError;
        const auto hole=[&](int r,int col) {return r>=0 && col>=0 && r<8 && col<8 && (c.holes&(1u<<((r/2)*4+col/2)));};
        for(unsigned lod=2;lod<5;++lod) {
            d.firstIndex[lod]=out.indexCount;d.lodError[lod]=0;
            const auto patch=[&](auto&& self,unsigned row,unsigned col,unsigned step)->void {
                bool any=false;
                for(unsigned y=row;y<row+step;y+=2) for(unsigned x=col;x<col+step;x+=2) any=any||hole(y,x);
                if(any) {
                    if(step==2)return;
                    for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x) self(self,row+y*step/2,col+x*step/2,step/2);
                    return;
                }
                uint16_t boundary[32];unsigned n=0;
                // Clockwise in row/column space => positive world-z winding.
                for(unsigned edge=0;edge<4;++edge) for(unsigned i=0;i<step;++i) {
                    unsigned r=row,cc=col;
                    if(edge==0) r+=i;
                    else if(edge==1) {r+=step;cc+=i;}
                    else if(edge==2) {r+=step-i;cc+=step;}
                    else cc+=step-i;
                    const bool exterior=r==0 || r==8 || cc==0 || cc==8;
                    const bool besideHole=hole(int(r)-1,int(cc)-1)||hole(int(r)-1,cc)||hole(r,int(cc)-1)||hole(r,cc);
                    if(exterior || besideHole || !(i&1)) boundary[n++]=Corner(r,cc);
                }
                if(n==4) {tri(boundary[0],boundary[1],boundary[2]);tri(boundary[0],boundary[2],boundary[3]);}
                else {
                    const uint16_t center=Corner(row+step/2,col+step/2);
                    for(unsigned i=0;i<n;++i)tri(center,boundary[i],boundary[(i+1)%n]);
                }
                // Both fine and coarse surfaces are convex interpolants of
                // heights inside this patch. Range is conservative even at
                // crossings of their different triangulations (not just points).
                float low=c.heights[Corner(row,col)],high=low;
                for(unsigned y=row;y<=row+step;++y) {
                    for(unsigned x=col;x<=col+step;++x) {const float h=c.heights[Corner(y,x)];low=std::min(low,h);high=std::max(high,h);}
                    if(y<row+step)for(unsigned x=col;x<col+step;++x) {const float h=c.heights[y*17+9+x];low=std::min(low,h);high=std::max(high,h);}
                }
                d.lodError[lod]=std::max(d.lodError[lod],high-low);
            };
            const unsigned step=1u<<(lod-1);
            for(unsigned row=0;row<8;row+=step)for(unsigned col=0;col<8;col+=step)patch(patch,row,col,step);
            d.indexCount[lod]=out.indexCount-d.firstIndex[lod];
        }
        for(unsigned i=0;i<4096;++i) {
            auto* pixel=out.blend.data()+BlendOffset(ci,i%64,i/64);
            pixel[0]=pixel[1]=pixel[2]=0;
            const bool shadow=c.shadowBytes && (t.encoded[c.shadowOffset+i/8]&(1u<<(i&7)));
            pixel[3]=shadow?0:255;
        }
        for(unsigned l=0;l<4;++l) {
            if(l>=c.layerCount) continue;
            uint32_t id=c.layers[l].textureId, local=0;
            if(id>=t.textureCount) return PrepareResult::Malformed;
            for(;local<out.textureCount && ids[local]!=id;++local) {}
            if(local==out.textureCount) {
                if(out.textureCount==UsedTextures) return PrepareResult::Malformed;
                ids[out.textureCount++]=id;
            }
            d.layers[l].textureId=local;
        }
        for(unsigned l=1;l<4;++l) {
            uint8_t decoded[4096]{};
            if(l<c.layerCount && (c.layers[l].flags&0x100)) {
                const auto& layer=c.layers[l];
                if(layer.alphaOffset>=c.alphaBytes) return PrepareResult::Malformed;
                size_t at=layer.alphaOffset, end=c.alphaBytes;
                for(unsigned k=l+1;k<c.layerCount;++k) if(c.layers[k].flags&0x100) {end=c.layers[k].alphaOffset;break;}
                if(end>c.alphaBytes || at>=end) return PrepareResult::Malformed;
                const auto* p=t.encoded.get()+c.alphaOffset;
                if(layer.flags&0x200) {
                    size_t n=0;
                    while(n<4096) {
                        if(at==end) return PrepareResult::Malformed;
                        unsigned control=p[at++], count=control&127, input=control&128?1:count;
                        if(!count || count>4096-n || input>end-at) return PrepareResult::Malformed;
                        for(unsigned j=0;j<count;++j) decoded[n++]=p[at+(control&128?0:j)];
                        at+=input;
                    }
                } else if(t.mapFlags&4) {
                    if(end-at<4096) return PrepareResult::Malformed;
                    std::memcpy(decoded,p+at,4096);
                } else {
                    if(end-at<2048) return PrepareResult::Malformed;
                    for(unsigned i=0;i<4096;++i) decoded[i]=uint8_t(((p[at+i/2]>>((i&1)*4))&15)*17);
                }
                if(!(t.mapFlags&4) && !(c.flags&0x8000)) {
                    for(unsigned y=0;y<64;++y) decoded[y*64+63]=decoded[y*64+62];
                    std::memcpy(decoded+63*64,decoded+62*64,64);
                }
            }
            for(unsigned i=0;i<4096;++i) out.blend[BlendOffset(ci,i%64,i/64)+l-1]=decoded[i];
        }
        for(unsigned y=0;y<64;++y) {
            const auto left=BlendOffset(ci,0,y),right=BlendOffset(ci,63,y);
            std::memcpy(out.blend.data()+left-4,out.blend.data()+left,4);
            std::memcpy(out.blend.data()+right+4,out.blend.data()+right,4);
        }
        const auto top=BlendOffset(ci,0,0)-4,bottom=BlendOffset(ci,0,63)-4;
        std::memcpy(out.blend.data()+top-BlendAtlasSide*4,out.blend.data()+top,BlendChunkStride*4);
        std::memcpy(out.blend.data()+bottom+BlendAtlasSide*4,out.blend.data()+bottom,BlendChunkStride*4);
    }
    if(!Span(t,t.textureNamesOffset,t.textureNamesBytes)) return PrepareResult::Malformed;
    size_t at=t.textureNamesOffset,end=at+t.textureNamesBytes;
    for(uint32_t id=0;id<t.textureCount;++id) {
        if(at>=end) return PrepareResult::Malformed;
        auto* begin=t.encoded.get()+at;
        auto* zero=static_cast<const uint8_t*>(std::memchr(begin,0,end-at));
        if(!zero || zero==begin || size_t(zero-begin)>=260) return PrepareResult::Malformed;
        for(uint32_t i=0;i<out.textureCount;++i) if(ids[i]==id) {
            auto& tex=out.textures[i];tex.sourceId=id;
            std::memcpy(tex.name.data(),begin,size_t(zero-begin)+1);
            if(t.textureFlagsBytes) {
                if(!Span(t,t.textureFlagsOffset+id*4,4)) return PrepareResult::Malformed;
                tex.flags=U32(t.encoded.get()+t.textureFlagsOffset+id*4);
            }
        }
        at+=size_t(zero-begin)+1;
    }
    return PrepareResult::Ready;
}
PrepareResult PrepareTextureHeader(const uint8_t* h,size_t bytes,uint32_t fileBytes,
    uint32_t textureCount,PreparedTexture& t,MipRequest& request) {
    if(!h || bytes<148 || U32(h)!=0x32504c42 || U32(h+4)!=1) return PrepareResult::Unsupported;
    uint32_t width=U32(h+12),height=U32(h+16);
    if(!width || !height || !textureCount || textureCount>UsedTextures) return PrepareResult::Malformed;
    if(h[8]==2) {
        if(h[10]==0 && h[9]<=1) t.encoding=AlbedoEncoding::BC1;
        else if(h[10]==1 && h[9]==4) t.encoding=AlbedoEncoding::BC2;
        else if(h[10]==7 && h[9]==8) t.encoding=AlbedoEncoding::BC3;
        else return PrepareResult::Unsupported;
    } else if(h[8]==1 || h[8]==3) {
        if(h[8]==1 && h[9]!=0 && h[9]!=1 && h[9]!=4 && h[9]!=8) return PrepareResult::Unsupported;
        if(h[8]==1 && bytes<1172) return PrepareResult::Malformed;
        t.encoding=AlbedoEncoding::RGBA8;
    } else return PrepareResult::Unsupported;
    const size_t quota=MaterialBytes/textureCount;
    bool fallback=false;
    for(unsigned mip=0;mip<16;++mip) {
        if(mip && !(h[11]&15)) break;
        const uint32_t w=std::max(1u,width>>mip),hh=std::max(1u,height>>mip);
        const uint32_t offset=U32(h+20+mip*4),size=U32(h+84+mip*4);
        if(!offset || !size) continue;
        const uint64_t pixels=uint64_t(w)*hh;
        if(pixels>uint64_t(UINT32_MAX)*2) return PrepareResult::Malformed;
        const uint64_t encoded=h[8]==1?pixels+(pixels*h[9]+7)/8:h[8]==3?pixels*4:
            ((uint64_t(w)+3)/4)*((uint64_t(hh)+3)/4)*(t.encoding==AlbedoEncoding::BC1?8:16);
        const uint64_t output=t.encoding==AlbedoEncoding::RGBA8?pixels*4:encoded;
        if(offset<(h[8]==1?1172u:148u) || offset>fileBytes || size>fileBytes-offset || encoded!=size) return PrepareResult::Malformed;
        // Select a genuine authored mip under the shared bounded material span.
        // No full-file read, no absent-asset fiction for large source textures.
        if(w>64 || hh>64 || output>quota) {
            // Remember the coarsest genuine source mip for sparse fallback.
            t.sourceWidth=w;t.sourceHeight=hh;t.mip=mip;
            request={offset,size,fileBytes,h[8]==1?1172u:148u,true};fallback=true;continue;
        }
        t.sourceWidth=w;t.sourceHeight=hh;t.reduced=false;
        t.width=w;t.height=hh;t.mip=mip;t.bytes=uint32_t(output);
        request={offset,size,fileBytes,h[8]==1?1172u:148u};
        return PrepareResult::Ready;
    }
    if(!fallback) return PrepareResult::Malformed;
    t.width=t.sourceWidth;t.height=t.sourceHeight;
    while(t.width>64 || t.height>64 || uint64_t(t.width)*t.height*4>quota) {
        t.width=std::max(1u,t.width/2);t.height=std::max(1u,t.height/2);
    }
    t.encoding=AlbedoEncoding::RGBA8;t.reduced=true;t.bytes=t.width*t.height*4;
    return PrepareResult::Ready;
}
PrepareResult PrepareTextureMip(const uint8_t* h,const uint8_t* mip,size_t bytes,PreparedTexture& t,uint8_t* out,size_t capacity) {
    if(!h || !mip || t.bytes>capacity) return PrepareResult::Malformed;
    if(t.encoding!=AlbedoEncoding::RGBA8) {
        if(bytes!=t.bytes) return PrepareResult::Malformed;
        std::memcpy(out,mip,bytes); return PrepareResult::Ready;
    }
    const size_t pixels=size_t(t.width)*t.height;
    if(h[8]==3) {
        if(bytes!=pixels*4) return PrepareResult::Malformed;
        for(size_t i=0;i<pixels;++i) {out[i*4]=mip[i*4+2];out[i*4+1]=mip[i*4+1];out[i*4+2]=mip[i*4];out[i*4+3]=mip[i*4+3];}
    } else {
        if(bytes!=pixels+(pixels*h[9]+7)/8) return PrepareResult::Malformed;
        for(size_t i=0;i<pixels;++i) {
            const auto* p=h+148+mip[i]*4;
            out[i*4]=p[2];out[i*4+1]=p[1];out[i*4+2]=p[0];
            out[i*4+3]=h[9]?uint8_t(((mip[pixels+(i*h[9])/8]>>((i*h[9])%8))&((1u<<h[9])-1))*255/((1u<<h[9])-1)):255;
        }
    }
    return PrepareResult::Ready;
}
namespace {
std::array<uint32_t,2> SampleCoordinate(const PreparedTexture& t,uint32_t sample) {
    return {uint32_t((uint64_t(sample%t.width)*2+1)*t.sourceWidth/(uint64_t(t.width)*2)),
            uint32_t((uint64_t(sample/t.width)*2+1)*t.sourceHeight/(uint64_t(t.height)*2))};
}
uint16_t U16(const uint8_t* p) {return uint16_t(p[0])|uint16_t(p[1])<<8;}
std::array<uint8_t,3> RGB565(uint16_t v) {
    return {uint8_t(((v>>11)*255+15)/31),uint8_t((((v>>5)&63)*255+31)/63),uint8_t(((v&31)*255+15)/31)};
}
}
MipRequest TextureSampleRequest(const uint8_t* h,const PreparedTexture& t,const MipRequest& base,uint32_t sample,bool alpha) {
    auto xy=SampleCoordinate(t,sample);const uint64_t pixel=uint64_t(xy[1])*t.sourceWidth+xy[0];
    MipRequest r=base;
    if(h[8]==1) {r.offset+=uint32_t(alpha?uint64_t(t.sourceWidth)*t.sourceHeight+pixel*h[9]/8:pixel);r.bytes=1;}
    else if(h[8]==3) {r.offset+=uint32_t(pixel*4);r.bytes=4;}
    else {r.bytes=h[10]==0?8:16;r.offset+=uint32_t((uint64_t(xy[1]/4)*((t.sourceWidth+3)/4)+xy[0]/4)*r.bytes);}
    return r;
}
PrepareResult PrepareTextureSample(const uint8_t* h,const uint8_t* b,size_t count,const PreparedTexture& t,uint32_t sample,uint8_t paletteIndex,uint8_t* out) {
    auto xy=SampleCoordinate(t,sample);const uint64_t pixel=uint64_t(xy[1])*t.sourceWidth+xy[0];
    if(h[8]==1) {
        if(count!=1)return PrepareResult::Malformed;
        const uint8_t index=h[9]?paletteIndex:b[0];auto* p=h+148+index*4;
        out[0]=p[2];out[1]=p[1];out[2]=p[0];
        out[3]=h[9]?uint8_t(((b[0]>>((pixel*h[9])%8))&((1u<<h[9])-1))*255/((1u<<h[9])-1)):255;
    } else if(h[8]==3) {
        if(count!=4)return PrepareResult::Malformed;
        out[0]=b[2];out[1]=b[1];out[2]=b[0];out[3]=b[3];
    } else {
        const bool bc1=h[10]==0,bc2=h[10]==1;
        if(count!=(bc1?8u:16u)) return PrepareResult::Malformed;
        const auto* c=b+(bc1?0:8);const uint16_t e0=U16(c),e1=U16(c+2);
        std::array<std::array<uint8_t,3>,4> colors{RGB565(e0),RGB565(e1),{}, {}};
        for(unsigned k=0;k<3;++k) {
            colors[2][k]=uint8_t(bc1 && e0<=e1?(unsigned(colors[0][k])+colors[1][k])/2:(2u*colors[0][k]+colors[1][k])/3);
            colors[3][k]=uint8_t(bc1 && e0<=e1?0:(unsigned(colors[0][k])+2u*colors[1][k])/3);
        }
        const unsigned slot=(xy[1]&3)*4+(xy[0]&3),index=(U32(c+4)>>(slot*2))&3;
        for(unsigned k=0;k<3;++k)out[k]=colors[index][k];
        out[3]=bc1 && e0<=e1 && index==3?0:255;
        if(bc2) out[3]=uint8_t(((b[slot/2]>>((slot&1)*4))&15)*17);
        else if(!bc1) {
            uint8_t a[8]={b[0],b[1],0,0,0,0,0,255};
            if(a[0]>a[1])for(unsigned i=1;i<7;++i)a[i+1]=uint8_t(((7-i)*unsigned(a[0])+i*unsigned(a[1]))/7);
            else for(unsigned i=1;i<5;++i)a[i+1]=uint8_t(((5-i)*unsigned(a[0])+i*unsigned(a[1]))/5);
            uint64_t bits=0;for(unsigned i=0;i<6;++i)bits|=uint64_t(b[i+2])<<(i*8);
            out[3]=a[(bits>>(slot*3))&7];
        }
    }
    return PrepareResult::Ready;
}
std::array<float,2> LayerUvOffset(uint32_t flags,std::array<float,2> v) {
    if(!(flags&0x40)) return {};
    constexpr float rates[8]={64,48,32,16,8,4,2,1};
    // MapRenderChunk animation translation: 1/geoToTex/rate, geoToTex=0.24.
    const float scale=(TileSize/128.f)/rates[(flags>>3)&7];
    return {v[0]*scale,v[1]*scale};
}
}
