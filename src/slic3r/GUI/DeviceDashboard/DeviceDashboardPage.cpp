#include "DeviceDashboardPage.hpp"

#include "DeviceCardFrame.hpp"
#include "DeviceUiStyle.hpp"
#include "PrinterOfflineOverlay.hpp"
#include "panels/CameraPanel.hpp"
#include "panels/FilamentPanel.hpp"
#include "panels/MovementPanel.hpp"
#include "panels/PrinterStatusPanel.hpp"
#include "panels/PrintStatusPanel.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "../I18N.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/timer.h>

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

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_content_panel, 1, wxEXPAND | wxALL, FromDIP(10));
    SetSizer(root);

    m_offline_overlay = new PrinterOfflineOverlay(this);

    auto forward_command = [this](const DeviceCommand& command) {
        if (!m_can_send_commands)
            return;
        if (m_offline_overlay != nullptr && m_offline_overlay->IsShown())
            return;
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
    m_print_status_panel->set_resume_handler([forward_command]() {
        DeviceCommand command;
        command.kind = DeviceCommandKind::ResumePrint;
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
    Layout();
    layout_offline_overlay();
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
    layout_offline_overlay();
    m_refreshing_scroll = false;
}

void DeviceDashboardPage::apply_state(const DeviceDashboardState& state)
{
    m_can_send_commands = state.connection.can_send_commands;
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

    update_controls_enabled();
    refresh_scroll();
}

void DeviceDashboardPage::msw_rescale()
{
    if (m_movement_panel != nullptr)
        m_movement_panel->msw_rescale();
    Layout();
    refresh_scroll();
}

void DeviceDashboardPage::layout_offline_overlay()
{
    if (m_offline_overlay != nullptr)
        m_offline_overlay->layout_over_parent();
}

void DeviceDashboardPage::set_connecting_visible(bool visible, const wxString &)
{
    // Connecting UI is owned by MonitorPanel so it can cover the left menu.
    if (!visible)
        return;
    if (m_offline_overlay != nullptr && m_offline_overlay->IsShown())
        set_offline_overlay_visible(false);
}

void DeviceDashboardPage::set_offline_overlay_visible(bool visible, const wxString &printer_name)
{
    if (m_offline_overlay == nullptr || m_content_panel == nullptr)
        return;
    m_offline_overlay->set_visible(visible, printer_name);
    update_controls_enabled();
    if (wxPanel *host = camera_webview_host())
        host->Show(!visible);
    m_content_panel->Show(true);
    if (wxSizer *sizer = GetSizer())
        sizer->Show(m_content_panel, true);
    Refresh();
}

void DeviceDashboardPage::update_controls_enabled()
{
    const bool overlay_blocks = m_offline_overlay != nullptr && m_offline_overlay->IsShown();
    const bool interactive = !overlay_blocks;
    if (m_camera_panel != nullptr)
        m_camera_panel->Enable(interactive);
    if (m_print_status_panel != nullptr)
        m_print_status_panel->Enable(interactive);
    if (m_filament_panel != nullptr)
        m_filament_panel->Enable(interactive);
    if (m_movement_panel != nullptr)
        m_movement_panel->Enable(interactive);
    if (m_printer_status_panel != nullptr)
        m_printer_status_panel->Enable(interactive);
}

void DeviceDashboardPage::set_offline_retry_handler(std::function<void()> handler)
{
    if (m_offline_overlay != nullptr)
        m_offline_overlay->set_retry_handler(std::move(handler));
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

namespace {
constexpr int kCardRadiusDip = 15;

bool overlay_uses_dark_scrim(PrinterOfflineOverlay::Kind kind)
{
    return kind == PrinterOfflineOverlay::Kind::Connecting
        || kind == PrinterOfflineOverlay::Kind::Failed;
}

int overlay_scrim_alpha(bool dark)
{
    return dark ? 140 : 153;
}

wxColour overlay_scrim_colour(bool dark)
{
#ifdef __WXMSW__
    // Opaque gray: child-window alpha on MSW either blacked out or tore the
    // content underneath. Match ~55% black over the Device page (#EEE).
    return dark ? wxColour(0x8F, 0x8F, 0x8F) : wxColour(0xD9, 0xD9, 0xD9);
#else
    return dark ? wxColour(0, 0, 0, overlay_scrim_alpha(true))
                : wxColour(0xD9, 0xD9, 0xD9, overlay_scrim_alpha(false));
#endif
}

void bind_overlay_card(wxPanel *card, PrinterOfflineOverlay *overlay)
{
    (void) overlay;
    card->SetBackgroundColour(*wxWHITE);
    card->SetBackgroundStyle(wxBG_STYLE_PAINT);
#ifdef __WXMSW__
    card->SetDoubleBuffered(true);
#endif
    card->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
    card->Bind(wxEVT_PAINT, [card](wxPaintEvent &) {
#ifdef __WXMSW__
        wxAutoBufferedPaintDC dc(card);
        const wxSize sz = card->GetClientSize();
        if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
            return;
        // Fill the square HWND corners with the parent scrim so the white
        // rounded rect reads as a card sitting on the overlay, not under it.
        wxColour corner = *wxWHITE;
        if (wxWindow *parent = card->GetParent())
            corner = parent->GetBackgroundColour();
        dc.SetBackground(wxBrush(corner.IsOk() ? corner : *wxWHITE));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc == nullptr)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(*wxWHITE));
        gc->DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(),
            card->FromDIP(kCardRadiusDip));
#else
        (void) card;
#endif
    });
}
}

DeviceBusySpinner::DeviceBusySpinner(wxWindow *parent, const wxSize &size, const wxColour &bg)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
    , m_timer(this)
    , m_arc_colour(DeviceUiStyle::accent())
{
    SetMinSize(size);
    SetMaxSize(size);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(bg);
#ifdef __WXMSW__
    SetDoubleBuffered(true);
#endif
    Bind(wxEVT_PAINT, &DeviceBusySpinner::on_paint, this);
    Bind(wxEVT_TIMER, &DeviceBusySpinner::on_timer, this);
    Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
}

DeviceBusySpinner::~DeviceBusySpinner()
{
    Stop();
}

void DeviceBusySpinner::Play()
{
    if (!m_timer.IsRunning())
        m_timer.Start(33);
}

void DeviceBusySpinner::Stop()
{
    if (m_timer.IsRunning())
        m_timer.Stop();
}

void DeviceBusySpinner::on_timer(wxTimerEvent &)
{
    if (!IsShown() || !IsEnabled())
        return;
    m_angle_deg = std::fmod(m_angle_deg + 6.0, 360.0);
    Refresh(false);
}

void DeviceBusySpinner::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const wxSize sz = GetClientSize();
    if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
        return;

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;

    const double side = std::min(sz.GetWidth(), sz.GetHeight());
    const double cx = sz.GetWidth() * 0.5;
    const double cy = sz.GetHeight() * 0.5;
    const double radius = side * 0.32;
    const double stroke = std::max(2.0, side * 0.09);

    wxPen track(wxColour(230, 230, 230), int(std::lround(stroke)));
    track.SetCap(wxCAP_ROUND);
    gc->SetPen(gc->CreatePen(track));
    wxGraphicsPath track_path = gc->CreatePath();
    track_path.AddCircle(cx, cy, radius);
    gc->StrokePath(track_path);

    wxPen pen(m_arc_colour, int(std::lround(stroke)));
    pen.SetCap(wxCAP_ROUND);
    gc->SetPen(gc->CreatePen(pen));
    wxGraphicsPath path = gc->CreatePath();
    constexpr double kPi = 3.14159265358979323846;
    const double start_rad = m_angle_deg * kPi / 180.0;
    path.AddArc(cx, cy, radius, start_rad, start_rad + kPi, true);
    gc->StrokePath(path);
}

PrinterOfflineOverlay::PrinterOfflineOverlay(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
#ifdef __WXMSW__
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
            return;
        dc.SetBackground(wxBrush(GetBackgroundColour().IsOk()
            ? GetBackgroundColour()
            : overlay_scrim_colour(overlay_uses_dark_scrim(m_kind))));
        dc.Clear();
#else
        wxPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc == nullptr)
            return;
        const wxSize sz = GetClientSize();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(overlay_scrim_colour(overlay_uses_dark_scrim(m_kind))));
        gc->DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());
        if (wxPanel *card = active_card()) {
            const wxRect rect = card->GetRect();
            gc->SetBrush(*wxWHITE);
            gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height,
                FromDIP(kCardRadiusDip));
        }
#endif
    });
    Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
    const auto swallow_mouse = [](wxMouseEvent &event) { event.Skip(false); };
    Bind(wxEVT_LEFT_DOWN, swallow_mouse);
    Bind(wxEVT_LEFT_UP, swallow_mouse);
    Bind(wxEVT_LEFT_DCLICK, swallow_mouse);
    Bind(wxEVT_RIGHT_DOWN, swallow_mouse);
    Bind(wxEVT_MOTION, swallow_mouse);
    Bind(wxEVT_MOUSEWHEEL, swallow_mouse);

    m_connecting_card = new wxPanel(this, wxID_ANY);
    bind_overlay_card(m_connecting_card, this);
    m_connecting_card->SetMinSize(wxSize(FromDIP(200), FromDIP(176)));
    m_connecting_card->SetMaxSize(wxSize(FromDIP(200), FromDIP(176)));
    const int spinner_px = FromDIP(48);
    m_spinner = new DeviceBusySpinner(m_connecting_card, wxSize(spinner_px, spinner_px), *wxWHITE);
    m_connecting_label = new wxStaticText(m_connecting_card, wxID_ANY, _L("Connecting..."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_connecting_label->SetForegroundColour(DeviceUiStyle::text_primary());
    m_connecting_label->SetBackgroundColour(*wxWHITE);
    {
        wxFont f = m_connecting_label->GetFont();
        f.SetPointSize(std::max(13, f.GetPointSize() + 1));
        f.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_connecting_label->SetFont(f);
    }
    auto *loading_label = new wxStaticText(m_connecting_card, wxID_ANY, _L("Loading"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    loading_label->SetForegroundColour(wxColour(118, 118, 124));
    loading_label->SetBackgroundColour(*wxWHITE);
    auto *connecting_sizer = new wxBoxSizer(wxVERTICAL);
    connecting_sizer->AddStretchSpacer(1);
    connecting_sizer->Add(m_spinner, 0, wxALIGN_CENTER_HORIZONTAL);
    connecting_sizer->AddSpacer(FromDIP(14));
    connecting_sizer->Add(m_connecting_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
    connecting_sizer->AddSpacer(FromDIP(4));
    connecting_sizer->Add(loading_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
    connecting_sizer->AddStretchSpacer(1);
    m_connecting_card->SetSizer(connecting_sizer);
    m_connecting_card->Hide();

    m_failed_card = new wxPanel(this, wxID_ANY);
    bind_overlay_card(m_failed_card, this);
    m_failed_card->SetMinSize(wxSize(FromDIP(360), FromDIP(200)));
    m_failed_card->SetMaxSize(wxSize(FromDIP(360), -1));
    m_title = new wxStaticText(m_failed_card, wxID_ANY, _L("Could not connect to the printer."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_title->SetForegroundColour(DeviceUiStyle::text_primary());
    m_title->SetBackgroundColour(*wxWHITE);
    wxFont title_font = m_title->GetFont();
    title_font.SetPointSize(std::max(13, title_font.GetPointSize() + 1));
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_title->SetFont(title_font);
    m_hint = new wxStaticText(m_failed_card, wxID_ANY,
        _L("Check that the printer is powered on and on the same network."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_hint->SetForegroundColour(DeviceUiStyle::text_primary());
    m_hint->SetBackgroundColour(*wxWHITE);
    m_ok = new Button(m_failed_card, _L("Got it"));
    {
        const wxSize ok_size(FromDIP(120), FromDIP(32));
        m_ok->SetMinSize(ok_size);
        m_ok->SetMaxSize(ok_size);
        m_ok->SetCornerRadius(FromDIP(8));
        m_ok->SetBorderWidth(0);
        const wxColour accent = DeviceUiStyle::accent();
        StateColor bg(
            std::pair(wxColour(232, 232, 232), (int) StateColor::Disabled),
            std::pair(StateColor::LightenDarkenColor(accent, -18), (int) StateColor::Pressed),
            std::pair(StateColor::LightenDarkenColor(accent, 12), (int) StateColor::Hovered),
            std::pair(accent, (int) StateColor::Normal));
        bg.setTakeFocusedAsHovered(false);
        StateColor fg(
            std::pair(wxColour(180, 180, 180), (int) StateColor::Disabled),
            std::pair(*wxWHITE, (int) StateColor::Normal));
        fg.setTakeFocusedAsHovered(false);
        m_ok->SetBackgroundColor(bg);
        m_ok->SetTextColor(fg);
        m_ok->SetCanFocus(false);
    }
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_ok_handler)
            m_ok_handler();
        else if (m_retry_handler)
            m_retry_handler();
    });
    auto *failed_sizer = new wxBoxSizer(wxVERTICAL);
    failed_sizer->AddSpacer(FromDIP(28));
    failed_sizer->Add(m_title, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    failed_sizer->AddSpacer(FromDIP(10));
    failed_sizer->Add(m_hint, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    failed_sizer->AddStretchSpacer(1);
    failed_sizer->Add(m_ok, 0, wxALIGN_CENTER_HORIZONTAL);
    failed_sizer->AddSpacer(FromDIP(24));
    m_failed_card->SetSizer(failed_sizer);
    wrap_failed_labels(m_title->GetLabel(), m_hint->GetLabel());
    m_failed_card->Hide();

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->AddStretchSpacer(1);
    row->Add(m_connecting_card, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(m_failed_card, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    root->AddStretchSpacer(1);
    root->Add(row, 0, wxEXPAND);
    root->AddStretchSpacer(1);
    SetSizer(root);
    Hide();

    if (wxWindow *host = GetParent())
        host->Bind(wxEVT_SIZE, &PrinterOfflineOverlay::on_parent_size, this);
}

PrinterOfflineOverlay::~PrinterOfflineOverlay()
{
    if (m_spinner != nullptr)
        m_spinner->Stop();
    if (wxWindow *host = GetParent())
        host->Unbind(wxEVT_SIZE, &PrinterOfflineOverlay::on_parent_size, this);
}

wxPanel *PrinterOfflineOverlay::active_card() const
{
    if (m_kind == Kind::Connecting && m_connecting_card != nullptr && m_connecting_card->IsShown())
        return m_connecting_card;
    if (m_kind == Kind::Failed && m_failed_card != nullptr && m_failed_card->IsShown())
        return m_failed_card;
    return nullptr;
}

void PrinterOfflineOverlay::on_parent_size(wxSizeEvent &event)
{
    event.Skip();
    if (IsShown())
        layout_over_parent();
}

void PrinterOfflineOverlay::layout_over_parent()
{
    if (m_in_layout)
        return;
    wxWindow *host = GetParent();
    if (host == nullptr || !IsShown())
        return;
    const wxSize sz = host->GetClientSize();
    if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
        return;
    wxPoint origin(0, 0);
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(host))
        origin = scrolled->CalcUnscrolledPosition(wxPoint(0, 0));
    const wxRect want(origin.x, origin.y, sz.GetWidth(), sz.GetHeight());
    if (GetRect() == want)
        return;
    m_in_layout = true;
    SetSize(want);
    Layout();
    m_in_layout = false;
}

void PrinterOfflineOverlay::apply_kind()
{
    const bool connecting = m_kind == Kind::Connecting;
    const bool failed = m_kind == Kind::Failed;
    if (m_connecting_card != nullptr)
        m_connecting_card->Show(connecting);
    if (m_failed_card != nullptr)
        m_failed_card->Show(failed);
    if (wxSizer *sizer = GetSizer()) {
        if (m_connecting_card != nullptr)
            sizer->Show(m_connecting_card, connecting, true);
        if (m_failed_card != nullptr)
            sizer->Show(m_failed_card, failed, true);
    }
    if (m_spinner != nullptr) {
        if (connecting)
            m_spinner->Play();
        else
            m_spinner->Stop();
    }
    const bool show = m_kind != Kind::Hidden;
    SetBackgroundColour(overlay_scrim_colour(overlay_uses_dark_scrim(m_kind)));
    if (IsShown() != show)
        Show(show);
    if (show) {
        Raise();
        layout_over_parent();
        Layout();
    } else if (wxWindow *host = GetParent()) {
        host->Refresh();
    }
}

void PrinterOfflineOverlay::wrap_failed_labels(const wxString &title, const wxString &hint)
{
    const int wrap_px = FromDIP(312);
    auto apply_wrapped = [wrap_px](wxStaticText *label, const wxString &text) {
        if (label == nullptr || text.empty())
            return;
        wxClientDC dc(label);
        dc.SetFont(label->GetFont());
        wxString wrapped;
        const wxSize extent = Label::split_lines(dc, wrap_px, text, wrapped);
        label->SetLabel(wrapped);
        label->SetMinSize(wxSize(wrap_px, std::max(extent.GetHeight(), label->GetCharHeight())));
        label->InvalidateBestSize();
    };
    apply_wrapped(m_title, title);
    apply_wrapped(m_hint, hint);
    if (m_failed_card != nullptr) {
        m_failed_card->InvalidateBestSize();
        if (wxSizer *sizer = m_failed_card->GetSizer()) {
            sizer->Layout();
            sizer->SetSizeHints(m_failed_card);
        }
        m_failed_card->Fit();
    }
}

void PrinterOfflineOverlay::set_kind(Kind kind, const wxString &title, const wxString &hint)
{
    if (kind == Kind::Failed)
        wrap_failed_labels(title, hint);
    if (kind == Kind::Connecting && m_connecting_label != nullptr && !title.empty() &&
        m_connecting_label->GetLabelText() != title)
        m_connecting_label->SetLabelText(title);
    if (m_kind == kind && IsShown() == (kind != Kind::Hidden)) {
        if (kind != Kind::Hidden)
            layout_over_parent();
        return;
    }
    m_kind = kind;
    apply_kind();
}

void PrinterOfflineOverlay::set_visible(bool visible, const wxString &)
{
    set_kind(visible ? Kind::Dim : Kind::Hidden);
}

void PrinterOfflineOverlay::set_retry_handler(std::function<void()> handler)
{
    m_retry_handler = std::move(handler);
}

void PrinterOfflineOverlay::set_ok_handler(std::function<void()> handler)
{
    m_ok_handler = std::move(handler);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
