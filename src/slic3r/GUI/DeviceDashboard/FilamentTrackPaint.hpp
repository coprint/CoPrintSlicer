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
bool filament_track_fill_is_dark(const wxColour &fill);

// Edit pencil on the spool: true 14 DIP (not DeviceUiStyle::scaled / 0.8).
inline constexpr int kFilamentEditIconDip = 14;

void draw_filament_track_rails(wxGraphicsContext *gc, const wxRect &track, wxWindow *dip_window,
    bool paired_rails = false);

void draw_filament_track(wxGraphicsContext *gc, const wxRect &track, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material = wxEmptyString,
    bool paired_rails = false);

// Dashboard spool: 1 DIP #C0C0C0 left border + 2 DIP rail, both 103 DIP tall;
// center 40x91. Outer bounds are 46x103.
wxSize filament_spool_bounds_size(wxWindow *dip_window);
void draw_filament_spool(wxGraphicsContext *gc, const wxRect &bounds, int tool_1based,
    const wxColour &color, bool has_filament, FilamentTrackCenter center,
    const wxBitmap &edit_icon, wxWindow *dip_window, const wxString &material = wxEmptyString);

}}} // namespace Slic3r::GUI::DeviceDashboard
