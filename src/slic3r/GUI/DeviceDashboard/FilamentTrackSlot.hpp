#pragma once

#include "FilamentTrackPaint.hpp"

#include <wx/colour.h>
#include <wx/panel.h>

namespace Slic3r { namespace GUI { namespace DeviceDashboard {

/** Single filament track slot (device dashboard style), tool number in center. */
class FilamentTrackSlot : public wxPanel
{
public:
    FilamentTrackSlot(wxWindow *parent, int tool_1based, const wxColour &color, bool has_filament,
        FilamentTrackCenter center = FilamentTrackCenter::ToolNumber,
        const wxString &material = wxEmptyString);

    void set_filament(const wxColour &color, bool has_filament);
    void set_material(const wxString &material);
    void enable_hover(bool enable);
    void set_selected(bool selected);
    void set_hovered(bool hovered);

    int tool_1based() const { return m_tool_1based; }

private:
    void on_paint(wxPaintEvent &event);
    void on_enter(wxMouseEvent &event);
    void on_leave(wxMouseEvent &event);
    void on_motion(wxMouseEvent &event);
    bool mouse_is_inside() const;

    int                 m_tool_1based{1};
    wxColour            m_color;
    bool                m_has_filament{false};
    FilamentTrackCenter m_center{FilamentTrackCenter::ToolNumber};
    wxString            m_material;
    bool                m_hover_enabled{false};
    bool                m_hovered{false};
    bool                m_selected{false};
};

}}} // namespace Slic3r::GUI::DeviceDashboard
