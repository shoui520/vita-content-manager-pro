#include <limits>
#include <paf.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shellutil.h>
#include <psp2/sysmodule.h>
#include <psp2/apputil.h>

#include "wifi_service.h"
#include "queue_ui.h"

extern "C" void *sceMtpIfGetPort(int port);
extern "C" int sceMtpIfIsConnected(void *port);

char sceUserMainThreadName[] = "vcm_sysapp";
int sceUserMainThreadPriority = 0x10000100;
int sceUserMainThreadCpuAffinityMask = SCE_KERNEL_CPU_MASK_SYSTEM;
SceSize sceUserMainThreadStackSize = 0x4000;

void operator delete(void *pointer, unsigned int) { sce_paf_free(pointer); }

namespace {

paf::Framework *g_framework = NULL;
paf::ui::Text *g_status = NULL;
int g_usb_dialog_slot = -1;
bool g_usb_cancel_pending = false;
bool g_usb_connected = false;
unsigned int g_usb_lock_mask = 0;
int g_shell_events_status = -1;
bool g_music_mount_active = false;
bool g_music_apputil_active = false;

int EnsureDataDirectory() {
    static const char path[] = "ux0:/data/vita-content-manager";
    int status = sceIoMkdir(path, 0777);
    if (status >= 0) return 0;
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    if (sceIoGetstat(path, &stat) < 0 || !SCE_S_ISDIR(stat.st_mode)) return status;
    return 0;
}

void LogUsbMode(const char *event, int value) {
    SceUID fd = sceIoOpen("ux0:/data/vita-content-manager/vcm-usb-mode.log",
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    char line[96];
    int length = sceClibSnprintf(line, sizeof(line), "%s 0x%08X\n", event, (unsigned int)value);
    if (length > 0 && length < (int)sizeof(line)) sceIoWrite(fd, line, length);
    sceIoClose(fd);
}

void EnsureMusicAccess() {
    if (g_music_mount_active) return;
    int status = sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
    LogUsbMode("load-music-apputil", status);
    if (status < 0) return;
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    sceClibMemset(&init, 0, sizeof(init));
    sceClibMemset(&boot, 0, sizeof(boot));
    status = sceAppUtilInit(&init, &boot);
    LogUsbMode("init-music-apputil", status);
    if (status < 0) return;
    g_music_apputil_active = true;
    status = sceAppUtilMusicMount();
    LogUsbMode("mount-music", status);
    if (status >= 0) g_music_mount_active = true;
    else {
        LogUsbMode("shutdown-music-apputil", sceAppUtilShutdown());
        g_music_apputil_active = false;
    }
}

void ReleaseMusicAccess() {
    if (g_music_mount_active) {
        LogUsbMode("unmount-music", sceAppUtilMusicUmount());
        g_music_mount_active = false;
    }
    if (g_music_apputil_active) {
        LogUsbMode("shutdown-music-apputil", sceAppUtilShutdown());
        g_music_apputil_active = false;
    }
}

void DisableUsbMode(bool close_dialog) {
    vcm_usb_mode_set(0);
    int slot = g_usb_dialog_slot;
    g_usb_dialog_slot = -1;
    g_usb_cancel_pending = false;
    g_usb_connected = false;
    if (close_dialog && slot >= 0)
        LogUsbMode("close-dialog", sce::CommonGuiDialog::Dialog::Close(slot));
    if (g_usb_lock_mask & SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION)
        LogUsbMode("unlock-usb", sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION));
    if (g_usb_lock_mask & SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN)
        LogUsbMode("unlock-ps", sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN));
    g_usb_lock_mask = 0;
}

void UsbDialogAction(int, sce::CommonGuiDialog::DIALOG_CB action, void *) {
    if (action == sce::CommonGuiDialog::DIALOG_CB_CANCEL)
        g_usb_cancel_pending = true;
}

void UpdateUsbConnection() {
    if (g_usb_dialog_slot < 0) return;
    void *port = sceMtpIfGetPort(1);
    bool link = port && sceMtpIfIsConnected(port) > 0;
    if (!link) vcm_usb_catalog_connection_lost();
    bool connected = link && vcm_usb_catalog_served();
    if (connected == g_usb_connected) return;
    g_usb_connected = connected;
    paf::ui::Widget *message = sce::CommonGuiDialog::Dialog::GetWidget(
        g_usb_dialog_slot, sce::CommonGuiDialog::REGISTER_ID_TEXT_MESSAGE_1);
    if (message) message->SetString(connected ? L"Connected" : L"Reconnect the USB cable");
    if (g_status) g_status->SetString(connected ? L"USB connected" : L"USB mode");
}

void UsbModePressed(int, paf::ui::Handler *, paf::ui::Event *, void *) {
    if (g_usb_dialog_slot >= 0) return;
    if (!queue_plugin) {
        if (g_status) g_status->SetString(L"USB mode is not ready yet.");
        return;
    }
    EnsureMusicAccess();
    if (g_shell_events_status < 0) {
        g_shell_events_status = sceShellUtilInitEvents(0);
        LogUsbMode("init-events", g_shell_events_status);
    }
    int usb_lock = sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION);
    LogUsbMode("lock-usb", usb_lock);
    if (usb_lock >= 0) g_usb_lock_mask |= SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION;
    int ps_lock = sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
    LogUsbMode("lock-ps", ps_lock);
    if (ps_lock >= 0) g_usb_lock_mask |= SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN;
    paf::wstring title(L"USB mode");
    paf::wstring message(L"Reconnect the USB cable");
    int slot = sce::CommonGuiDialog::Dialog::Show(
        queue_plugin, &title, &message, &sce::CommonGuiDialog::Param::s_dialogCancel,
        UsbDialogAction, NULL);
    if (slot < 0) {
        DisableUsbMode(false);
        if (g_status) g_status->SetString(L"Could not show USB mode.");
        return;
    }
    g_usb_dialog_slot = slot;
    g_usb_cancel_pending = false;
    g_usb_connected = false;
    vcm_usb_mode_set(1);
    if (g_status) g_status->SetString(
        usb_lock >= 0 && ps_lock >= 0 ? L"USB mode" : L"USB mode: lock error");
}

class ImportWorker : public paf::thread::Thread {
public:
    ImportWorker()
        : paf::thread::Thread(SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT, 0x20000,
                              "VcmImportWorker"), finished_(false), result_(-1) {}

    bool Finished() const { return finished_; }
    int Result() const { return result_; }

    void EntryFunction() {
        result_ = vcm_queue_run_imports();
        finished_ = true;
        Cancel();
    }

private:
    volatile bool finished_;
    int result_;
};

ImportWorker *g_worker = NULL;

class WifiWorker : public paf::thread::Thread {
public:
    WifiWorker()
        : paf::thread::Thread(SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT, 0x20000,
                              "VcmWifiWorker"), finished_(false), result_(-1) {}
    bool Finished() const { return finished_; }
    int Result() const { return result_; }
    void EntryFunction() {
        result_ = vcm_wifi_serve();
        finished_ = true;
        Cancel();
    }
private:
    volatile bool finished_;
    int result_;
};

WifiWorker *g_wifi_worker = NULL;
bool g_wifi_error_reported = false;

void PollImport(void *) {
    static SceUInt64 last_poll=0;
    static SceUInt64 last_ip_poll=0;
    static SceUInt64 last_power_tick=0;
    SceUInt64 now=sceKernelGetProcessTimeWide();
    if (g_usb_cancel_pending) {
        DisableUsbMode(true);
        if (g_status) g_status->SetString(L"Wi-Fi ready");
    }
    if (g_usb_dialog_slot >= 0 && now-last_power_tick >= 10000000) {
        last_power_tick = now;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
    if(now-last_poll<100000) return;
    last_poll=now;
    if(now-last_ip_poll>=1000000) {
        last_ip_poll=now;
        queue_ui_update_ip();
    }
    if (g_wifi_worker && g_wifi_worker->Finished() && !g_wifi_error_reported) {
        if (g_status) {
            char message[80];
            sceClibSnprintf(message, sizeof(message), "Wi-Fi failed: 0x%08X",
                            (unsigned int)g_wifi_worker->Result());
            queue_text(g_status, message);
        }
        g_wifi_error_reported = true;
    }
    if(g_worker && g_worker->Finished()) {
        g_worker->Join(); delete g_worker; g_worker=NULL;
    }
    VcmQueueInfo info; vcm_queue_info(&info);
    if(info.syncing && !g_worker) {
        g_worker=new ImportWorker();
        if(!g_worker || g_worker->Start()!=0) { delete g_worker; g_worker=NULL; vcm_queue_cancel(); }
    }
    queue_ui_update(info,g_usb_dialog_slot >= 0 ? NULL : g_status);
    if (g_usb_dialog_slot >= 0) UpdateUsbConnection();
}

void OnPluginLoaded(paf::Plugin *plugin) {
    paf::Plugin::PageOpenParam page_param;
    page_param.option = paf::Plugin::PageOption_None;
    paf::ui::Scene *scene = plugin->PageOpen("page_main", page_param);
    if (!scene) return;
    paf::ui::Text *title = static_cast<paf::ui::Text *>(scene->FindChild("title_text"));
    g_status = static_cast<paf::ui::Text *>(scene->FindChild("status_text"));
    if (title) title->SetString(L"PC → PS Vita");
    paf::ui::Widget *usb_button = scene->FindChild("usb_mode_button");
    if (usb_button) {
        usb_button->SetString(L"USB mode");
        usb_button->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE,
            static_cast<paf::ui::HandlerCB>(UsbModePressed), NULL);
    }
    if (vcm_queue_init() == 0) {
        queue_ui_init(plugin,scene);
        g_wifi_worker = new WifiWorker();
        if (!g_wifi_worker || g_wifi_worker->Start() != 0) {
            if (g_status) g_status->SetString(L"Could not start Wi-Fi receiver.");
            delete g_wifi_worker;
            g_wifi_worker = NULL;
        }
    }
    paf::common::MainThreadCallList::Register(PollImport, NULL);
}

int RunPafApplication() {
    paf::Framework::InitParam framework_param;
    framework_param.screen_width = 960;
    framework_param.screen_height = 544;
    framework_param.surface_pool_size = 0x00500000;
    framework_param.text_surface_pool_size = 0x00080000;
    framework_param.mode = paf::Framework::Mode_Application;
    framework_param.allow_button_control = true;
    framework_param.graphics_option = 7;

    g_framework = new paf::Framework(framework_param);
    if (!g_framework) return -1;
    g_framework->LoadCommonResourceSync();

    paf::Plugin::InitParam plugin_param;
    plugin_param.name = "vita_content_manager";
    plugin_param.caller_name = "__main__";
    plugin_param.resource_file = "app0:/vita_content_manager.rco";
    plugin_param.init_func = NULL;
    plugin_param.start_func = OnPluginLoaded;
    plugin_param.stop_func = NULL;
    plugin_param.exit_func = NULL;
    paf::Plugin::LoadSync(plugin_param);
    g_framework->Run();

    paf::common::MainThreadCallList::Unregister(PollImport, NULL);
    DisableUsbMode(true);
    vcm_wifi_stop();
    if (g_wifi_worker) {
        g_wifi_worker->Join();
        delete g_wifi_worker;
        g_wifi_worker = NULL;
    }
    if (g_worker) {
        g_worker->Join();
        delete g_worker;
        g_worker = NULL;
    }
    ReleaseMusicAccess();
    return 0;
}

} // namespace

extern "C" {

static void log_startup(const char *stage,int status,int detail) {
    SceUID fd=sceIoOpen("ux0:/data/vita-content-manager/vcm-startup.log",
                       SCE_O_WRONLY|SCE_O_CREAT|SCE_O_APPEND,0666);
    if(fd<0) return;
    char line[128];
    int length=sceClibSnprintf(line,sizeof(line),"%s status=0x%08X detail=0x%08X\n",
                               stage,(unsigned int)status,(unsigned int)detail);
    if(length>0) sceIoWrite(fd,line,(unsigned int)length);
    sceIoClose(fd);
}

struct ScePafInit {
    SceSize global_heap_size;
    int a2;
    int a3;
    int cdlg_mode;
    int heap_opt_param1;
    int heap_opt_param2;
};

int module_start(SceSize, void *) {
    EnsureDataDirectory();
    log_startup("module_start",0,0);
    ScePafInit init_param;
    init_param.global_heap_size = 0x00800000;
    init_param.a2 = 0xEA60;
    init_param.a3 = 0x40000;
    init_param.cdlg_mode = 0;
    init_param.heap_opt_param1 = 0;
    init_param.heap_opt_param2 = 0;

    int load_result = 0xDEADBEEF;
    SceSysmoduleOpt sysmodule_opt;
    sceClibMemset(&sysmodule_opt, 0, sizeof(sysmodule_opt));
    sysmodule_opt.result = &load_result;
    int result = sceSysmoduleLoadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF, sizeof(init_param), &init_param, &sysmodule_opt);
    log_startup("paf_load",result,load_result);
    if ((result | load_result) != 0) return SCE_KERNEL_START_FAILED;
    // NPXS10008 loads this before using its native PAF-backed dialogs.
    result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_COMMON_GUI_DIALOG);
    log_startup("common_gui_load",result,0);
    if (result < 0) {
        sceClibPrintf("VCM: CommonGuiDialog load failed: 0x%08X\n",(unsigned int)result);
        return SCE_KERNEL_START_FAILED;
    }
    RunPafApplication();
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_COMMON_GUI_DIALOG);
    return SCE_KERNEL_START_SUCCESS;
}

} // extern "C"
