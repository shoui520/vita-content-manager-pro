#include "library.h"
#include "media_database.h"
#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/apputil.h>
#include <psp2/sysmodule.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
extern int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
extern int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
extern int sqlite3_bind_int(sqlite3_stmt *, int, int);
extern int sqlite3_bind_int64(sqlite3_stmt *, int, long long);
extern int sqlite3_step(sqlite3_stmt *);
extern int sqlite3_finalize(sqlite3_stmt *);
extern int sqlite3_reset(sqlite3_stmt *);
extern int sqlite3_close(sqlite3 *);
extern int sqlite3_column_int(sqlite3_stmt *, int);
extern long long sqlite3_column_int64(sqlite3_stmt *, int);
extern const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
extern const void *sqlite3_column_blob(sqlite3_stmt *, int);
extern int sqlite3_column_bytes(sqlite3_stmt *, int);

static const char *database_path(int kind) {
    static const char *paths[]={"ux0:/mms/photo/AVContent.db",
                                "ux0:/mms/music/AVContent.db",
                                "ux0:/mms/video/AVContent.db"};
    return kind>=0 && kind<3 ? paths[kind] : 0;
}
static const char *list_query(int kind) {
    static const char *queries[]={
        "SELECT p.mrid,p.title,p.content_path,p.size,p.created_time,'','',"
        "EXISTS(SELECT 1 FROM tbl_Icon i WHERE i.content_id=p.mrid AND i.data_type=1 "
        "AND i.codec_type=25 AND length(i.data) BETWEEN 128 AND 65536),p.width,p.height "
        "FROM tbl_VPContent p WHERE p.content_type=4 AND p.status=2 "
        "AND p.content_path IS NOT NULL ORDER BY p.created_time DESC,p.mrid DESC LIMIT ?1 OFFSET ?2",
        "SELECT mrid,title,content_path,size,imported_time,artist,album_name,0,0,0 "
        "FROM tbl_Music WHERE status=2 AND content_path IS NOT NULL "
        "ORDER BY title COLLATE NOCASE,mrid LIMIT ?1 OFFSET ?2",
        "SELECT mrid,title,content_path,size,created_time,'','',0,0,0 "
        "FROM tbl_VPContent WHERE content_type=5 AND status=2 "
        "AND content_path IS NOT NULL ORDER BY title COLLATE NOCASE,mrid LIMIT ?1 OFFSET ?2"
    };
    return kind>=0 && kind<3 ? queries[kind] : 0;
}
static int open_database(int kind, sqlite3 **db) {
    *db=0;
    if (!database_path(kind) || vcm_database_init()<0) return -1;
    int result=sqlite3_open_v2(database_path(kind),db,1,0);
    if(result!=0) { if(*db) sqlite3_close(*db); *db=0; return -1; }
    return 0;
}
static int physical_path(int kind,const char *stored,char *path,unsigned int capacity) {
    const char *prefix=kind==VCM_LIBRARY_PHOTO?"photo0:/":kind==VCM_LIBRARY_MUSIC?"music0:/":"video0:/";
    const char *physical=kind==VCM_LIBRARY_PHOTO?"ux0:/picture/":kind==VCM_LIBRARY_MUSIC?"ux0:/music/":"ux0:/video/";
    if(!stored || !path || capacity<32) return -1;
    const char *suffix=0;
    unsigned int prefix_len=(unsigned int)sceClibStrnlen(prefix,32);
    unsigned int physical_len=(unsigned int)sceClibStrnlen(physical,32);
    if(!sceClibStrncmp(stored,prefix,prefix_len)) suffix=stored+prefix_len;
    else if(!sceClibStrncmp(stored,physical,physical_len)) suffix=stored+physical_len;
    if(!suffix || !*suffix || sceClibStrstr(suffix,"..") || sceClibStrchr(suffix,'\\') ||
       sceClibStrchr(suffix,':') || sceClibStrchr(suffix,'\n')) return -1;
    int length=sceClibSnprintf(path,capacity,"%s%s",physical,suffix);
    return length>0 && (unsigned int)length<capacity?0:-1;
}
static int sidecar_path(char *path,unsigned int capacity) {
    char *slash=sceClibStrrchr(path,'/');
    char *dot=sceClibStrrchr(path,'.');
    if(!dot || (slash && dot<slash) || (unsigned int)(dot-path)+5>capacity) return -1;
    sceClibMemcpy(dot,".m4t",5);
    return 0;
}
static int append(char *out,unsigned int cap,unsigned int *used,const char *text) {
    unsigned int n=(unsigned int)sceClibStrnlen(text,cap);
    if(n>=cap || *used>cap-n-1) return -1;
    sceClibMemcpy(out+*used,text,n); *used+=n; out[*used]=0; return 0;
}
static int append_quoted(char *out,unsigned int cap,unsigned int *used,const unsigned char *text) {
    if(append(out,cap,used,"\"")<0) return -1;
    if(!text) text=(const unsigned char *)"";
    unsigned int limit=(unsigned int)sceClibStrnlen((const char *)text,257);
    if(limit>256) {
        limit=256;
        while(limit && (text[limit]&0xc0)==0x80) --limit;
    }
    for(unsigned int i=0;i<limit;++i) {
        unsigned char c=text[i];
        if(c=='"' || c=='\\') {
            char escaped[3]={'\\',(char)c,0}; if(append(out,cap,used,escaped)<0) return -1;
        } else if(c<32) {
            char escaped[7]; sceClibSnprintf(escaped,sizeof(escaped),"\\u%04x",c);
            if(append(out,cap,used,escaped)<0) return -1;
        } else {
            if(*used+1>=cap) return -1;
            out[(*used)++]=(char)c; out[*used]=0;
        }
    }
    return append(out,cap,used,"\"");
}
int vcm_library_list(int kind,int offset,int limit,char *json,unsigned int capacity) {
    if(!json || capacity<256 || offset<0 || limit<1 || limit>32 || !list_query(kind)) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(kind,&db)<0) return -1;
    int rc=sqlite3_prepare_v2(db,list_query(kind),-1,&stmt,0);
    if(rc==0) rc=sqlite3_bind_int(stmt,1,limit+1);
    if(rc==0) rc=sqlite3_bind_int(stmt,2,offset);
    unsigned int used=0; int count=0;
    if(rc==0) rc=append(json,capacity,&used,"{\"items\":[");
    while(rc==0) {
        int step=sqlite3_step(stmt);
        if(step==101) break;
        if(step!=100) { rc=-1; break; }
        if(count==limit) { ++count; break; }
        const unsigned char *path=sqlite3_column_text(stmt,2);
        const unsigned char *name=path ? (const unsigned char *)sceClibStrrchr((const char *)path,'/') : 0;
        name=name ? name+1 : path;
        char number[160];
        sceClibSnprintf(number,sizeof(number),"%s{\"id\":\"%llu\",\"title\":",count?",":"",
                        (unsigned long long)sqlite3_column_int64(stmt,0));
        if(append(json,capacity,&used,number)<0 ||
           append_quoted(json,capacity,&used,sqlite3_column_text(stmt,1))<0 ||
           append(json,capacity,&used,",\"name\":")<0 ||
           append_quoted(json,capacity,&used,name)<0) { rc=-1; break; }
        sceClibSnprintf(number,sizeof(number),",\"size\":%llu,\"created\":",
                        (unsigned long long)sqlite3_column_int64(stmt,3));
        if(append(json,capacity,&used,number)<0 ||
           append_quoted(json,capacity,&used,sqlite3_column_text(stmt,4))<0 ||
           append(json,capacity,&used,",\"artist\":")<0 ||
           append_quoted(json,capacity,&used,sqlite3_column_text(stmt,5))<0 ||
           append(json,capacity,&used,",\"album\":")<0 ||
           append_quoted(json,capacity,&used,sqlite3_column_text(stmt,6))<0) { rc=-1; break; }
        unsigned long long sidecar_size=0;
        if(kind==VCM_LIBRARY_VIDEO) {
            char sidecar[1024]; SceIoStat stat;
            if(physical_path(kind,(const char *)path,sidecar,sizeof(sidecar))==0 &&
               sidecar_path(sidecar,sizeof(sidecar))==0 &&
               sceIoGetstat(sidecar,&stat)>=0 && stat.st_size>0)
                sidecar_size=(unsigned long long)stat.st_size;
        }
        sceClibSnprintf(number,sizeof(number),",\"thumbnail\":%s,\"width\":%d,\"height\":%d,\"sidecar_size\":%llu}",
            sqlite3_column_int(stmt,7)?"true":"false",sqlite3_column_int(stmt,8),
            sqlite3_column_int(stmt,9),sidecar_size);
        if(append(json,capacity,&used,number)<0) { rc=-1; break; }
        ++count;
    }
    char tail[72];
    sceClibSnprintf(tail,sizeof(tail),"],\"next_offset\":%d}",count>limit?offset+limit:-1);
    if(rc==0) rc=append(json,capacity,&used,tail);
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    return rc==0 ? (int)used : -1;
}
int vcm_library_photo_icons(int offset,int limit,VcmLibraryWrite write,void *context) {
    if(offset<0 || limit<1 || limit>32 || !write) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(VCM_LIBRARY_PHOTO,&db)<0) return -1;
    const char *sql="SELECT p.mrid FROM tbl_VPContent p "
        "WHERE p.content_type=4 AND p.status=2 AND p.content_path IS NOT NULL "
        "ORDER BY p.created_time DESC,p.mrid DESC LIMIT ?1 OFFSET ?2";
    int rc=sqlite3_prepare_v2(db,sql,-1,&stmt,0);
    sqlite3_stmt *icon=0;
    if(rc==0) rc=sqlite3_prepare_v2(db,"SELECT data FROM tbl_Icon WHERE content_id=?1 "
        "AND data_type=1 AND codec_type=25 AND length(data) BETWEEN 128 AND 65536 "
        "ORDER BY id LIMIT 1",-1,&icon,0);
    if(rc==0) rc=sqlite3_bind_int(stmt,1,limit);
    if(rc==0) rc=sqlite3_bind_int(stmt,2,offset);
    while(rc==0) {
        int step=sqlite3_step(stmt);
        if(step==101) break;
        if(step!=100) { rc=-1; break; }
        uint64_t id=(uint64_t)sqlite3_column_int64(stmt,0);
        rc=sqlite3_bind_int64(icon,1,(long long)id);
        if(rc!=0) break;
        int icon_step=sqlite3_step(icon);
        if(icon_step!=100 && icon_step!=101) { rc=-1; break; }
        int size=icon_step==100?sqlite3_column_bytes(icon,0):0;
        if(size<128 || size>65536) size=0;
        unsigned char header[12];
        for(int i=0;i<8;++i) header[i]=(unsigned char)(id>>(8*i));
        for(int i=0;i<4;++i) header[8+i]=(unsigned char)((unsigned int)size>>(8*i));
        if(write(context,header,sizeof(header))<0 ||
           (size && write(context,sqlite3_column_blob(icon,0),(unsigned int)size)<0)) rc=-1;
        if(sqlite3_reset(icon)!=0) rc=-1;
    }
    if(icon && sqlite3_finalize(icon)!=0) rc=-1;
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    return rc;
}
int vcm_library_photo_icon(uint64_t id,void *data,unsigned int capacity) {
    if(!id || id>INT64_MAX || !data || capacity<128) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(VCM_LIBRARY_PHOTO,&db)<0) return -1;
    const char *sql="SELECT i.data FROM tbl_Icon i JOIN tbl_VPContent p "
        "ON p.mrid=i.content_id WHERE p.mrid=?1 AND p.content_type=4 AND p.status=2 "
        "AND i.data_type=1 AND i.codec_type=25 AND length(i.data) BETWEEN 128 AND 65536 "
        "ORDER BY i.id LIMIT 1";
    int rc=sqlite3_prepare_v2(db,sql,-1,&stmt,0);
    if(rc==0) rc=sqlite3_bind_int64(stmt,1,(long long)id);
    if(rc==0) rc=sqlite3_step(stmt);
    int size=-1;
    if(rc==100) {
        int available=sqlite3_column_bytes(stmt,0);
        const void *blob=sqlite3_column_blob(stmt,0);
        if(blob && available>=128 && (unsigned int)available<=capacity) {
            sceClibMemcpy(data,blob,(unsigned int)available);
            size=available;
        }
    }
    if(stmt && sqlite3_finalize(stmt)!=0) size=-1;
    if(sqlite3_close(db)!=0) size=-1;
    return size;
}
int vcm_library_resolve(int kind,uint64_t id,char *path,unsigned int capacity) {
    if(!path || capacity<32 || !database_path(kind)) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(kind,&db)<0) return -1;
    const char *sql=kind==VCM_LIBRARY_MUSIC
        ? "SELECT content_path FROM tbl_Music WHERE mrid=?1 AND status=2"
        : kind==VCM_LIBRARY_PHOTO
        ? "SELECT content_path FROM tbl_VPContent WHERE mrid=?1 AND content_type=4 AND status=2"
        : "SELECT content_path FROM tbl_VPContent WHERE mrid=?1 AND content_type=5 AND status=2";
    int rc=sqlite3_prepare_v2(db,sql,-1,&stmt,0);
    if(rc==0) rc=sqlite3_bind_int64(stmt,1,(long long)id);
    if(rc==0) rc=sqlite3_step(stmt);
    const char *stored=rc==100?(const char *)sqlite3_column_text(stmt,0):0;
    rc=physical_path(kind,stored,path,capacity);
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    return rc;
}
int vcm_library_resolve_sidecar(uint64_t id,char *path,unsigned int capacity) {
    return vcm_library_resolve(VCM_LIBRARY_VIDEO,id,path,capacity)==0
        ? sidecar_path(path,capacity) : -1;
}

static int extension_is(const char *actual,const char *expected) {
    return sceClibStrncasecmp(actual,expected,sceClibStrnlen(expected,16)+1)==0;
}
static uint16_t mtp_format(int kind,const char *name) {
    const char *dot=sceClibStrrchr(name,'.');
    if(!dot) return 0;
    if(kind==VCM_LIBRARY_PHOTO) {
        if(extension_is(dot,".jpg") || extension_is(dot,".jpeg")) return 0x3801;
        if(extension_is(dot,".png")) return 0x380b;
        if(extension_is(dot,".bmp")) return 0x3804;
        if(extension_is(dot,".gif")) return 0x3807;
        if(extension_is(dot,".tif") || extension_is(dot,".tiff")) return 0x380d;
        if(extension_is(dot,".mpo")) return 0xb301;
    } else if(kind==VCM_LIBRARY_MUSIC) {
        if(extension_is(dot,".mp3")) return 0x3009;
        if(extension_is(dot,".m4a") || extension_is(dot,".aac")) return 0xb903;
        if(extension_is(dot,".wav")) return 0x3008;
    } else if(kind==VCM_LIBRARY_VIDEO && extension_is(dot,".mp4")) return 0xb982;
    return 0;
}

static void mtp_stat_date(char out[24],const SceDateTime *value) {
    out[0]=0;
    if(value->year<1970 || value->year>9999 || value->month<1 || value->month>12 ||
       value->day<1 || value->day>31 || value->hour>23 || value->minute>59 ||
       value->second>59) return;
    sceClibSnprintf(out,24,"%04u%02u%02uT%02u%02u%02u",
        value->year,value->month,value->day,value->hour,value->minute,value->second);
}

static void mtp_db_date(char out[24],const char *value) {
    out[0]=0;
    if(!value || sceClibStrnlen(value,24)<19 || value[4]!='-' || value[7]!='-' ||
       value[10]!='T' || value[13]!=':' || value[16]!=':') return;
    static const unsigned char digits[]={0,1,2,3,5,6,8,9,11,12,14,15,17,18};
    for(unsigned i=0;i<sizeof(digits);++i)
        if(value[digits[i]]<'0' || value[digits[i]]>'9') return;
    sceClibSnprintf(out,24,"%.4s%.2s%.2sT%.2s%.2s%.2s",value,value+5,value+8,
                    value+11,value+14,value+17);
}

static uint32_t blob32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1]<<8 |
           (uint32_t)bytes[2]<<16 | (uint32_t)bytes[3]<<24;
}

int vcm_library_enumerate(int kind,VcmLibraryMedia visit,void *context) {
    if(!visit || !database_path(kind)) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(kind,&db)<0) return -1;
    const char *query=kind==VCM_LIBRARY_MUSIC
        ? "SELECT mrid,content_path,title,imported_time,artist,album_name,NULL,0,0 "
          "FROM tbl_Music WHERE status=2 AND content_path IS NOT NULL ORDER BY mrid"
        : kind==VCM_LIBRARY_PHOTO
        ? "SELECT p.mrid,p.content_path,p.title,p.created_time,'','',"
          "(SELECT substr(i.data,13,8) FROM tbl_Icon i WHERE i.content_id=p.mrid "
          "AND i.data_type=1 AND i.codec_type=25 AND length(i.data) BETWEEN 128 AND 65536 "
          "ORDER BY i.id LIMIT 1),p.width,p.height FROM tbl_VPContent p WHERE p.content_type=4 "
          "AND p.status=2 AND p.content_path IS NOT NULL ORDER BY p.mrid"
        : "SELECT mrid,content_path,title,created_time,'','',NULL,width,height FROM tbl_VPContent "
          "WHERE content_type=5 AND status=2 AND content_path IS NOT NULL ORDER BY mrid";
    int rc=sqlite3_prepare_v2(db,query,-1,&stmt,0);
    while(rc==0) {
        int step=sqlite3_step(stmt);
        if(step==101) break;
        if(step!=100) { rc=-1; break; }
        uint64_t id=(uint64_t)sqlite3_column_int64(stmt,0);
        const char *stored=(const char *)sqlite3_column_text(stmt,1);
        const char *slash=stored?sceClibStrrchr(stored,'/'):0;
        const char *name=slash?slash+1:0;
        char path[1024]; SceIoStat stat;
        if(!id || !name || !*name || !mtp_format(kind,name) ||
           physical_path(kind,stored,path,sizeof(path))<0 ||
           sceIoGetstat(path,&stat)<0 || stat.st_size<=0 || (uint64_t)stat.st_size>0xfffffff3ull) continue;
        char created[24],modified[24];
        mtp_db_date(created,(const char *)sqlite3_column_text(stmt,3));
        if(!created[0]) mtp_stat_date(created,&stat.st_ctime);
        mtp_stat_date(modified,&stat.st_mtime);
        VcmLibraryMediaInfo media={0};
        media.kind=kind; media.id=id; media.name=name;
        media.title=(const char *)sqlite3_column_text(stmt,2);
        media.artist=(const char *)sqlite3_column_text(stmt,4);
        media.album=(const char *)sqlite3_column_text(stmt,5);
        media.created=created; media.modified=modified;
        media.size=(uint64_t)stat.st_size; media.mtp_format=mtp_format(kind,name);
        int width=sqlite3_column_int(stmt,7),height=sqlite3_column_int(stmt,8);
        if(width>0 && width<=65535 && height>0 && height<=65535) {
            media.width=(uint32_t)width; media.height=(uint32_t)height;
        }
        if(kind==VCM_LIBRARY_PHOTO && sqlite3_column_bytes(stmt,6)==8) {
            const unsigned char *icon=(const unsigned char *)sqlite3_column_blob(stmt,6);
            uint32_t height=blob32(icon),width=blob32(icon+4);
            if(width && height && width<=256 && height<=256 &&
               !(width&3) && !(height&3)) {
                media.thumb_width=(uint16_t)width;
                media.thumb_height=(uint16_t)height;
            }
        }
        if(visit(context,&media)<0) {
            rc=-1; break;
        }
        if(kind==VCM_LIBRARY_VIDEO && sidecar_path(path,sizeof(path))==0 &&
           sceIoGetstat(path,&stat)>=0 && stat.st_size>0 && (uint64_t)stat.st_size<=0xfffffff3ull) {
            char sidecar_name[256];
            unsigned int name_length=(unsigned int)sceClibStrnlen(name,sizeof(sidecar_name));
            if(name_length<sizeof(sidecar_name)) {
                sceClibMemcpy(sidecar_name,name,name_length+1);
                if(sidecar_path(sidecar_name,sizeof(sidecar_name))==0) {
                    media.kind=3; media.name=sidecar_name;
                    media.size=(uint64_t)stat.st_size; media.mtp_format=0xba82;
                    media.title=""; media.artist=""; media.album="";
                    media.thumb_width=0; media.thumb_height=0;
                    media.width=0; media.height=0;
                    mtp_stat_date(modified,&stat.st_mtime);
                    if(visit(context,&media)<0) { rc=-1; break; }
                }
            }
        }
    }
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    return rc==0?0:-1;
}

int vcm_library_audit_music(char *json,unsigned int capacity) {
    if(!json || capacity<256) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(VCM_LIBRARY_MUSIC,&db)<0) return -1;
    int module=sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
    SceAppUtilInitParam init; SceAppUtilBootParam boot;
    sceClibMemset(&init,0,sizeof(init)); sceClibMemset(&boot,0,sizeof(boot));
    int apputil=module<0?module:sceAppUtilInit(&init,&boot);
    int mount=apputil<0?apputil:sceAppUtilMusicMount();
    const char *query="SELECT mrid,content_path FROM tbl_Music WHERE status=2 "
                      "AND content_path IS NOT NULL ORDER BY mrid";
    int rc=sqlite3_prepare_v2(db,query,-1,&stmt,0);
    unsigned int rows=0, eligible=0, invalid=0, unsupported=0, bad_path=0, stat_failed=0, bad_size=0;
    int first_stat=0, first_open=0, alias_stat=0;
    while(rc==0) {
        int step=sqlite3_step(stmt);
        if(step==101) break;
        if(step!=100) { rc=-1; break; }
        ++rows;
        uint64_t id=(uint64_t)sqlite3_column_int64(stmt,0);
        const char *stored=(const char *)sqlite3_column_text(stmt,1);
        const char *slash=stored?sceClibStrrchr(stored,'/'):0;
        const char *name=slash?slash+1:0;
        char path[1024]; SceIoStat stat;
        if(!id || !name || !*name) { ++invalid; continue; }
        if(!mtp_format(VCM_LIBRARY_MUSIC,name)) { ++unsupported; continue; }
        if(physical_path(VCM_LIBRARY_MUSIC,stored,path,sizeof(path))<0) {
            ++bad_path; continue;
        }
        int status=sceIoGetstat(path,&stat);
        if(status<0) {
            ++stat_failed;
            if(!first_stat) {
                first_stat=status;
                SceUID file=sceIoOpen(path,SCE_O_RDONLY,0);
                first_open=file<0?file:0;
                if(file>=0) sceIoClose(file);
                char alias[1024];
                int length=sceClibSnprintf(alias,sizeof(alias),"music0:/%s",path+sizeof("ux0:/music/")-1);
                if(length>0 && (unsigned int)length<sizeof(alias))
                    alias_stat=sceIoGetstat(alias,&stat);
            }
            continue;
        }
        if(stat.st_size<=0 || (uint64_t)stat.st_size>0xfffffff3ull) { ++bad_size; continue; }
        ++eligible;
    }
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    if(mount>=0) sceAppUtilMusicUmount();
    if(apputil>=0) sceAppUtilShutdown();
    if(rc<0) return -1;
    int size=sceClibSnprintf(json,capacity,
        "{\"rows\":%u,\"eligible\":%u,\"invalid\":%u,\"unsupported\":%u,"
        "\"bad_path\":%u,\"stat_failed\":%u,\"bad_size\":%u,"
        "\"first_stat\":%d,\"first_open\":%d,\"alias_stat\":%d,"
        "\"apputil\":%d,\"mount\":%d}",
        rows,eligible,invalid,unsupported,bad_path,stat_failed,bad_size,
        first_stat,first_open,alias_stat,apputil,mount);
    return size>0 && (unsigned int)size<capacity?size:-1;
}

int vcm_library_count(int kind,uint32_t *count) {
    if(!count || !database_path(kind)) return -1;
    sqlite3 *db; sqlite3_stmt *stmt=0;
    if(open_database(kind,&db)<0) return -1;
    const char *query=kind==VCM_LIBRARY_MUSIC
        ? "SELECT COUNT(*) FROM tbl_Music WHERE status=2 AND content_path IS NOT NULL"
        : kind==VCM_LIBRARY_PHOTO
        ? "SELECT COUNT(*) FROM tbl_VPContent WHERE content_type=4 AND status=2 "
          "AND content_path IS NOT NULL"
        : "SELECT COUNT(*) FROM tbl_VPContent WHERE content_type=5 AND status=2 "
          "AND content_path IS NOT NULL";
    int rc=sqlite3_prepare_v2(db,query,-1,&stmt,0);
    if(rc==0) rc=sqlite3_step(stmt)==100?0:-1;
    if(rc==0) {
        long long value=sqlite3_column_int64(stmt,0);
        if(value<0 || value>0xffffffffll) rc=-1;
        else *count=(uint32_t)value;
    }
    if(stmt && sqlite3_finalize(stmt)!=0) rc=-1;
    if(sqlite3_close(db)!=0) rc=-1;
    return rc==0?0:-1;
}
