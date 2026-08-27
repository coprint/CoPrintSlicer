#ifndef slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_

#include <wx/colour.h>

class wxWindow;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

struct DeviceUiStyle {
    static wxColour page_background();
    static wxColour card_background();
    static wxColour card_header_background();
    static wxColour card_header_border();
    static wxColour card_border();
    static wxColour control_background();
    static wxColour text_primary();
    static wxColour text_muted();
    static wxColour accent();
    static wxColour danger();

    static int card_radius();
    static int card_border_width();
    static int card_pad_horizontal(wxWindow* win);
    static int card_pad_vertical(wxWindow* win);

    // 80% of the original DIP size, used to shrink Movement / Printer Status / Filament.
    static int scaled(int dip_value);
    static int dip(wxWindow* win, int dip_value);
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceUiStyle_hpp_
