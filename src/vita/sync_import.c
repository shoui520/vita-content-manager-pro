#include "sync_import.h"
#include "media_database.h"
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/musicexport.h>
#include <psp2/photoexport.h>
#include <psp2/videoexport.h>
#include <psp2/sysmodule.h>
#include <psp2common/kernel/iofilemgr.h>

extern int scePhotoExportIsAvailableFromFile(const char *, SceBool *);
static const char inbox[]="ux0:/data/vita-content-manager/wifi";
static int close_media_apps(void) {
    static const char *apps[]={"NPXS10009","NPXS10010","NPXS10004","NPXS10026"};
    SceUID log=sceIoOpen("ux0:/data/vita-content-manager/vcm-sync.log",SCE_O_CREAT|SCE_O_WRONLY|SCE_O_APPEND,0666);
    for(unsigned int i=0;i<sizeof(apps)/sizeof(apps[0]);++i) {
        /* Always request closure. An absent app is an expected no-op. */
        int result=sceAppMgrDestroyAppByName(apps[i]);
        char line[128];
        sceClibSnprintf(line,sizeof(line),"pre-import destroy %s: 0x%08X\n",apps[i],(unsigned)result);
        if(log>=0) sceIoWrite(log,line,sceClibStrnlen(line,sizeof(line)));
    }
    int running=0;
    /* Destruction can be asynchronous; never write the DB while still alive.
     * This runs on the import worker, not the PAF UI thread. */
    for(int attempt=0;attempt<50;++attempt) {
        running=0;
        for(unsigned int i=0;i<sizeof(apps)/sizeof(apps[0]);++i) {
            SceUID pid=-1;
            if(sceAppMgrGetIdByName(&pid,apps[i])==0 && pid>=0) running=1;
        }
        if(!running) break;
        sceKernelDelayThread(100000);
    }
    if(log>=0) { sceIoSyncByFd(log,0); sceIoClose(log); }
    return running?-1001:0;
}
static int receipt(const char *path, const char *text) {
    SceUID fd=sceIoOpen(path,SCE_O_CREAT|SCE_O_WRONLY|SCE_O_TRUNC,0666);
    if(fd<0) return fd;
    int size=sceClibStrnlen(text,2048), result=sceIoWrite(fd,text,size);
    if(result==size) result=sceIoSyncByFd(fd,0); else result=-1;
    int close=sceIoClose(fd);
    return result<0?result:close;
}
static int stage_named(const VcmQueueItem *item,char source[1024],const char *journal) {
    char directory[180], staged[1024], name[256], text[1200];
    sceClibStrncpy(name,item->name,sizeof(name));
    for(char *p=name;*p;++p) if(*p=='/'||*p=='\\'||*p==':'||*p=='*'||*p=='?'||*p=='"'||*p=='<'||*p=='>'||*p=='|') *p='_';
    /* The host classifies content, so its extension can differ from the
     * original filename. Always give the exporter the canonical extension. */
    char *dot=0; for(char *p=name;*p;++p) if(*p=='.') dot=p;
    if(dot) *dot=0;
    sceClibSnprintf(directory,sizeof(directory),"%s/%s",inbox,item->id);
    sceClibSnprintf(staged,sizeof(staged),"%s/%s.%s",directory,name,item->extension);
    int status=sceIoMkdir(directory,0777);
    if(status>=0) status=sceIoRename(source,staged);
    if(status>=0) {
        sceClibStrncpy(source,staged,1024);
        sceClibSnprintf(text,sizeof(text),"exporting\n%s\n",source);
        status=receipt(journal,text);
    }
    return status;
}
int vcm_sync_import(const VcmQueueItem *item) {
    char source[1024], output[1024]={0}, journal[180], done[180], text[2048];
    sceClibSnprintf(source,sizeof(source),"%s/%s.%s",inbox,item->id,item->extension);
    sceClibSnprintf(journal,sizeof(journal),"%s/%s.importing",inbox,item->id);
    sceClibSnprintf(done,sizeof(done),"%s/%s.done",inbox,item->id);
    SceIoStat st;
    if(sceIoGetstat(done,&st)==0) return 0;
    /* An interrupted vendor export needs recovery, not blind resubmission. */
    if(sceIoGetstat(journal,&st)==0) return -1002;
    if(close_media_apps()<0) return -1001;
    sceClibSnprintf(text,sizeof(text),"starting\n%s\n%s\n",source,item->name);
    if(receipt(journal,text)<0 || sceIoSync("ux0:",0)<0) return -1003;
    SceUID log=sceIoOpen("ux0:/data/vita-content-manager/vcm-sync.log",SCE_O_CREAT|SCE_O_WRONLY|SCE_O_APPEND,0666);
    int status=-1;
    if(item->kind==1 && !sceClibStrcmp(item->extension,"mp3")) {
        status=vcm_music_sync_mp3(log,item,source,output);
    } else {
        status=stage_named(item,source,journal);
        int module=item->kind==0?SCE_SYSMODULE_PHOTO_EXPORT:SCE_SYSMODULE_MUSIC_EXPORT;
        if(status>=0) status=item->kind==2?sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_VIDEO_EXPORT):sceSysmoduleLoadModule(module);
        SceUID memory=-1; void *work=0;
        if(status>=0) memory=sceKernelAllocMemBlock("vcm_sync_export",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,0x10000,0);
        if(memory>=0) status=sceKernelGetMemBlockBase(memory,&work); else status=memory;
        if(status>=0 && work) {
            if(item->kind==0) {
                SceBool available=0;
                status=scePhotoExportIsAvailableFromFile(source,&available);
                if(status>=0 && !available) status=-1004;
                PhotoExportParam param; sceClibMemset(&param,0,sizeof(param)); param.version=0x03150021;
                if(status>=0) status=scePhotoExportFromFile(source,&param,work,0,0,output,sizeof(output));
            } else if(item->kind==2) {
                if(status>=0) {
                    VideoExportInputParam input; VideoExportOutputParam result;
                    sceClibMemset(&input,0,sizeof(input)); sceClibMemset(&result,0,sizeof(result));
                    sceClibStrncpy(input.path,source,sizeof(input.path)-1);
                    status=sceVideoExportFromFile(&input,1,work,0,0,0,0,&result);
                    if(status>=0) sceClibStrncpy(output,result.path,sizeof(output)-1);
                }
            } else {
                MusicExportParam param; sceClibMemset(&param,0,sizeof(param));
                status=sceMusicExportFromFile(source,&param,work,0,0,0,output,sizeof(output));
            }
        }
        if(memory>=0) sceKernelFreeMemBlock(memory);
    }
    if(status>=0 && !output[0]) status=-1005;
    if(status>=0) {
        if(item->kind==0) {
            sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
            SceAppUtilInitParam init; SceAppUtilBootParam boot;
            sceClibMemset(&init,0,sizeof(init)); sceClibMemset(&boot,0,sizeof(boot));
            sceAppUtilInit(&init,&boot);
            sceAppUtilPhotoMount();
            char mount[16]={0}; sceAppMgrMmsMount(0x190,mount);
        } else if(item->kind==2) {
            char mount[16]={0}; sceAppMgrMmsMount(0x192,mount);
        }
        char physical[1100];
        if(!sceClibStrncmp(output,"photo0:/",8)) sceClibSnprintf(physical,sizeof(physical),"ux0:/picture/%s",output+8);
        else if(!sceClibStrncmp(output,"music0:/",8)) sceClibSnprintf(physical,sizeof(physical),"ux0:/music/%s",output+8);
        else sceClibSnprintf(physical,sizeof(physical),"%s",output);
        int stat_status=sceIoGetstat(physical,&st);
        if(stat_status<0) stat_status=sceIoGetstat(output,&st);
        sceClibSnprintf(text,sizeof(text),"verify stat=0x%08X bytes=%llu expected=%llu\n",(unsigned)stat_status,(unsigned long long)st.st_size,(unsigned long long)item->size);
        if(log>=0) sceIoWrite(log,text,sceClibStrnlen(text,sizeof(text)));
        if(stat_status<0 || (uint64_t)st.st_size!=item->size || vcm_verify_library_item(log,item->kind,output)<0) status=-1008;
    }
    if(status>=0 && item->subtitle_size) {
        char sidecar[180], destination[1100];
        sceClibSnprintf(sidecar,sizeof(sidecar),"%s/%s.m4t",inbox,item->id);
        sceClibSnprintf(destination,sizeof(destination),"%s",output);
        char *dot=0; for(char *p=destination;*p;++p) if(*p=='.') dot=p;
        if(!dot) status=-1005;
        else { sceClibMemcpy(dot,".m4t",5); status=sceIoRename(sidecar,destination); }
    }
    if(status>=0 && item->kind==2 && item->folder[0]) status=vcm_video_folder(log,item->folder,output);
    sceClibSnprintf(text,sizeof(text),"%s\nresult=0x%08X\nsource=%s\noutput=%s\n",
        item->name,(unsigned)status,source,output);
    if(log>=0) { sceIoWrite(log,text,sceClibStrnlen(text,sizeof(text))); sceIoSyncByFd(log,0); sceIoClose(log); }
    if(status<0) { receipt(journal,text); return status; }
    if(sceIoSync("ux0:",0)<0 || receipt(journal,text)<0 || sceIoRename(journal,done)<0 || sceIoSync("ux0:",0)<0) return -1003;
    /* Delete only our staged upload after export/DB commit and a durable receipt.
     * Public video export or the music rename may already have consumed it. */
    sceIoRemove(source);
    char directory[180];
    sceClibSnprintf(directory,sizeof(directory),"%s/%s",inbox,item->id);
    sceIoRmdir(directory); /* Only removes this item's directory if empty. */
    return 0;
}
