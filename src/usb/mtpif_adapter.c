#include "mtpif_adapter.h"
#include "catalog_client.h"

#include <stdint.h>
#include <psp2/kernel/clib.h>

/* Reconstructed from SceMtpIf's 0x20-byte syscall record and SceUsbMtp's
 * consumer. The final two words are written back with transfer progress. */
typedef struct {
    uint32_t transaction;
    const void *bytes;
    uint32_t chunk_length;
    uint32_t chunk_length_high;
    uint32_t total_length;
    uint32_t total_length_high;
    uint32_t transferred;
    uint32_t transferred_high;
} MtpIfSendData;

_Static_assert(sizeof(MtpIfSendData) == 0x20, "SceMtpIf data record ABI");

extern int sceMtpIfChangePhase(void *port, int phase);
extern int sceMtpIfSendDataWithParam(void *port, MtpIfSendData *data,
                                     unsigned int record_size);
extern int sceMtpIfRecvDataWithParam(void *port, MtpIfSendData *data,
                                     unsigned int record_size);
extern int sceMtpIfSendResponse(void *port, const VcmMtpCommand *response,
                                unsigned int record_size);

typedef struct {
    void *port;
    VcmBridgeStream stream;
    uint32_t sent;
    uint32_t total;
} MtpIfContext;

static uint8_t g_scratch[64 * 1024] __attribute__((aligned(64)));
static uint8_t g_thumbnail[256 * 256 * 3 + 54] __attribute__((aligned(64)));

enum { MTP_OK=0x2001, MTP_GENERAL_ERROR=0x2002, MTP_INVALID_PARAMETER=0x201d };

static int private_response(void *port, const VcmMtpCommand *command, uint16_t code,
                            const uint32_t *params, unsigned int count) {
    VcmMtpCommand response = *command;
    response.code=code; response.reserved=0; response.param_count=count;
    for(unsigned int i=0;i<5;++i) response.params[i]=i<count?params[i]:0;
    int phase=sceMtpIfChangePhase(port,8);
    return phase<0?phase:sceMtpIfSendResponse(port,&response,sizeof(response));
}

static int receive_private_data(void *port, const VcmMtpCommand *command,
                                uint64_t total, VcmBridgeUpload *upload) {
    uint64_t received=0;
    while(received<total) {
        unsigned int wanted=(total-received)>sizeof(g_scratch)?sizeof(g_scratch):(unsigned int)(total-received);
        MtpIfSendData record; sceClibMemset(&record,0,sizeof(record));
        record.transaction=command->transaction; record.bytes=g_scratch;
        record.chunk_length=wanted;
        int result=sceMtpIfRecvDataWithParam(port,&record,sizeof(record));
        if(result<0 || !record.transferred || record.transferred_high ||
           record.transferred>wanted ||
           vcm_bridge_upload_write(upload,g_scratch,record.transferred)<0) return -1;
        received+=record.transferred;
    }
    return received==total?0:-1;
}

static int dispatch_private(void *port, const VcmMtpCommand *command) {
    uint32_t params[5]; int status=-1;
    for(unsigned int i=0;i<5;++i) params[i]=0;
    if(command->code==0x9ff0 || command->code==0x9ff1) {
        uint64_t total;
        VcmBridgeUpload upload;
        upload.socket=-1; upload.expected=0; upload.written=0;
        if(command->code==0x9ff0) {
            if(command->param_count!=2) return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
            total=(uint64_t)command->params[0] | ((uint64_t)command->params[1]<<32);
            status=vcm_bridge_queue_begin(&upload,total);
        } else {
            if(command->param_count!=4 || command->params[1]>1)
                return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
            total=(uint64_t)command->params[2] | ((uint64_t)command->params[3]<<32);
            status=vcm_bridge_file_begin(&upload,command->params[0],(int)command->params[1],total);
        }
        if(status==0 && receive_private_data(port,command,total,&upload)==0)
            status=vcm_bridge_upload_finish(&upload);
        else vcm_bridge_upload_abort(&upload);
    } else if(command->code==0x9ff2) {
        if(command->param_count) return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
        status=vcm_bridge_queue_action("sync");
    } else if(command->code==0x9ff3) {
        VcmBridgeQueueStatus value;
        if(command->param_count) return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
        status=vcm_bridge_queue_status(&value);
        if(status==0) { params[0]=value.count; params[1]=value.imported; params[2]=value.failed;
            params[3]=value.syncing; params[4]=value.cancelled; }
    } else if(command->code==0x9ff4) {
        int32_t error=0;
        if(command->param_count!=1 || command->params[0]>=128)
            return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
        status=vcm_bridge_item_status(command->params[0],&params[0],&error); params[1]=(uint32_t)error;
    } else if(command->code==0x9ff5) {
        if(command->param_count) return private_response(port,command,MTP_INVALID_PARAMETER,0,0);
        status=vcm_bridge_queue_action("cancel");
    } else return 0;
    return private_response(port,command,status==0?MTP_OK:MTP_GENERAL_ERROR,params,
        status<0?0:command->code==0x9ff3?5:command->code==0x9ff4?2:0);
}

static int send_data(void *opaque, const VcmMtpCommand *command,
                     const void *bytes, uint32_t count, uint32_t offset, uint32_t total) {
    MtpIfContext *context = opaque;
    if (!bytes || !count || count > 0x10000 || offset != context->sent ||
        offset > total || (context->sent && total != context->total) ||
        count > total - offset) return -1;
    MtpIfSendData record;
    sceClibMemset(&record, 0, sizeof(record));
    record.transaction = command->transaction;
    record.bytes = bytes;
    record.chunk_length = count;
    record.total_length = total;
    int result = sceMtpIfSendDataWithParam(context->port, &record, sizeof(record));
    if (result < 0 || record.transferred != count || record.transferred_high) return -1;
    context->sent += count;
    context->total = total;
    return 0;
}

static int send_response(void *opaque, const VcmMtpCommand *response) {
    MtpIfContext *context = opaque;
    int result = sceMtpIfChangePhase(context->port, 8);
    if (result < 0) return result;
    return sceMtpIfSendResponse(context->port, response, sizeof(*response));
}

static int open_object(void *opaque, const VcmMtpEntry *entry) {
    MtpIfContext *context = opaque;
    context->stream.socket = -1;
    return vcm_bridge_stream_open(&context->stream, entry);
}
static int read_object(void *opaque, void *bytes, uint32_t capacity) {
    return vcm_bridge_stream_read(&((MtpIfContext *)opaque)->stream, bytes, capacity);
}
static void close_object(void *opaque) {
    vcm_bridge_stream_close(&((MtpIfContext *)opaque)->stream);
}
static int read_thumb(void *opaque, const VcmMtpEntry *entry,
                      void *bytes, uint32_t capacity) {
    (void)opaque;
    return vcm_bridge_thumbnail_fetch(entry, bytes, capacity);
}

int vcm_mtpif_dispatch(void *port, VcmMtpServer *server,
                       const VcmMtpCommand *command) {
    if (!port || !server || !command) return -1;
    int phase = sceMtpIfChangePhase(port, 1);
    if (phase < 0) return phase;
    MtpIfContext context;
    sceClibMemset(&context, 0, sizeof(context));
    context.port = port;
    context.stream.socket = -1;
    if (command->code >= 0x9ff0 && command->code <= 0x9ff5)
        return dispatch_private(port, command);
    const VcmMtpTransport transport = {
        send_data, send_response, open_object, read_object, close_object, read_thumb
    };
    return vcm_mtp_engine_run(server, command, &transport, &context,
                               g_scratch, sizeof(g_scratch),
                               g_thumbnail, sizeof(g_thumbnail));
}
