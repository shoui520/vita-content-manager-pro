#ifndef VCM_MTPIF_ADAPTER_H
#define VCM_MTPIF_ADAPTER_H

#include "../vita/mtp_engine.h"

/* Dispatches one already-received command on Sony's existing MTP port.
 * The caller must use this only in the port owner's receive thread. */
int vcm_mtpif_dispatch(void *port, VcmMtpServer *server,
                       const VcmMtpCommand *command);

#endif
