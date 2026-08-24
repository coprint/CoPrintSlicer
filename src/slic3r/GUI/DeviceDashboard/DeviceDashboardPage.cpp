#include "DeviceDashboardPage.hpp"

#include "DeviceCardFrame.hpp"
#include "DeviceUiStyle.hpp"
#include "panels/CameraPanel.hpp"
#include "panels/FilamentPanel.hpp"
#include "panels/MovementPanel.hpp"
#include "panels/PrinterStatusPanel.hpp"
#include "panels/PrintStatusPanel.hpp"
#include "../I18N.hpp"

#include <algorithm>
#include <utility>

#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

DeviceDashboardPage::DeviceDashboardPage(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxHSCROLL | wxVSCROLL)
{
    SetBackgroundColour(DeviceUiStyle::page_background());
    SetScrollRate(FromDIP(10), FromDIP(10));
    EnableScrolling(true, true);

    m_content_panel = new wxPanel(this, wxID_ANY);
    m_content_panel->SetBackgroundColour(DeviceUiStyle::page_background());

    auto* content_columns = new wxBoxSizer(wxHORIZONTAL);

    m_camera_panel = new CameraPanel(m_content_panel);
    m_print_status_panel = new PrintStatusPanel(m_content_panel);

    auto* right_card = new DeviceCardFrame(m_content_panel, wxString::FromUTF8("Control"));
    wxWindow* right_host = right_card->content_parent();
    m_movement_panel = new MovementPanel(right_host);
    m_printer_status_panel = new PrinterStatusPanel(m_movement_panel->status_slot());
    if (wxWindow* slot = m_movement_panel->status_slot()) {
        if (wxSizer* slot_sizer = slot->GetSizer())
            slot_sizer->Add(m_printer_status_panel, 0);
    }
    m_filament_panel = new FilamentPanel(m_content_panel);

    auto* right_stack = new wxBoxSizer(wxVERTICAL);
    right_stack->Add(m_movement_panel, 0, wxEXPAND);
    right_card->set_content(right_stack);

    auto* right_column = new wxBoxSizer(wxVERTICAL);
    right_column->Add(right_card, 0, wxEXPAND);
    right_column->AddSpacer(FromDIP(8));
    right_column->Add(m_filament_panel, 0, wxEXPAND);

    auto* left_main_column = new wxBoxSizer(wxVERTICAL);
    left_main_column->Add(m_camera_panel, 0, wxEXPAND);
    left_main_column->AddSpacer(FromDIP(8));
    left_main_column->Add(m_print_status_panel, 0, wxEXPAND);

    content_columns->Add(left_main_column, 1, wxEXPAND);
    content_columns->AddSpacer(FromDIP(20));
    content_columns->Add(right_column, 0, wxALIGN_TOP);

    m_content_panel->SetSizer(content_columns);

    m_connecting_overlay = new wxPanel(this, wxID_ANY);
    m_connecting_overlay->SetBackgroundColour(wxColour(238, 238, 239));
    auto* overlay_sizer = new wxBoxSizer(wxVERTICAL);
    overlay_sizer->AddStretchSpacer(1);
    m_connecting_label = new wxStaticText(m_connecting_overlay, wxID_ANY, _L("Connecting..."));
    m_connecting_label->SetForegroundColour(DeviceUiStyle::text_primary());
    m_connecting_label->SetBackgroundColour(wxColour(238, 238, 239));
    wxFont connecting_font = m_connecting_label->GetFont();
    connecting_font.SetPointSize(std::max(16, connecting_font.GetPointSize() + 3));
    connecting_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_connecting_label->SetFont(connecting_font);
    overlay_sizer->Add(m_connecting_label, 0, wxALIGN_CENTER_HORIZONTAL);
    overlay_sizer->AddStretchSpacer(1);
    m_connecting_overlay->SetSizer(overlay_sizer);
    m_connecting_overlay->Hide();

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_content_panel, 1, wxEXPAND | wxALL, FromDIP(10));
    root->Add(m_connecting_overlay, 1, wxEXPAND | wxALL, FromDIP(10));
    SetSizer(root);

    auto forward_command = [this](const DeviceCommand& command) {
        if (m_command_handler)
            m_command_handler(command);
    };
    m_movement_panel->set_command_handler(forward_command);
    m_filament_panel->set_command_handler(forward_command);
    m_printer_status_panel->set_print_speed_handler([forward_command](int percent) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::SetPrintSpeed;
        command.value = percent;
        forward_command(command);
    });
    m_print_status_panel->set_pause_handler([forward_command]() {
        DeviceCommand command;
        command.kind = DeviceCommandKind::PausePrint;
        forward_command(command);
    });
    m_print_status_panel->set_stop_handler([forward_command]() {
        DeviceCommand command;
        command.kind = DeviceCommandKind::StopPrint;
        forward_command(command);
    });
    bind_size_handler();
}

void DeviceDashboardPage::bind_size_handler()
{
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        if (m_refreshing_scroll || m_pending_scroll_refresh)
            return;
        m_pending_scroll_refresh = true;
        CallAfter([this] {
            m_pending_scroll_refresh = false;
            refresh_scroll();
        });
    });
}

void DeviceDashboardPage::refresh_scroll()
{
    if (m_refreshing_scroll || GetSizer() == nullptr || !IsShownOnScreen())
        return;
    m_refreshing_scroll = true;
    if (m_connecting_overlay != nullptr && m_connecting_overlay->IsShown()) {
        Layout();
        m_refreshing_scroll = false;
        return;
    }
    Layout();
    if (m_camera_panel != nullptr && m_print_status_panel != nullptr) {
        const wxSize client = GetClientSize();
        const int print_h = m_print_status_panel->GetBestSize().GetHeight();
        // Grow the camera until Print Status would leave the visible page.
        const int window_cap = client.GetHeight() - FromDIP(20) - print_h - FromDIP(8);
        int max_w = m_camera_panel->GetClientSize().GetWidth();
        if (max_w <= 0)
            max_w = std::max(FromDIP(420), client.GetWidth() / 2);
        m_camera_panel->fit_preview(max_w, std::max(FromDIP(180), window_cap));
    }
    const wxSize min_size = GetSizer()->CalcMin();
    const wxSize client = GetClientSize();
    SetVirtualSize(std::max(min_size.GetWidth(), client.GetWidth()),
                   std::max(min_size.GetHeight(), client.GetHeight()));
    Layout();
    m_refreshing_scroll = false;
}

void DeviceDashboardPage::apply_state(const DeviceDashboardState& state)
{
    if (m_camera_panel != nullptr)
        m_camera_panel->apply_state(state.camera);
    if (m_print_status_panel != nullptr)
        m_print_status_panel->apply_state(state.print_job);
    if (m_movement_panel != nullptr)
        m_movement_panel->apply_state(state.movement);
    if (m_printer_status_panel != nullptr)
        m_printer_status_panel->apply_state(state.tools, state.bed, state.movement.print_speed_percent,
            state.print_job.has_active_job);
    if (m_filament_panel != nullptr)
        m_filament_panel->apply_state(state.filament);

    refresh_scroll();
}

void DeviceDashboardPage::set_connecting_visible(bool visible, const wxString &message)
{
    if (m_connecting_overlay == nullptr || m_content_panel == nullptr)
        return;
    if (visible && m_connecting_label != nullptr && !message.empty() &&
        m_connecting_label->GetLabelText() != message)
        m_connecting_label->SetLabelText(message);
    if (m_connecting_overlay->IsShown() == visible && m_content_panel->IsShown() != visible)
        return;
    m_connecting_overlay->Show(visible);
    m_content_panel->Show(!visible);
    if (wxSizer* sizer = GetSizer()) {
        sizer->Show(m_connecting_overlay, visible);
        sizer->Show(m_content_panel, !visible);
        sizer->Layout();
    } else {
        Layout();
    }
}

void DeviceDashboardPage::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void DeviceDashboardPage::update_camera_host_responsive_size()
{
    wxPanel* const host = camera_webview_host();
    if (m_camera_panel == nullptr || host == nullptr)
        return;
    wxWindow* const box = host->GetParent();
    if (box == nullptr)
        return;
    const wxSize sz = box->GetClientSize();
    if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
        return;
    host->SetSize(0, 0, sz.GetWidth(), sz.GetHeight());
    host->Layout();
    refresh_scroll();
}

wxPanel* DeviceDashboardPage::camera_webview_host() const
{
    return m_camera_panel != nullptr ? m_camera_panel->webview_host() : nullptr;
}

wxStaticBitmap* DeviceDashboardPage::thumbnail_widget() const
{
    return m_print_status_panel != nullptr ? m_print_status_panel->thumbnail_widget() : nullptr;
}

void DeviceDashboardPage::set_camera_refresh_handler(std::function<void()> handler)
{
    if (m_camera_panel != nullptr)
        m_camera_panel->set_refresh_handler(std::move(handler));
}

void DeviceDashboardPage::set_camera_play_handler(std::function<void()> handler)
{
    if (m_camera_panel != nullptr)
        m_camera_panel->set_play_handler(std::move(handler));
}

void DeviceDashboardPage::set_camera_timelapse_handler(std::function<void()> handler)
{
    if (m_camera_panel != nullptr)
        m_camera_panel->set_timelapse_handler(std::move(handler));
}

void DeviceDashboardPage::set_printer_status_handlers(
    std::function<void(int)> tool_select,
    std::function<void(int, int)> nozzle_temp,
    std::function<void(int, int)> fan_speed,
    std::function<void(int)> bed_temp)
{
    if (m_printer_status_panel == nullptr)
        return;
    m_printer_status_panel->set_tool_select_handler(std::move(tool_select));
    m_printer_status_panel->set_nozzle_temp_handler(std::move(nozzle_temp));
    m_printer_status_panel->set_fan_speed_handler(std::move(fan_speed));
    m_printer_status_panel->set_bed_temp_handler(std::move(bed_temp));
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
