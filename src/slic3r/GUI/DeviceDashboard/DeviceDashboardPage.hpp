#ifndef slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_

#include "DeviceCommandService.hpp"
#include "DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>
#include <wx/scrolwin.h>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class PrinterOfflineOverlay;
class CameraPanel;
class FilamentPanel;
class MovementPanel;
class PrinterStatusPanel;
class PrintStatusPanel;

class DeviceDashboardPage : public wxScrolledWindow
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit DeviceDashboardPage(wxWindow* parent);

    void apply_state(const DeviceDashboardState& state);
    void set_connecting_visible(bool visible, const wxString &message = wxEmptyString);
    void set_offline_overlay_visible(bool visible, const wxString &printer_name = wxEmptyString);
    void set_offline_retry_handler(std::function<void()> handler);
    void set_command_handler(CommandHandler handler);

    void update_camera_host_responsive_size();

    CameraPanel* camera_panel() const { return m_camera_panel; }
    PrintStatusPanel* print_status_panel() const { return m_print_status_panel; }
    MovementPanel* movement_panel() const { return m_movement_panel; }
    PrinterStatusPanel* printer_status_panel() const { return m_printer_status_panel; }
    FilamentPanel* filament_panel() const { return m_filament_panel; }

    wxPanel* camera_webview_host() const;
    wxStaticBitmap* thumbnail_widget() const;

    void set_camera_refresh_handler(std::function<void()> handler);
    void set_camera_play_handler(std::function<void()> handler);
    void set_camera_timelapse_handler(std::function<void()> handler);

    void set_printer_status_handlers(
        std::function<void(int)> tool_select,
        std::function<void(int, int)> nozzle_temp,
        std::function<void(int, int)> fan_speed,
        std::function<void(int)> bed_temp);

private:
    void bind_size_handler();
    void refresh_scroll();
    void layout_offline_overlay();
    void update_controls_enabled();

    CameraPanel* m_camera_panel{nullptr};
    PrintStatusPanel* m_print_status_panel{nullptr};
    MovementPanel* m_movement_panel{nullptr};
    PrinterStatusPanel* m_printer_status_panel{nullptr};
    FilamentPanel* m_filament_panel{nullptr};
    wxPanel* m_content_panel{nullptr};
    PrinterOfflineOverlay* m_offline_overlay{nullptr};
    CommandHandler m_command_handler;
    bool m_refreshing_scroll{false};
    bool m_pending_scroll_refresh{false};
    bool m_can_send_commands{false};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceDashboardPage_hpp_
