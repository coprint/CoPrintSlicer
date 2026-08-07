#ifndef slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/image.h>
#include <wx/panel.h>

class wxStaticBitmap;
class wxStaticText;
class wxFrame;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

class CameraPanel : public wxPanel
{
public:

    using RefreshHandler = std::function<void()>;
    using PlayHandler = std::function<void()>;
    using FullscreenHandler = std::function<void()>;
    using TimelapseHandler = std::function<void()>;

    explicit CameraPanel(wxWindow* parent);

    void apply_state(const CameraState& state);
    void set_refresh_handler(RefreshHandler handler);
    void set_play_handler(PlayHandler handler);
    void set_fullscreen_handler(FullscreenHandler handler);
    void set_timelapse_handler(TimelapseHandler handler);
    void set_stream_started(bool started);
    // Shrink/grow the camera viewport for short laptop screens so the Device tab fits.
    // height_px is already DPI-scaled (use FromDIP / client sizes).
    void set_viewport_height_px(int height_px);

    // Panel that hosts the live WebView stream (not the idle logo layer).
    wxPanel* webview_host() const;

private:
    void update_idle_visibility(bool stream_available);
    void layout_viewport_layers();
    void apply_viewport_height_px();
    // Moves the live stream host into a maximized top-level window (Esc, the
    // on-screen close button, or the window's own close box all return it to
    // the Device tab -- no more getting stuck needing Alt+Tab to escape).
    void toggle_fullscreen();

    DeviceCardFrame* m_frame{nullptr};
    wxFrame* m_fullscreen_frame{nullptr};
    wxPanel* m_viewport{nullptr};
    int m_viewport_height_px{0};
    wxPanel* m_idle_placeholder{nullptr};
    wxImage m_idle_source_image;
    wxPanel* m_stream_host{nullptr};
    wxStaticText* m_empty_state{nullptr};
    wxStaticBitmap* m_refresh_btn{nullptr};
    wxStaticBitmap* m_timelapse_btn{nullptr};
    wxStaticBitmap* m_fullscreen_btn{nullptr};
    wxPanel* m_play_btn{nullptr};
    RefreshHandler m_refresh_handler;
    PlayHandler m_play_handler;
    FullscreenHandler m_fullscreen_handler;
    TimelapseHandler m_timelapse_handler;
    bool m_stream_started{false};
    bool m_stream_available{false};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
