#include "media_database.h"
#include "music_rw_vfs.h"
#include "id3_tags.h"

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sqlite.h>
#include <psp2/sysmodule.h>
#include <psp2common/kernel/iofilemgr.h>

/* VitaSDK provides the SceSqlite stub but not SQLite's public header. These
 * declarations match SQLite's documented ABI and are used read-only here. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
extern int sqlite3_initialize(void);
extern int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
extern int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
extern int sqlite3_step(sqlite3_stmt *);
extern int sqlite3_column_int(sqlite3_stmt *, int);
extern int sqlite3_finalize(sqlite3_stmt *);
extern int sqlite3_close(sqlite3 *);
extern int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **),
                        void *, char **);
extern int sqlite3_bind_int64(sqlite3_stmt *, int, long long);
extern int sqlite3_bind_int(sqlite3_stmt *, int, int);
extern int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
extern int sqlite3_bind_null(sqlite3_stmt *, int);
extern int sqlite3_changes(sqlite3 *);

static const char kDb[] = "ux0:/mms/music/AVContent.db";

static int alloc_calls = -1;
static int alloc_failures = -1;
static int largest_request = -1;
static SceClibMspace db_space = (SceClibMspace)-1;
static void *sqlite_malloc(int bytes) {
    ++alloc_calls;
    if (bytes > largest_request) largest_request = bytes;
    void *pointer = bytes > 0 ? sceClibMspaceMalloc(db_space, (SceSize)bytes) : 0;
    if (!pointer) ++alloc_failures;
    return pointer;
}
static void *sqlite_realloc(void *pointer, int bytes) {
    ++alloc_calls;
    if (bytes > largest_request) largest_request = bytes;
    void *result = bytes >= 0 ? sceClibMspaceRealloc(db_space, pointer, (SceSize)bytes) : 0;
    if (!result && bytes) ++alloc_failures;
    return result;
}
static void sqlite_free(void *pointer) { sceClibMspaceFree(db_space, pointer); }

static void record(SceUID fd, const char *step, int value) {
    char line[160];
    sceClibSnprintf(line, sizeof(line), "media DB: %s 0x%08X\n", step,
                    (unsigned int)value);
    unsigned int length = (unsigned int)sceClibStrnlen(line, sizeof(line));
    if (fd >= 0) {
        sceIoWrite(fd, line, length);
    }
}

static int scalar(sqlite3 *db, const char *sql, int *value) {
    sqlite3_stmt *statement = 0;
    int status = sqlite3_prepare_v2(db, sql, -1, &statement, 0);
    if (status != 0) return status;
    status = sqlite3_step(statement);
    if (status == 100) *value = sqlite3_column_int(statement, 0);
    int final_status = sqlite3_finalize(statement);
    if (status != 100) return status;
    return final_status;
}

static int initialize_database(SceUID log_fd) {
    static int ready;
    if (ready) return 0;
    if (db_space && db_space != (SceClibMspace)-1) return -1;
    int status = sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
    record(log_fd, "load SQLite sysmodule", status);
    if (status < 0) return -1;
    SceUID arena_uid = sceKernelAllocMemBlock("vcm_db_arena",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * 1024 * 1024, 0);
    record(log_fd, "allocate SQLite arena", arena_uid);
    if (arena_uid < 0) return -1;
    void *arena_base = 0;
    status = sceKernelGetMemBlockBase(arena_uid, &arena_base);
    record(log_fd, "map SQLite arena", status);
    if (status < 0 || !arena_base) return -1;
    db_space = sceClibMspaceCreate(arena_base, 4 * 1024 * 1024);
    record(log_fd, "create SQLite allocator", db_space && db_space != (SceClibMspace)-1 ? 0 : -1);
    if (!db_space || db_space == (SceClibMspace)-1) return -1;
    SceSqliteMallocMethods methods = {sqlite_malloc, sqlite_realloc, sqlite_free};
    status = sceSqliteConfigMallocMethods(&methods);
    record(log_fd, "configure SQLite allocator", status);
    if (status != 0) return -1;
    status = sqlite3_initialize();
    record(log_fd, "initialize SQLite", status);
    record(log_fd, "SQLite allocator call count", alloc_calls + 1);
    record(log_fd, "SQLite allocator failure count", alloc_failures + 1);
    record(log_fd, "SQLite largest request", largest_request);
    if (status != 0) return -1;
    ready=1;
    return 0;
}

int vcm_database_init(void) { return initialize_database(-1); }

static int probe(SceUID log_fd) {
    if(initialize_database(log_fd)<0) return -1;
    int status;
    sqlite3 *db = 0;
    status = sqlite3_open_v2(kDb, &db, 0x00000001, 0);
    record(log_fd, "open live database read-only", status);
    if (status != 0) {
        if (db) sqlite3_close(db);
        return -1;
    }
    int version = -1;
    status = scalar(db, "PRAGMA user_version", &version);
    record(log_fd, "read user_version status", status);
    record(log_fd, "user_version value", version);
    int count = -1;
    if (status == 0) {
        status = scalar(db, "SELECT count(*) FROM tbl_Music", &count);
        record(log_fd, "read track count status", status);
        record(log_fd, "track count value", count);
    }
    int close_status = sqlite3_close(db);
    record(log_fd, "close database", close_status);
    return status == 0 && close_status == 0 && version == 265 && count >= 0 ? 0 : -1;
}

int vcm_video_folder(SceUID log, const char *title, const char *path) {
    if(!title[0]) return 0;
    if(initialize_database(log)<0 || vcm_register_music_rw_vfs()!=0) return -1;
    sqlite3 *db=0;
    int status=sqlite3_open_v2("ux0:/mms/video/AVContent.db",&db,2,"vcm_music_rw");
    /* INSERT/SELECT may materialize a small intermediate result. Our bounded
     * VFS deliberately does not create unnamed OS temporary files. */
    if(status==0) status=sqlite3_exec(db,"PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE",0,0,0);
    const char *queries[]={
        "INSERT INTO tbl_AVContentList(type,title,title_for_sort) SELECT 4,?1,CAST(?1 AS BLOB) WHERE NOT EXISTS(SELECT 1 FROM tbl_AVContentList WHERE type=4 AND title=?1)",
        "INSERT INTO tbl_AVContentItem(list_id,item_id,item_type,order_of_item) SELECT l.mrid,p.mrid,5,COALESCE((SELECT MAX(order_of_item)+1 FROM tbl_AVContentItem WHERE list_id=l.mrid),0) FROM tbl_AVContentList l,tbl_VPContent p WHERE l.type=4 AND l.title=?1 AND p.content_path=?2 AND NOT EXISTS(SELECT 1 FROM tbl_AVContentItem WHERE list_id=l.mrid AND item_id=p.mrid)",
        "SELECT count(*) FROM tbl_AVContentItem i JOIN tbl_AVContentList l ON l.mrid=i.list_id JOIN tbl_VPContent p ON p.mrid=i.item_id WHERE l.type=4 AND l.title=?1 AND p.content_path=?2"
    };
    for(int i=0;i<3 && status==0;++i) {
        sqlite3_stmt *stmt=0;
        status=sqlite3_prepare_v2(db,queries[i],-1,&stmt,0);
        record(log,"folder statement index",i);
        record(log,"folder prepare",status);
        if(status==0) status=sqlite3_bind_text(stmt,1,title,-1,0);
        if(status==0 && i) status=sqlite3_bind_text(stmt,2,path,-1,0);
        if(status==0) {
            int step=sqlite3_step(stmt);
            record(log,"folder step",step);
            if(i==2 && step==100) record(log,"folder verified members",sqlite3_column_int(stmt,0));
            status=i==2?(step==100 && sqlite3_column_int(stmt,0)==1?0:-1):(step==101?0:-1);
        }
        if(stmt) sqlite3_finalize(stmt);
    }
    if(db) {
        int final=sqlite3_exec(db,status==0?"COMMIT":"ROLLBACK",0,0,0);
        if(status==0) status=final;
        if(sqlite3_close(db)!=0) status=-1;
    }
    record(log,"register video folder membership",status);
    return status==0?0:-1;
}

int vcm_verify_library_item(SceUID log, int kind, const char *path) {
    if(initialize_database(log)<0) return -1;
    char physical[1100];
    if(kind==1 && !sceClibStrncmp(path,"music0:/",8)) {
        sceClibSnprintf(physical,sizeof(physical),"ux0:/music/%s",path+8);
        path=physical;
    }
    const char *database=kind==0?"ux0:/mms/photo/AVContent.db":kind==1?kDb:"ux0:/mms/video/AVContent.db";
    const char *sql=kind==1?"SELECT count(*) FROM tbl_Music WHERE content_path=?":"SELECT count(*) FROM tbl_VPContent WHERE content_path=?";
    sqlite3 *db=0; sqlite3_stmt *stmt=0;
    int status=vcm_register_music_rw_vfs(), count=0;
    if(status==0) status=sqlite3_open_v2(database,&db,0x00000001,"vcm_music_rw");
    record(log,"verify database open",status);
    if(status==0) status=sqlite3_prepare_v2(db,sql,-1,&stmt,0);
    record(log,"verify database query",status);
    if(status==0) status=sqlite3_bind_text(stmt,1,path,-1,0);
    if(status==0) { status=sqlite3_step(stmt); if(status==100) count=sqlite3_column_int(stmt,0); }
    if(stmt) sqlite3_finalize(stmt);
    int closed=db?sqlite3_close(db):0;
    record(log,"verify imported library row",count==1 && closed==0?0:-1);
    return count==1 && closed==0?0:-1;
}

static int copy_song(SceUID log_fd, const char *source_path,
                     const char *output_path, long long *file_size) {
    SceIoStat source_stat;
    int status = sceIoGetstat(source_path, &source_stat);
    record(log_fd, "song source stat", status);
    if (status < 0 || source_stat.st_size == 0) return -1;
    SceIoStat destination_stat;
    if (sceIoGetstat(output_path, &destination_stat) == 0) {
        if (destination_stat.st_size != source_stat.st_size) {
            record(log_fd, "destination size mismatch", -1);
            return -1;
        }
        record(log_fd, "reuse verified staged song", 0);
        *file_size = (long long)source_stat.st_size;
        return 0;
    }
    SceUID input = sceIoOpen(source_path, SCE_O_RDONLY, 0);
    record(log_fd, "open song source", input);
    if (input < 0) return -1;
    SceUID output = sceIoOpen(output_path,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
    record(log_fd, "create song destination", output);
    if (output < 0) { sceIoClose(input); return -1; }
    SceUID buffer_uid = sceKernelAllocMemBlock("vcm_song_copy",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 65536, 0);
    if (buffer_uid < 0) {
        record(log_fd, "allocate copy buffer", buffer_uid);
        sceIoClose(input); sceIoClose(output); return -1;
    }
    void *buffer = 0;
    status = sceKernelGetMemBlockBase(buffer_uid, &buffer);
    record(log_fd, "map copy buffer", status);
    long long copied = 0;
    if (status == 0 && buffer) {
        while (copied < (long long)source_stat.st_size) {
            int got = sceIoRead(input, buffer, 65536);
            if (got <= 0) { status = got < 0 ? got : -1; break; }
            int offset = 0;
            while (offset < got) {
                int wrote = sceIoWrite(output, (char *)buffer + offset, got - offset);
                if (wrote <= 0) { status = wrote < 0 ? wrote : -1; break; }
                offset += wrote;
            }
            if (status < 0) break;
            copied += got;
        }
    } else status = -1;
    if (status == 0 && copied == (long long)source_stat.st_size)
        status = sceIoSyncByFd(output, 0);
    record(log_fd, "copy and sync song", status);
    sceKernelFreeMemBlock(buffer_uid);
    sceIoClose(input);
    sceIoClose(output);
    if (status < 0 || copied != (long long)source_stat.st_size) return -1;
    *file_size = copied;
    return 0;
}

static int register_song_with_facts(SceUID log_fd, const char *database,
                                    const char *output_path, const char *title,
                                    long long file_size, int refresh,
                                    const VcmId3Tags *tags, int sample_rate,
                                    int channel_type, int duration_ms) {
    int vfs_status = vcm_register_music_rw_vfs();
    record(log_fd, "register writable SQLite VFS", vfs_status);
    if (vfs_status != 0) return -1;
    sqlite3 *db = 0;
    int status = sqlite3_open_v2(database, &db, 0x00000002, "vcm_music_rw");
    record(log_fd, "open music database read-write", status);
    if (status != 0) { if (db) sqlite3_close(db); return -1; }
    status = sqlite3_exec(db, "PRAGMA journal_mode=DELETE", 0, 0, 0);
    record(log_fd, "require DELETE journal", status);
    if (status == 0) status = sqlite3_exec(db, "PRAGMA synchronous=FULL", 0, 0, 0);
    record(log_fd, "require FULL sync", status);
    if (status == 0) status = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);
    record(log_fd, "begin music transaction", status);
    if (status != 0) { sqlite3_close(db); return -1; }
    static const char sql[] =
        "INSERT INTO tbl_Music (codec_type,track_num,disc_num,size,"
        "container_type,status,analyzed,permission_type,"
        "sampling_rate,channel_type,duration,icon_data_type,"
        "icon_width,icon_height,icon_size,icon_offset,icon_codec_type,"
        "artist,title,genre,"
        "created_time,last_updated_time,imported_time,content_path,"
        "album_artist,album_name) VALUES "
        "(12,?,1,?,7,2,1,1,?,?,?,?,?,?,?,?,?,?,?,?,"
        "strftime('%Y-%m-%dT%H:%M:%f','now'),"
        "strftime('%Y-%m-%dT%H:%M:%f','now'),"
        "strftime('%Y-%m-%dT%H:%M:%f','now'),?,?,?)";
    sqlite3_stmt *statement = 0;
    status = sqlite3_prepare_v2(db, sql, -1, &statement, 0);
    record(log_fd, "prepare music row", status);
    if (status == 0) status = sqlite3_bind_int(statement, 1, tags ? tags->track : 0);
    if (status == 0) status = sqlite3_bind_int64(statement, 2, file_size);
    if (status == 0) status = sqlite3_bind_int(statement, 3, sample_rate);
    if (status == 0) status = sqlite3_bind_int(statement, 4, channel_type);
    if (status == 0) status = sqlite3_bind_int(statement, 5, duration_ms);
    if (status == 0) status = sqlite3_bind_int(statement, 6,
        tags && tags->cover_size ? 1 : -1);
    if (status == 0) status = sqlite3_bind_int(statement, 7,
        tags ? tags->cover_width : 0);
    if (status == 0) status = sqlite3_bind_int(statement, 8,
        tags ? tags->cover_height : 0);
    if (status == 0) status = sqlite3_bind_int64(statement, 9,
        tags ? tags->cover_size : 0);
    if (status == 0) status = sqlite3_bind_int64(statement, 10,
        tags ? tags->cover_offset : 0);
    if (status == 0) status = sqlite3_bind_int(statement, 11,
        tags ? tags->cover_codec_type : 0);
    if (status == 0) status = sqlite3_bind_text(statement, 12,
        tags ? tags->artist : "Unknown", -1, 0);
    if (status == 0) status = sqlite3_bind_text(statement, 13,
        tags ? tags->title : title, -1, 0);
    if (status == 0) status = tags && tags->genre[0]
        ? sqlite3_bind_text(statement, 14, tags->genre, -1, 0)
        : sqlite3_bind_null(statement, 14);
    if (status == 0) status = sqlite3_bind_text(statement, 15, output_path, -1, 0);
    if (status == 0) status = sqlite3_bind_text(statement, 16,
        tags ? tags->album_artist : "Unknown", -1, 0);
    if (status == 0) status = sqlite3_bind_text(statement, 17,
        tags ? tags->album : "Unknown", -1, 0);
    record(log_fd, "bind music row", status);
    if (status == 0) status = sqlite3_step(statement);
    record(log_fd, "insert music row", status);
    int inserted = status == 101;
    if (statement) sqlite3_finalize(statement);
    if (inserted) {
        /* The stock Music app groups tracks by these BLOB keys. Derive every
         * key from the row just inserted, so Unicode tags and untagged fields
         * follow the same rule for any source file. The raw UTF-8 fallback
         * gives stable membership, though not Sony's locale-aware ordering. */
        static const char sort_sql[] =
            "UPDATE tbl_Music SET "
            "title_for_sort=CAST(ifnull(title,'') AS BLOB),"
            "artist_for_sort=CAST(ifnull(artist,'') AS BLOB),"
            "album_artist_for_sort=CAST(ifnull(album_artist,'') AS BLOB),"
            "album_name_for_sort=CAST(ifnull(album_name,'') AS BLOB),"
            "genre_for_sort=CAST(ifnull(genre,'') AS BLOB) "
            "WHERE content_path=?";
        statement = 0;
        status = sqlite3_prepare_v2(db, sort_sql, -1, &statement, 0);
        if (status == 0) status = sqlite3_bind_text(statement, 1, output_path, -1, 0);
        if (status == 0) status = sqlite3_step(statement);
        int updated = status == 101 && sqlite3_changes(db) == 1;
        record(log_fd, "populate new song category keys", updated ? 0 : status);
        if (statement) sqlite3_finalize(statement);
        inserted = updated;
    }
    if (inserted && refresh) {
        status = sqlite3_exec(db, "UPDATE tbl_config SET val=0 WHERE ikey=0", 0, 0, 0);
        record(log_fd, "refresh music database config", status);
        if (status != 0) inserted = 0;
    }
    status = sqlite3_exec(db, inserted ? "COMMIT" : "ROLLBACK", 0, 0, 0);
    record(log_fd, inserted ? "commit music row" : "rollback music row", status);
    int close_status = sqlite3_close(db);
    record(log_fd, "close music database", close_status);
    return inserted && status == 0 && close_status == 0 ? 0 : -1;
}

static int read_id3_from_vita(SceUID log_fd, const char *path, VcmId3Tags *tags) {
    SceUID input = sceIoOpen(path, SCE_O_RDONLY, 0);
    record(log_fd, "open original MP3 tags", input);
    if (input < 0) return -1;
    unsigned char header[10];
    int got = sceIoRead(input, header, sizeof(header));
    if (got != (int)sizeof(header) || header[0] != 'I' ||
        header[1] != 'D' || header[2] != '3') {
        record(log_fd, "read ID3 header", got);
        sceIoClose(input);
        return -1;
    }
    for (int i = 6; i < 10; ++i) {
        if (header[i] & 0x80) { sceIoClose(input); return -1; }
    }
    unsigned int tag_size = ((unsigned int)header[6] << 21) |
                            ((unsigned int)header[7] << 14) |
                            ((unsigned int)header[8] << 7) | header[9];
    unsigned int total = tag_size + 10;
    if (total > 1024 * 1024 || total < 10) {
        record(log_fd, "ID3 tag outside 1 MiB bound", (int)total);
        sceIoClose(input);
        return -1;
    }
    unsigned int arena_size = (total + 4095) & ~4095u;
    SceUID arena_uid = sceKernelAllocMemBlock("vcm_id3_tag",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, arena_size, 0);
    record(log_fd, "allocate bounded ID3 buffer", arena_uid);
    if (arena_uid < 0) { sceIoClose(input); return -1; }
    unsigned char *buffer = 0;
    int status = sceKernelGetMemBlockBase(arena_uid, (void **)&buffer);
    if (status == 0 && buffer) {
        sceClibMemcpy(buffer, header, sizeof(header));
        unsigned int offset = 10;
        while (offset < total) {
            got = sceIoRead(input, buffer + offset, total - offset);
            if (got <= 0) { status = got < 0 ? got : -1; break; }
            offset += (unsigned int)got;
        }
        if (status == 0) status = vcm_parse_id3v23(buffer, total, tags);
    } else status = -1;
    record(log_fd, "parse original ID3v2.3 tags", status);
    sceKernelFreeMemBlock(arena_uid);
    sceIoClose(input);
    return status == 0 ? 0 : -1;
}

static int mount_music_for_file_import(SceUID log_fd) {
    int status = sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
    record(log_fd, "load AppUtil for music mount", status);
    if (status < 0) return -1;
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    sceClibMemset(&init, 0, sizeof(init));
    sceClibMemset(&boot, 0, sizeof(boot));
    status = sceAppUtilInit(&init, &boot);
    record(log_fd, "initialize AppUtil for music mount", status);
    if (status < 0) return -1;
    status = sceAppUtilMusicMount();
    record(log_fd, "mount music0 for file import", status);
    if (status < 0) {
        record(log_fd, "shutdown AppUtil after mount failure", sceAppUtilShutdown());
        return -1;
    }
    return 0;
}

int vcm_music_sync_mp3(SceUID log, const VcmQueueItem *item, const char *source, char *output) {
    if (mount_music_for_file_import(log) < 0) return -1;
    int result = -1;
    sceClibSnprintf(output, 1024, "ux0:/music/vcm-%s.mp3", item->id);
    SceIoStat st;
    VcmId3Tags tags;
    sceClibMemset(&tags, 0, sizeof(tags));
    /* Host metadata also covers untagged MP3 and ID3v2.4. Parse v2.3 when
     * present to preserve its directly addressable embedded cover. */
    read_id3_from_vita(log, source, &tags);
    sceClibStrncpy(tags.title, item->title[0] ? item->title : item->name, sizeof(tags.title)-1);
    sceClibStrncpy(tags.artist, item->artist[0] ? item->artist : "Unknown", sizeof(tags.artist)-1);
    sceClibStrncpy(tags.album, item->album[0] ? item->album : "Unknown", sizeof(tags.album)-1);
    sceClibStrncpy(tags.album_artist, item->album_artist[0] ? item->album_artist : tags.artist, sizeof(tags.album_artist)-1);
    sceClibStrncpy(tags.genre, item->genre, sizeof(tags.genre)-1);
    tags.track = item->track;
    if (probe(log) == 0 && sceIoGetstat(output, &st) < 0 && sceIoGetstat(source, &st) == 0) {
        /* Both paths are on ux0:; renaming avoids a second full music copy.
         * Restore the staged source if the database transaction fails. */
        char mounted[160];
        sceClibSnprintf(mounted,sizeof(mounted),"music0:/vcm-%s.mp3",item->id);
        result = sceIoRename(source, mounted);
        int moved = result == 0;
        if (!moved) {
            long long size=0;
            result=copy_song(log,source,mounted,&size);
        }
        if (result == 0) {
            result = sceIoSync("ux0:", 0);
            if (result == 0) result = register_song_with_facts(log, kDb, output, tags.title,
                st.st_size, 1, &tags, item->sample_rate, item->channels, item->duration_ms);
            if (result != 0 && moved) sceIoRename(mounted, source);
        }
    }
    sceAppUtilMusicUmount();
    sceAppUtilShutdown();
    return result;
}
