#include "PrintStatusPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Label.hpp"
#include "../../Widgets/ProgressBar.hpp"
#include "../../I18N.hpp"
#include "libslic3r/Utils.hpp"
#ifdef __APPLE__
#include "../../../Utils/MacDarkMode.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

bool set_label_if_changed(wxStaticText* label, const wxString& text)
{
    if (label == nullptr || label->GetLabelText() == text)
        return false;
    label->SetLabelText(text);
    return true;
}

void apply_status_text_style(wxStaticText* label, int point_size)
{
    if (label == nullptr)
        return;
    wxFont font = Label::sysFont(point_size, false);
    font.SetWeight(wxFONTWEIGHT_LIGHT);
    label->SetFont(font);
    label->SetForegroundColour(wxColour("#434343"));
}

wxBitmap load_png_icon(wxWindow* host, const char* filename, int dip)
{
    wxImage img;
    const wxString path = wxString::FromUTF8((resources_dir() + "/images/" + filename).c_str());
    if (!img.LoadFile(path, wxBITMAP_TYPE_PNG) || !img.IsOk())
        return {};

#ifdef __APPLE__
    const double scale = std::max(1.0, mac_max_scaling_factor());
    const int px = std::max(1, (int) std::lround(dip * scale));
    img.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(std::move(img), -1, scale);
#else
    const int px = std::max(1, host->FromDIP(dip));
    if (img.GetWidth() != px || img.GetHeight() != px)
        img.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(img);
#endif
}

} // namespace

PrintStatusPanel::PrintStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Print Status"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::card_background());

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);
    content_sizer->SetMinSize(wxSize(-1, FromDIP(120) + FromDIP(42)));

    const wxSize thumb_size(FromDIP(120), FromDIP(120));
    m_thumbnail = new wxStaticBitmap(m_frame->content_parent(), wxID_ANY, make_thumbnail_placeholder());
    m_thumbnail->SetBackgroundColour(wxColour("#EFEEED"));
    m_thumbnail->SetMinSize(thumb_size);
    m_thumbnail->SetMaxSize(thumb_size);
    content_sizer->Add(m_thumbnail, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));

    auto* details = new wxPanel(m_frame->content_parent(), wxID_ANY);
    details->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* details_sizer = new wxBoxSizer(wxVERTICAL);

    m_file_name = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("N/A"),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    apply_status_text_style(m_file_name, 16);
    m_file_name->SetMinSize(wxSize(FromDIP(120), -1));
    details_sizer->Add(m_file_name, 0, wxBOTTOM, FromDIP(8));

    auto* total_row = new wxBoxSizer(wxHORIZONTAL);
    auto* timer_icon = new wxStaticBitmap(details, wxID_ANY, load_png_icon(this, "cop_timer.png", 16));
    timer_icon->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    timer_icon->SetBackgroundColour(DeviceUiStyle::card_background());
    m_elapsed_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Total: N/A"));
    apply_status_text_style(m_elapsed_time, 12);
    total_row->Add(timer_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    total_row->Add(m_elapsed_time, 0, wxALIGN_CENTER_VERTICAL);
    details_sizer->Add(total_row, 0, wxBOTTOM, FromDIP(10));

    m_progress = new ProgressBar(details, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(28)), true);
    m_progress->SetMinSize(wxSize(-1, FromDIP(28)));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(28)));
    m_progress->SetRadius(FromDIP(14));
    m_progress->SetPadding(FromDIP(1));
    m_progress->SetProgressForedColour(wxColour(132, 162, 188));
    m_progress->SetProgressBackgroundColour(wxColour(126, 158, 184));
    m_progress->SetBackgroundColour(DeviceUiStyle::card_background());
    wxFont progress_font = Label::sysFont(10, false);
    progress_font.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_progress->SetFont(progress_font);
    details_sizer->Add(m_progress, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    auto* lower_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_info = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Layer: N/A/N/A"));
    apply_status_text_style(m_layer_info, 12);
    lower_row->Add(m_layer_info, 1, wxALIGN_CENTER_VERTICAL);
    m_remaining_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Remaining: N/A"));
    apply_status_text_style(m_remaining_time, 12);
    lower_row->Add(m_remaining_time, 0, wxALIGN_CENTER_VERTICAL);
    details_sizer->Add(lower_row, 0, wxEXPAND);

    details->SetSizer(details_sizer);
    content_sizer->Add(details, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));

    auto* buttons = new wxPanel(m_frame->content_parent(), wxID_ANY);
    buttons->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    const wxSize icon_size(FromDIP(20), FromDIP(20));
    m_pause_bitmap = load_png_icon(this, "pause.png", 20);
    m_resume_bitmap = load_png_icon(this, "resume.png", 20);
    m_pause_icon = new wxStaticBitmap(buttons, wxID_ANY, m_pause_bitmap);
    m_pause_icon->SetMinSize(icon_size);
    m_pause_icon->SetMaxSize(icon_size);
    m_pause_icon->SetCursor(wxCursor(wxCURSOR_HAND));
    m_pause_icon->SetBackgroundColour(DeviceUiStyle::card_background());
    m_stop_icon = new wxStaticBitmap(buttons, wxID_ANY, load_png_icon(this, "stop.png", 20));
    m_stop_icon->SetMinSize(icon_size);
    m_stop_icon->SetMaxSize(icon_size);
    m_stop_icon->SetCursor(wxCursor(wxCURSOR_HAND));
    m_stop_icon->SetBackgroundColour(DeviceUiStyle::card_background());
    btn_sizer->Add(m_pause_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(13));
    btn_sizer->Add(m_stop_icon, 0, wxALIGN_CENTER_VERTICAL);
    buttons->SetSizer(btn_sizer);
    m_pause_icon->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (!m_print_actions_enabled)
            return;
        if (m_print_paused) {
            if (m_resume_handler)
                m_resume_handler();
        } else if (m_pause_handler) {
            m_pause_handler();
        }
    });
    m_stop_icon->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (m_print_actions_enabled && m_stop_handler)
            m_stop_handler();
    });
    set_print_actions_enabled(false);
    content_sizer->Add(buttons, 0, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(22));

    m_frame->set_content(content_sizer);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void PrintStatusPanel::apply_state(const PrintJobState& state)
{
    bool file_layout_needed = false;
    const bool active = state.has_active_job;

    file_layout_needed |= set_label_if_changed(m_file_name, active && !state.file_name.IsEmpty() ? state.file_name : wxString::FromUTF8("N/A"));

    const int progress = active ? std::clamp(state.progress_percent, 0, 100) : 0;
    if (m_progress != nullptr)
        m_progress->SetValue(progress);

    set_label_if_changed(m_elapsed_time, wxString::FromUTF8("Total: ") + time_text(active ? state.elapsed_seconds : -1));
    wxString layer_text = wxString::FromUTF8("Layer: N/A/N/A");
    if (active && state.total_layers > 0)
        layer_text = wxString::Format("Layer: %d/%d", std::max(0, state.current_layer), state.total_layers);
    else if (active && state.current_layer > 0)
        layer_text = wxString::Format("Layer: %d/N/A", state.current_layer);
    set_label_if_changed(m_layer_info, layer_text);
    set_label_if_changed(m_remaining_time, wxString::FromUTF8("Remaining: ") + time_text(active ? state.remaining_seconds : -1));
    set_print_actions_enabled(active);
    set_pause_resume_icon(active && state.state == PrintCommandState::Paused);
    if (!active)
        reset_thumbnail_placeholder();

    if (file_layout_needed) {
        Freeze();
        Layout();
        Thaw();
    }
}

void PrintStatusPanel::set_print_actions_enabled(bool enabled)
{
    if (m_print_actions_enabled == enabled)
        return;
    m_print_actions_enabled = enabled;

    const wxCursor cursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW);
    if (m_pause_icon != nullptr) {
        m_pause_icon->Enable(enabled);
        m_pause_icon->SetCursor(cursor);
    }
    if (m_stop_icon != nullptr) {
        m_stop_icon->Enable(enabled);
        m_stop_icon->SetCursor(cursor);
    }
}

void PrintStatusPanel::set_pause_resume_icon(bool paused)
{
    if (m_pause_icon == nullptr || m_print_paused == paused)
        return;
    m_print_paused = paused;
    const wxBitmap &bitmap = paused && m_resume_bitmap.IsOk() ? m_resume_bitmap : m_pause_bitmap;
    if (bitmap.IsOk())
        m_pause_icon->SetBitmap(bitmap);
    m_pause_icon->Refresh();
}

void PrintStatusPanel::set_pause_handler(ActionHandler handler)
{
    m_pause_handler = std::move(handler);
}

void PrintStatusPanel::set_resume_handler(ActionHandler handler)
{
    m_resume_handler = std::move(handler);
}

void PrintStatusPanel::set_stop_handler(ActionHandler handler)
{
    m_stop_handler = std::move(handler);
}

void PrintStatusPanel::reset_thumbnail_placeholder()
{
    if (m_thumbnail == nullptr)
        return;

    m_thumbnail->SetBitmap(make_thumbnail_placeholder());
    m_thumbnail->Refresh();
}

wxBitmap PrintStatusPanel::make_thumbnail_placeholder()
{
    const int width = FromDIP(120);
    const int height = FromDIP(120);
    wxBitmap bitmap(width, height);

    wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(wxColour("#EFEEED")));
    dc.Clear();

    wxFont font = GetFont();
    font.SetPointSize(12);
    font.SetWeight(wxFONTWEIGHT_BOLD);
    dc.SetFont(font);

    const wxString text = wxString::FromUTF8("Co Print");
    const wxSize text_size = dc.GetTextExtent(text);
    const int gap = FromDIP(4);
    const int center_y = height / 2;
    const int logo_w = FromDIP(14);
    const int logo_h = FromDIP(21);

    wxImage logo(wxString::FromUTF8((resources_dir() + "/images/CoPrintLogo.png").c_str()), wxBITMAP_TYPE_PNG);
    bool drew_logo = false;
    if (logo.IsOk() && logo.GetWidth() > 0 && logo.GetHeight() > 0) {
        logo.Rescale(logo_w, logo_h, wxIMAGE_QUALITY_HIGH);
        if (!logo.HasAlpha())
            logo.InitAlpha();
        if (unsigned char* alpha = logo.GetAlpha()) {
            const int pixels = logo.GetWidth() * logo.GetHeight();
            for (int i = 0; i < pixels; ++i)
                alpha[i] = static_cast<unsigned char>(alpha[i] * 30 / 100);
        }
        drew_logo = true;
    }

    const int group_width = (drew_logo ? logo_w + gap : 0) + text_size.GetWidth();
    const int start_x = std::max(FromDIP(4), (width - group_width) / 2);
    wxGCDC gcdc(dc);
    gcdc.SetFont(font);
    if (drew_logo)
        gcdc.DrawBitmap(wxBitmap(logo), start_x, center_y - logo_h / 2, true);

    const int text_x = drew_logo ? start_x + logo_w + gap : (width - text_size.GetWidth()) / 2;
    gcdc.SetTextForeground(wxColour(0xAD, 0xAB, 0xAC, 76));
    gcdc.DrawText(text, text_x, center_y - text_size.GetHeight() / 2);

    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

wxString PrintStatusPanel::time_text(int seconds)
{
    if (seconds < 0)
        return wxString::FromUTF8("N/A");

    const int minutes = seconds / 60;
    const int hours = minutes / 60;
    const int remaining_minutes = minutes % 60;
    if (hours > 0)
        return wxString::Format("%dh %dm", hours, remaining_minutes);
    return wxString::Format("%dm", remaining_minutes);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
