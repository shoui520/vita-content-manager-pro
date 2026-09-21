#include "music_rw_vfs.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2common/kernel/iofilemgr.h>

/* SQLite 3.20 public VFS ABI, limited to its version-1 methods. Sony's
 * libSceSqlite exposes the same sqlite3_vfs_register interface. */
typedef struct VcmSqliteFile VcmSqliteFile;
typedef struct VcmSqliteVfs VcmSqliteVfs;
typedef struct VcmSqliteIo VcmSqliteIo;
struct VcmSqliteFile { const VcmSqliteIo *pMethods; };
struct VcmSqliteIo {
    int iVersion;
    int (*xClose)(VcmSqliteFile *);
    int (*xRead)(VcmSqliteFile *, void *, int, long long);
    int (*xWrite)(VcmSqliteFile *, const void *, int, long long);
    int (*xTruncate)(VcmSqliteFile *, long long);
    int (*xSync)(VcmSqliteFile *, int);
    int (*xFileSize)(VcmSqliteFile *, long long *);
    int (*xLock)(VcmSqliteFile *, int);
    int (*xUnlock)(VcmSqliteFile *, int);
    int (*xCheckReservedLock)(VcmSqliteFile *, int *);
    int (*xFileControl)(VcmSqliteFile *, int, void *);
    int (*xSectorSize)(VcmSqliteFile *);
    int (*xDeviceCharacteristics)(VcmSqliteFile *);
};
struct VcmSqliteVfs {
    int iVersion, szOsFile, mxPathname;
    VcmSqliteVfs *pNext;
    const char *zName;
    void *pAppData;
    int (*xOpen)(VcmSqliteVfs *, const char *, VcmSqliteFile *, int, int *);
    int (*xDelete)(VcmSqliteVfs *, const char *, int);
    int (*xAccess)(VcmSqliteVfs *, const char *, int, int *);
    int (*xFullPathname)(VcmSqliteVfs *, const char *, int, char *);
    void *(*xDlOpen)(VcmSqliteVfs *, const char *);
    void (*xDlError)(VcmSqliteVfs *, int, char *);
    void (*(*xDlSym)(VcmSqliteVfs *, void *, const char *))(void);
    void (*xDlClose)(VcmSqliteVfs *, void *);
    int (*xRandomness)(VcmSqliteVfs *, int, char *);
    int (*xSleep)(VcmSqliteVfs *, int);
    int (*xCurrentTime)(VcmSqliteVfs *, double *);
    int (*xGetLastError)(VcmSqliteVfs *, int, char *);
};
typedef struct { VcmSqliteFile base; SceUID fd; } VitaFile;
extern VcmSqliteVfs *sqlite3_vfs_find(const char *);
extern int sqlite3_vfs_register(VcmSqliteVfs *, int);

#define SQLITE_OK 0
#define SQLITE_CANTOPEN 14
#define SQLITE_IOERR_READ 266
#define SQLITE_IOERR_SHORT_READ 522
#define SQLITE_IOERR_WRITE 778
#define SQLITE_IOERR_FSYNC 1034
#define SQLITE_IOERR_TRUNCATE 1546
#define SQLITE_IOERR_FSTAT 1802
#define SQLITE_IOERR_DELETE 2570
#define SQLITE_OPEN_READONLY 0x01
#define SQLITE_OPEN_READWRITE 0x02
#define SQLITE_OPEN_CREATE 0x04
#define SQLITE_OPEN_EXCLUSIVE 0x10

static int vcm_close(VcmSqliteFile *file) {
    VitaFile *f = (VitaFile *)file;
    int rc = sceIoClose(f->fd);
    return rc < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}
static int vcm_read(VcmSqliteFile *file, void *buffer, int length, long long offset) {
    VitaFile *f = (VitaFile *)file;
    if (sceIoLseek(f->fd, offset, SCE_SEEK_SET) != offset) return SQLITE_IOERR_READ;
    int got = sceIoRead(f->fd, buffer, length);
    if (got == length) return SQLITE_OK;
    if (got < 0) return SQLITE_IOERR_READ;
    sceClibMemset((char *)buffer + got, 0, (SceSize)(length - got));
    return SQLITE_IOERR_SHORT_READ;
}
static int vcm_write(VcmSqliteFile *file, const void *buffer, int length, long long offset) {
    VitaFile *f = (VitaFile *)file;
    if (sceIoLseek(f->fd, offset, SCE_SEEK_SET) != offset) return SQLITE_IOERR_WRITE;
    const char *bytes = (const char *)buffer;
    int written = 0;
    while (written < length) {
        int rc = sceIoWrite(f->fd, bytes + written, length - written);
        if (rc <= 0) return SQLITE_IOERR_WRITE;
        written += rc;
    }
    return SQLITE_OK;
}
static int vcm_truncate(VcmSqliteFile *file, long long size) {
    VitaFile *f = (VitaFile *)file;
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    stat.st_size = size;
    return sceIoChstatByFd(f->fd, &stat, SCE_CST_SIZE) < 0
        ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}
static int vcm_sync(VcmSqliteFile *file, int flags) {
    (void)flags;
    return sceIoSyncByFd(((VitaFile *)file)->fd, 0) < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}
static int vcm_size(VcmSqliteFile *file, long long *size) {
    SceIoStat stat;
    if (sceIoGetstatByFd(((VitaFile *)file)->fd, &stat) < 0) return SQLITE_IOERR_FSTAT;
    *size = stat.st_size;
    return SQLITE_OK;
}
/* The one-shot importer must run while no media apps are open. This lock
 * implementation does not coordinate with Sony's private media service. */
static int vcm_lock(VcmSqliteFile *file, int level) { (void)file; (void)level; return SQLITE_OK; }
static int vcm_unlock(VcmSqliteFile *file, int level) { (void)file; (void)level; return SQLITE_OK; }
static int vcm_reserved(VcmSqliteFile *file, int *held) {
    (void)file; *held = 0; return SQLITE_OK;
}
static int vcm_control(VcmSqliteFile *file, int op, void *arg) {
    (void)file; (void)op; (void)arg; return SQLITE_OK;
}
static int vcm_sector(VcmSqliteFile *file) { (void)file; return 512; }
static int vcm_device(VcmSqliteFile *file) { (void)file; return 0; }
static const VcmSqliteIo kMethods = {
    1, vcm_close, vcm_read, vcm_write, vcm_truncate, vcm_sync,
    vcm_size, vcm_lock, vcm_unlock, vcm_reserved, vcm_control,
    vcm_sector, vcm_device
};

static int vcm_open(VcmSqliteVfs *vfs, const char *name, VcmSqliteFile *file,
                    int flags, int *out_flags) {
    (void)vfs;
    if (!name) return SQLITE_CANTOPEN;
    int io_flags = (flags & SQLITE_OPEN_READWRITE) ? SCE_O_RDWR : SCE_O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) io_flags |= SCE_O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) io_flags |= SCE_O_EXCL;
    SceUID fd = sceIoOpen(name, io_flags, 0666);
    if (fd < 0) return SQLITE_CANTOPEN;
    VitaFile *f = (VitaFile *)file;
    f->fd = fd;
    f->base.pMethods = &kMethods;
    if (out_flags) *out_flags = flags;
    return SQLITE_OK;
}
static int vcm_delete(VcmSqliteVfs *vfs, const char *name, int sync_dir) {
    (void)vfs; (void)sync_dir;
    return sceIoRemove(name) < 0 ? SQLITE_IOERR_DELETE : SQLITE_OK;
}
static int vcm_access(VcmSqliteVfs *vfs, const char *name, int flags, int *out) {
    (void)vfs; (void)flags;
    SceIoStat stat;
    *out = sceIoGetstat(name, &stat) == 0;
    return SQLITE_OK;
}
static int vcm_fullpath(VcmSqliteVfs *vfs, const char *name, int cap, char *out) {
    (void)vfs;
    unsigned int len = (unsigned int)sceClibStrnlen(name, (SceSize)cap);
    if (len >= (unsigned int)cap) return SQLITE_CANTOPEN;
    sceClibMemcpy(out, name, len + 1);
    return SQLITE_OK;
}

static VcmSqliteVfs kVfs = {
    1, sizeof(VitaFile), 1024, 0, "vcm_music_rw", 0,
    vcm_open, vcm_delete, vcm_access, vcm_fullpath,
    0, 0, 0, 0, 0, 0, 0, 0
};
int vcm_register_music_rw_vfs(void) {
    VcmSqliteVfs *native = sqlite3_vfs_find(0);
    if (!native) return SQLITE_CANTOPEN;
    kVfs.xDlOpen = native->xDlOpen;
    kVfs.xDlError = native->xDlError;
    kVfs.xDlSym = native->xDlSym;
    kVfs.xDlClose = native->xDlClose;
    kVfs.xRandomness = native->xRandomness;
    kVfs.xSleep = native->xSleep;
    kVfs.xCurrentTime = native->xCurrentTime;
    kVfs.xGetLastError = native->xGetLastError;
    return sqlite3_vfs_register(&kVfs, 0);
}
