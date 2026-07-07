#pragma once

#include <wx/colour.h>
#include <wx/gdicmn.h>

class wxBitmap;
class wxGraphicsContext;
class wxWindow;

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

enum class FilamentTrackCenter {
    ToolNumber,
    EditIcon,
    PlusSign,
};

wxColour readable_filament_track_colour(const wxColour &colour, const wxColour &fallback);

void draw_filament_track_rails(wxGraphicsContext *gc, const wxRect &track, wxWindow *dip_window);

void draw_filament_track(wxGraphicsContext *gc, const wxRect &track, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window);

}}} // namespace Slic3r::GUI::DeviceDashboard
