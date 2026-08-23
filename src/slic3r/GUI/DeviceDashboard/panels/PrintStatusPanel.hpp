#ifndef slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/bitmap.h>
#include <wx/panel.h>

class wxStaticBitmap;
class wxStaticText;
class ProgressBar;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class PrintStatusPanel : public wxPanel
{
public:
    using ActionHandler = std::function<void()>;

    explicit PrintStatusPanel(wxWindow* parent);

    void apply_state(const PrintJobState& state);
    void set_pause_handler(ActionHandler handler);
    void set_stop_handler(ActionHandler handler);
    void reset_thumbnail_placeholder();

    wxStaticBitmap* thumbnail_widget() const { return m_thumbnail; }

private:
    static wxString time_text(int seconds);
    wxBitmap make_thumbnail_placeholder();
    void set_print_actions_enabled(bool enabled);

    DeviceCardFrame* m_frame{nullptr};
    wxStaticBitmap* m_thumbnail{nullptr};
    wxStaticText* m_file_name{nullptr};
    wxStaticText* m_elapsed_time{nullptr};
    wxStaticText* m_layer_info{nullptr};
    wxStaticText* m_remaining_time{nullptr};
    ProgressBar* m_progress{nullptr};
    wxStaticBitmap* m_pause_icon{nullptr};
    wxStaticBitmap* m_stop_icon{nullptr};
    ActionHandler m_pause_handler;
    ActionHandler m_stop_handler;
    bool m_print_actions_enabled{true};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_PrintStatusPanel_hpp_
