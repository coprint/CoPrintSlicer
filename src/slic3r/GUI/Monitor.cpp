#include "Tab.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>
#include <wx/wupdlock.h>
#include <wx/dataview.h>
#include <wx/tglbtn.h>

#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "format.hpp"
#include "MediaPlayCtrl.h"
#include "MediaFilePanel.h"
#include "Plater.hpp"
#include "BindDialog.hpp"
#include "PrinterWebView.hpp"
#include "CoPrintPrinterPicker.hpp"
#include "MultiTaskManagerPage.hpp"
#include "DeviceDashboard/PrinterOfflineOverlay.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {

#define REFRESH_INTERVAL       1000

namespace {
void rehost_expand(wxWindow *win, wxWindow *new_parent, wxSizer *new_sizer)
{
    if (win == nullptr || new_parent == nullptr || new_sizer == nullptr)
        return;
    if (win->GetParent() == new_parent && win->GetContainingSizer() == new_sizer)
        return;
    if (wxSizer *old = win->GetContainingSizer())
        old->Detach(win);
    if (win->GetParent() != new_parent)
        win->Reparent(new_parent);
    if (new_sizer->GetItem(win) == nullptr)
        new_sizer->Add(win, 1, wxEXPAND);
    win->Show();
    new_parent->Layout();
}
} // namespace

AddMachinePanel::AddMachinePanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxPanel(parent, id, pos, size, style)
{
    this->SetBackgroundColour(0xEEEEEE);

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    topsizer->AddStretchSpacer();

    m_bitmap_empty = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
    m_bitmap_empty->SetBitmap(create_scaled_bitmap("monitor_status_empty", nullptr, 250));
    topsizer->Add(m_bitmap_empty, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 0);
    topsizer->AddSpacer(46);

    wxBoxSizer* horiz_sizer = new wxBoxSizer(wxHORIZONTAL);
    horiz_sizer->Add(0, 0, 538, 0, 0);

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxVERTICAL);
    m_button_add_machine = new Button(this, "", "monitor_add_machine", FromDIP(24));
    m_button_add_machine->SetCornerRadius(FromDIP(12));
    StateColor button_bg(
        std::pair<wxColour, int>(0xCECECE, StateColor::Pressed),
        std::pair<wxColour, int>(0xCECECE, StateColor::Hovered),
        std::pair<wxColour, int>(this->GetBackgroundColour(), StateColor::Normal)
    );
    m_button_add_machine->SetBackgroundColor(button_bg);
    m_button_add_machine->SetBorderColor(0x909090);
    m_button_add_machine->SetMinSize(wxSize(96, 39));
    btn_sizer->Add(m_button_add_machine, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 5);
    m_staticText_add_machine = new wxStaticText(this, wxID_ANY, _L("click to add machine"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_add_machine->Wrap(-1);
    m_staticText_add_machine->SetForegroundColour(0x909090);
    btn_sizer->Add(m_staticText_add_machine, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 5);

    horiz_sizer->Add(btn_sizer);
    horiz_sizer->Add(0, 0, 624, 0, 0);

    topsizer->Add(horiz_sizer, 0, wxEXPAND, 0);

    topsizer->AddStretchSpacer();

    this->SetSizer(topsizer);
    this->Layout();

    // Connect Events
    m_button_add_machine->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(AddMachinePanel::on_add_machine), NULL, this);
}

void AddMachinePanel::msw_rescale() {

}

void AddMachinePanel::on_add_machine(wxCommandEvent& event) {
    // load a url
}

AddMachinePanel::~AddMachinePanel() {
    m_button_add_machine->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(AddMachinePanel::on_add_machine), NULL, this);
}

MonitorPanel::MonitorPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style),
    m_select_machine(SelectMachinePopup(this))
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    init_bitmap();

    init_tabpanel();

    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxLEFT, 0);
    SetSizerAndFit(m_main_sizer);

    init_timer();

    m_side_tools->get_panel()->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(MonitorPanel::on_printer_clicked), NULL, this);

    Bind(wxEVT_TIMER, &MonitorPanel::on_timer, this);
    Bind(wxEVT_SIZE, &MonitorPanel::on_size, this);
    Bind(wxEVT_COMMAND_CHOICE_SELECTED, &MonitorPanel::on_select_printer, this);

    m_select_machine.Bind(EVT_FINISHED_UPDATE_MACHINE_LIST, [this](wxCommandEvent& e) {
        m_side_tools->start_interval();
        });

    Bind(EVT_ALREADY_READ_HMS, [this](wxCommandEvent& e) {
        auto key = e.GetString().ToStdString();
        auto iter = m_hms_panel->temp_hms_list.find(key);
        if (iter != m_hms_panel->temp_hms_list.end()) {
            m_hms_panel->temp_hms_list[key].set_read();
        }

        update_hms_tag();
        e.Skip();
        });
}

MonitorPanel::~MonitorPanel()
{
    m_side_tools->get_panel()->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(MonitorPanel::on_printer_clicked), NULL, this);

    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;
}

void MonitorPanel::init_bitmap()
{
    m_signal_strong_img = create_scaled_bitmap("monitor_signal_strong", nullptr, 24);
    m_signal_middle_img = create_scaled_bitmap("monitor_signal_middle", nullptr, 24);
    m_signal_weak_img = create_scaled_bitmap("monitor_signal_weak", nullptr, 24);
    m_signal_no_img   = create_scaled_bitmap("monitor_signal_no", nullptr, 24);
    m_printer_img = create_scaled_bitmap("monitor_printer", nullptr, 26);
    m_arrow_img = create_scaled_bitmap("monitor_arrow",nullptr, 14);
}

void MonitorPanel::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    m_refresh_timer->Start(REFRESH_INTERVAL);
    if (update_flag) { update_all();}
}

void MonitorPanel::init_tabpanel()
{
    m_side_tools = new SideTools(this, wxID_ANY);
    wxBoxSizer* sizer_side_tools = new wxBoxSizer(wxVERTICAL);
    sizer_side_tools->Add(m_side_tools, 1, wxEXPAND, 0);
    m_tabpanel             = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_side_tools->set_table_panel(m_tabpanel);
    m_tabpanel->SetBackgroundColour(wxColour("#FEFFFF"));
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
        auto page = m_tabpanel->GetCurrentPage();
        if (page == m_media_file_panel) {
            auto title = m_tabpanel->GetPageText(m_tabpanel->GetSelection());
            m_media_file_panel->SwitchStorage(title == _L("Storage"));
        } else if (is_quadro_device_ui()) {
            if (m_coprint_printer_picker != nullptr)
                m_coprint_printer_picker->set_status_page_active(page == m_coprint_status_panel);
            if (page == m_coprint_print_models_page)
                m_coprint_print_models_page->reload_media_models();
            else if (page == m_coprint_update_page && m_coprint_backend != nullptr) {
                m_coprint_backend->refresh_update_page_from_selected_machine();
            }
        }
        page->SetFocus();
        update_all();
        }, m_tabpanel->GetId());

    //m_status_add_machine_panel = new AddMachinePanel(m_tabpanel);
    m_status_info_panel        = new StatusPanel(m_tabpanel);
    m_tabpanel->AddPage(m_status_info_panel, _L("Status"), "", true);

    m_media_file_panel = new MediaFilePanel(m_tabpanel);
    m_tabpanel->AddPage(m_media_file_panel, _L("Storage"), "", false);

    m_upgrade_panel = new UpgradePanel(m_tabpanel);
    m_tabpanel->AddPage(m_upgrade_panel, _L("System"), "", false);

    m_hms_panel = new HMSPanel(m_tabpanel);
    m_tabpanel->AddPage(m_hms_panel, _L("Assistant(HMS)"),    "", false);
    m_bbl_hms_tab_index = static_cast<int>(m_tabpanel->GetPageCount()) - 1;

    m_coprint_status_panel = new wxPanel(m_tabpanel, wxID_ANY);
    m_coprint_status_panel->SetBackgroundColour(wxColour("#EEEEEF"));
    m_coprint_status_panel->Hide();
    m_tabpanel->AddPage(m_coprint_status_panel, _L("Status"), "", false);
    m_coprint_status_tab_index = static_cast<int>(m_tabpanel->GetPageCount()) - 1;

    m_coprint_storage_page = new CloudTaskManagerPage(m_tabpanel, CloudTaskManagerPage::MediaPresentation::TimelapseOnly);
    m_coprint_storage_page->Hide();
    m_tabpanel->AddPage(m_coprint_storage_page, _L("Timelapse"), "", false);
    m_coprint_storage_tab_index = static_cast<int>(m_tabpanel->GetPageCount()) - 1;

    m_coprint_print_models_page = new CloudTaskManagerPage(m_tabpanel, CloudTaskManagerPage::MediaPresentation::ModelOnly);
    m_coprint_print_models_page->Hide();
    m_tabpanel->AddPage(m_coprint_print_models_page, _L("Media"), "", false);
    m_coprint_models_tab_index = static_cast<int>(m_tabpanel->GetPageCount()) - 1;

    m_coprint_update_page = new wxPanel(m_tabpanel, wxID_ANY);
    m_coprint_update_page->SetBackgroundColour(wxColour("#EEEEEF"));
    m_coprint_update_page->Hide();
    m_tabpanel->AddPage(m_coprint_update_page, _L("System"), "", false);
    m_coprint_update_tab_index = static_cast<int>(m_tabpanel->GetPageCount()) - 1;

    m_initialized = true;
    show_status((int)MonitorStatus::MONITOR_NO_PRINTER);
}

void MonitorPanel::ensure_coprint_backend()
{
    if (m_coprint_backend != nullptr)
        return;

    m_coprint_backend = new PrinterWebView(this);
    m_coprint_backend->Hide();
    if (m_main_sizer != nullptr && m_main_sizer->GetItem(m_coprint_backend) == nullptr)
        m_main_sizer->Add(m_coprint_backend, 1, wxEXPAND);

    m_coprint_controller = std::make_unique<DeviceDashboard::MoonrakerDeviceController>(m_coprint_backend);

    if (m_coprint_session_overlay == nullptr) {
        m_coprint_session_overlay = new DeviceDashboard::PrinterOfflineOverlay(this);
        m_coprint_session_overlay->set_ok_handler([this]() {
            if (m_coprint_backend != nullptr)
                m_coprint_backend->acknowledge_device_connect_failure();
        });
        m_coprint_backend->set_device_session_ui_handler(
            [this](PrinterWebView::DeviceSessionUi ui, const wxString &message, const wxString &hint) {
                if (m_coprint_session_overlay == nullptr)
                    return;
                using Kind = DeviceDashboard::PrinterOfflineOverlay::Kind;
                if (ui == PrinterWebView::DeviceSessionUi::Connecting)
                    m_coprint_session_overlay->set_kind(Kind::Connecting, message);
                else if (ui == PrinterWebView::DeviceSessionUi::Failed)
                    m_coprint_session_overlay->set_kind(Kind::Failed, message,
                        hint.empty()
                            ? _L("Check that the printer is powered on and on the same network.")
                            : hint);
                else
                    m_coprint_session_overlay->set_kind(Kind::Hidden);
            });
    }

    m_coprint_backend->attach_media_pages(m_coprint_storage_page, m_coprint_print_models_page);
}

void MonitorPanel::sync_coprint_page_hosting(bool embedded)
{
    if (m_coprint_backend == nullptr)
        return;

    m_coprint_backend->set_embedded_in_monitor(embedded);

    wxPanel *status = m_coprint_backend->coprint_status_host();
    wxPanel *update = m_coprint_backend->coprint_update_page();
    wxPanel *content = m_coprint_backend->content_host();

    if (embedded) {
        if (status != nullptr && m_coprint_status_panel != nullptr) {
            wxSizer *host_sizer = m_coprint_status_panel->GetSizer();
            if (host_sizer == nullptr) {
                host_sizer = new wxBoxSizer(wxVERTICAL);
                m_coprint_status_panel->SetSizer(host_sizer);
            }
            rehost_expand(status, m_coprint_status_panel, host_sizer);
        }
        if (update != nullptr && m_coprint_update_page != nullptr) {
            wxSizer *sizer = m_coprint_update_page->GetSizer();
            if (sizer == nullptr) {
                sizer = new wxBoxSizer(wxVERTICAL);
                m_coprint_update_page->SetSizer(sizer);
            }
            rehost_expand(update, m_coprint_update_page, sizer);
        }
        return;
    }

    if (content == nullptr)
        return;
    if (wxSizer *cs = content->GetSizer()) {
        if (status != nullptr)
            rehost_expand(status, content, cs);
        if (update != nullptr)
            rehost_expand(update, content, cs);
    }
    m_coprint_backend->select_tab(PrinterWebViewTab::Status);
}

void MonitorPanel::configure_device_ui(DeviceUiMode mode)
{
    if (!m_initialized)
        return;
    if (mode == DeviceUiMode::CoPrint || mode == DeviceUiMode::CoPrintLegacy)
        ensure_coprint_backend();

    if (mode == m_device_ui_mode) {
        if (mode == DeviceUiMode::CoPrint || mode == DeviceUiMode::CoPrintLegacy)
            refresh_coprint_printer_names();
        return;
    }

    m_device_ui_mode = mode;
    const bool quadro = mode == DeviceUiMode::CoPrint;
    const bool legacy = mode == DeviceUiMode::CoPrintLegacy;
    const bool coprint = quadro || legacy;

    if (coprint)
        sync_coprint_page_hosting(quadro);
    else if (m_coprint_session_overlay != nullptr)
        m_coprint_session_overlay->set_kind(DeviceDashboard::PrinterOfflineOverlay::Kind::Hidden);

    auto show_tab = [this](int index, bool show) {
        if (index < 0)
            return;
        if (wxWindow* page = m_tabpanel->GetPage(index))
            page->Show(show);
        m_tabpanel->GetBtnsListCtrl()->showPage(static_cast<size_t>(index), show);
    };

    show_tab(0, !coprint); // BBL Status
    show_tab(1, !coprint); // BBL Storage
    show_tab(2, !coprint); // BBL Update
    show_tab(m_bbl_hms_tab_index, !coprint);

    if (m_coprint_status_tab_index >= 0)
        m_tabpanel->GetBtnsListCtrl()->showPage(static_cast<size_t>(m_coprint_status_tab_index), false);
    if (!quadro)
        show_tab(m_coprint_status_tab_index, false);
    show_tab(m_coprint_storage_tab_index, false);
    show_tab(m_coprint_models_tab_index, quadro);
    show_tab(m_coprint_update_tab_index, quadro);

    if (m_coprint_printer_picker == nullptr && quadro) {
        wxWindow* side_parent = m_side_tools->GetParent();
        if (side_parent != nullptr) {
            wxSizer* side_sizer = side_parent->GetSizer();
            if (side_sizer != nullptr) {
                m_coprint_printer_picker = new CoPrintPrinterPicker(side_parent, m_coprint_backend);
                m_coprint_printer_picker->set_open_status_handler([this]() { show_coprint_status_page(); });
                side_sizer->Insert(0, m_coprint_printer_picker, 0, wxEXPAND);
            }
        }
    }
    if (m_coprint_printer_picker != nullptr)
        m_coprint_printer_picker->Show(quadro);

    if (m_side_tools != nullptr)
        m_side_tools->Show(mode == DeviceUiMode::Bambu);

    if (m_main_sizer != nullptr) {
        if (m_tabpanel != nullptr)
            m_main_sizer->Show(m_tabpanel, !legacy);
        if (m_coprint_backend != nullptr)
            m_main_sizer->Show(m_coprint_backend, legacy);
    } else {
        if (m_tabpanel != nullptr)
            m_tabpanel->Show(!legacy);
        if (m_coprint_backend != nullptr)
            m_coprint_backend->Show(legacy);
    }

    if (quadro && m_coprint_status_tab_index >= 0) {
        m_tabpanel->SetSelection(m_coprint_status_tab_index);
        if (m_coprint_printer_picker != nullptr)
            m_coprint_printer_picker->set_status_page_active(true);
    }

    if (coprint) {
        if (auto* dev = wxGetApp().getDeviceManager()) {
            if (dev->get_selected_machine() == nullptr)
                dev->load_last_machine();
        }
        refresh_coprint_printer_names();
    }

    {
        wxWindowUpdateLocker freeze(this);
        Layout();
    }
    if (m_coprint_session_overlay != nullptr)
        m_coprint_session_overlay->layout_over_parent();
    update_all();
}

void MonitorPanel::show_coprint_status_page()
{
    if (m_tabpanel == nullptr || m_coprint_status_tab_index < 0)
        return;
    m_tabpanel->SetSelection(m_coprint_status_tab_index);
    if (m_coprint_backend != nullptr)
        m_coprint_backend->refresh_layer_info_from_selected_machine();
    if (m_coprint_printer_picker != nullptr)
        m_coprint_printer_picker->set_status_page_active(true);
    Layout();
}

void MonitorPanel::set_default()
{
    obj = nullptr;
    last_conn_type = "undefined";

    /* reset status panel*/
    m_status_info_panel->set_default();

    /* reset side tool*/
    //m_bitmap_wifi_signal->SetBitmap(wxNullBitmap);
}

wxWindow* MonitorPanel::create_side_tools()
{
    //TEST function
    //m_bitmap_wifi_signal->Connect(wxEVT_LEFT_DCLICK, wxMouseEventHandler(MonitorPanel::on_update_all), NULL, this);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    auto        panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(0, FromDIP(50)));
    panel->SetBackgroundColour(wxColour(135,206,250));
    panel->SetSizer(sizer);
    sizer->Layout();
    panel->Fit();
    return panel;
}

void MonitorPanel::on_sys_color_changed()
{
    m_status_info_panel->on_sys_color_changed();
    m_upgrade_panel->on_sys_color_changed();
    m_media_file_panel->Rescale();
}

void MonitorPanel::msw_rescale()
{
    init_bitmap();

    /* side_tool rescale */
    m_side_tools->msw_rescale();
    m_tabpanel->Rescale();
    if (m_coprint_printer_picker != nullptr) {
        const int sidebar_w = FromDIP(DEVICE_SIDEBAR_DIP_WIDTH);
        m_coprint_printer_picker->SetMinSize(wxSize(sidebar_w, -1));
#ifdef __WXMSW__
        m_coprint_printer_picker->SetSize(wxSize(sidebar_w, m_coprint_printer_picker->GetSize().GetHeight()));
#endif
    }
    //m_status_add_machine_panel->msw_rescale();
    m_status_info_panel->msw_rescale();
    m_media_file_panel->Rescale();
    m_upgrade_panel->msw_rescale();
    m_hms_panel->msw_rescale();
    if (m_coprint_backend != nullptr)
        m_coprint_backend->msw_rescale();

    Layout();
    Refresh();
}

void MonitorPanel::sync_session_overlay()
{
    if (m_coprint_session_overlay != nullptr)
        m_coprint_session_overlay->layout_over_parent();
}

void MonitorPanel::select_machine(std::string machine_sn)
{
    wxCommandEvent *event = new wxCommandEvent(wxEVT_COMMAND_CHOICE_SELECTED);
    event->SetString(machine_sn);
    wxQueueEvent(this, event);
}


void MonitorPanel::on_timer(wxTimerEvent& event)
{
    if (update_flag) {
        update_all();
        //Layout();
    }
}

void MonitorPanel::on_select_printer(wxCommandEvent& event)
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    if ( dev->get_selected_machine() && (dev->get_selected_machine()->get_dev_id() != event.GetString().ToStdString()) && m_hms_panel) {
        m_hms_panel->clear_hms_tag();
    }

    if (!dev->set_selected_machine(event.GetString().ToStdString()))
        return;

    if (is_coprint_device_ui()) {
        if (is_quadro_device_ui() && m_coprint_printer_picker != nullptr)
            m_coprint_printer_picker->update_selection();
        if (m_coprint_controller)
            m_coprint_controller->refresh();
        Layout();
        return;
    }

    set_default();
    update_all();

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_) {
        obj_->last_cali_version = -1;
        obj_->reset_pa_cali_history_result();
        obj_->reset_pa_cali_result();
        Sidebar &sidebar = GUI::wxGetApp().sidebar();
        sidebar.update_sync_status(obj_);
        sidebar.set_need_auto_sync_after_connect_printer(sidebar.need_auto_sync_extruder_list_after_connect_priner(obj_));
    }

    Layout();
}

void MonitorPanel::on_printer_clicked(wxMouseEvent &event)
{
    if (is_quadro_device_ui()) {
        if (m_coprint_backend != nullptr)
            m_coprint_backend->toggle_printers_popup_at(m_side_tools);
        return;
    }

    auto mouse_pos = ClientToScreen(event.GetPosition());
    wxPoint rect = m_side_tools->ClientToScreen(wxPoint(0, 0));

    if (!m_side_tools->is_in_interval()) {
        wxPoint pos = m_side_tools->ClientToScreen(wxPoint(0, 0));
        pos.y += m_side_tools->GetRect().height;
        //pos.x = pos.x < 0? 0:pos.x;
        m_select_machine.Move(pos);

#ifdef __linux__
        m_select_machine.SetSize(wxSize(m_side_tools->GetSize().x, -1));
        m_select_machine.SetMaxSize(wxSize(m_side_tools->GetSize().x, -1));
        m_select_machine.SetMinSize(wxSize(m_side_tools->GetSize().x, -1));
#endif

        m_select_machine.Popup();
    }
}

void MonitorPanel::on_size(wxSizeEvent &event)
{
    event.Skip();
    if (m_in_on_size)
        return;
    m_in_on_size = true;
    Layout();
    m_in_on_size = false;
}

void MonitorPanel::refresh_coprint_printer_names()
{
    if (!is_coprint_device_ui() || m_coprint_backend == nullptr)
        return;

    m_coprint_backend->refresh_coprint_device_names([this]() {
        if (m_coprint_printer_picker == nullptr)
            return;
        m_coprint_printer_picker->refresh_list(true);
        m_coprint_printer_picker->update_selection();
    });
}

void MonitorPanel::force_refresh_device()
{
    if (!m_initialized)
        return;

    if (is_coprint_device_ui()) {
        if (m_coprint_backend != nullptr)
            m_coprint_backend->invalidate_device_cache_and_refresh();
        if (is_quadro_device_ui()) {
            if (m_coprint_print_models_page != nullptr)
                m_coprint_print_models_page->invalidate_media_cache_and_reload();
            if (m_coprint_storage_page != nullptr)
                m_coprint_storage_page->invalidate_media_cache_and_reload();
            if (m_coprint_printer_picker != nullptr) {
                m_coprint_printer_picker->refresh_list(true);
                m_coprint_printer_picker->update_selection();
            }
        }
        refresh_coprint_printer_names();
        return;
    }

    update_all();
}

void MonitorPanel::update_all()
{
    if (!m_initialized)
        return;

    NetworkAgent* m_agent = wxGetApp().getAgent();
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    obj = dev->get_selected_machine();

    if (!obj) {
        show_status((int)MONITOR_NO_PRINTER);
        m_hms_panel->clear_hms_tag();
        m_tabpanel->GetBtnsListCtrl()->showNewTag(PT_HMS, false);
        if (is_coprint_device_ui() && m_coprint_controller &&
            (is_quadro_device_ui() ? (m_tabpanel != nullptr && m_tabpanel->GetCurrentPage() == m_coprint_status_panel)
                                   : true))
            m_coprint_controller->refresh();
        if (is_quadro_device_ui() && m_coprint_printer_picker != nullptr)
            m_coprint_printer_picker->update_selection();
        else if (!is_coprint_device_ui() && m_status_info_panel->IsShown()) {
            m_status_info_panel->m_media_play_ctrl->SetMachineObject(obj);
            m_status_info_panel->update(obj);
        }
        return;
    }

    if (obj->connection_type() != last_conn_type) { last_conn_type = obj->connection_type(); }

    m_side_tools->update_status(obj);

    // CoPrint dashboard talks to Moonraker over HTTP, not Bambu push_status.
    // is_connecting()/is_connected() stay stuck after a picker click (reset()
    // zeroes m_push_count), so those gates must not skip the Status refresh.
    if (is_coprint_device_ui()) {
        if (is_quadro_device_ui()) {
            auto current_page = m_tabpanel->GetCurrentPage();
            // Media/System tab switches must not layout the Device dashboard.
            // That re-entered wxSizer::Layout until CalcMin crashed, and cancelled
            // in-flight wxWebRequest objects on the NSURLSession thread.
            if (current_page == m_coprint_status_panel && m_coprint_controller)
                m_coprint_controller->refresh();
            if (m_coprint_printer_picker != nullptr)
                m_coprint_printer_picker->update_selection();
            if (current_page == m_coprint_storage_page)
                m_coprint_storage_page->update_page();
            else if (current_page == m_coprint_print_models_page) {
                m_coprint_print_models_page->update_page();
                m_coprint_print_models_page->ensure_media_models_for_selected_machine();
            }
        } else if (m_coprint_controller) {
            m_coprint_controller->refresh();
        }
        if (obj->is_connecting())
            show_status(MONITOR_CONNECTING);
        else if (!obj->is_connected())
            show_status((int) MONITOR_DISCONNECTED);
        else
            show_status(MONITOR_NORMAL);
        return;
    }

    if (obj->is_connecting()) {
        show_status(MONITOR_CONNECTING);
        return;
    } else if (!obj->is_connected()) {
        int server_status = 0;
        // only disconnected server in cloud mode
        if (obj->connection_type() != "lan") {
            if (m_agent) {
                server_status = m_agent->is_server_connected(wxGetApp().get_printer_cloud_provider()) ? 0 : (int)MONITOR_DISCONNECTED_SERVER;
            }
        }
        show_status((int) MONITOR_DISCONNECTED + server_status);
        return;
    }

    show_status(MONITOR_NORMAL);

    auto current_page = m_tabpanel->GetCurrentPage();
    if (current_page == m_status_info_panel) {
        if (m_status_info_panel->IsShown()) {
            m_status_info_panel->obj = obj;
            m_status_info_panel->m_media_play_ctrl->SetMachineObject(obj);
            m_status_info_panel->update(obj);
        }
    } else if (current_page == m_upgrade_panel) {
        m_upgrade_panel->update(obj);
    } else if (current_page == m_media_file_panel) {
        m_media_file_panel->UpdateByObj(obj);
    }

    if (current_page == m_hms_panel || (obj->GetHMS()->GetHMSItems().size() != m_hms_panel->temp_hms_list.size())) {
        m_hms_panel->update(obj);
    }

    update_hms_tag();
}

void MonitorPanel::update_hms_tag()
{
    for (auto hmsitem : m_hms_panel->temp_hms_list) {

        if (!obj) { break;}

        const wxString &msg = wxGetApp().get_hms_query()->query_hms_msg(obj->get_dev_id(), hmsitem.second.get_long_error_code());
        if (msg.empty()){ continue;} /*STUDIO-10363 it's hidden message*/

        if (!hmsitem.second.has_read()) {
            //show HMS new tag
            m_tabpanel->GetBtnsListCtrl()->showNewTag(PT_HMS, true);
            return;
        }
    }

    m_tabpanel->GetBtnsListCtrl()->showNewTag(PT_HMS, false);
}

bool MonitorPanel::Show(bool show)
{
#ifdef __APPLE__
    wxGetApp().mainframe->SetMinSize(wxGetApp().plater()->GetMinSize());
#endif

    NetworkAgent* m_agent = wxGetApp().getAgent();
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (show) {
        start_update();
        update_network_version_footer();

        if (dev) {
            obj = dev->get_selected_machine();
            if (obj == nullptr)
                dev->load_last_machine();
            obj = dev->get_selected_machine();
            if (obj != nullptr)
                obj->reset_update_time();
        }

        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(REFRESH_INTERVAL);
        if (update_flag) { update_all(); }
        if (is_coprint_device_ui() && m_coprint_controller &&
            (is_quadro_device_ui() ? (m_tabpanel != nullptr && m_tabpanel->GetCurrentPage() == m_coprint_status_panel)
                                   : true))
            m_coprint_controller->refresh();
        if (is_coprint_device_ui())
            refresh_coprint_printer_names();
    } else {
        stop_update();
        m_refresh_timer->Stop();
    }
    return wxPanel::Show(show);
}

void MonitorPanel::show_status(int status)
{
    if (!m_initialized) return;
    if (last_status == status)return;
    if ((last_status & (int)MonitorStatus::MONITOR_CONNECTING) != 0) {
        NetworkAgent* agent = wxGetApp().getAgent();
        json j;
        j["dev_id"] = obj ? obj->get_dev_id() : "obj_nullptr";
        if ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0) {
            j["result"] = "failed";
        }
        else if ((status & (int)MonitorStatus::MONITOR_NORMAL) != 0) {
            j["result"] = "success";
        }
    }
    last_status = status;

    BOOST_LOG_TRIVIAL(info) << "monitor: show_status = " << status;

    if (is_coprint_device_ui()) {
        if (is_quadro_device_ui() && m_coprint_printer_picker != nullptr) {
            m_coprint_printer_picker->refresh_list();
            m_coprint_printer_picker->update_selection();
        }
        if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
            set_default();
            if (m_tabpanel != nullptr && m_tabpanel->IsShown())
                m_tabpanel->Layout();
        }
        return;
    }

    //Freeze();
    // update panels
    if (m_side_tools) { m_side_tools->show_status(status); };
    m_status_info_panel->show_status(status);
    m_hms_panel->show_status(status);
    m_upgrade_panel->show_status(status);

    if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
        set_default();
        m_tabpanel->Layout();
    } else if (((status & (int)MonitorStatus::MONITOR_NORMAL) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int) MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0) )
    {

        if (((status & (int) MonitorStatus::MONITOR_DISCONNECTED) != 0)
            || ((status & (int) MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
            || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
        {
            set_default();
        }
        m_tabpanel->Layout();
    }
    Layout();
    //Thaw();
}

std::string MonitorPanel::get_string_from_tab(PrinterTab tab)
{
    switch (tab) {
    case PT_STATUS :
        return "status";
    case PT_MEDIA:
        return "sd_card";
    case PT_UPDATE:
        return "update";
    case PT_HMS:
        return "HMS";
    case PT_DEBUG:
        return "debug";
    default:
        return "";
    }
    return "";
}

void MonitorPanel::jump_to_HMS()
{
    if (!this->IsShown())
        return;
    auto page = m_tabpanel->GetCurrentPage();
    if (page && page != m_hms_panel)
        m_tabpanel->SetSelection(PT_HMS);
}

void MonitorPanel::jump_to_LiveView()
{
    if (!this->IsShown()) { return; }

    auto page = m_tabpanel->GetCurrentPage();
    if (page && page != m_hms_panel)
    {
        m_tabpanel->SetSelection(PT_STATUS);
    }

    m_status_info_panel->get_media_play_ctrl()->jump_to_play();
}

void MonitorPanel::update_network_version_footer()
{
}

} // GUI
} // Slic3r
