#ifndef VCM_COMMON_GUI_PROGRESS_H
#define VCM_COMMON_GUI_PROGRESS_H

// The component's full header pulls in legacy Sony SDK headers. These are the
// exact PAF-backed CommonGuiDialog declarations used here.
namespace sce { namespace CommonGuiDialog {

enum DIALOG_CB {
    DIALOG_CB_X = 1,
    DIALOG_CB_OK = 2,
    DIALOG_CB_CANCEL = 3
};

enum REGISTER_ID {
    REGISTER_ID_TEXT_MESSAGE_1 = 1,
    REGISTER_ID_DIALOG = 4
};

class Param {
public:
    static Param s_dialogCancel;
};

class Dialog {
public:
    static int Show(paf::Plugin *plugin, paf::wstring *title, paf::wstring *message,
                    Param *param, void (*callback)(int,DIALOG_CB,void *), void *user);
    static paf::ui::Widget *GetWidget(int slot, REGISTER_ID id);
    static int Close(int slot);
};

} }
#endif
