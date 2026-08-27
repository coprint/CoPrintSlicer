#include "DeviceUiStyle.hpp"

#include <algorithm>
#include <cmath>

#include <wx/window.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

wxColour DeviceUiStyle::page_background() { return wxColour(238, 238, 239); }
wxColour DeviceUiStyle::card_background() { return wxColour(255, 255, 255); }
wxColour DeviceUiStyle::card_header_background() { return wxColour(0xF9, 0xF8, 0xF9); }
wxColour DeviceUiStyle::card_header_border() { return wxColour(0xE1, 0xE1, 0xE1); }
wxColour DeviceUiStyle::card_border() { return wxColour(199, 199, 199); }
wxColour DeviceUiStyle::control_background() { return wxColour(245, 245, 245); }
wxColour DeviceUiStyle::text_primary() { return wxColour(0x43, 0x43, 0x43); }
wxColour DeviceUiStyle::text_muted() { return wxColour(0x43, 0x43, 0x43); }
wxColour DeviceUiStyle::accent() { return wxColour(44, 182, 125); }
wxColour DeviceUiStyle::danger() { return wxColour(255, 125, 114); }

int DeviceUiStyle::card_radius() { return 6; }
int DeviceUiStyle::card_border_width() { return 0; }

int DeviceUiStyle::card_pad_horizontal(wxWindow* win)
{
    const int base = win != nullptr ? win->FromDIP(100) : 100;
    return std::max(0, static_cast<int>(std::lround(8.5 * static_cast<double>(base) / 100.0)));
}

int DeviceUiStyle::card_pad_vertical(wxWindow* win)
{
    return win != nullptr ? win->FromDIP(10) : 10;
}

int DeviceUiStyle::scaled(int dip_value)
{
    if (dip_value <= 0)
        return 0;
    return std::max(1, static_cast<int>(std::lround(dip_value * 0.8)));
}

int DeviceUiStyle::dip(wxWindow* win, int dip_value)
{
    const int value = scaled(dip_value);
    return win != nullptr ? win->FromDIP(value) : value;
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
