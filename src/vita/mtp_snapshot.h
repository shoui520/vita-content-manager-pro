#ifndef VCM_MTP_SNAPSHOT_H
#define VCM_MTP_SNAPSHOT_H
#include "mtp_catalog.h"

typedef struct {
    VcmMtpCatalog catalog;
    VcmMtpEntry *allocation;
} VcmMtpSnapshot;

int vcm_mtp_snapshot_build(VcmMtpSnapshot *snapshot);
void vcm_mtp_snapshot_destroy(VcmMtpSnapshot *snapshot);

#endif
