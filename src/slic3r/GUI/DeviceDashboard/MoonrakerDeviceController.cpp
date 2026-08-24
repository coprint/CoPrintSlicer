#include "MoonrakerDeviceController.hpp"

#include "slic3r/GUI/PrinterWebView.hpp"

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

MoonrakerDeviceController::MoonrakerDeviceController(PrinterWebView* backend)
    : m_backend(backend)
{
}

void MoonrakerDeviceController::refresh()
{
    if (m_backend != nullptr)
        m_backend->refresh_layer_info_from_selected_machine();
}

void MoonrakerDeviceController::update_mode()
{
    if (m_backend != nullptr)
        m_backend->update_mode();
}

void MoonrakerDeviceController::sync_model_colors_from_plater()
{
    if (m_backend != nullptr)
        m_backend->sync_model_colors_from_plater();
}

void MoonrakerDeviceController::sync_loaded_tool_filaments(MachineObject* obj, std::function<void()> on_done)
{
    if (m_backend != nullptr)
        m_backend->sync_loaded_tool_filaments(obj, std::move(on_done));
    else if (on_done)
        on_done();
}

void MoonrakerDeviceController::fetch_filament_selections(MachineObject* obj, std::function<void(bool ok)> on_done)
{
    if (m_backend != nullptr)
        m_backend->fetch_filament_selections(obj, std::move(on_done));
    else if (on_done)
        on_done(false);
}

bool MoonrakerDeviceController::get_loaded_tool_filament(int tool_0based, wxColour* color_out, wxString* material_out) const
{
    return m_backend != nullptr && m_backend->get_loaded_tool_filament(tool_0based, color_out, material_out);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
