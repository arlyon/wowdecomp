#ifndef WOW_WORLDCLIENT_FARADTRENDER_H
#define WOW_WORLDCLIENT_FARADTRENDER_H
class CMapAreaLow;
// World owner only. Uploads consume already prepared immutable bytes; no asset
// reads, texture callbacks, decoding, simulation or extra render preparation.
bool FarAdtRenderTile(CMapAreaLow* area);
void FarAdtRenderMaintain();
void FarAdtRenderReset();
#endif
