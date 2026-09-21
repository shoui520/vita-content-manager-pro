#include "wifi_service.h"
#include "sync_queue.h"
#include "library.h"
#include "mtp_snapshot.h"
#include "mtp_thumb.h"
#include <paf/std/stdlib.h>

#include <stdint.h>
#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2common/kernel/iofilemgr.h>

#define VCM_WIFI_PORT 39323
#define VCM_HEADER_CAPACITY 4096
#define VCM_CHUNK_SIZE 16384
#define VCM_MAX_UPLOAD (8ULL * 1024ULL * 1024ULL * 1024ULL)

static const char kDataRoot[] = "ux0:/data/vita-content-manager";
static const char kInbox[] = "ux0:/data/vita-content-manager/wifi";
static const char kLog[] = "ux0:/data/vita-content-manager/vcm-wifi.log";
static volatile int g_stop;
static volatile int g_listening;
static volatile int g_netctl_ready;
static volatile int g_usb_mode_enabled;
static volatile int g_usb_catalog_served;
static unsigned char g_net_memory[512 * 1024];

static int ensure_directory(const char *path) {
    int status = sceIoMkdir(path, 0777);
    if (status >= 0) return 0;
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    if (sceIoGetstat(path, &stat) < 0 || !SCE_S_ISDIR(stat.st_mode)) return status;
    return 0;
}

static void log_event(const char *event, int status) {
    SceUID fd = sceIoOpen(kLog, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    char line[160];
    int length = sceClibSnprintf(line, sizeof(line), "%s 0x%08X\n", event, (unsigned int)status);
    if (length > 0 && length < (int)sizeof(line)) sceIoWrite(fd, line, length);
    sceIoClose(fd);
}

void vcm_wifi_stop(void) { g_usb_mode_enabled = 0; g_stop = 1; }
void vcm_usb_mode_set(int enabled) {
    g_usb_catalog_served = 0;
    g_usb_mode_enabled = enabled != 0;
}
int vcm_usb_mode_enabled(void) { return g_usb_mode_enabled; }
int vcm_usb_catalog_served(void) { return g_usb_catalog_served; }
void vcm_usb_catalog_connection_lost(void) { g_usb_catalog_served = 0; }
int vcm_wifi_listening(void) { return g_listening; }
int vcm_wifi_ip_address(char *address, unsigned int capacity) {
    if (!address || capacity < 16 || !g_netctl_ready) return -1;
    int state = SCE_NETCTL_STATE_DISCONNECTED;
    SceNetCtlInfo info;
    sceClibMemset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED ||
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0 ||
        !info.ip_address[0] || !sceClibStrcmp(info.ip_address, "0.0.0.0")) return -1;
    int length = sceClibSnprintf(address, capacity, "%s", info.ip_address);
    return length > 0 && (unsigned int)length < capacity ? 0 : -1;
}

static int send_all(int socket, const char *data, unsigned int length) {
    unsigned int offset = 0;
    while (offset < length) {
        int sent = sceNetSend(socket, data + offset, length - offset, 0);
        if (sent <= 0) return -1;
        offset += (unsigned int)sent;
    }
    return 0;
}

static void respond(int socket, int status, const char *reason, const char *json) {
    char header[256];
    unsigned int body_size = (unsigned int)sceClibStrnlen(json, 32768);
    int length = sceClibSnprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n",
        status, reason, body_size);
    if (length > 0 && length < (int)sizeof(header)) {
        send_all(socket, header, (unsigned int)length);
        send_all(socket, json, body_size);
    }
}

static void respond_binary(int socket, const void *body, unsigned int body_size) {
    char header[192];
    int length = sceClibSnprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n", body_size);
    if (length > 0 && length < (int)sizeof(header) && send_all(socket, header, length) == 0)
        send_all(socket, body, body_size);
}

typedef struct {
    uint32_t magic, count, imported, failed, syncing, cancelled;
    uint64_t transferred, total;
} VcmUsbQueueStatus;

typedef struct { uint32_t magic, state; int32_t error; } VcmUsbItemStatus;

static void send_usb_queue_status(int socket, int item_index) {
    if (item_index < 0) {
        VcmQueueInfo info; vcm_queue_info(&info);
        VcmUsbQueueStatus status = {0x53554356u, (uint32_t)info.count,
            (uint32_t)info.imported, (uint32_t)info.failed, (uint32_t)info.syncing,
            (uint32_t)info.cancelled, info.transferred, info.total};
        respond_binary(socket, &status, sizeof(status));
    } else {
        VcmQueueItem item;
        if (vcm_queue_item(item_index, &item) < 0) {
            respond(socket, 404, "Not Found", "{\"error\":\"Queue item not found\"}");
            return;
        }
        VcmUsbItemStatus status = {0x49554356u, (uint32_t)item.state, item.error};
        respond_binary(socket, &status, sizeof(status));
    }
}

static int send_chunk(void *context, const void *data, unsigned int size) {
    int socket=*(int *)context;
    char length[24];
    int n=sceClibSnprintf(length,sizeof(length),"%X\r\n",size);
    if(n<=0 || n>=(int)sizeof(length) || send_all(socket,length,(unsigned int)n)<0 ||
       send_all(socket,(const char *)data,size)<0 || send_all(socket,"\r\n",2)<0) return -1;
    return 0;
}

static int parse_library_number(const char *text, uint64_t *value) {
    if(!*text) return -1;
    uint64_t result=0;
    for(const char *p=text;*p;++p) {
        if(*p<'0' || *p>'9' || result>(UINT64_MAX-(unsigned)(*p-'0'))/10) return -1;
        result=result*10+(unsigned)(*p-'0');
    }
    *value=result; return 0;
}

static void send_library_file(int socket,int kind,uint64_t id,int sidecar) {
    char path[1024];
    if(id>INT64_MAX || (sidecar ? vcm_library_resolve_sidecar(id,path,sizeof(path))
                               : vcm_library_resolve(kind,id,path,sizeof(path)))<0) {
        respond(socket,404,"Not Found","{\"error\":\"Media item not found\"}"); return;
    }
    SceIoStat stat;
    if(sceIoGetstat(path,&stat)<0 || stat.st_size<=0) {
        respond(socket,404,"Not Found","{\"error\":\"Media file unavailable\"}"); return;
    }
    SceUID fd=sceIoOpen(path,SCE_O_RDONLY,0);
    if(fd<0) { respond(socket,404,"Not Found","{\"error\":\"Media file unavailable\"}"); return; }
    char header[220];
    int n=sceClibSnprintf(header,sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %llu\r\nConnection: close\r\n\r\n",
        (unsigned long long)stat.st_size);
    if(n>0 && n<(int)sizeof(header) && send_all(socket,header,(unsigned int)n)==0) {
        char chunk[VCM_CHUNK_SIZE]; uint64_t remaining=(uint64_t)stat.st_size;
        while(remaining) {
            int wanted=remaining<sizeof(chunk)?(int)remaining:(int)sizeof(chunk);
            int got=sceIoRead(fd,chunk,wanted);
            if(got<=0 || send_all(socket,chunk,(unsigned int)got)<0) break;
            remaining-=(unsigned int)got;
        }
    }
    sceIoClose(fd);
}

static void send_library(int socket,const char *request_line) {
    const char *start="GET /v2/library/";
    const char *end=sceClibStrstr(request_line," HTTP/1.1");
    if(!end || end[9]) { respond(socket,400,"Bad Request","{\"error\":\"Invalid request\"}"); return; }
    char target[128]; unsigned int target_length=(unsigned int)(end-(request_line+sceClibStrnlen(start,32)));
    if(target_length>=sizeof(target)) { respond(socket,400,"Bad Request","{\"error\":\"Invalid library route\"}"); return; }
    sceClibMemcpy(target,request_line+sceClibStrnlen(start,32),target_length); target[target_length]=0;
    int kind=-1; const char *rest=0;
    if(!sceClibStrncmp(target,"photo",5)) { kind=VCM_LIBRARY_PHOTO; rest=target+5; }
    else if(!sceClibStrncmp(target,"music",5)) { kind=VCM_LIBRARY_MUSIC; rest=target+5; }
    else if(!sceClibStrncmp(target,"video",5)) { kind=VCM_LIBRARY_VIDEO; rest=target+5; }
    if(kind<0) { respond(socket,404,"Not Found","{\"error\":\"Unknown library\"}"); return; }
    if(*rest=='/') {
        if(kind==VCM_LIBRARY_PHOTO && !sceClibStrncmp(rest,"/thumbs?",8)) {
            rest+=8;
        } else {
            uint64_t id;
            char *sidecar=sceClibStrstr((char *)rest+1,"/m4t");
            if(sidecar && kind==VCM_LIBRARY_VIDEO && !sidecar[4]) *sidecar=0;
            else sidecar=0;
            if(parse_library_number(rest+1,&id)<0) respond(socket,400,"Bad Request","{\"error\":\"Invalid media ID\"}");
            else send_library_file(socket,kind,id,sidecar!=0);
            return;
        }
    } else if(*rest=='?') ++rest;
    else { respond(socket,400,"Bad Request","{\"error\":\"Page required\"}"); return; }
    int icons=kind==VCM_LIBRARY_PHOTO && !sceClibStrncmp(target,"photo/thumbs?",13);
    if(sceClibStrncmp(rest,"offset=",7)) { respond(socket,400,"Bad Request","{\"error\":\"Invalid page\"}"); return; }
    char *separator=sceClibStrstr((char *)rest+7,"&limit=");
    if(!separator) { respond(socket,400,"Bad Request","{\"error\":\"Invalid page\"}"); return; }
    *separator=0;
    uint64_t offset,limit;
    int valid=parse_library_number(rest+7,&offset)==0 &&
              parse_library_number(separator+7,&limit)==0 &&
              offset<=1000000 && limit>=1 && limit<=32;
    if(!valid) { respond(socket,400,"Bad Request","{\"error\":\"Invalid page\"}"); return; }
    if(icons) {
        const char *header="HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        if(send_all(socket,header,(unsigned int)sceClibStrnlen(header,256))==0 &&
           vcm_library_photo_icons((int)offset,(int)limit,send_chunk,&socket)==0)
            send_all(socket,"0\r\n\r\n",5);
        return;
    }
    static char json[32768];
    if(vcm_library_list(kind,(int)offset,(int)limit,json,sizeof(json))<0)
        respond(socket,500,"Internal Server Error","{\"error\":\"Could not read the Vita media library\"}");
    else respond(socket,200,"OK",json);
}

static int parse_decimal(const char *text, uint64_t *value) {
    if (!*text) return -1;
    uint64_t result = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return -1;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *value = result;
    return 0;
}

static int upload_name(const char *target, char *name, unsigned int capacity) {
    const char prefix[] = "/v2/files/";
    if (sceClibStrncmp(target, prefix, sizeof(prefix) - 1) != 0) return -1;
    const char *value = target + sizeof(prefix) - 1;
    const char *dot = value + 32;
    if (sceClibStrnlen(value, 64) < 36 || *dot != '.') return -1;
    for (int i = 0; i < 32; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    const char *extension = dot + 1;
    static const char *allowed[] = {"mp3", "mp4", "jpg", "jpeg", "png", "gif",
                                    "bmp", "tif", "tiff", "mpo", "m4a", "wav", "m4t"};
    int valid = 0;
    for (unsigned int i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
        if (sceClibStrcmp(extension, allowed[i]) == 0) valid = 1;
    if (!valid) return -1;
    int length = sceClibSnprintf(name, capacity, "%s", value);
    return length > 0 && (unsigned int)length < capacity ? 0 : -1;
}

static int usb_upload_target(const char *target, unsigned int *index, unsigned int *sidecar) {
    const char prefix[]="/v2/usb/files/";
    if(sceClibStrncmp(target,prefix,sizeof(prefix)-1)) return -1;
    const char *cursor=target+sizeof(prefix)-1;
    uint64_t first=0,second=0;
    const char *slash=sceClibStrchr(cursor,'/');
    if(!slash || slash==cursor || slash[1]<'0' || slash[1]>'1' || slash[2]) return -1;
    char number[12]; unsigned int length=(unsigned int)(slash-cursor);
    if(length>=sizeof(number)) return -1;
    sceClibMemcpy(number,cursor,length); number[length]=0;
    if(parse_decimal(number,&first)<0 || parse_decimal(slash+1,&second)<0 ||
       first>=VCM_QUEUE_MAX || second>1) return -1;
    *index=(unsigned int)first; *sidecar=(unsigned int)second; return 0;
}

static int read_request(int socket, char header[VCM_HEADER_CAPACITY],
                        unsigned int *used, unsigned int *body_offset) {
    *used = 0;
    while (*used < VCM_HEADER_CAPACITY - 1) {
        int got = sceNetRecv(socket, header + *used, VCM_HEADER_CAPACITY - 1 - *used, 0);
        if (got <= 0) return -1;
        *used += (unsigned int)got;
        header[*used] = 0;
        char *end = sceClibStrstr(header, "\r\n\r\n");
        if (end) {
            *body_offset = (unsigned int)(end + 4 - header);
            *end = 0;
            return 0;
        }
    }
    return -1;
}

static void receive_file(int socket, const char *name, uint64_t expected,
                         const char *first, unsigned int first_size) {
    if (!expected || expected > VCM_MAX_UPLOAD || first_size > expected) {
        respond(socket, 413, "Payload Too Large", "{\"error\":\"Invalid file size\"}");
        return;
    }
    int sidecar=0;
    int item_index=vcm_queue_find(name,expected,&sidecar);
    if(item_index<0) {
        respond(socket,409,"Conflict","{\"error\":\"File does not match the active queue\"}");
        return;
    }
    SceIoDevInfo storage;
    sceClibMemset(&storage, 0, sizeof(storage));
    if (sceIoDevctl("ux0:", 0x3001, 0, 0, &storage, sizeof(storage)) < 0 ||
        storage.free_size < 0 ||
        (uint64_t)storage.free_size < expected + 8 * 1024 * 1024) {
        respond(socket, 507, "Insufficient Storage", "{\"error\":\"Not enough Vita storage\"}");
        vcm_queue_received(item_index,sidecar,-1006);
        return;
    }
    char final[180], partial[190];
    int n = sceClibSnprintf(final, sizeof(final), "%s/%s", kInbox, name);
    if (n <= 0 || n >= (int)sizeof(final)) {
        respond(socket, 400, "Bad Request", "{\"error\":\"Invalid file name\"}");
        vcm_queue_received(item_index,sidecar,-1);
        return;
    }
    sceClibSnprintf(partial, sizeof(partial), "%s.vcm-part", final);
    SceIoStat stat;
    if (sceIoGetstat(final, &stat) >= 0 || sceIoGetstat(partial, &stat) >= 0) {
        respond(socket, 409, "Conflict", "{\"error\":\"Transfer ID already exists\"}");
        vcm_queue_received(item_index,sidecar,-1002);
        return;
    }
    SceUID output = sceIoOpen(partial, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
    if (output < 0) {
        log_event("create partial", output);
        respond(socket, 500, "Internal Server Error", "{\"error\":\"Cannot create file\"}");
        vcm_queue_received(item_index,sidecar,output);
        return;
    }
    uint64_t received = 0;
    int failure = 0;
    char chunk[VCM_CHUNK_SIZE];
    while (received < expected && !failure && !g_stop && !vcm_queue_cancelled()) {
        const char *data;
        unsigned int size;
        if (received == 0 && first_size) { data = first; size = first_size; }
        else {
            unsigned int wanted = expected - received < sizeof(chunk) ?
                                  (unsigned int)(expected - received) : sizeof(chunk);
            int got = sceNetRecv(socket, chunk, wanted, 0);
            if (got <= 0) { failure = 1; break; }
            data = chunk;
            size = (unsigned int)got;
        }
        unsigned int written = 0;
        while (written < size) {
            int result = sceIoWrite(output, data + written, size - written);
            if (result <= 0) { failure = 1; break; }
            written += (unsigned int)result;
        }
        received += size;
        vcm_queue_progress(item_index,received,sidecar);
    }
    if (received != expected) failure = 1;
    if (!failure && sceIoSyncByFd(output, 0) < 0) failure = 1;
    if (sceIoClose(output) < 0) failure = 1;
    if (!failure && sceIoRename(partial, final) < 0) failure = 1;
    if (failure) {
        log_event("receive failed", -1);
        sceIoRemove(partial); /* This request created the partial. */
        respond(socket, 500, "Internal Server Error", "{\"error\":\"Transfer did not complete\"}");
        vcm_queue_received(item_index,sidecar,-1007);
        return;
    }
    int sync_status = sceIoSync("ux0:", 0);
    if (sync_status < 0) log_event("sync published upload", sync_status);
    log_event("receive published", 0);
    vcm_queue_received(item_index,sidecar,sync_status<0?-1003:0);
    respond(socket, 201, "Created", sync_status < 0
        ? "{\"ok\":true,\"state\":\"received\",\"durable\":false}"
        : "{\"ok\":true,\"state\":\"received\",\"durable\":true}");
}

static void send_status(int socket) {
    VcmQueueInfo info; vcm_queue_info(&info);
    static char json[32768];
    int n=sceClibSnprintf(json,sizeof(json),
        "{\"service\":\"vita-content-manager\",\"version\":2,\"video_folders\":true,\"library_export\":true,\"queue\":\"%s\",\"count\":%d,\"imported\":%d,\"failed\":%d,\"syncing\":%d,\"cancelled\":%d,\"transferred\":%llu,\"total\":%llu,\"items\":[",
        info.id,info.count,info.imported,info.failed,info.syncing,info.cancelled,
        (unsigned long long)info.transferred,(unsigned long long)info.total);
    for(int i=0;i<info.count;++i) {
        VcmQueueItem item; vcm_queue_item(i,&item);
        n+=sceClibSnprintf(json+n,sizeof(json)-n,"%s{\"id\":\"%s\",\"state\":%d,\"error\":%d}",i?",":"",item.id,item.state,item.error);
    }
    sceClibSnprintf(json+n,sizeof(json)-n,"]}");
    respond(socket,200,"OK",json);
}
static int ux0_space(uint64_t *max_size,uint64_t *free_size) {
    if(!max_size || !free_size) return -1;
    SceIoDevInfo storage;
    sceClibMemset(&storage,0,sizeof(storage));
    int space=sceIoDevctl("ux0:",0x3001,0,0,&storage,sizeof(storage));
    if(space>=0 && storage.max_size>0 && storage.free_size>=0 &&
       storage.free_size<=storage.max_size) {
        *max_size=(uint64_t)storage.max_size;
        *free_size=(uint64_t)storage.free_size;
        return 0;
    }
    if(sceAppMgrGetDevInfo("ux0:",max_size,free_size)<0 ||
       !*max_size || *free_size>*max_size) return -1;
    return 0;
}
static void send_usb_catalog(int socket) {
    VcmMtpSnapshot snapshot;
    if (vcm_mtp_snapshot_build(&snapshot) < 0) {
        respond(socket,500,"Internal Server Error","{\"error\":\"USB catalogue unavailable\"}");
        return;
    }
    uint64_t max_size=0,free_size=0;
    if(ux0_space(&max_size,&free_size)<0) {
        vcm_mtp_snapshot_destroy(&snapshot);
        respond(socket,500,"Internal Server Error","{\"error\":\"ux0 space unavailable\"}");
        return;
    }
    VcmMtpCatalogHeader header={0x434d4356u,3u,snapshot.catalog.count,
                                sizeof(VcmMtpEntry),max_size,free_size};
    uint32_t length = sizeof(header) + snapshot.catalog.count * sizeof(VcmMtpEntry);
    char response[192];
    int size = sceClibSnprintf(response,sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n",length);
    if(size>0 && size<(int)sizeof(response) &&
       send_all(socket,response,(unsigned int)size)==0 &&
       send_all(socket,(const char *)&header,sizeof(header))==0 &&
       send_all(socket,(const char *)snapshot.catalog.entries,
                snapshot.catalog.count*sizeof(VcmMtpEntry))==0)
        g_usb_catalog_served=1;
    vcm_mtp_snapshot_destroy(&snapshot);
}
static void send_usb_catalog_summary(int socket) {
    VcmMtpSnapshot snapshot;
    if (vcm_mtp_snapshot_build(&snapshot) < 0) {
        respond(socket,500,"Internal Server Error","{\"error\":\"USB catalogue unavailable\"}");
        return;
    }
    unsigned int counts[4]={0};
    for(unsigned int i=3;i<snapshot.catalog.count;++i) {
        unsigned int kind=snapshot.catalog.entries[i].kind;
        if(kind<4) ++counts[kind];
    }
    char json[192];
    sceClibSnprintf(json,sizeof(json),
        "{\"photo\":%u,\"music\":%u,\"video\":%u,\"sidecar\":%u,\"total\":%u}",
        counts[0],counts[1],counts[2],counts[3],snapshot.catalog.count);
    vcm_mtp_snapshot_destroy(&snapshot);
    respond(socket,200,"OK",json);
}
static void send_usb_thumbnail(int socket, const char *request_line) {
    const char *prefix="GET /v2/usb/thumb/";
    const char *start=request_line+sceClibStrnlen(prefix,32);
    const char *end=sceClibStrstr(start," HTTP/1.1");
    if(!end || end[9] || end<=start || end-start>20) {
        respond(socket,400,"Bad Request","{\"error\":\"Invalid thumbnail ID\"}"); return;
    }
    char number[24];
    sceClibMemcpy(number,start,(unsigned int)(end-start)); number[end-start]=0;
    uint64_t id;
    if(parse_library_number(number,&id)<0 || !id) {
        respond(socket,400,"Bad Request","{\"error\":\"Invalid thumbnail ID\"}"); return;
    }
    unsigned char *dds=(unsigned char *)sce_paf_malloc(65536);
    unsigned char *bmp=(unsigned char *)sce_paf_malloc(256*256*3+54);
    if(!dds || !bmp) {
        if(dds) sce_paf_free(dds);
        if(bmp) sce_paf_free(bmp);
        respond(socket,500,"Internal Server Error","{\"error\":\"No thumbnail memory\"}"); return;
    }
    int dds_size=vcm_library_photo_icon(id,dds,65536);
    int bmp_size=dds_size>0?vcm_mtp_dxt1_dds_to_bmp(dds,(uint32_t)dds_size,bmp,256*256*3+54):-1;
    sce_paf_free(dds);
    if(bmp_size<0) {
        sce_paf_free(bmp);
        respond(socket,404,"Not Found","{\"error\":\"Thumbnail unavailable\"}"); return;
    }
    char response[192];
    int size=sceClibSnprintf(response,sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",bmp_size);
    if(size>0 && size<(int)sizeof(response) && send_all(socket,response,(unsigned int)size)==0)
        send_all(socket,(const char *)bmp,(unsigned int)bmp_size);
    sce_paf_free(bmp);
}
static void handle_client(int socket, int is_loopback) {
    /* Listener is nonblocking for clean shutdown; accepted connections must
     * wait for their request bytes instead of returning EAGAIN immediately. */
    unsigned int blocking = 0;
    int mode_status = sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET,
                                       SCE_NET_SO_NBIO, &blocking, sizeof(blocking));
    if (mode_status < 0) {
        log_event("configure client blocking mode", mode_status);
        return;
    }
    const int timeout = 10000000;
    sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    char header[VCM_HEADER_CAPACITY];
    unsigned int used = 0, body_offset = 0;
    if (read_request(socket, header, &used, &body_offset) < 0) {
        respond(socket, 400, "Bad Request", "{\"error\":\"Invalid request headers\"}");
        return;
    }
    char *line_end = sceClibStrstr(header, "\r\n");
    if (!line_end) {
        respond(socket, 400, "Bad Request", "{\"error\":\"Invalid request line\"}");
        return;
    }
    *line_end = 0;
    if (sceClibStrcmp(header, "GET /v2/status HTTP/1.1") == 0) {
        send_status(socket);
        return;
    }
    if (sceClibStrcmp(header,"GET /v2/usb/catalog HTTP/1.1") == 0) {
        if (is_loopback && !vcm_usb_mode_enabled())
            respond(socket,409,"Conflict","{\"error\":\"USB mode is off\"}");
        else if (is_loopback) send_usb_catalog(socket);
        else respond(socket,403,"Forbidden","{\"error\":\"Local USB bridge only\"}");
        return;
    }
    if (sceClibStrcmp(header,"GET /v2/usb/catalog/summary HTTP/1.1") == 0) {
        send_usb_catalog_summary(socket);
        return;
    }
    if (sceClibStrcmp(header,"GET /v2/usb/queue/status HTTP/1.1") == 0) {
        if (is_loopback && vcm_usb_mode_enabled()) send_usb_queue_status(socket, -1);
        else respond(socket,403,"Forbidden","{\"error\":\"Local USB bridge only\"}");
        return;
    }
    if (!sceClibStrncmp(header,"GET /v2/usb/queue/item/",23)) {
        uint64_t index = 0;
        const char *value = header + 23;
        const char *suffix = sceClibStrstr(value, " HTTP/1.1");
        char number[12];
        unsigned int length = suffix ? (unsigned int)(suffix - value) : sizeof(number);
        if (!is_loopback || !vcm_usb_mode_enabled())
            respond(socket,403,"Forbidden","{\"error\":\"Local USB bridge only\"}");
        else if (!suffix || suffix[9] || !length || length >= sizeof(number))
            respond(socket,400,"Bad Request","{\"error\":\"Invalid queue item\"}");
        else {
            sceClibMemcpy(number, value, length); number[length] = 0;
            if (parse_decimal(number, &index) < 0 || index >= VCM_QUEUE_MAX)
                respond(socket,400,"Bad Request","{\"error\":\"Invalid queue item\"}");
            else send_usb_queue_status(socket, (int)index);
        }
        return;
    }
    if (sceClibStrcmp(header,"GET /v2/diagnostics/music HTTP/1.1") == 0) {
        char audit[256];
        if(vcm_library_audit_music(audit,sizeof(audit))<0)
            respond(socket,500,"Internal Server Error","{\"error\":\"Music audit failed\"}");
        else respond(socket,200,"OK",audit);
        return;
    }
    if (sceClibStrcmp(header,"GET /v2/diagnostics/storage HTTP/1.1") == 0) {
        uint64_t max_size=0,free_size=0;
        if(ux0_space(&max_size,&free_size)<0)
            respond(socket,500,"Internal Server Error","{\"error\":\"ux0 space unavailable\"}");
        else {
            char values[160];
            sceClibSnprintf(values,sizeof(values),
                "{\"capacity\":%llu,\"free\":%llu,\"used\":%llu}",
                (unsigned long long)max_size,(unsigned long long)free_size,
                (unsigned long long)(max_size-free_size));
            respond(socket,200,"OK",values);
        }
        return;
    }
    if (!sceClibStrncmp(header,"GET /v2/usb/thumb/",18)) {
        if(is_loopback) send_usb_thumbnail(socket,header);
        else respond(socket,403,"Forbidden","{\"error\":\"Local USB bridge only\"}");
        return;
    }
    if (!sceClibStrncmp(header,"GET /v2/library/",16)) {
        send_library(socket,header);
        return;
    }
    int usb_publish=!sceClibStrcmp(header,"POST /v2/usb/queue HTTP/1.1");
    int usb_sync=!sceClibStrcmp(header,"POST /v2/usb/sync HTTP/1.1");
    int usb_cancel=!sceClibStrcmp(header,"POST /v2/usb/cancel HTTP/1.1");
    int publish=!sceClibStrcmp(header,"POST /v2/queue HTTP/1.1") || usb_publish;
    int sync=!sceClibStrcmp(header,"POST /v2/sync HTTP/1.1") || usb_sync;
    int cancel=!sceClibStrcmp(header,"POST /v2/cancel HTTP/1.1") || usb_cancel;
    int upload=!sceClibStrncmp(header,"PUT ",4);
    if (!publish && !sync && !cancel && !upload) {
        respond(socket, 404, "Not Found", "{\"error\":\"Unknown endpoint\"}");
        return;
    }
    char name[64];
    int usb_upload = 0;
    if(upload) {
      char *version=sceClibStrstr(header+4," HTTP/1.1");
      if(!version || version[9]) { respond(socket,400,"Bad Request","{\"error\":\"Invalid request line\"}"); return; }
      *version=0;
      unsigned int index = 0, sidecar = 0;
      uint64_t usb_expected = 0;
      if (is_loopback && vcm_usb_mode_enabled() &&
          usb_upload_target(header + 4,&index,&sidecar) == 0 &&
          vcm_queue_usb_target((int)index, (int)sidecar, name, sizeof(name), &usb_expected) == 0) {
          char exact[64];
          sceClibSnprintf(exact, sizeof(exact), "/v2/usb/files/%u/%u", index, sidecar);
          usb_upload = !sceClibStrcmp(header + 4, exact);
      }
      if (!usb_upload && upload_name(header + 4, name, sizeof(name)) < 0) {
        respond(socket, 400, "Bad Request", "{\"error\":\"Invalid upload path\"}");
        return;
      }
    }
    uint64_t length = 0;
    int length_seen = 0, session_seen=0, invalid=0;
    char session[33]={0};
    for (char *line = line_end + 2; *line;) {
        char *next = sceClibStrstr(line, "\r\n");
        if (next) *next = 0;
        if (sceClibStrncasecmp(line, "Content-Length: ", 16) == 0) {
            if (length_seen++ || parse_decimal(line + 16, &length) < 0) invalid=1;
        } else if (sceClibStrncasecmp(line, "X-VCM-Queue: ", 13) == 0) {
            if(session_seen++ || sceClibStrnlen(line+13,34)!=32) invalid=1;
            else sceClibMemcpy(session,line+13,33);
        } else if(!sceClibStrncasecmp(line,"Transfer-Encoding:",18)) {
            invalid=1;
        }
        if (!next) break;
        line = next + 2;
    }
    if(invalid) { respond(socket,400,"Bad Request","{\"error\":\"Invalid request headers\"}"); return; }
    int usb_request = usb_publish || usb_sync || usb_cancel || usb_upload;
    if (usb_request && (!is_loopback || !vcm_usb_mode_enabled())) {
        respond(socket,403,"Forbidden","{\"error\":\"Local USB bridge only\"}");
        return;
    }
    if(!publish && !usb_request && (session_seen!=1 || !vcm_queue_matches(session))) {
        respond(socket,409,"Conflict","{\"error\":\"The active queue has changed\"}");
        return;
    }
    if (length_seen != 1) {
        respond(socket, 411, "Length Required", "{\"error\":\"Content-Length required\"}");
        return;
    }
    if(publish) {
        static char body[512*1024];
        unsigned int received=used-body_offset;
        if(!length || length>=sizeof(body) || received>length) { respond(socket,413,"Too Large","{\"error\":\"Queue too large\"}"); return; }
        sceClibMemcpy(body,header+body_offset,received);
        while(received<length) {
            int got=sceNetRecv(socket,body+received,(unsigned int)length-received,0);
            if(got<=0) return;
            received+=(unsigned int)got;
        }
        for(unsigned int i=0;i<received;++i) if(!body[i]) { respond(socket,400,"Bad Request","{\"error\":\"Invalid queue\"}"); return; }
        body[received]=0;
        int result=vcm_queue_publish(body);
        respond(socket,result==0?201:result==-2?409:400,result==0?"Created":"Rejected",
            result==0?"{\"ok\":true}":result==-2?"{\"error\":\"Another transfer is active\"}":"{\"error\":\"Invalid queue metadata\"}");
    } else if(sync) {
        int result=vcm_queue_sync();
        respond(socket,result==0?202:409,result==0?"Accepted":"Conflict",
            result==0?"{\"ok\":true}":"{\"error\":\"Queue is not fully transferred\"}");
    } else if(cancel) {
        vcm_queue_cancel(); respond(socket,200,"OK","{\"ok\":true}");
    } else receive_file(socket, name, length, header + body_offset, used - body_offset);
}

static int open_listener(int loopback_only) {
    int server = sceNetSocket("vcm_media", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (server < 0) return server;
    int reuse = 1;
    sceNetSetsockopt(server, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &reuse, sizeof(reuse));
    unsigned int nonblocking = 1;
    int status = sceNetSetsockopt(server, SCE_NET_SOL_SOCKET,
                                  SCE_NET_SO_NBIO, &nonblocking, sizeof(nonblocking));
    SceNetSockaddrIn address;
    sceClibMemset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(VCM_WIFI_PORT);
    address.sin_addr.s_addr = loopback_only ? sceNetHtonl(0x7f000001u) : SCE_NET_INADDR_ANY;
    if (status >= 0) status = sceNetBind(server, (const SceNetSockaddr *)&address, sizeof(address));
    if (status >= 0) status = sceNetListen(server, 2);
    if (status < 0) {
        sceNetSocketClose(server);
        return status;
    }
    return server;
}

int vcm_wifi_serve(void) {
    g_stop = 0;
    g_listening = 0;
    int status = ensure_directory(kDataRoot);
    if (status < 0) return status;
    status = ensure_directory(kInbox);
    if (status < 0) { log_event("create Wi-Fi inbox", status); return status; }
    status = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (status < 0) { log_event("load network module", status); return status; }
    SceNetInitParam init = {g_net_memory, sizeof(g_net_memory), 0};
    status = sceNetInit(&init);
    if (status < 0) { log_event("initialize network", status); return status; }
    int netctl_status = sceNetCtlInit();
    if (netctl_status < 0) log_event("initialize netctl", netctl_status);
    else g_netctl_ready = 1;
    int loopback_only = g_usb_mode_enabled != 0;
    int server = open_listener(loopback_only);
    if (server < 0) {
        log_event("listen", server);
        g_netctl_ready = 0;
        if (netctl_status >= 0) sceNetCtlTerm();
        return server;
    }
    g_listening = 1;
    log_event("listening on port 39323", 0);
    while (!g_stop) {
        int want_loopback = g_usb_mode_enabled != 0;
        if (want_loopback != loopback_only || server < 0) {
            g_listening = 0;
            if (server >= 0) sceNetSocketClose(server);
            server = open_listener(want_loopback);
            if (server < 0) {
                log_event("switch listener", server);
                sceKernelDelayThread(250000);
                continue;
            }
            loopback_only = want_loopback;
            g_listening = 1;
            log_event(loopback_only ? "USB loopback only" : "Wi-Fi listener restored", 0);
        }
        SceNetSockaddrIn peer;
        unsigned int peer_length=sizeof(peer);
        sceClibMemset(&peer,0,sizeof(peer));
        int client = sceNetAccept(server, (SceNetSockaddr *)&peer, &peer_length);
        if (client >= 0) {
            int is_loopback=peer_length>=sizeof(peer) && peer.sin_family==SCE_NET_AF_INET &&
                sceNetNtohl(peer.sin_addr.s_addr)==0x7f000001u;
            handle_client(client,is_loopback);
            sceNetSocketClose(client);
        } else {
            int error = *sceNetErrnoLoc();
            if (error != SCE_NET_EAGAIN && error != SCE_NET_EWOULDBLOCK) {
                log_event("accept", error);
                break;
            }
            sceKernelDelayThread(50000);
        }
    }
    g_listening = 0;
    g_netctl_ready = 0;
    if (netctl_status >= 0) sceNetCtlTerm();
    if (server >= 0) sceNetSocketClose(server);
    return 0;
}
