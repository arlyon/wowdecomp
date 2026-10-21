#ifndef WOW_WORLDCLIENT_FARADTSERVICE_H
#define WOW_WORLDCLIENT_FARADTSERVICE_H
#include "FarAdtProducer.h"
#include "FarAdtPreparation.h"
// Owner-thread production adapter. Prepared CPU output, no raster/RT enrollment.
void FarAdtPump();
void FarAdtReset();
const FarAdt::Producer* FarAdtGetProducer();
FarAdt::ReadyPayload FarAdtConsumeReady(FarAdt::Key key);
void FarAdtRelease(FarAdt::ReadyPayload& payload);
const FarAdt::Diagnostics& FarAdtCensus();
// Every call and final shared reader release must run on OWNER THREAD. A
// retained output is charged even after invalidation; Current gates enrollment.
std::shared_ptr<const FarAdt::PreparedTile> FarAdtConsumePrepared();
bool FarAdtPreparedCurrent(const FarAdt::PreparedTile&);
void FarAdtReleasePrepared(std::shared_ptr<const FarAdt::PreparedTile>&);
const FarAdt::PreparationDiagnostics& FarAdtPreparationCensus();
#endif
