#include "CameraPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../wxExtensions.hpp"

#include <utility>

#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

class CameraPlayButton final : public wxPanel
{
public:
    using ClickHandler = std::function<void()>;

    explicit CameraPlayButton(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(86), FromDIP(30)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &CameraPlayButton::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_click_handler)
                m_click_handler();
        });
    }

    void set_click_handler(ClickHandler handler) { m_click_handler = std::move(handler); }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : DeviceUiStyle::page_background()));
        dc.Clear();

        const wxSize size = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(DeviceUiStyle::accent()));
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(8));

        const wxString label = wxString::FromUTF8("Play");
        wxCoord text_w = 0;
        wxCoord text_h = 0;
        dc.SetTextForeground(*wxWHITE);
        dc.GetTextExtent(label, &text_w, &text_h);

        const int gap = FromDIP(6);
        const int icon_w = FromDIP(9);
        const int icon_h = FromDIP(10);
        const int total_w = icon_w + gap + text_w;
        int x = (size.GetWidth() - total_w) / 2;

        const int icon_y = (size.GetHeight() - icon_h) / 2;
        wxPoint play_shape[] = {
            wxPoint(x, icon_y),
            wxPoint(x, icon_y + icon_h),
            wxPoint(x + icon_w, icon_y + icon_h / 2)
        };
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(*wxWHITE));
        dc.DrawPolygon(3, play_shape);
        x += icon_w + gap;

        dc.DrawText(label, x, (size.GetHeight() - text_h) / 2);
    }

    ClickHandler m_click_handler;
};

} // namespace

CameraPanel::CameraPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());
    SetMinSize(wxSize(FromDIP(460), FromDIP(555)));

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Live Camera"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::page_background());
    auto* content_sizer = new wxBoxSizer(wxVERTICAL);

    auto* header_actions = new wxPanel(m_frame, wxID_ANY);
    header_actions->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* header_actions_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_timelapse_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_scaled_bitmap("camera_timelapse_white", header_actions, 18));
    m_timelapse_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_timelapse_btn->SetToolTip(wxString::FromUTF8("Timelapse"));
    m_timelapse_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_timelapse_handler) m_timelapse_handler();
    });
    header_actions_sizer->Add(m_timelapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

    m_fullscreen_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_scaled_bitmap("camera_fullscreen_white", header_actions, 18));
    m_fullscreen_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_fullscreen_btn->SetToolTip(wxString::FromUTF8("Fullscreen"));
    m_fullscreen_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_fullscreen_handler) m_fullscreen_handler();
    });
    header_actions_sizer->Add(m_fullscreen_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

    m_refresh_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_scaled_bitmap("camera_refresh_white", m_frame, 16));
    m_refresh_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_refresh_btn->SetToolTip(wxString::FromUTF8("Refresh"));
    m_refresh_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_refresh_handler) m_refresh_handler();
    });
    header_actions_sizer->Add(m_refresh_btn, 0, wxALIGN_CENTER_VERTICAL);
    header_actions->SetSizer(header_actions_sizer);
    m_frame->set_header_action(header_actions);

    m_viewport = new wxPanel(m_frame->content_parent(), wxID_ANY);
    m_viewport->SetBackgroundColour(*wxBLACK);
    m_viewport->SetMinSize(wxSize(FromDIP(420), FromDIP(390)));
    auto* viewport_sizer = new wxBoxSizer(wxVERTICAL);
    m_empty_state = new wxStaticText(m_viewport, wxID_ANY,
        wxString::FromUTF8("Camera unavailable"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    m_empty_state->SetForegroundColour(DeviceUiStyle::text_muted());
    m_empty_state->Hide();
    viewport_sizer->AddStretchSpacer(1);
    viewport_sizer->Add(m_empty_state, 0, wxALIGN_CENTER_HORIZONTAL);
    viewport_sizer->AddStretchSpacer(1);
    m_viewport->SetSizer(viewport_sizer);

    auto* control_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* play_btn = new CameraPlayButton(m_frame->content_parent());
    play_btn->set_click_handler([this]() {
        if (m_play_handler)
            m_play_handler();
    });
    m_play_btn = play_btn;
    control_sizer->Add(m_play_btn, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));

    content_sizer->Add(m_viewport, 1, wxEXPAND);
    content_sizer->Add(control_sizer, 0, wxEXPAND);
    m_frame->set_content(content_sizer);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void CameraPanel::apply_state(const CameraState& state)
{
    if (m_empty_state != nullptr)
        m_empty_state->Show(m_stream_started && (!state.available || state.stream_url.IsEmpty()));

    if (m_viewport != nullptr)
        m_viewport->Layout();
}

void CameraPanel::set_refresh_handler(RefreshHandler handler)
{
    m_refresh_handler = std::move(handler);
}

void CameraPanel::set_play_handler(PlayHandler handler)
{
    m_play_handler = std::move(handler);
}

void CameraPanel::set_fullscreen_handler(FullscreenHandler handler)
{
    m_fullscreen_handler = std::move(handler);
}

void CameraPanel::set_timelapse_handler(TimelapseHandler handler)
{
    m_timelapse_handler = std::move(handler);
}

void CameraPanel::set_stream_started(bool started)
{
    if (m_stream_started == started)
        return;
    m_stream_started = started;
    if (m_empty_state != nullptr)
        m_empty_state->Show(false);
    if (m_viewport != nullptr)
        m_viewport->Layout();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
