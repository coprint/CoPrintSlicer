#include "FilamentTrackPaint.hpp"

#include "slic3r/GUI/Widgets/StateColor.hpp"

#include <wx/dcgraph.h>
#include <wx/font.h>
#include <wx/graphics.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

static bool filament_track_text_is_dark(const wxColour &fill)
{
    const int brightness = (fill.Red() * 299 + fill.Green() * 587 + fill.Blue() * 114) / 1000;
    return brightness < 140;
}

wxColour readable_filament_track_colour(const wxColour &colour, const wxColour &fallback)
{
    return colour.IsOk() ? colour : fallback;
}

static void draw_centered_text(wxGraphicsContext *gc, const wxString &text, const wxRect &rect,
    const wxColour &colour, int point_size_dip, bool bold, wxWindow *dip_window)
{
    wxFont font = dip_window->GetFont();
    font.SetPointSize(std::max(1, dip_window->FromDIP(point_size_dip)));
    font.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
    gc->SetFont(font, colour);
    double tw = 0.0;
    double th = 0.0;
    gc->GetTextExtent(text, &tw, &th);
    gc->DrawText(text,
        rect.x + std::max(0.0, (rect.width - tw) / 2.0),
        rect.y + std::max(0.0, (rect.height - th) / 2.0));
}

static void draw_centered_bitmap(wxGraphicsContext *gc, const wxBitmap &bitmap, const wxRect &rect)
{
    if (!bitmap.IsOk())
        return;
    const int x = rect.x + std::max(0, (rect.width - bitmap.GetWidth()) / 2);
    const int y = rect.y + std::max(0, (rect.height - bitmap.GetHeight()) / 2);
    gc->DrawBitmap(bitmap, x, y, bitmap.GetWidth(), bitmap.GetHeight());
}

void draw_filament_track_rails(wxGraphicsContext *gc, const wxRect &track, wxWindow *dip_window)
{
    const int extend = dip_window->FromDIP(5);
    gc->SetPen(wxPen(wxColour(175, 178, 182), dip_window->FromDIP(2)));
    gc->StrokeLine(track.x, track.y - extend, track.x, track.y + track.height + extend);
    gc->StrokeLine(track.GetRight(), track.y - extend, track.GetRight(), track.y + track.height + extend);
}

void draw_filament_track(wxGraphicsContext *gc, const wxRect &track, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window)
{
    if (has_filament && color.IsOk()) {
        const wxColour base = readable_filament_track_colour(color, wxColour(70, 126, 205));
        wxGraphicsGradientStops stops(
            StateColor::LightenDarkenColor(base, 10),
            StateColor::LightenDarkenColor(base, -14));
        stops.Add(base, 0.5f);
        gc->SetBrush(gc->CreateLinearGradientBrush(
            track.x, track.y, track.x + track.width, track.y, stops));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(track.x, track.y, track.width, track.height);
    } else {
        gc->SetBrush(wxBrush(*wxWHITE));
        gc->SetPen(wxPen(wxColour(200, 203, 208), dip_window->FromDIP(1)));
        gc->DrawRectangle(track.x, track.y, track.width, track.height);
    }

    draw_filament_track_rails(gc, track, dip_window);

    switch (center) {
    case FilamentTrackCenter::ToolNumber: {
        const wxColour base = readable_filament_track_colour(color, wxColour(70, 126, 205));
        const wxColour text_colour = has_filament && color.IsOk()
            ? (filament_track_text_is_dark(base) ? *wxWHITE : wxColour(48, 48, 50))
            : wxColour(130, 134, 140);
        draw_centered_text(gc, wxString::Format("%d", tool_1based), track, text_colour, 22, true, dip_window);
        break;
    }
    case FilamentTrackCenter::EditIcon:
        draw_centered_bitmap(gc, edit_icon, track);
        break;
    case FilamentTrackCenter::PlusSign:
        draw_centered_text(gc, "+", track, wxColour(130, 134, 140), 26, false, dip_window);
        break;
    }
}

}}} // namespace Slic3r::GUI::DeviceDashboard
