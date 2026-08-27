#ifndef slic3r_Monitor_hpp_
#define slic3r_Monitor_hpp_

#include "Tabbook.hpp"
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/grid.h>
#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/statline.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/gbsizer.h>
#include <wx/statbox.h>
#include <wx/tglbtn.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/webrequest.h>
#include <map>
#include <vector>
#include <memory>
#include "Event.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "wxExtensions.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/MonitorBasePanel.h"
#include "slic3r/GUI/StatusPanel.hpp"
#include "slic3r/GUI/UpgradePanel.hpp"
#include "slic3r/GUI/HMSPanel.hpp"
#include "slic3r/GUI/AmsWidgets.hpp"
#include "slic3r/GUI/DeviceDashboard/MoonrakerDeviceController.hpp"
#include "Widgets/SideTools.hpp"
#include "SelectMachinePop.hpp"

namespace Slic3r {
namespace GUI {

class MediaFilePanel;
class PrinterWebView;
class CoPrintPrinterPicker;
class CloudTaskManagerPage;
namespace DeviceDashboard {
class PrinterOfflineOverlay;
}

class AddMachinePanel : public wxPanel
{
protected:
	Button* m_button_add_machine;
	wxStaticText* m_staticText_add_machine;
	wxStaticBitmap* m_bitmap_empty;

	void on_add_machine(wxCommandEvent& event);

public:

	AddMachinePanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL, const wxString& name = wxEmptyString);
	~AddMachinePanel();

	void msw_rescale();
};

class MonitorPanel : public wxPanel
{
public:
    enum class DeviceUiMode {
        Bambu,
        CoPrint,
        CoPrintLegacy
    };

private:
    Tabbook*		m_tabpanel{ nullptr };
    wxSizer*        m_main_sizer{ nullptr };

    AddMachinePanel*    m_status_add_machine_panel;
    StatusPanel*        m_status_info_panel;
    MediaFilePanel*     m_media_file_panel;
    UpgradePanel*       m_upgrade_panel;
    HMSPanel*           m_hms_panel;

    wxPanel*                              m_coprint_status_panel{nullptr};
    CloudTaskManagerPage*                 m_coprint_storage_page{nullptr};
    CloudTaskManagerPage*                 m_coprint_print_models_page{nullptr};
    wxPanel*                              m_coprint_update_page{nullptr};
    CoPrintPrinterPicker*                 m_coprint_printer_picker{nullptr};
    PrinterWebView*                       m_coprint_backend{nullptr};
    DeviceDashboard::PrinterOfflineOverlay* m_coprint_session_overlay{nullptr};
    std::unique_ptr<DeviceDashboard::MoonrakerDeviceController> m_coprint_controller;
    DeviceUiMode                          m_device_ui_mode{DeviceUiMode::Bambu};
    int                                   m_coprint_status_tab_index{-1};
    int                                   m_coprint_storage_tab_index{-1};
    int                                   m_coprint_models_tab_index{-1};
    int                                   m_coprint_update_tab_index{-1};
    int                                   m_bbl_hms_tab_index{-1};

	/* side tools */
    SideTools*      m_side_tools{nullptr};
    wxStaticBitmap* m_bitmap_printer_type;
    wxStaticBitmap* m_bitmap_arrow;
    wxStaticText*   m_staticText_printer_name;
    wxStaticBitmap* m_bitmap_wifi_signal;
    wxBoxSizer *    m_side_tools_sizer;
    SelectMachinePopup m_select_machine;

	/* images */
    wxBitmap m_signal_strong_img;
    wxBitmap m_signal_middle_img;
    wxBitmap m_signal_weak_img;
    wxBitmap m_signal_no_img;
    wxBitmap m_printer_img;
    wxBitmap m_arrow_img;

    int last_wifi_signal = -1;
    int last_status;
    bool m_initialized { false };
    bool update_flag{false};
    bool m_in_on_size{false};
    wxTimer* m_refresh_timer = nullptr;

public:
    MonitorPanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~MonitorPanel();

    enum PrinterTab {
        PT_STATUS  = 0,
        PT_MEDIA   = 1,
        PT_UPDATE  = 2,
        PT_HMS     = 3,
        PT_DEBUG   = 4,
        PT_MAX_NUM = 5
    };

	void init_bitmap();
    void init_timer();
    void init_tabpanel();
    void configure_device_ui(DeviceUiMode mode);
    void ensure_coprint_backend();
    void sync_coprint_page_hosting(bool embedded);
    bool is_coprint_device_ui() const
    {
        return m_device_ui_mode == DeviceUiMode::CoPrint
            || m_device_ui_mode == DeviceUiMode::CoPrintLegacy;
    }
    bool is_quadro_device_ui() const { return m_device_ui_mode == DeviceUiMode::CoPrint; }
    void show_coprint_status_page();
    Tabbook* get_tabpanel() { return m_tabpanel; };
    DeviceDashboard::MoonrakerDeviceController* coprint_device_controller() { return m_coprint_controller.get(); }
    PrinterWebView* coprint_backend() { return m_coprint_backend; }
    void set_default();
    wxWindow* create_side_tools();

    void on_sys_color_changed();
    void msw_rescale();

    StatusPanel* get_status_panel() {return m_status_info_panel;};
	void select_machine(std::string machine_sn);
    void on_timer(wxTimerEvent& event);
    void on_select_printer(wxCommandEvent& event);
    void on_printer_clicked(wxMouseEvent &event);
    void on_size(wxSizeEvent &event);

    /* update apis */
    //void update_ams(MachineObject* obj);
    void update_all();
    void force_refresh_device();
    void refresh_coprint_printer_names();

    void update_hms_tag();
    bool Show(bool show);

    void show_status(int status);

    std::string get_string_from_tab(PrinterTab tab);

    MachineObject *obj { nullptr };
    std::string last_conn_type = "undedefined";

    void stop_update() {update_flag = false;};
    void start_update() {update_flag = true;};


    void jump_to_HMS();
    void jump_to_LiveView();
    void update_network_version_footer();
};


} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
