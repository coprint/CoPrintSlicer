#include "DashboardStateAdapter.hpp"

#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"

#include <algorithm>
#include <cmath>

#include <wx/filename.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

wxString display_file_name(const MachineObject* machine)
{
    if (machine == nullptr)
        return wxString();

    auto clean_name = [](wxString name) {
        name.Trim(true);
        name.Trim(false);
        if (name.empty() || name == "N/A")
            return wxString();

        name.Replace("\\", "/");
        if (name.StartsWith("file://"))
            name = name.Mid(7);

        const wxString lower = name.Lower();
        const int gcodes_pos = lower.Find("/gcodes/");
        if (gcodes_pos != wxNOT_FOUND)
            name = name.Mid(gcodes_pos + 8);
        else if (lower.StartsWith("gcodes/"))
            name = name.Mid(7);
        else if (wxFileName(name).IsAbsolute())
            name = wxFileName(name).GetFullName();

        while (name.StartsWith("/"))
            name = name.Mid(1);
        return name;
    };

    if (!machine->subtask_name.empty())
        return clean_name(from_u8(machine->subtask_name));
    if (machine->subtask_ != nullptr && !machine->subtask_->task_name.empty())
        return clean_name(from_u8(machine->subtask_->task_name));
    if (machine->slice_info != nullptr && !machine->slice_info->gcode_name.empty())
        return clean_name(from_u8(machine->slice_info->gcode_name));
    if (machine->slice_info != nullptr && !machine->slice_info->title.empty())
        return clean_name(from_u8(machine->slice_info->title));
    if (!machine->m_gcode_file.empty())
        return clean_name(from_u8(machine->m_gcode_file));
    if (machine->model_task != nullptr && !machine->model_task->profile_name.empty())
        return clean_name(from_u8(machine->model_task->profile_name));
    if (machine->model_task != nullptr && !machine->model_task->model_name.empty())
        return clean_name(from_u8(machine->model_task->model_name));

    return wxString();
}

int predicted_seconds(const MachineObject* machine)
{
    if (machine == nullptr)
        return -1;
    if (machine->slice_info != nullptr && machine->slice_info->prediction > 0)
        return machine->slice_info->prediction;
    if (machine->subtask_ != nullptr && machine->subtask_->slice_info.prediction > 0)
        return machine->subtask_->slice_info.prediction;
    return -1;
}

std::string host_without_port(std::string value)
{
    const auto scheme = value.find("://");
    if (scheme != std::string::npos)
        value = value.substr(scheme + 3);
    const auto slash = value.find('/');
    if (slash != std::string::npos)
        value = value.substr(0, slash);
    if (std::count(value.begin(), value.end(), ':') == 1) {
        const auto colon = value.rfind(':');
        if (colon != std::string::npos)
            value = value.substr(0, colon);
    }
    return value;
}

} // namespace

DeviceDashboardState DashboardStateAdapter::from_machine(MachineObject* machine)
{
    DeviceDashboardState state;
    apply_default_tools(state);

    if (machine == nullptr) {
        state.connection.status = ConnectionStatus::Offline;
        state.connection.can_send_commands = false;
        return state;
    }

    state.printer.id = machine->get_dev_id();
    state.printer.name = machine->get_dev_name();
    state.printer.ip = host_without_port(machine->get_dev_ip());
    state.printer.type = machine->printer_type;
    state.printer.firmware_version = machine->get_ota_version();

    state.connection.status = machine->is_online() ? ConnectionStatus::Online : ConnectionStatus::Offline;
    state.connection.can_send_commands = machine->is_online();

    if (auto* extruders = machine->GetExtderSystem()) {
        const int extruder_count = std::max(0, extruders->GetTotalExtderCount());
        state.movement.available_tool_count = std::clamp(extruder_count, 1, MaxDashboardTools);
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (i >= extruder_count)
                continue;

            ToolState& tool = state.tools[i];
            const double cur = static_cast<double>(extruders->GetNozzleTempCurrent(i));
            const double tgt = static_cast<double>(extruders->GetNozzleTempTarget(i));
            if (cur > 0.0 || tgt > 0.0) {
                tool.available = true;
                tool.nozzle.available = true;
                tool.nozzle.current = cur;
                tool.nozzle.target = tgt;
                state.filament.tools[i].nozzle = tool.nozzle;
                state.filament.tools[i].available = true;
            }

            const double nozzle_fan = static_cast<double>(extruders->GetNozzleFanSpeed(i));
            if (nozzle_fan >= 0.0) {
                tool.fan.available = true;
                tool.fan.percent = std::clamp(static_cast<int>(std::round(nozzle_fan * 100.0)), 0, 100);
                state.filament.tools[i].fan = tool.fan;
            }
        }
    }

    if (auto* bed = machine->GetBed()) {
        const double cur = static_cast<double>(bed->GetBedTemp());
        const double tgt = static_cast<double>(bed->GetBedTempTarget());
        if (cur > 0.0 || tgt > 0.0) {
            state.bed.temperature.available = true;
            state.bed.temperature.current = cur;
            state.bed.temperature.target = tgt;
        }
    }

    if (auto* fan = machine->GetFan()) {
        bool any_per_tool_fan = false;
        for (int i = 0; i < MaxDashboardTools; ++i)
            any_per_tool_fan = any_per_tool_fan || state.tools[i].fan.available;

        if (!any_per_tool_fan) {
            bool fan_available = false;
            int fan_percent = 0;

            const auto air_duct = fan->GetAirDuctData();
            for (const auto& part : air_duct.parts) {
                if (part.id == static_cast<int>(AIR_FUN::FAN_COOLING_0_AIRDOOR)) {
                    fan_available = true;
                    fan_percent = std::clamp(static_cast<int>(std::round(part.state / 10.0)), 0, 100);
                    break;
                }
            }

            if (!fan_available) {
                const int raw_speed = static_cast<int>(std::round(fan->GetCoolingFanSpeed() / 25.5f));
                if (fan->GetFanGear() != 0 || fan->GetCoolingFanSpeed() > 0) {
                    fan_available = true;
                    fan_percent = std::clamp(raw_speed, 0, 100);
                }
            }

            if (fan_available) {
                state.tools[0].fan.available = true;
                state.tools[0].fan.percent = fan_percent;
                state.filament.tools[0].fan = state.tools[0].fan;
            }
        }
    }

    state.print_job.has_active_job = machine->is_in_printing();
    if (machine->is_in_printing_pause())
        state.print_job.state = PrintCommandState::Paused;
    else if (state.print_job.has_active_job)
        state.print_job.state = PrintCommandState::Printing;
    if (state.print_job.has_active_job) {
        state.print_job.file_name = display_file_name(machine);
        if (machine->slice_info != nullptr)
            state.print_job.thumbnail_url = from_u8(machine->slice_info->thumbnail_url);

        state.print_job.progress_percent = std::clamp(machine->mc_print_percent, 0, 100);
        state.print_job.current_layer = machine->curr_layer;
        state.print_job.total_layers = machine->total_layers;
        state.print_job.remaining_seconds = machine->mc_left_time > 0 ? machine->mc_left_time : -1;
        state.print_job.elapsed_seconds = predicted_seconds(machine);
        if (state.print_job.remaining_seconds <= 0 && state.print_job.elapsed_seconds > 0 &&
            state.print_job.progress_percent > 0 && state.print_job.progress_percent < 100) {
            state.print_job.remaining_seconds = std::max(0, static_cast<int>(std::round(
                state.print_job.elapsed_seconds * (100.0 - state.print_job.progress_percent) / 100.0)));
        }
    }

    switch (machine->GetPrintingSpeedLevel()) {
    case SPEED_LEVEL_SILENCE:
        state.movement.print_speed_percent = 50;
        break;
    case SPEED_LEVEL_NORMAL:
        state.movement.print_speed_percent = 100;
        break;
    case SPEED_LEVEL_RAPID:
        state.movement.print_speed_percent = 125;
        break;
    case SPEED_LEVEL_RAMPAGE:
        state.movement.print_speed_percent = 150;
        break;
    default:
        state.movement.print_speed_percent = machine->printing_speed_mag > 0 ? machine->printing_speed_mag : 100;
        break;
    }

    state.movement.can_move = state.connection.can_send_commands &&
        state.print_job.state != PrintCommandState::Printing;
    return state;
}

void DashboardStateAdapter::apply_default_tools(DeviceDashboardState& state)
{
    const std::array<wxColour, MaxDashboardTools> colors{
        wxColour(255, 255, 255),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42)
    };

    for (int i = 0; i < MaxDashboardTools; ++i) {
        state.tools[i].index = i;
        state.tools[i].label = wxString::Format("Tool %d", i + 1);
        state.tools[i].material = wxString::FromUTF8("N/A");
        state.tools[i].color = colors[i];

        state.filament.tools[i] = state.tools[i];
        state.filament.model_colors[i] = colors[i];
        state.filament.model_materials[i] = wxString::FromUTF8("N/A");
        state.filament.model_weights[i] = wxString::FromUTF8("--");
        state.filament.assigned_colors[i] = colors[i];
        state.filament.model_slot_to_tool[i] = i;
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
