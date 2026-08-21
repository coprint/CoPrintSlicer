#ifndef slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/image.h>
#include <wx/panel.h>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;

enum class CameraLoadState {
    Idle,
    Initializing,
    Live,
    Failed
};

class CameraPanel : public wxPanel
{
public:

    using RefreshHandler = std::function<void()>;
    using PlayHandler = std::function<void()>;
    using TimelapseHandler = std::function<void()>;

    explicit CameraPanel(wxWindow* parent);

    void apply_state(const CameraState& state);
    void set_refresh_handler(RefreshHandler handler);
    void set_play_handler(PlayHandler handler);
    void set_timelapse_handler(TimelapseHandler handler);
    void set_load_state(CameraLoadState state);
    CameraLoadState load_state() const { return m_load_state; }
    void fit_preview(int max_width, int max_height);

    // Panel that hosts the live WebView stream (not the idle logo layer).
    wxPanel* webview_host() const;

private:
    void update_from_load_state();
    void layout_viewport_layers();
    int chrome_height() const;

    DeviceCardFrame* m_frame{nullptr};
    wxPanel* m_viewport{nullptr};
    wxPanel* m_idle_placeholder{nullptr};
    wxImage m_idle_source_image;
    wxPanel* m_stream_host{nullptr};
    wxStaticText* m_empty_state{nullptr};
    wxStaticText* m_status_label{nullptr};
    wxStaticBitmap* m_refresh_btn{nullptr};
    wxStaticBitmap* m_timelapse_btn{nullptr};
    wxPanel* m_play_btn{nullptr};
    RefreshHandler m_refresh_handler;
    PlayHandler m_play_handler;
    TimelapseHandler m_timelapse_handler;
    CameraLoadState m_load_state{CameraLoadState::Idle};
    bool m_fitting_preview{false};
    wxSize m_fitted_preview{wxDefaultSize};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_CameraPanel_hpp_
