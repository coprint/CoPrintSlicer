#ifndef slic3r_GUI_DeviceDashboard_MoonrakerDeviceController_hpp_
#define slic3r_GUI_DeviceDashboard_MoonrakerDeviceController_hpp_

#include <functional>

#include <wx/colour.h>
#include <wx/string.h>

namespace Slic3r {
class MachineObject;

namespace GUI {

class PrinterWebView;

namespace DeviceDashboard {

/** Thin facade over PrinterWebView dashboard/Moonraker logic for Monitor and Start Print. */
class MoonrakerDeviceController
{
public:
    explicit MoonrakerDeviceController(PrinterWebView* backend);

    void refresh();
    void update_mode();

    void sync_model_colors_from_plater();
    void sync_loaded_tool_filaments(MachineObject* obj, std::function<void()> on_done = {});
    void fetch_filament_selections(MachineObject* obj, std::function<void(bool ok)> on_done);
    void reset_loaded_tool_filaments();
    bool get_loaded_tool_filament(int tool_0based, wxColour* color_out, wxString* material_out) const;

private:
    PrinterWebView* m_backend{nullptr};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_MoonrakerDeviceController_hpp_
