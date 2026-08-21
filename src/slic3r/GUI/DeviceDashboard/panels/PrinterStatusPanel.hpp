#ifndef slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_
#define slic3r_GUI_DeviceDashboard_panels_PrinterStatusPanel_hpp_

#include "../DeviceDashboardState.hpp"

#include <array>
#include <functional>

#include <wx/panel.h>

class wxStaticText;
class wxStaticBitmap;
class wxTextCtrl;
class StaticBox;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class PrintSpeedPopup;
class FanSpeedPopup;

class PrinterStatusPanel : public wxPanel
{
public:
    using ToolSelectHandler = std::function<void(int tool_index)>;
    using NozzleTempHandler = std::function<void(int tool_index, int temperature)>;
    using FanSpeedHandler   = std::function<void(int tool_index, int percent)>;
    using BedTempHandler    = std::function<void(int temperature)>;
    using PrintSpeedHandler = std::function<void(int percent)>;

    explicit PrinterStatusPanel(wxWindow *parent);

    void apply_state(const std::array<ToolState, MaxDashboardTools> &tools, const BedState &bed,
                     int print_speed_percent = 100);
    void set_active_tool(int tool_index);

    void set_tool_select_handler(ToolSelectHandler handler);
    void set_nozzle_temp_handler(NozzleTempHandler handler);
    void set_fan_speed_handler(FanSpeedHandler handler);
    void set_bed_temp_handler(BedTempHandler handler);
    void set_print_speed_handler(PrintSpeedHandler handler);

    struct TempView {
        wxStaticBitmap *icon{nullptr};
        wxStaticText *  temp_current{nullptr};
        wxStaticText *  temp_slash{nullptr};
        wxStaticText *  temp_target{nullptr};
        wxTextCtrl *    temp_input{nullptr};
        wxStaticText *  temp_unit{nullptr};
        wxWindow *      temp_target_hit{nullptr};
        bool            temp_editing{false};
        bool            temp_available{false};
        int             temp_last_target{0};
    };

private:
    static wxString temp_slot_text(bool available, double value);
    void begin_target_edit(int tool_index);
    void end_target_edit(int tool_index, bool commit);
    void cancel_all_temp_edits();
    void bind_temp_edit(TempView &view, int tool_index);
    void open_fan_popup();
    void open_speed_popup();

    std::array<TempView, MaxDashboardTools> m_tools;
    TempView m_bed;
    wxWindow *m_fan_cell{nullptr};
    wxWindow *m_speed_cell{nullptr};
    PrintSpeedPopup *m_speed_popup{nullptr};
    FanSpeedPopup *m_fan_popup{nullptr};
    std::array<int, MaxDashboardTools> m_fan_percent{0, 0, 0, 0};
    int m_print_speed_percent{100};
    int m_active_tool{0};

    ToolSelectHandler m_tool_select_handler;
    NozzleTempHandler m_nozzle_temp_handler;
    FanSpeedHandler   m_fan_speed_handler;
    BedTempHandler    m_bed_temp_handler;
    PrintSpeedHandler m_print_speed_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif
