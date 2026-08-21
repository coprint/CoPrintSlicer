#include "StatusPresetPopups.hpp"

#include "DeviceUiStyle.hpp"
#include "PresetStepSlider.hpp"

#include <algorithm>
#include <vector>

#include <wx/cursor.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

const wxColour kPopupBg(0xEB, 0xEB, 0xEB);

int percent_to_speed_index(int percent)
{
    if (percent <= 50)
        return 0;
    if (percent <= 100)
        return 1;
    if (percent <= 125)
        return 2;
    return 3;
}

int speed_index_to_percent(int index)
{
    switch (index) {
    case 0: return 50;
    case 1: return 100;
    case 2: return 125;
    default: return 166;
    }
}

int percent_to_fan_index(int percent)
{
    const int clamped = std::clamp(percent, 0, 100);
    return (clamped + 12) / 25;
}

int fan_index_to_percent(int index)
{
    return std::clamp(index, 0, 4) * 25;
}

void style_popup_shell(wxWindow *win)
{
    win->SetBackgroundColour(kPopupBg);
    win->SetCursor(wxCursor(wxCURSOR_ARROW));
#ifdef __WXOSX__
    // wxPopupTransientWindow releases mouse on idle and can leave the
    // app without hover/cursor tracking after the popup is dismissed.
    win->Bind(wxEVT_IDLE, [](wxIdleEvent &) {});
#endif
}

void release_mouse_captures(wxWindow *root)
{
    if (root == nullptr)
        return;
    if (root->HasCapture())
        root->ReleaseMouse();
    for (wxWindow *child : root->GetChildren())
        release_mouse_captures(child);
}

void position_popup(PopupWindow *popup, wxWindow *anchor)
{
    if (popup == nullptr || anchor == nullptr)
        return;
    popup->Layout();
    popup->Fit();
    popup->SetSize(popup->GetBestSize());
    const wxPoint screen = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().GetHeight() + 4));
    popup->Position(screen, wxSize(0, 0));
    wxSetCursor(wxCursor(wxCURSOR_ARROW));
    popup->Popup();
}

} // namespace

PrintSpeedPopup::PrintSpeedPopup(wxWindow *parent)
    : PopupWindow(parent, wxBORDER_SIMPLE | wxPU_CONTAINS_CONTROLS)
{
    style_popup_shell(this);
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *hint = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("This only takes effect during printing"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    hint->SetForegroundColour(DeviceUiStyle::text_muted());
    hint->SetBackgroundColour(kPopupBg);
    hint->SetCursor(wxCursor(wxCURSOR_ARROW));
    m_slider = new PresetStepSlider(this, {
        wxString::FromUTF8("Silent"),
        wxString::FromUTF8("Standard"),
        wxString::FromUTF8("Sport"),
        wxString::FromUTF8("Ludicrous")
    });
    m_slider->SetBackgroundColour(kPopupBg);
    m_slider->set_change_handler([this](int index) {
        if (m_change_handler)
            m_change_handler(speed_index_to_percent(index));
    });
    root->Add(hint, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(14));
    root->Add(m_slider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(root);
}

void PrintSpeedPopup::set_percent(int percent)
{
    if (m_slider != nullptr)
        m_slider->set_selection(percent_to_speed_index(percent));
}

void PrintSpeedPopup::set_change_handler(ChangeHandler handler)
{
    m_change_handler = std::move(handler);
}

void PrintSpeedPopup::popup_at(wxWindow *anchor)
{
    position_popup(this, anchor);
}

void PrintSpeedPopup::OnDismiss()
{
    release_mouse_captures(this);
    wxSetCursor(wxNullCursor);
    PopupWindow::OnDismiss();
}

FanSpeedPopup::FanSpeedPopup(wxWindow *parent)
    : PopupWindow(parent, wxBORDER_SIMPLE | wxPU_CONTAINS_CONTROLS)
{
    style_popup_shell(this);
    auto *root = new wxBoxSizer(wxVERTICAL);
    const std::vector<wxString> steps{
        wxString::FromUTF8("0%"),
        wxString::FromUTF8("25%"),
        wxString::FromUTF8("50%"),
        wxString::FromUTF8("75%"),
        wxString::FromUTF8("100%")
    };
    for (int i = 0; i < 4; ++i) {
        auto *title = new wxStaticText(this, wxID_ANY, wxString::Format("Tool %d", i + 1));
        title->SetForegroundColour(DeviceUiStyle::text_primary());
        title->SetBackgroundColour(kPopupBg);
        title->SetCursor(wxCursor(wxCURSOR_ARROW));
        m_sliders[i] = new PresetStepSlider(this, steps);
        m_sliders[i]->SetBackgroundColour(kPopupBg);
        m_sliders[i]->set_change_handler([this, i](int index) {
            if (m_change_handler)
                m_change_handler(i, fan_index_to_percent(index));
        });
        root->Add(title, 0, wxLEFT | wxTOP, FromDIP(14));
        root->Add(m_sliders[i], 0, wxEXPAND | wxLEFT | wxRIGHT | (i == 3 ? wxBOTTOM : 0), FromDIP(8));
    }
    SetSizer(root);
}

void FanSpeedPopup::set_percents(const std::array<int, 4> &percents)
{
    for (int i = 0; i < 4; ++i) {
        if (m_sliders[i] != nullptr)
            m_sliders[i]->set_selection(percent_to_fan_index(percents[i]));
    }
}

void FanSpeedPopup::set_change_handler(ChangeHandler handler)
{
    m_change_handler = std::move(handler);
}

void FanSpeedPopup::popup_at(wxWindow *anchor)
{
    position_popup(this, anchor);
}

void FanSpeedPopup::OnDismiss()
{
    release_mouse_captures(this);
    wxSetCursor(wxNullCursor);
    PopupWindow::OnDismiss();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
