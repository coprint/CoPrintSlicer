#pragma once

#include <wx/colour.h>
#include <wx/panel.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

/** Single filament track slot (device dashboard style), tool number in center. */
class FilamentTrackSlot : public wxPanel
{
public:
    FilamentTrackSlot(wxWindow *parent, int tool_1based, const wxColour &color, bool has_filament);

    void set_filament(const wxColour &color, bool has_filament);

    int tool_1based() const { return m_tool_1based; }

private:
    void on_paint(wxPaintEvent &event);

    int      m_tool_1based{1};
    wxColour m_color;
    bool     m_has_filament{false};
};

}}} // namespace Slic3r::GUI::DeviceDashboard
