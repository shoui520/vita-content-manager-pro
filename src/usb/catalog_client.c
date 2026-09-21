#include "catalog_client.h"
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

enum { VCM_CATALOG_MAX = 24 * 1024 * 1024 + sizeof(VcmMtpCatalogHeader) };
static unsigned char g_net_memory[256 * 1024];

static int connect_local(void) {
    int socket = sceNetSocket("vcm_mtp_ipc", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (socket < 0) {
        sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        SceNetInitParam init = {g_net_memory, sizeof(g_net_memory), 0};
        sceNetInit(&init);
        socket = sceNetSocket("vcm_mtp_ipc", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    }
    if (socket < 0) return socket;
    const int timeout = 5000000;
    sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    SceNetSockaddrIn target;
    sceClibMemset(&target, 0, sizeof(target));
    target.sin_len = sizeof(target);
    target.sin_family = SCE_NET_AF_INET;
    target.sin_port = sceNetHtons(39323);
    target.sin_addr.s_addr = sceNetHtonl(0x7f000001u);
    if (sceNetConnect(socket, (const SceNetSockaddr *)&target, sizeof(target)) < 0) {
        sceNetSocketClose(socket);
        return -101;
    }
    return socket;
}

static int recv_exact(int socket, unsigned char *data, unsigned int size) {
    unsigned int received = 0;
    while (received < size) {
        int got = sceNetRecv(socket, data + received, size - received, 0);
        if (got <= 0) return -1;
        received += (unsigned int)got;
    }
    return 0;
}
static int send_exact(int socket, const char *data, unsigned int size) {
    unsigned int sent = 0;
    while (sent < size) {
        int count = sceNetSend(socket, data + sent, size - sent, 0);
        if (count <= 0) return -1;
        sent += (unsigned int)count;
    }
    return 0;
}
static int response_length(int socket, uint64_t *length) {
    char headers[512];
    unsigned int used = 0;
    while (used + 1 < sizeof(headers)) {
        char c;
        if (sceNetRecv(socket, &c, 1, 0) != 1) break;
        headers[used++] = c;
        if (used >= 4 && !sceClibStrncmp(headers + used - 4, "\r\n\r\n", 4)) break;
    }
    headers[used] = 0;
    if (used < 4 || sceClibStrncmp(headers, "HTTP/1.1 200 ", 13) ||
        sceClibStrncmp(headers + used - 4, "\r\n\r\n", 4)) return -1;
    const char *field = sceClibStrstr(headers, "\r\nContent-Length: ");
    if (!field) return -1;
    field += 18;
    if (*field < '0' || *field > '9') return -1;
    uint64_t number = 0;
    while (*field >= '0' && *field <= '9') {
        unsigned int digit = (unsigned int)(*field++ - '0');
        if (number > (UINT64_MAX - digit) / 10) return -1;
        number = number * 10 + digit;
    }
    if (*field != '\r') return -1;
    *length = number;
    return 0;
}

static int response_status(int socket, int expected_status, uint64_t *length) {
    char headers[512];
    unsigned int used = 0;
    while (used + 1 < sizeof(headers)) {
        char c;
        if (sceNetRecv(socket, &c, 1, 0) != 1) return -1;
        headers[used++] = c;
        if (used >= 4 && !sceClibStrncmp(headers + used - 4, "\r\n\r\n", 4)) break;
    }
    headers[used] = 0;
    char prefix[24];
    sceClibSnprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", expected_status);
    if (used < 4 || sceClibStrncmp(headers, prefix, sceClibStrnlen(prefix, sizeof(prefix)))) return -1;
    const char *field = sceClibStrstr(headers, "\r\nContent-Length: ");
    if (!field) return -1;
    field += 18;
    uint64_t value = 0;
    if (*field < '0' || *field > '9') return -1;
    while (*field >= '0' && *field <= '9') value = value * 10 + (unsigned)(*field++ - '0');
    if (*field != '\r') return -1;
    if (length) *length = value;
    return 0;
}
static int validate(VcmBridgeCatalog *out, unsigned char *body, unsigned int length) {
    if (length < sizeof(VcmMtpCatalogHeader)) return -1;
    const VcmMtpCatalogHeader *header = (const VcmMtpCatalogHeader *)body;
    if (header->magic != 0x434d4356u || header->version != 3 ||
        header->entry_size != sizeof(VcmMtpEntry) || header->count < 3 ||
        header->count > (length - sizeof(*header)) / sizeof(VcmMtpEntry) ||
        length != sizeof(*header) + header->count * sizeof(VcmMtpEntry) ||
        !header->max_size || header->free_size > header->max_size) return -1;
    VcmMtpEntry *entries = (VcmMtpEntry *)(body + sizeof(*header));
    for (uint32_t i = 0; i < header->count; ++i) {
        const VcmMtpEntry *entry = &entries[i];
        if (entry->handle != i + 1 || entry->kind > VCM_MTP_FOLDER ||
            entry->parent != (i < 3 ? VCM_MTP_NO_PARENT :
                entry->kind == VCM_MTP_PHOTO ? 1u : entry->kind == VCM_MTP_MUSIC ? 2u : 3u) ||
            (i < 3 ? entry->kind != VCM_MTP_FOLDER : !entry->native_id || entry->kind == VCM_MTP_FOLDER))
            return -1;
        unsigned int len = 0;
        while (len < sizeof(entry->name) && entry->name[len]) {
            unsigned char c = (unsigned char)entry->name[len++];
            if (c < 32 || c == '/' || c == '\\' || c == ':') return -1;
        }
        if (len == 0 || len == sizeof(entry->name)) return -1;
        len = 0;
        while (len < sizeof(entry->display_name) && entry->display_name[len]) {
            unsigned char c = (unsigned char)entry->display_name[len++];
            if (c < 32 || c == '/' || c == '\\' || c == ':' || c == 127) return -1;
        }
        if (len == 0 || len == sizeof(entry->display_name)) return -1;
        if (sceClibStrnlen(entry->title, sizeof(entry->title)) == sizeof(entry->title) ||
            sceClibStrnlen(entry->artist, sizeof(entry->artist)) == sizeof(entry->artist) ||
            sceClibStrnlen(entry->album, sizeof(entry->album)) == sizeof(entry->album) ||
            sceClibStrnlen(entry->created, sizeof(entry->created)) == sizeof(entry->created) ||
            sceClibStrnlen(entry->modified, sizeof(entry->modified)) == sizeof(entry->modified))
            return -1;
        if (entry->thumb_width > 256 || entry->thumb_height > 256 ||
            (entry->thumb_size && (entry->kind != VCM_MTP_PHOTO ||
             entry->thumb_size != 54u + ((entry->thumb_width * 3u + 3u) & ~3u) * entry->thumb_height)))
            return -1;
    }
    out->catalog.entries = entries;
    out->catalog.count = header->count;
    out->catalog.capacity = header->count;
    out->max_size = header->max_size;
    out->free_size = header->free_size;
    return 0;
}

int vcm_bridge_catalog_fetch(VcmBridgeCatalog *out) {
    if (!out) return -1;
    out->block = -1;
    out->catalog.entries = 0;
    out->catalog.count = 0;
    out->catalog.capacity = 0;
    out->max_size = 0;
    out->free_size = 0;
    int socket = connect_local();
    if (socket < 0) return socket;
    static const char request[] = "GET /v2/usb/catalog HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";
    uint64_t response_size = 0;
    if (send_exact(socket, request, sizeof(request) - 1) < 0) {
        sceNetSocketClose(socket); return -102;
    }
    if (response_length(socket, &response_size) < 0) {
        sceNetSocketClose(socket); return -103;
    }
    if (response_size < sizeof(VcmMtpCatalogHeader) || response_size > VCM_CATALOG_MAX) {
        sceNetSocketClose(socket); return -104;
    }
    unsigned int length = (unsigned int)response_size;
    unsigned int allocation = (length + 0xfff) & ~0xfffu;
    SceUID block = sceKernelAllocMemBlock("vcm_mtp_catalog", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                         allocation, 0);
    if (block < 0) { sceNetSocketClose(socket); return block; }
    void *base = 0;
    int result = sceKernelGetMemBlockBase(block, &base);
    if (result >= 0 && base) result = recv_exact(socket, base, length);
    sceNetSocketClose(socket);
    if (result < 0 || validate(out, base, length) < 0) {
        sceKernelFreeMemBlock(block);
        return result < 0 ? -105 : -106;
    }
    out->block = block;
    return 0;
}

int vcm_bridge_stream_open(VcmBridgeStream *stream, const VcmMtpEntry *entry) {
    if (!stream || !entry || entry->kind >= VCM_MTP_FOLDER || !entry->native_id)
        return -1;
    stream->socket = -1;
    stream->remaining = 0;
    const char *kind = entry->kind == VCM_MTP_PHOTO ? "photo" :
                       entry->kind == VCM_MTP_MUSIC ? "music" : "video";
    char request[192];
    int length = sceClibSnprintf(request, sizeof(request),
        "GET /v2/library/%s/%llu%s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nConnection: close\r\n\r\n",
        kind, (unsigned long long)entry->native_id,
        entry->kind == VCM_MTP_SIDECAR ? "/m4t" : "");
    if (length <= 0 || length >= (int)sizeof(request)) return -1;
    int socket = connect_local();
    if (socket < 0) return socket;
    uint64_t available = 0;
    if (send_exact(socket, request, (unsigned int)length) < 0 ||
        response_length(socket, &available) < 0 || available != entry->size) {
        sceNetSocketClose(socket);
        return -1;
    }
    stream->socket = socket;
    stream->remaining = available;
    return 0;
}

int vcm_bridge_stream_read(VcmBridgeStream *stream, void *data, unsigned int capacity) {
    if (!stream || stream->socket < 0 || (!data && capacity)) return -1;
    if (!stream->remaining || !capacity) return 0;
    if (capacity > stream->remaining) capacity = (unsigned int)stream->remaining;
    int received = sceNetRecv(stream->socket, data, capacity, 0);
    if (received <= 0) return -1;
    stream->remaining -= (unsigned int)received;
    return received;
}

void vcm_bridge_stream_close(VcmBridgeStream *stream) {
    if (!stream) return;
    if (stream->socket >= 0) sceNetSocketClose(stream->socket);
    stream->socket = -1;
    stream->remaining = 0;
}

int vcm_bridge_thumbnail_fetch(const VcmMtpEntry *entry, void *bmp, unsigned int capacity) {
    if (!entry || entry->kind != VCM_MTP_PHOTO || !entry->native_id || !bmp ||
        capacity < 54) return -1;
    char request[176];
    int length = sceClibSnprintf(request, sizeof(request),
        "GET /v2/usb/thumb/%llu HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nConnection: close\r\n\r\n",
        (unsigned long long)entry->native_id);
    if (length <= 0 || length >= (int)sizeof(request)) return -1;
    int socket = connect_local();
    if (socket < 0) return socket;
    uint64_t available = 0;
    if (send_exact(socket, request, (unsigned int)length) < 0 ||
        response_length(socket, &available) < 0 || available < 54 ||
        available > capacity || available > 256u * 256u * 3u + 54u ||
        recv_exact(socket, bmp, (unsigned int)available) < 0) {
        sceNetSocketClose(socket);
        return -1;
    }
    sceNetSocketClose(socket);
    const unsigned char *bytes = (const unsigned char *)bmp;
    unsigned int declared = (unsigned int)bytes[2] | ((unsigned int)bytes[3] << 8) |
        ((unsigned int)bytes[4] << 16) | ((unsigned int)bytes[5] << 24);
    if (bytes[0] != 'B' || bytes[1] != 'M' || declared != available) return -1;
    return (int)available;
}

void vcm_bridge_catalog_release(VcmBridgeCatalog *catalog) {
    if (!catalog) return;
    if (catalog->block >= 0) sceKernelFreeMemBlock(catalog->block);
    catalog->block = -1;
    catalog->catalog.entries = 0;
    catalog->catalog.count = 0;
    catalog->catalog.capacity = 0;
    catalog->max_size = 0;
    catalog->free_size = 0;
}


static int upload_begin(VcmBridgeUpload *upload, const char *method,
                        const char *target, uint64_t size) {
    if (!upload || !target || !size) return -1;
    upload->socket = -1; upload->expected = size; upload->written = 0;
    int socket = connect_local();
    if (socket < 0) return socket;
    char request[256];
    int length = sceClibSnprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %llu\r\n"
        "Connection: close\r\n\r\n", method, target, (unsigned long long)size);
    if (length <= 0 || length >= (int)sizeof(request) ||
        send_exact(socket, request, (unsigned int)length) < 0) {
        sceNetSocketClose(socket); return -1;
    }
    upload->socket = socket;
    return 0;
}

int vcm_bridge_queue_begin(VcmBridgeUpload *upload, uint64_t size) {
    return size <= 512u * 1024u ? upload_begin(upload, "POST", "/v2/usb/queue", size) : -1;
}

int vcm_bridge_file_begin(VcmBridgeUpload *upload, unsigned int index, int sidecar,
                          uint64_t size) {
    if (index >= 128 || (sidecar != 0 && sidecar != 1) || !size || size > 8ULL*1024*1024*1024)
        return -1;
    char target[64];
    sceClibSnprintf(target, sizeof(target), "/v2/usb/files/%u/%u", index, sidecar);
    return upload_begin(upload, "PUT", target, size);
}

int vcm_bridge_upload_write(VcmBridgeUpload *upload, const void *data, unsigned int size) {
    if (!upload || upload->socket < 0 || (!data && size) ||
        upload->written > upload->expected || size > upload->expected - upload->written)
        return -1;
    if (size && send_exact(upload->socket, data, size) < 0) return -1;
    upload->written += size;
    return 0;
}

void vcm_bridge_upload_abort(VcmBridgeUpload *upload) {
    if (!upload) return;
    if (upload->socket >= 0) sceNetSocketClose(upload->socket);
    upload->socket=-1; upload->expected=upload->written=0;
}

int vcm_bridge_upload_finish(VcmBridgeUpload *upload) {
    if (!upload || upload->socket < 0 || upload->written != upload->expected) return -1;
    uint64_t body = 0;
    int result = response_status(upload->socket, 201, &body);
    char discard[256];
    while (result == 0 && body) {
        unsigned int wanted = body < sizeof(discard) ? (unsigned int)body : sizeof(discard);
        int got = sceNetRecv(upload->socket, discard, wanted, 0);
        if (got <= 0) { result = -1; break; }
        body -= (unsigned int)got;
    }
    vcm_bridge_upload_abort(upload);
    return result;
}

static int get_binary(const char *target, void *out, unsigned int size) {
    int socket=connect_local(); if(socket<0) return socket;
    char request[160]; int n=sceClibSnprintf(request,sizeof(request),
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",target);
    uint64_t length=0; int result=-1;
    if(n>0 && n<(int)sizeof(request) && send_exact(socket,request,(unsigned)n)==0 &&
       response_status(socket,200,&length)==0 && length==size && recv_exact(socket,out,size)==0) result=0;
    sceNetSocketClose(socket); return result;
}

int vcm_bridge_queue_action(const char *action) {
    if (!action || (sceClibStrcmp(action,"sync") && sceClibStrcmp(action,"cancel"))) return -1;
    int socket=connect_local(); if(socket<0) return socket;
    char request[192]; int n=sceClibSnprintf(request,sizeof(request),
        "POST /v2/usb/%s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",action);
    uint64_t body=0; int expected=!sceClibStrcmp(action,"sync")?202:200;
    int result=(n>0 && n<(int)sizeof(request) && send_exact(socket,request,(unsigned)n)==0 &&
                response_status(socket,expected,&body)==0)?0:-1;
    char discard[128];
    while (result == 0 && body) {
        unsigned int wanted = body < sizeof(discard) ? (unsigned int)body : sizeof(discard);
        int got = sceNetRecv(socket, discard, wanted, 0);
        if (got <= 0) { result = -1; break; }
        body -= (unsigned int)got;
    }
    sceNetSocketClose(socket); return result;
}

int vcm_bridge_queue_status(VcmBridgeQueueStatus *status) {
    struct { uint32_t magic,count,imported,failed,syncing,cancelled; uint64_t transferred,total; } value;
    if(!status || get_binary("/v2/usb/queue/status",&value,sizeof(value))<0 || value.magic!=0x53554356u) return -1;
    status->count=value.count; status->imported=value.imported; status->failed=value.failed;
    status->syncing=value.syncing; status->cancelled=value.cancelled;
    status->transferred=value.transferred; status->total=value.total; return 0;
}

int vcm_bridge_item_status(unsigned int index, uint32_t *state, int32_t *error) {
    if(index>=128 || !state || !error) return -1;
    char target[64]; sceClibSnprintf(target,sizeof(target),"/v2/usb/queue/item/%u",index);
    struct { uint32_t magic,state; int32_t error; } value;
    if(get_binary(target,&value,sizeof(value))<0 || value.magic!=0x49554356u) return -1;
    *state=value.state; *error=value.error; return 0;
}
