#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>
#include "catalog_client.h"
#include "mtpif_adapter.h"

#ifndef VCM_ENABLE_ACTIVE_MTP
#define VCM_ENABLE_ACTIVE_MTP 0
#endif

/* Passive hook: it NEVER claims the USB port or modifies a command. */
static tai_hook_ref_t g_recv_ref;
static SceUID g_recv_hook = -1;
static SceUID g_worker = -1;
static volatile int g_running;
static volatile unsigned int g_logged;
static unsigned int g_hook_failures;
static volatile unsigned int g_log_lines;
#if VCM_ENABLE_ACTIVE_MTP
static VcmBridgeCatalog g_catalog = {.block = -1};
static VcmMtpServer g_server;
static int g_active;
static unsigned int g_catalog_failures;
/* Sony owns the port until the first VCM DeviceInfo response. Its responder
 * may already have accepted OpenSession before that handoff. */
static uint32_t g_sony_session;

static void release_catalog(void) {
    if (g_active) vcm_bridge_catalog_release(&g_catalog);
    g_active = 0;
}
#endif

static void log_line(const char *event, int value, int detail) {
    if (g_log_lines >= 64) return;
    ++g_log_lines;
    SceUID fd = sceIoOpen("ux0:/data/vita-content-manager/mtp-hook.log",
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    char line[128];
    int length = sceClibSnprintf(line, sizeof(line), "%s pid=%08X tid=%08X value=%08X detail=%08X\n",
                                 event, (unsigned int)sceKernelGetProcessId(),
                                 (unsigned int)sceKernelGetThreadId(),
                                 (unsigned int)value, (unsigned int)detail);
    if (length > 0 && length < (int)sizeof(line)) sceIoWrite(fd, line, (unsigned int)length);
    sceIoClose(fd);
}

/* Preserve r2 as well: the syscall checks the output buffer against it. */
static int recv_command_hook(void *port, void *command, unsigned int command_size) {
    /* taiHEN's generic TAI_CONTINUE macro uses an unprototyped function
     * pointer; this toolchain rejects its three-argument call. Preserve r2. */
    struct _tai_hook_user *current = (struct _tai_hook_user *)g_recv_ref;
    struct _tai_hook_user *next = (struct _tai_hook_user *)current->next;
    int (*original)(void *, void *, unsigned int) =
        (int (*)(void *, void *, unsigned int))(next ? next->func : current->old);
    for (;;) {
        int result = original(port, command, command_size);
        if (result < 0) {
#if VCM_ENABLE_ACTIVE_MTP
            release_catalog();
#endif
            return result;
        }
        if (!command) return result;
        VcmMtpCommand *request = (VcmMtpCommand *)command;
        unsigned int opcode = request->code;
        if (g_logged < 32 && g_logged++ < 32) {
            log_line("recv", opcode, result);
        }
#if VCM_ENABLE_ACTIVE_MTP
        if (!g_active && opcode == 0x1002 && request->param_count == 1)
            g_sony_session = request->params[0];
        else if (!g_active && opcode == 0x1003)
            g_sony_session = 0;
        if (!g_active && opcode == 0x1001) {
            int status = vcm_bridge_catalog_fetch(&g_catalog);
            if (status == 0) {
                vcm_mtp_server_init(&g_server, &g_catalog.catalog);
                vcm_mtp_server_set_storage(&g_server, g_catalog.max_size,
                                           g_catalog.free_size);
                g_server.session = request->session ? request->session : g_sony_session;
                g_active = 1;
                log_line("active", g_catalog.catalog.count, g_server.session);
            } else if (g_catalog_failures++ < 4) {
                log_line("catalog-fail", status, 0);
            }
        }
        /* Once VCM answers DeviceInfo, never mix Sony and VCM sessions. The
         * portable dispatcher returns OperationNotSupported for unknown codes. */
        if (g_active) {
            if (g_logged < 56 && g_logged++ < 56)
                log_line("vcm-op", opcode, request->param_count);
            int handled = vcm_mtpif_dispatch(port, &g_server, request);
            if (handled < 0) {
                log_line("dispatch-error", opcode, handled);
                release_catalog();
                return handled;
            }
            /* Sony's usbChargeThread likewise receives the next command
             * immediately after SendResponse, without an extra phase 0. */
            continue;
        }
#endif
        return result;
    }
}

static int watcher(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (g_running && g_recv_hook < 0) {
        tai_module_info_t module;
        module.size = sizeof(module);
        if (taiGetModuleInfo("SceLibMtp", &module) >= 0) {
            g_recv_hook = taiHookFunctionImport(&g_recv_ref, "SceLibMtp",
                                                0x5EFA4138u, 0x468B6E3Eu,
                                                recv_command_hook);
            if (g_recv_hook >= 0) {
                log_line("hook", g_recv_hook, module.modid);
                break;
            }
            if (g_hook_failures++ == 0)
                log_line("hook-error", g_recv_hook, module.modid);
        }
        sceKernelDelayThread(1000000);
    }
    return 0;
}

int _start(SceSize args, const void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, const void *argp) {
    (void)args; (void)argp;
    g_running = 1;
    g_logged = 0;
    g_hook_failures = 0;
    g_log_lines = 0;
#if VCM_ENABLE_ACTIVE_MTP
    g_catalog_failures = 0;
    g_sony_session = 0;
#endif
    log_line("start", 0, 0);
    g_worker = sceKernelCreateThread("VcmMtpBridgeProbe", watcher, 0x10000100, 0x4000, 0, 0, 0);
    if (g_worker < 0) {
        g_running = 0;
        log_line("thread-error", g_worker, 0);
        return SCE_KERNEL_START_FAILED;
    }
    int result = sceKernelStartThread(g_worker, 0, 0);
    if (result < 0) {
        g_running = 0;
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
        log_line("thread-error", result, 0);
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, const void *argp) {
    (void)args; (void)argp;
    g_running = 0;
    if (g_worker >= 0) {
        sceKernelWaitThreadEnd(g_worker, 0, 0);
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
    }
    if (g_recv_hook >= 0) {
        taiHookRelease(g_recv_hook, g_recv_ref);
        g_recv_hook = -1;
    }
#if VCM_ENABLE_ACTIVE_MTP
    release_catalog();
#endif
    log_line("stop", 0, 0);
    return SCE_KERNEL_STOP_SUCCESS;
}
