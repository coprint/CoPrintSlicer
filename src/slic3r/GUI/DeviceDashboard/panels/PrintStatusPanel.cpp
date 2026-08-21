#include "PrintStatusPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Button.hpp"
#include "../../Widgets/ProgressBar.hpp"
#include "../../I18N.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <utility>

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

} // namespace

PrintStatusPanel::PrintStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Print Status"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::card_background());

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_thumbnail_host = new wxPanel(m_frame->content_parent(), wxID_ANY);
    m_thumbnail_host->SetBackgroundColour(*wxBLACK);
    m_thumbnail_host->SetMinSize(wxSize(FromDIP(240), FromDIP(170)));
    auto* thumbnail_sizer = new wxBoxSizer(wxVERTICAL);
    m_thumbnail = new wxStaticBitmap(m_thumbnail_host, wxID_ANY, make_thumbnail_placeholder());
    m_thumbnail->SetBackgroundColour(*wxBLACK);
    m_thumbnail->SetMinSize(wxSize(FromDIP(220), FromDIP(150)));
    m_thumbnail->SetMaxSize(wxSize(FromDIP(220), FromDIP(150)));
    thumbnail_sizer->AddStretchSpacer(1);
    thumbnail_sizer->Add(m_thumbnail, 0, wxALIGN_CENTER);
    thumbnail_sizer->AddStretchSpacer(1);
    m_thumbnail_host->SetSizer(thumbnail_sizer);
    content_sizer->Add(m_thumbnail_host, 0, wxEXPAND | wxRIGHT, FromDIP(14));

    auto* details = new wxPanel(m_frame->content_parent(), wxID_ANY);
    details->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* details_sizer = new wxBoxSizer(wxVERTICAL);

    auto* printing_label = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Printing File:"));
    printing_label->SetForegroundColour(DeviceUiStyle::accent());
    m_file_name = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("N/A"),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_file_name->SetForegroundColour(DeviceUiStyle::text_primary());
    m_file_name->SetMinSize(wxSize(FromDIP(120), -1));
    details_sizer->Add(printing_label, 0, wxBOTTOM, FromDIP(2));
    details_sizer->Add(m_file_name, 0, wxBOTTOM, FromDIP(8));

    m_elapsed_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Total: N/A"));
    m_elapsed_time->SetForegroundColour(DeviceUiStyle::text_primary());
    details_sizer->Add(m_elapsed_time, 0, wxBOTTOM, FromDIP(10));

    m_progress = new ProgressBar(details, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(28)), true);
    m_progress->SetMinSize(wxSize(-1, FromDIP(28)));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(28)));
    m_progress->SetRadius(FromDIP(14));
    m_progress->SetPadding(FromDIP(3));
    m_progress->SetProgressForedColour(wxColour(132, 162, 188));
    m_progress->SetProgressBackgroundColour(wxColour(126, 158, 184));
    m_progress->SetBackgroundColour(DeviceUiStyle::card_background());
    details_sizer->Add(m_progress, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    auto* lower_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_info = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Layer: N/A/N/A"));
    m_layer_info->SetForegroundColour(DeviceUiStyle::text_primary());
    lower_row->Add(m_layer_info, 1, wxALIGN_CENTER_VERTICAL);
    m_remaining_time = new wxStaticText(details, wxID_ANY, wxString::FromUTF8("Remaining: N/A"));
    m_remaining_time->SetForegroundColour(DeviceUiStyle::text_primary());
    lower_row->Add(m_remaining_time, 0, wxALIGN_CENTER_VERTICAL);
    details_sizer->Add(lower_row, 0, wxEXPAND);

    auto* action_row = new wxBoxSizer(wxHORIZONTAL);
    m_pause_button = new Button(details, wxString::FromUTF8("Pause"), "print_control_pause_amber", 0, 14);
    m_pause_button->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    m_pause_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    m_pause_button->SetCornerRadius(FromDIP(8));
    m_pause_button->SetBackgroundColor(StateColor(
        std::pair(wxColour(0xF2, 0xEE, 0xE8), (int) StateColor::Disabled),
        std::pair(wxColour(0xF8, 0xE5, 0xC9), (int) StateColor::Pressed),
        std::pair(wxColour(0xFF, 0xF2, 0xE4), (int) StateColor::Hovered),
        std::pair(wxColour(0xFF, 0xF9, 0xF1), (int) StateColor::Normal)));
    m_pause_button->SetBorderColor(StateColor(
        std::pair(wxColour(0xC8, 0xC0, 0xB6), (int) StateColor::Disabled),
        std::pair(wxColour(0xBD, 0x82, 0x3D), (int) StateColor::Pressed),
        std::pair(wxColour(0xE2, 0xAD, 0x70), (int) StateColor::Hovered),
        std::pair(wxColour(0xD7, 0xA4, 0x6D), (int) StateColor::Normal)));
    m_pause_button->SetTextColor(StateColor(
        std::pair(wxColour(0xB9, 0xB3, 0xAA), (int) StateColor::Disabled),
        std::pair(wxColour(0xB7, 0x78, 0x2E), (int) StateColor::Pressed),
        std::pair(wxColour(0xDE, 0x9D, 0x55), (int) StateColor::Hovered),
        std::pair(wxColour(0xD7, 0xA4, 0x6D), (int) StateColor::Normal)));
    m_stop_button = new Button(details, wxString::FromUTF8("Stop"), "print_control_stop_red", 0, 14);
    m_stop_button->SetMinSize(wxSize(FromDIP(80), FromDIP(40)));
    m_stop_button->SetMaxSize(wxSize(FromDIP(80), FromDIP(40)));
    m_stop_button->SetCornerRadius(FromDIP(8));
    m_stop_button->SetBackgroundColor(StateColor(
        std::pair(wxColour(0xF2, 0xEC, 0xEC), (int) StateColor::Disabled),
        std::pair(wxColour(0xFF, 0xDF, 0xDF), (int) StateColor::Pressed),
        std::pair(wxColour(0xFF, 0xEF, 0xEF), (int) StateColor::Hovered),
        std::pair(wxColour(0xFF, 0xF9, 0xF9), (int) StateColor::Normal)));
    m_stop_button->SetBorderColor(StateColor(
        std::pair(wxColour(0xC8, 0xB8, 0xB8), (int) StateColor::Disabled),
        std::pair(wxColour(0xE9, 0x55, 0x4E), (int) StateColor::Pressed),
        std::pair(wxColour(0xFF, 0x66, 0x5C), (int) StateColor::Hovered),
        std::pair(wxColour(0xFF, 0x7D, 0x72), (int) StateColor::Normal)));
    m_stop_button->SetTextColor(StateColor(
        std::pair(wxColour(0xBA, 0xAD, 0xAD), (int) StateColor::Disabled),
        std::pair(wxColour(0xE8, 0x4C, 0x45), (int) StateColor::Pressed),
        std::pair(wxColour(0xFF, 0x5B, 0x52), (int) StateColor::Hovered),
        std::pair(wxColour(0xFF, 0x7D, 0x72), (int) StateColor::Normal)));
    action_row->Add(m_pause_button, 0, wxRIGHT, FromDIP(10));
    action_row->Add(m_stop_button, 0);
    details_sizer->Add(action_row, 0, wxTOP, FromDIP(14));

    m_pause_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_pause_handler)
            m_pause_handler();
    });
    m_stop_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_stop_handler)
            m_stop_handler();
    });

    details->SetSizer(details_sizer);
    content_sizer->Add(details, 1, wxEXPAND);
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

    if (file_layout_needed) {
        Freeze();
        Layout();
        Thaw();
    }
}

void PrintStatusPanel::set_pause_handler(ActionHandler handler)
{
    m_pause_handler = std::move(handler);
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
    const int width = FromDIP(220);
    const int height = FromDIP(150);
    wxBitmap bitmap(width, height);

    wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(*wxBLACK));
    dc.Clear();

    wxFont font = GetFont();
    font.SetPointSize(std::max(18, font.GetPointSize() + 12));
    font.SetWeight(wxFONTWEIGHT_BOLD);
    dc.SetFont(font);
    dc.SetTextForeground(wxColour(0xF4, 0xF6, 0xF8));

    const wxString text = wxString::FromUTF8("Co Print");
    const wxSize text_size = dc.GetTextExtent(text);
    const int logo_size = FromDIP(56);
    const int gap = FromDIP(12);
    const int group_width = logo_size + gap + text_size.GetWidth();
    const int start_x = std::max(FromDIP(12), (width - group_width) / 2);
    const int center_y = height / 2;

    wxImage logo(wxString::FromUTF8((resources_dir() + "/images/CoPrintSlicer_192px_transparent.png").c_str()), wxBITMAP_TYPE_PNG);
    bool drew_logo = false;

    if (logo.IsOk()) {
        logo.Rescale(logo_size, logo_size, wxIMAGE_QUALITY_HIGH);
        dc.DrawBitmap(wxBitmap(logo), start_x, center_y - logo_size / 2, true);
        drew_logo = true;
    }

    const int text_x = drew_logo ? start_x + logo_size + gap : (width - text_size.GetWidth()) / 2;
    dc.DrawText(text, text_x, center_y - text_size.GetHeight() / 2);

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
