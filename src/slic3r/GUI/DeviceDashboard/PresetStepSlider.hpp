#ifndef slic3r_GUI_DeviceDashboard_PresetStepSlider_hpp_
#define slic3r_GUI_DeviceDashboard_PresetStepSlider_hpp_

#include <functional>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class PresetStepSlider : public wxPanel
{
public:
    using ChangeHandler = std::function<void(int index)>;

    PresetStepSlider(wxWindow *parent, std::vector<wxString> labels);

    void set_selection(int index);
    int  selection() const { return m_selection; }
    void set_change_handler(ChangeHandler handler);
    void set_enabled(bool enabled);
    bool enabled() const { return m_enabled; }

private:
    int  hit_test(const wxPoint &pos) const;
    wxPoint knot_center(int index) const;
    int  side_pad() const;
    void on_paint(wxPaintEvent &);
    void on_left_down(wxMouseEvent &event);
    void on_motion(wxMouseEvent &event);
    void on_left_up(wxMouseEvent &event);
    void on_capture_lost(wxMouseCaptureLostEvent &);
    void end_drag();
    void select_from_mouse(const wxPoint &pos);

    std::vector<wxString> m_labels;
    int                   m_selection{0};
    bool                  m_dragging{false};
    bool                  m_enabled{true};
    ChangeHandler         m_change_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif
