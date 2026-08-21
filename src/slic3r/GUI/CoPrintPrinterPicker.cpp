#include "CoPrintPrinterPicker.hpp"

#include "I18N.hpp"
#include "GUI.hpp"
#include "PrinterWebView.hpp"
#include "GUI_App.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "Widgets/StaticBox.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

CoPrintPrinterPicker::CoPrintPrinterPicker(wxWindow* parent, PrinterWebView* backend)
    : wxPanel(parent, wxID_ANY)
    , m_backend(backend)
{
    SetBackgroundColour(wxColour("#FEFFFF"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxPanel(this, wxID_ANY);
    header->SetBackgroundColour(wxColour("#FEFFFF"));
    auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(header, wxID_ANY, _L("Printers"));
    {
        wxFont f = title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(f);
    }
    header_sizer->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    header->SetSizer(header_sizer);
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(header, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    m_pick_row = new StaticBox(this, wxID_ANY);
    m_pick_row->SetCornerRadius(static_cast<double>(FromDIP(5)));
    m_pick_row->SetBorderWidth(1);
    m_pick_row->SetBorderColorNormal(wxColour("#C7C7C7"));
    m_pick_row->SetBackgroundColorNormal(wxColour("#FEFFFF"));
    m_pick_row->SetBackgroundColour(wxColour("#FEFFFF"));
    m_pick_row->SetMinSize(wxSize(-1, FromDIP(32)));
    m_pick_row->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* pick_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_name_label = new wxStaticText(m_pick_row, wxID_ANY, _L("Select printer"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_name_label->SetForegroundColour(wxColour(40, 40, 44));
    m_name_label->SetCursor(wxCursor(wxCURSOR_HAND));
    m_status_dot = new wxStaticText(m_pick_row, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
    m_status_dot->SetCursor(wxCursor(wxCURSOR_HAND));
    m_status_dot->Hide();
    pick_sizer->Add(m_name_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    pick_sizer->Add(m_status_dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_pick_row->SetSizer(pick_sizer);
    sizer->Add(m_pick_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    SetSizer(sizer);

    auto open_popup = [this](wxMouseEvent& evt) {
        evt.StopPropagation();
        if (m_backend != nullptr)
            m_backend->toggle_printers_popup_at(m_pick_row);
    };
    m_pick_row->Bind(wxEVT_LEFT_DOWN, open_popup);
    m_name_label->Bind(wxEVT_LEFT_DOWN, open_popup);
    m_status_dot->Bind(wxEVT_LEFT_DOWN, open_popup);

    update_selection();
}

void CoPrintPrinterPicker::refresh_list()
{
    if (m_backend != nullptr)
        m_backend->rebuild_printers_popup();
}

void CoPrintPrinterPicker::update_selection()
{
    if (m_name_label == nullptr || m_status_dot == nullptr)
        return;

    const wxColour k_green("#35AD27");
    const wxColour k_yellow("#F2C94C");
    const wxColour k_red("#E74C3C");

    Slic3r::DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;

    if (obj == nullptr) {
        m_name_label->SetLabel(_L("Select printer"));
        m_name_label->SetForegroundColour(wxColour(40, 40, 44));
        m_status_dot->Hide();
    } else {
        wxString name = m_backend != nullptr
            ? m_backend->sidebar_display_name_for(obj)
            : from_u8(obj->get_dev_name());
        m_name_label->SetLabel(name);
        m_name_label->SetForegroundColour(wxColour(28, 28, 28));
        m_status_dot->Show();

        if (obj->is_online()) {
            m_status_dot->SetForegroundColour(k_green);
        } else if (obj->is_connecting()) {
            m_status_dot->SetForegroundColour(k_yellow);
        } else {
            m_status_dot->SetForegroundColour(k_red);
        }
    }

    if (m_pick_row != nullptr) {
        m_pick_row->Layout();
        Layout();
    }
}

} // namespace GUI
} // namespace Slic3r
