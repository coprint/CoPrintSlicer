#include "FilamentTrackSlot.hpp"

#include "DeviceUiStyle.hpp"
#include "FilamentTrackPaint.hpp"

#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

FilamentTrackSlot::FilamentTrackSlot(wxWindow *parent, int tool_1based, const wxColour &color, bool has_filament,
    FilamentTrackCenter center, const wxString &material)
    : wxPanel(parent, wxID_ANY)
    , m_tool_1based(tool_1based)
    , m_color(color)
    , m_has_filament(has_filament)
    , m_center(center)
    , m_material(material)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(DeviceUiStyle::page_background());
    SetCursor(wxCursor(center == FilamentTrackCenter::SlashSign ? wxCURSOR_ARROW : wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &FilamentTrackSlot::on_paint, this);
}

void FilamentTrackSlot::set_filament(const wxColour &color, bool has_filament)
{
    m_color        = color;
    m_has_filament = has_filament;
    Refresh();
}

void FilamentTrackSlot::set_material(const wxString &material)
{
    m_material = material;
    Refresh();
}

void FilamentTrackSlot::enable_hover(bool enable)
{
    if (m_hover_enabled == enable)
        return;
    m_hover_enabled = enable;
    if (enable) {
        Bind(wxEVT_ENTER_WINDOW, &FilamentTrackSlot::on_enter, this);
        Bind(wxEVT_LEAVE_WINDOW, &FilamentTrackSlot::on_leave, this);
        Bind(wxEVT_MOTION, &FilamentTrackSlot::on_motion, this);
    } else {
        Unbind(wxEVT_ENTER_WINDOW, &FilamentTrackSlot::on_enter, this);
        Unbind(wxEVT_LEAVE_WINDOW, &FilamentTrackSlot::on_leave, this);
        Unbind(wxEVT_MOTION, &FilamentTrackSlot::on_motion, this);
        set_hovered(false);
    }
}

void FilamentTrackSlot::set_selected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    Refresh();
}

void FilamentTrackSlot::set_hovered(bool hovered)
{
    if (!m_hover_enabled)
        hovered = false;
    if (m_hovered == hovered)
        return;
    m_hovered = hovered;
    Refresh();
}

bool FilamentTrackSlot::mouse_is_inside() const
{
    const wxPoint pt = ScreenToClient(wxGetMousePosition());
    return GetClientRect().Contains(pt);
}

void FilamentTrackSlot::on_enter(wxMouseEvent &event)
{
    set_hovered(true);
    event.Skip();
}

void FilamentTrackSlot::on_leave(wxMouseEvent &event)
{
    // Refresh() can emit a fake leave; only clear when the cursor is really outside.
    if (!mouse_is_inside())
        set_hovered(false);
    event.Skip();
}

void FilamentTrackSlot::on_motion(wxMouseEvent &event)
{
    set_hovered(true);
    event.Skip();
}

void FilamentTrackSlot::on_paint(wxPaintEvent &event)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        event.Skip();
        return;
    }

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    const wxRect client = GetClientRect();
    if (m_hovered || m_selected) {
        const double inset = FromDIP(1);
        const double radius = FromDIP(6);
        const unsigned char alpha = m_hovered ? 55 : 32;
        gc->SetBrush(wxBrush(wxColour(47, 128, 237, alpha)));
        gc->SetPen(wxPen(wxColour(47, 128, 237), FromDIP(2)));
        gc->DrawRoundedRectangle(client.x + inset, client.y + inset,
            client.width - inset * 2, client.height - inset * 2, radius);
    }

    wxRect track = client;
    track.Deflate(FromDIP((m_hovered || m_selected) ? 4 : 1), FromDIP(6));

    const wxBitmap empty;
    draw_filament_track(gc.get(), track, m_tool_1based, m_color, m_has_filament,
        m_center, empty, this, m_material, true);
    event.Skip();
}

}}} // namespace Slic3r::GUI::DeviceDashboard
