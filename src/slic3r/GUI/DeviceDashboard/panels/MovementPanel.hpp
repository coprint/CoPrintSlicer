#ifndef slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_

#include "../DeviceCommandService.hpp"
#include "../DeviceDashboardState.hpp"

#include <cstdint>
#include <functional>
#include <array>

#include <wx/panel.h>

class Button;
class wxBoxSizer;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class MovementPanel : public wxPanel
{
public:
    using CommandHandler = std::function<void(const DeviceCommand&)>;

    explicit MovementPanel(wxWindow* parent);

    void apply_state(const MovementState& state);
    void set_command_handler(CommandHandler handler);
    void msw_rescale();
    wxWindow* status_slot() const { return m_status_slot; }

private:
    void relayout_joystick();
    Button* make_tool_button(wxWindow* parent, const wxString& label);
    Button* make_option_button(wxWindow* parent, const wxString& label);
    void dispatch_axis(Axis axis, double direction) const;
    void dispatch(DeviceCommand command) const;
    void set_active_tool_button(int tool_index);
    void set_available_tool_count(int tool_count);
    void set_active_distance_button(double distance_mm);
    void refresh_selection_styles();
    void set_controls_enabled(bool enabled);

    wxBoxSizer* m_controls_sizer{nullptr};
    wxWindow* m_status_slot{nullptr};
    wxWindow* m_xy_area{nullptr};
    Button* m_center_button{nullptr};
    wxWindow* m_z_plus_host{nullptr};
    wxWindow* m_z_minus_host{nullptr};
    std::array<Button*, MaxDashboardTools> m_tool_buttons{nullptr, nullptr, nullptr, nullptr};
    std::array<Button*, 4> m_distance_buttons{nullptr, nullptr, nullptr, nullptr};
    std::array<int8_t, MaxDashboardTools> m_tool_button_active{-1, -1, -1, -1};
    std::array<int8_t, 4> m_distance_button_active{-1, -1, -1, -1};
    double m_selected_distance_mm{1.0};
    int m_selected_tool{0};
    int m_available_tool_count{MaxDashboardTools};
    bool m_controls_enabled{true};
    bool m_relayout_busy{false};
    int m_last_square{-1};
    CommandHandler m_command_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_panels_MovementPanel_hpp_
