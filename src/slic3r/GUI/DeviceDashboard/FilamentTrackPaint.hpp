#pragma once

#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

class wxBitmap;
class wxGraphicsContext;
class wxWindow;

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

enum class FilamentTrackCenter {
    ToolNumber,
    EditIcon,
    PlusSign,
    SlashSign,
};

wxColour readable_filament_track_colour(const wxColour &colour, const wxColour &fallback);

void draw_filament_track_rails(wxGraphicsContext *gc, const wxRect &track, wxWindow *dip_window,
    bool paired_rails = false);

void draw_filament_track(wxGraphicsContext *gc, const wxRect &track, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material = wxEmptyString,
    bool paired_rails = false);

}}} // namespace Slic3r::GUI::DeviceDashboard
