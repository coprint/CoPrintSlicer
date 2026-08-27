#include <unistd.h>
#include <sys/sysctl.h>
#import <AppKit/AppKit.h>
#import <wx/osx/cocoa/dataview.h>
#include <wx/app.h>
#include <wx/colordlg.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/fontdlg.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/weakref.h>
#import "GUI_Utils.hpp"

namespace Slic3r {
namespace GUI {

namespace {

NSWindow *ns_window_from_wx(wxWindow *window)
{
    if (window == nullptr)
        return nil;

    WXWidget handle = window->GetHandle();
    if (handle == nullptr)
        return nil;

    NSView *view = (NSView *) handle;
    if ([view isKindOfClass:[NSWindow class]])
        return (NSWindow *) view;
    return [view window];
}

bool macos_should_manage_dialog(wxDialog *dialog)
{
    if (dialog == nullptr)
        return false;
    if (dialog->HasFlag(wxSTAY_ON_TOP))
        return false;
    if (dialog->GetName() == "settings_dialog")
        return false;
    if (dialog->GetName() == "coprint_preferences")
        return false;
    if (wxDynamicCast(dialog, wxFileDialog) != nullptr)
        return false;
    if (wxDynamicCast(dialog, wxDirDialog) != nullptr)
        return false;
    if (wxDynamicCast(dialog, wxColourDialog) != nullptr)
        return false;
    if (wxDynamicCast(dialog, wxFontDialog) != nullptr)
        return false;
    if (wxDynamicCast(dialog, wxMessageDialog) != nullptr)
        return false;
    if (wxDynamicCast(dialog, wxRichMessageDialog) != nullptr)
        return false;
    return true;
}

class MacDialogZOrderFilter : public wxEventFilter
{
public:
    MacDialogZOrderFilter() { wxEvtHandler::AddFilter(this); }
    ~MacDialogZOrderFilter() { wxEvtHandler::RemoveFilter(this); }

    int FilterEvent(wxEvent &event) override
    {
        if (event.GetEventType() != wxEVT_SHOW)
            return Event_Skip;

        auto *dlg = dynamic_cast<wxDialog *>(event.GetEventObject());
        if (!macos_should_manage_dialog(dlg))
            return Event_Skip;

        auto &show_event = static_cast<wxShowEvent &>(event);
        if (show_event.IsShown()) {
            macos_attach_dialog_to_parent(dlg);
            wxWeakRef<wxDialog> weak(dlg);
            dlg->CallAfter([weak]() {
                if (weak && weak->IsShown())
                    macos_attach_dialog_to_parent(weak.get());
            });
        } else {
            macos_detach_dialog_from_parent(dlg);
        }
        return Event_Skip;
    }
};

MacDialogZOrderFilter *g_dialog_zorder_filter = nullptr;

} // namespace

void macos_install_dialog_zorder_filter()
{
    if (g_dialog_zorder_filter == nullptr)
        g_dialog_zorder_filter = new MacDialogZOrderFilter();
}

void macos_remove_dialog_zorder_filter()
{
    delete g_dialog_zorder_filter;
    g_dialog_zorder_filter = nullptr;
}

void macos_exclude_from_system_settings(wxWindow *window)
{
    NSWindow *nsw = ns_window_from_wx(window);
    if (nsw == nil)
        return;
    nsw.excludedFromWindowsMenu = YES;
    nsw.hidesOnDeactivate = YES;
    NSWindowCollectionBehavior behavior = nsw.collectionBehavior;
    behavior |= NSWindowCollectionBehaviorTransient;
    behavior |= NSWindowCollectionBehaviorIgnoresCycle;
    nsw.collectionBehavior = behavior;
}

void macos_attach_dialog_to_parent(wxDialog *dialog)
{
    if (!macos_should_manage_dialog(dialog))
        return;

    NSWindow *dlg_window = ns_window_from_wx(dialog);
    if (dlg_window == nil)
        return;

    dlg_window.hidesOnDeactivate = NO;
    if ([dlg_window isKindOfClass:[NSPanel class]]) {
        NSPanel *panel = (NSPanel *) dlg_window;
        [panel setFloatingPanel:NO];
        [panel setBecomesKeyOnlyIfNeeded:NO];
    }

    NSWindowCollectionBehavior behavior = dlg_window.collectionBehavior;
    behavior |= NSWindowCollectionBehaviorManaged;
    behavior |= NSWindowCollectionBehaviorMoveToActiveSpace;
    behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    behavior &= ~NSWindowCollectionBehaviorTransient;
    behavior &= ~NSWindowCollectionBehaviorCanJoinAllSpaces;
    dlg_window.collectionBehavior = behavior;

    [dlg_window setLevel:NSModalPanelWindowLevel];

    // Do not call addChildWindow: here. A child NSWindow cannot enter
    // runModalForWindow: — on macOS 13+ that aborts the process with no
    // DiagnosticReport, which is exactly how Preferences/Settings dies.
    // Window level is enough to keep the dialog above the main frame.
    [dlg_window makeKeyAndOrderFront:nil];
}

void macos_detach_dialog_from_parent(wxDialog *dialog)
{
    if (dialog == nullptr)
        return;

    NSWindow *dlg_window = ns_window_from_wx(dialog);
    if (dlg_window == nil)
        return;

    if (dlg_window.parentWindow != nil)
        [dlg_window.parentWindow removeChildWindow:dlg_window];
    [dlg_window setLevel:NSNormalWindowLevel];
}

void dataview_remove_insets(wxDataViewCtrl* dv) {
    NSScrollView* scrollview = (NSScrollView*) ((wxCocoaDataViewControl*)dv->GetDataViewPeer())->GetWXWidget();
    NSOutlineView* outlineview = scrollview.documentView;
    [outlineview setIntercellSpacing: NSMakeSize(0.0, 1.0)];
    if (@available(macOS 11, *)) {
        [outlineview setStyle:NSTableViewStylePlain];
    }
}

void staticbox_remove_margin(wxStaticBox* sb) {
    NSBox* nativeBox = (NSBox*)sb->GetHandle();
    [nativeBox setBoxType:NSBoxCustom];
    [nativeBox setBorderWidth:0];
}

bool is_debugger_present()
// Returns true if the current process is being debugged (either
// running under the debugger or has a debugger attached post facto).
// https://stackoverflow.com/a/2200786/3289421
{
    int                 junk;
    int                 mib[4];
    struct kinfo_proc   info;
    size_t              size;

    // Initialize the flags so that, if sysctl fails for some bizarre
    // reason, we get a predictable result.

    info.kp_proc.p_flag = 0;

    // Initialize mib, which tells sysctl the info we want, in this case
    // we're looking for information about a specific process ID.

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    // Call sysctl.

    size = sizeof(info);
    junk = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.

    return ( (info.kp_proc.p_flag & P_TRACED) != 0 );
}

}
}

