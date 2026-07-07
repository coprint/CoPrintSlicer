#include "FilamentTrackSlot.hpp"

#include "DeviceUiStyle.hpp"
#include "FilamentTrackPaint.hpp"

#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

FilamentTrackSlot::FilamentTrackSlot(wxWindow *parent, int tool_1based, const wxColour &color, bool has_filament)
    : wxPanel(parent, wxID_ANY)
    , m_tool_1based(tool_1based)
    , m_color(color)
    , m_has_filament(has_filament)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(DeviceUiStyle::page_background());
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &FilamentTrackSlot::on_paint, this);
}

void FilamentTrackSlot::set_filament(const wxColour &color, bool has_filament)
{
    m_color        = color;
    m_has_filament = has_filament;
    Refresh();
}

void FilamentTrackSlot::on_paint(wxPaintEvent &event)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(DeviceUiStyle::card_background()));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        event.Skip();
        return;
    }

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    wxRect track = GetClientRect();
    track.Deflate(0, FromDIP(6));

    const wxBitmap empty;
    draw_filament_track(gc.get(), track, m_tool_1based, m_color, m_has_filament,
        FilamentTrackCenter::ToolNumber, empty, this);
    event.Skip();
}

}}} // namespace Slic3r::GUI::DeviceDashboard
