#ifndef slic3r_GUI_StartPrintDialog_hpp_
#define slic3r_GUI_StartPrintDialog_hpp_

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "../GUI_Utils.hpp"
#include "../SelectMachine.hpp"
#include "../Widgets/Button.hpp"
#include "../Widgets/CheckBox.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/Label.hpp"
#include "../Widgets/StaticBox.hpp"

namespace Slic3r { namespace GUI {

class Plater;

/** Filament mapping row: model half (left) -> printer tool half (right). */
class StartPrintFilamentSlot : public wxPanel
{
public:
    using ToolPickHandler = std::function<void(int model_slot_index, wxWindow *anchor)>;

    StartPrintFilamentSlot(wxWindow *parent, int model_slot_index);

    void set_visible(bool visible);
    void set_model_filament(const std::string &type, const wxColour &color);
    void set_mapped_tool(int mapped_tool);
    void update_printer_tool(const wxColour &color, const wxString &material, int tool_1based);
    void bind_tool_pick_handler(ToolPickHandler handler);

    int model_slot_index() const { return m_model_slot_index; }
    int get_mapped_tool() const { return m_mapped_tool; }

private:
    void style_half(wxPanel *half, wxStaticText *tag, wxStaticText *type_label,
                    const wxColour &bg, const wxString &tag_text, const wxString &type_text);
    void on_printer_half_clicked(wxMouseEvent &event);

    int            m_model_slot_index{0};
    int            m_mapped_tool{1};
    ToolPickHandler m_pick_handler;

    StaticBox     *m_row_card{nullptr};
    wxPanel       *m_model_half{nullptr};
    wxPanel       *m_printer_half{nullptr};
    wxStaticText  *m_model_type{nullptr};
    wxStaticBitmap *m_printer_filament_icon{nullptr};
    wxStaticText  *m_printer_tag{nullptr};
    wxStaticText  *m_printer_type{nullptr};
};

/** Pre-slice print confirmation dialog (model, filament map, printer, options). */
class StartPrintDialog : public DPIDialog
{
public:
    explicit StartPrintDialog(wxWindow *parent);

    void prepare(int print_plate_idx);

    int ShowModal() override;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build_ui();
    void bind_events();
    void refresh_from_plate();
    void refresh_printer_list();
    void update_printer_status();
    void update_start_button_state();
    void refresh_filament_printer_sides();
    void show_tool_picker_for_slot(int model_slot, wxWindow *anchor);

    MachineObject *selected_machine() const;
    std::string    selected_machine_id() const;

    void on_refresh_printers(wxCommandEvent &event);
    void on_printer_changed(wxCommandEvent &event);
    void on_cancel(wxCommandEvent &event);
    void on_start_print(wxCommandEvent &event);
    void on_timer(wxTimerEvent &event);
    void reset_print_options();

    Plater *m_plater{nullptr};
    int     m_print_plate_idx{0};

    ThumbnailPanel *m_thumbnail_panel{nullptr};
    wxStaticText   *m_thumbnail_placeholder{nullptr};
    wxStaticText   *m_task_label{nullptr};
    wxStaticText   *m_task_name_label{nullptr};
    wxStaticText   *m_time_label{nullptr};
    wxStaticText   *m_weight_label{nullptr};
    wxStaticText   *m_target_printer_label{nullptr};
    StaticBox      *m_preview_card{nullptr};

    std::array<StartPrintFilamentSlot *, 4> m_filament_slots{};
    wxStaticText                           *m_filament_hint{nullptr};

    ComboBox     *m_printer_combo{nullptr};
    Button       *m_refresh_button{nullptr};
    wxStaticText *m_printer_status{nullptr};
    std::array<StaticBox *, 4> m_printer_tool_swatches{};

    CheckBox *m_bed_leveling{nullptr};
    CheckBox *m_timelapse{nullptr};
    CheckBox *m_flow_calibration{nullptr};

    Button *m_cancel_button{nullptr};
    Button *m_start_button{nullptr};

    std::vector<std::string> m_printer_ids;
    wxTimer                  m_refresh_timer;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_StartPrintDialog_hpp_
