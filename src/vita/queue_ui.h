#ifndef VCM_QUEUE_UI_H
#define VCM_QUEUE_UI_H
#include "sync_queue.h"
#include "common_gui_progress.h"

static paf::Plugin *queue_plugin;
static paf::ui::ListView *queue_list;
static paf::ui::Text *empty_label;
static paf::ui::Widget *queue_ip_label;
static paf::ui::ProgressBar *queue_transfer_bar, *queue_import_bar;
static paf::ui::Text *queue_transfer_value, *queue_import_value;
static int queue_dialog_slot=-1;

static void queue_text(paf::ui::Widget *widget, const char *text) {
    if (!widget) return;
    wchar_t wide[512]; unsigned int n=0;
    const unsigned char *p=(const unsigned char *)text;
    while (*p && n<509) {
        unsigned int c=*p++;
        int more=c<0x80?0:(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:0;
        if(more) { c&=(1u<<(6-more))-1; for(int i=0;i<more;++i) { if((*p&0xc0)!=0x80) { c=0xfffd; break; } c=(c<<6)|(*p++&63); } }
        if(c>0x10ffff || (c>=0xd800 && c<=0xdfff)) c=0xfffd;
        if(c>0xffff) { c-=0x10000; wide[n++]=(wchar_t)(0xd800+(c>>10)); wide[n++]=(wchar_t)(0xdc00+(c&1023)); }
        else wide[n++]=(wchar_t)c;
    }
    wide[n]=0; widget->SetString(wide);
}
static void queue_row(paf::ui::ListItem *row, int index) {
    VcmQueueItem item;
    if (!row || vcm_queue_item(index,&item)<0) return;
    queue_text(row->FindChild("row_name"),item.name);
    static const char *states[]={"Waiting","Transferring","Transferred","Importing","Imported","Failed"};
    char detail[100];
    sceClibSnprintf(detail,sizeof(detail),"%.1f MB",(double)item.size/1048576.0);
    queue_text(row->FindChild("row_detail"),detail);
    queue_text(row->FindChild("row_state"),states[item.state]);
    paf::ui::Widget *icon=row->FindChild("row_icon");
    if(icon) icon->SetTexture(queue_plugin->GetTexture(item.kind==0?"photo_icon":item.kind==1?"music_icon":"video_icon"));
}
class QueueFactory : public paf::ui::listview::ItemFactory {
public:
    paf::ui::ListItem *Create(CreateParam &param) {
        paf::Plugin::TemplateOpenParam options;
        if(queue_plugin->TemplateOpen(param.parent,"queue_row",options)<0) return NULL;
        return static_cast<paf::ui::ListItem *>(param.parent->GetChild(param.parent->GetChildrenNum()-1));
    }
    void Start(StartParam &param) { queue_row(param.list_item,param.cell_index); }
    void Stop(StopParam &) {}
    void Dispose(DisposeParam &param) { if(param.list_item) param.list_item->DestroyWidget(); }
};
static QueueFactory *queue_factory;

static void queue_on_dialog_action(int, sce::CommonGuiDialog::DIALOG_CB action, void *) {
    if(action==sce::CommonGuiDialog::DIALOG_CB_CANCEL) vcm_queue_cancel();
}

static bool queue_dialog_open() {
    paf::wstring title(L"Copying to PS Vita");
    paf::wstring message(L"");
    int slot=sce::CommonGuiDialog::Dialog::Show(
        queue_plugin,&title,&message,&sce::CommonGuiDialog::Param::s_dialogCancel,
        queue_on_dialog_action,NULL);
    if(slot<0) return false;
    paf::ui::Widget *native_dialog=sce::CommonGuiDialog::Dialog::GetWidget(
        slot,sce::CommonGuiDialog::REGISTER_ID_DIALOG);
    if(!native_dialog) {
        sce::CommonGuiDialog::Dialog::Close(slot);
        return false;
    }
    paf::Plugin::TemplateOpenParam options;
    if(queue_plugin->TemplateOpen(native_dialog,"transfer_progress_contents",options)<0) {
        sce::CommonGuiDialog::Dialog::Close(slot);
        return false;
    }
    queue_transfer_bar=static_cast<paf::ui::ProgressBar *>(native_dialog->FindChild("transfer_bar"));
    queue_import_bar=static_cast<paf::ui::ProgressBar *>(native_dialog->FindChild("import_bar"));
    queue_transfer_value=static_cast<paf::ui::Text *>(native_dialog->FindChild("transfer_value"));
    queue_import_value=static_cast<paf::ui::Text *>(native_dialog->FindChild("import_value"));
    queue_text(native_dialog->FindChild("transfer_label"),"Transferred");
    queue_text(native_dialog->FindChild("import_label"),"Imported");
    if(!queue_transfer_bar || !queue_import_bar || !queue_transfer_value || !queue_import_value) {
        sce::CommonGuiDialog::Dialog::Close(slot);
        queue_transfer_bar=queue_import_bar=NULL;
        queue_transfer_value=queue_import_value=NULL;
        return false;
    }
    queue_transfer_bar->SetMaxValue(100);
    queue_import_bar->SetMaxValue(100);
    queue_transfer_bar->SetValue(0,false);
    queue_import_bar->SetValue(0,false);
    queue_dialog_slot=slot;
    return true;
}

static void queue_dialog_update(const VcmQueueInfo &info) {
    bool complete=info.imported+info.failed==info.count && !info.syncing;
    bool active=info.count && !info.cancelled && !complete && (info.transferred>0 || info.syncing);
    if(!active && queue_dialog_slot>=0) {
        sce::CommonGuiDialog::Dialog::Close(queue_dialog_slot);
        queue_dialog_slot=-1;
        queue_transfer_bar=queue_import_bar=NULL;
        queue_transfer_value=queue_import_value=NULL;
    }
    if(!active) return;
    if(queue_dialog_slot<0 && !queue_dialog_open()) return;
    float transfer=info.total?(float)(100.0*(double)info.transferred/(double)info.total):0.0f;
    if(transfer>100.0f) transfer=100.0f;
    float imported=info.count?(float)(100.0*(double)(info.imported+info.failed)/(double)info.count):0.0f;
    if(queue_transfer_bar) queue_transfer_bar->SetValue(transfer,false);
    if(queue_import_bar) queue_import_bar->SetValue(imported,false);
    char label[80];
    sceClibSnprintf(label,sizeof(label),"%d%%",(int)(transfer+0.5f));
    queue_text(queue_transfer_value,label);
    sceClibSnprintf(label,sizeof(label),"%d / %d",info.imported+info.failed,info.count);
    queue_text(queue_import_value,label);
}

static void queue_ui_init(paf::Plugin *plugin,paf::ui::Scene *scene) {
    queue_plugin=plugin;
    queue_list=static_cast<paf::ui::ListView *>(scene->FindChild("queue_list"));
    if(queue_list) {
        queue_factory=new QueueFactory();
        queue_list->SetItemFactory(queue_factory);
        queue_list->SetScrollType(paf::ui::ListView::SCROLL_TYPE_VERTICAL);
        queue_list->InsertSegment(0,1);
        queue_list->SetSegmentLayoutType(0,paf::ui::ListView::LAYOUT_TYPE_LIST);
        queue_list->SetCellSizeDefault(0,paf::math::v4(856,80,0,0));
    }
    empty_label=static_cast<paf::ui::Text *>(scene->FindChild("empty_text"));
    queue_text(empty_label,"Select media on your PC to begin copying.");
    queue_ip_label=scene->FindChild("system_label");
    queue_text(queue_ip_label,"Wi-Fi unavailable");
}
static void queue_ui_update_ip() {
    static char displayed[32]="";
    char address[16];
    const char *label=vcm_wifi_ip_address(address,sizeof(address))==0 ? address : "Wi-Fi unavailable";
    if(sceClibStrcmp(displayed,label)) {
        sceClibSnprintf(displayed,sizeof(displayed),"%s",label);
        queue_text(queue_ip_label,displayed);
    }
}
static void queue_ui_update(const VcmQueueInfo &info,paf::ui::Text *status) {
    static unsigned int revision=~0u;
    if(revision==info.revision) return;
    revision=info.revision;
    if(queue_list) {
        int old=queue_list->GetCellNum(0);
        if(old<info.count) queue_list->InsertCell(0,old,info.count-old);
        if(old>info.count) queue_list->DeleteCell(0,info.count,old-info.count);
        for(int i=0;i<queue_list->GetGeneratedNum();++i) {
            paf::ui::listview::ListPos pos=queue_list->GetGeneratedPos(i);
            queue_row(queue_list->GetListItem(pos.m_segment_index,pos.m_cell_index),pos.m_cell_index);
        }
    }
    if(empty_label) { if(info.count) empty_label->Hide(); else empty_label->Show(); }
    queue_dialog_update(info);
    char summary[96];
    if(!info.count) sceClibSnprintf(summary,sizeof(summary),"Wi-Fi ready");
    else if(info.cancelled) sceClibSnprintf(summary,sizeof(summary),"Stopped");
    else if(info.syncing) sceClibSnprintf(summary,sizeof(summary),"Importing %d/%d",info.imported+info.failed,info.count);
    else if(info.imported+info.failed==info.count) sceClibSnprintf(summary,sizeof(summary),"Complete");
    else sceClibSnprintf(summary,sizeof(summary),"Receiving %d%%",info.total?(int)(100*info.transferred/info.total):0);
    queue_text(status,summary);
}
#endif
