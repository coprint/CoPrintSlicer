#include "FilamentTrackPaint.hpp"

#include "DeviceUiStyle.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

#include <algorithm>

#include <wx/dcgraph.h>
#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

bool filament_track_fill_is_dark(const wxColour &fill)
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

void draw_filament_track_rails(wxGraphicsContext *gc, const wxRect &track, wxWindow *dip_window,
    bool paired_rails)
{
    const int extend = DeviceUiStyle::dip(dip_window, 5);
    const double pen_w = std::max(1.0, static_cast<double>(dip_window->FromDIP(1)));
    gc->SetPen(wxPen(wxColour(175, 178, 182), static_cast<int>(pen_w)));
    const double left_outer  = track.x + pen_w / 2.0;
    const double right_outer = track.x + track.width - pen_w / 2.0;
    const double y0 = track.y - extend;
    const double y1 = track.y + track.height + extend;
    gc->StrokeLine(left_outer, y0, left_outer, y1);
    gc->StrokeLine(right_outer, y0, right_outer, y1);
    if (paired_rails) {
        gc->StrokeLine(left_outer + pen_w, y0, left_outer + pen_w, y1);
        gc->StrokeLine(right_outer - pen_w, y0, right_outer - pen_w, y1);
    }
}

void draw_filament_track(wxGraphicsContext *gc, const wxRect &track, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material, bool paired_rails)
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
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(track.x, track.y, track.width, track.height);
    }

    draw_filament_track_rails(gc, track, dip_window, paired_rails);

    if (paired_rails && !(has_filament && color.IsOk())) {
        const double pen_w = std::max(1.0, static_cast<double>(dip_window->FromDIP(1)));
        gc->SetPen(wxPen(wxColour(175, 178, 182), static_cast<int>(pen_w)));
        const double x0 = track.x + pen_w / 2.0;
        const double x1 = track.x + track.width - pen_w / 2.0;
        const double y0 = track.y + pen_w / 2.0;
        const double y1 = track.y + track.height - pen_w / 2.0;
        gc->StrokeLine(x0, y0, x1, y0);
        gc->StrokeLine(x0, y1, x1, y1);
    }

    switch (center) {
    case FilamentTrackCenter::ToolNumber: {
        const wxColour base = readable_filament_track_colour(color, wxColour(70, 126, 205));
        const wxColour text_colour = has_filament && color.IsOk()
            ? (filament_track_fill_is_dark(base) ? *wxWHITE : wxColour(48, 48, 50))
            : wxColour(130, 134, 140);
        const bool compact = track.height < DeviceUiStyle::dip(dip_window, 70);
        if (material.IsEmpty()) {
            draw_centered_text(gc, wxString::Format("%d", tool_1based), track, text_colour,
                compact ? DeviceUiStyle::scaled(12) : DeviceUiStyle::scaled(22), true, dip_window);
        } else {
            wxRect num_rect = track;
            num_rect.height = static_cast<int>(track.height * 0.58);
            wxRect mat_rect = track;
            mat_rect.y      = track.y + static_cast<int>(track.height * 0.52);
            mat_rect.height = track.height - (mat_rect.y - track.y);
            draw_centered_text(gc, wxString::Format("%d", tool_1based), num_rect, text_colour,
                compact ? DeviceUiStyle::scaled(10) : DeviceUiStyle::scaled(16), true, dip_window);
            draw_centered_text(gc, material, mat_rect, text_colour,
                compact ? DeviceUiStyle::scaled(6) : DeviceUiStyle::scaled(8), true, dip_window);
        }
        break;
    }
    case FilamentTrackCenter::EditIcon:
        draw_centered_bitmap(gc, edit_icon, track);
        break;
    case FilamentTrackCenter::PlusSign:
        draw_centered_text(gc, "+", track, wxColour(130, 134, 140), DeviceUiStyle::scaled(26), false, dip_window);
        break;
    case FilamentTrackCenter::SlashSign:
        draw_centered_text(gc, "/", track, wxColour(130, 134, 140), DeviceUiStyle::scaled(22), true, dip_window);
        break;
    }
}

namespace {

const wxColour kSpoolRailBorder(0xC0, 0xC0, 0xC0);
const wxColour kSpoolRailFill(0xCC, 0xCB, 0xCA);

int spool_border_w(wxWindow *win) { return win->FromDIP(1); }
int spool_rail_w(wxWindow *win) { return win->FromDIP(2); }
int spool_rail_h(wxWindow *win) { return win->FromDIP(103); }
int spool_center_w(wxWindow *win) { return win->FromDIP(40); }
int spool_center_h(wxWindow *win) { return win->FromDIP(91); }

void draw_spool_center_mark(wxGraphicsContext *gc, const wxRect &center, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter mark,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material)
{
    switch (mark) {
    case FilamentTrackCenter::ToolNumber: {
        const wxColour base = readable_filament_track_colour(color, wxColour(70, 126, 205));
        const wxColour text_colour = has_filament && color.IsOk()
            ? (filament_track_fill_is_dark(base) ? *wxWHITE : wxColour(48, 48, 50))
            : wxColour(130, 134, 140);
        if (material.IsEmpty()) {
            draw_centered_text(gc, wxString::Format("%d", tool_1based), center, text_colour,
                DeviceUiStyle::scaled(12), true, dip_window);
        } else {
            wxRect num_rect = center;
            num_rect.height = static_cast<int>(center.height * 0.58);
            wxRect mat_rect = center;
            mat_rect.y      = center.y + static_cast<int>(center.height * 0.52);
            mat_rect.height = center.height - (mat_rect.y - center.y);
            draw_centered_text(gc, wxString::Format("%d", tool_1based), num_rect, text_colour,
                DeviceUiStyle::scaled(10), true, dip_window);
            draw_centered_text(gc, material, mat_rect, text_colour,
                DeviceUiStyle::scaled(6), true, dip_window);
        }
        break;
    }
    case FilamentTrackCenter::EditIcon:
        draw_centered_bitmap(gc, edit_icon, center);
        break;
    case FilamentTrackCenter::PlusSign:
        draw_centered_text(gc, "+", center, wxColour(130, 134, 140), DeviceUiStyle::scaled(26), false, dip_window);
        break;
    case FilamentTrackCenter::SlashSign:
        draw_centered_text(gc, "/", center, wxColour(130, 134, 140), DeviceUiStyle::scaled(22), true, dip_window);
        break;
    }
}

} // namespace

wxSize filament_spool_bounds_size(wxWindow *dip_window)
{
    const int border = spool_border_w(dip_window);
    const int rail = spool_rail_w(dip_window);
    return wxSize(border + rail + spool_center_w(dip_window) + border + rail,
        spool_rail_h(dip_window));
}

void draw_filament_spool(wxGraphicsContext *gc, const wxRect &bounds, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material)
{
    const int border_w = spool_border_w(dip_window);
    const int rail_w = spool_rail_w(dip_window);
    const int rail_h = bounds.height;
    const int center_w = spool_center_w(dip_window);
    const int center_h = spool_center_h(dip_window);
    const int center_y = bounds.y + std::max(0, (rail_h - center_h) / 2);

    const int left_border_x = bounds.x;
    const int left_rail_x = left_border_x + border_w;
    const int center_x = left_rail_x + rail_w;
    const int right_border_x = center_x + center_w;
    const int right_rail_x = right_border_x + border_w;
    const wxRect center_rect(center_x, center_y, center_w, center_h);

    gc->SetPen(*wxTRANSPARENT_PEN);
    if (has_filament && color.IsOk()) {
        const wxColour base = readable_filament_track_colour(color, wxColour(70, 126, 205));
        wxGraphicsGradientStops stops(
            StateColor::LightenDarkenColor(base, 10),
            StateColor::LightenDarkenColor(base, -14));
        stops.Add(base, 0.5f);
        gc->SetBrush(gc->CreateLinearGradientBrush(
            center_rect.x, center_rect.y, center_rect.x + center_rect.width, center_rect.y, stops));
        gc->DrawRectangle(center_rect.x, center_rect.y, center_rect.width, center_rect.height);
    } else {
        gc->SetBrush(wxBrush(*wxWHITE));
        gc->DrawRectangle(center_rect.x, center_rect.y, center_rect.width, center_rect.height);
    }

    // Pixel-aligned bars: antialiasing turns a 2 DIP fill into a 1 px looking line.
    gc->SetAntialiasMode(wxANTIALIAS_NONE);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(kSpoolRailFill));
    gc->DrawRectangle(left_rail_x, bounds.y, rail_w, rail_h);
    gc->DrawRectangle(right_rail_x, bounds.y, rail_w, rail_h);
    gc->SetBrush(wxBrush(kSpoolRailBorder));
    gc->DrawRectangle(left_border_x, bounds.y, border_w, rail_h);
    gc->DrawRectangle(right_border_x, bounds.y, border_w, rail_h);
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    draw_spool_center_mark(gc, center_rect, tool_1based, color, has_filament, center, edit_icon,
        dip_window, material);
}

}}} // namespace Slic3r::GUI::DeviceDashboard
