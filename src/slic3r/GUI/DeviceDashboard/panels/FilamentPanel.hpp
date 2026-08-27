#ifndef slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_

#include "../DeviceCommandService.hpp"
#include "../DeviceDashboardState.hpp"

#include <functional>

#include <wx/panel.h>

class Button;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame;
class FilamentToolMapView;

class FilamentPanel : public wxPanel
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit FilamentPanel(wxWindow* parent);

    void apply_state(const FilamentState& state);
    void set_command_handler(CommandHandler handler);

private:
    void dispatch(DeviceCommand command) const;
    void set_selected_tool(int tool_index);
    void select_manage_tool(int tool_index);

    DeviceCardFrame* m_frame{nullptr};
    FilamentToolMapView* m_tool_map_view{nullptr};
    Button* m_load_button{nullptr};
    Button* m_unload_button{nullptr};
    int m_selected_tool_index{0};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_FilamentPanel_hpp_
