#ifndef slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class CameraPanel : public wxPanel
{
public:
    using RefreshHandler = std::function<void()>;
    using PlayHandler = std::function<void()>;

    explicit CameraPanel(wxWindow* parent);

    void apply_state(const CameraState& state);
    void set_refresh_handler(RefreshHandler handler);
    void set_play_handler(PlayHandler handler);
    void set_stream_started(bool started);

    wxPanel* webview_host() const { return m_viewport; }

private:
    DeviceCardFrame* m_frame{nullptr};
    wxPanel* m_viewport{nullptr};
    wxStaticText* m_empty_state{nullptr};
    wxStaticBitmap* m_refresh_btn{nullptr};
    wxPanel* m_play_btn{nullptr};
    RefreshHandler m_refresh_handler;
    PlayHandler m_play_handler;
    bool m_stream_started{false};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
