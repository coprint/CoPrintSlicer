#ifndef slic3r_GUI_StartPrintDialog_hpp_
#define slic3r_GUI_StartPrintDialog_hpp_

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/colour.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/timer.h>

#include "../GUI_Utils.hpp"
#include "../SelectMachine.hpp"
#include "../Widgets/Button.hpp"
#include "../Widgets/CheckBox.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/Label.hpp"
#include "../Widgets/StaticBox.hpp"
#include "../Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

class Plater;

struct PrinterStorageFilament {
    std::string type;
    wxColour    color;
};

struct PrinterStoragePrintRequest {
    std::string file_path;
    std::string machine_id;
    wxString    display_name;
    wxString    time_text;
    wxString    weight_text;
    wxString    printer_label;
    wxImage     thumbnail;
    std::vector<PrinterStorageFilament> filaments;
};

class PrinterToolSwatch;

/** Filament mapping column: model color on top, printer tool below. */
class StartPrintFilamentSlot : public wxPanel
{
public:
    using ToolPickHandler = std::function<void(int model_slot_index, wxWindow *anchor)>;

    StartPrintFilamentSlot(wxWindow *parent, int model_slot_index);

    void set_visible(bool visible);
    void set_model_slot_index(int model_slot_index);
    void set_model_filament(const std::string &type, const wxColour &color);
    void set_mapped_tool(int mapped_tool);
    void update_printer_tool(const wxColour &color, bool has_filament, int tool_1based);
    void set_interactive(bool interactive);
    void set_input_enabled(bool enabled);
    void bind_tool_pick_handler(ToolPickHandler handler);

    int      model_slot_index() const { return m_model_slot_index; }
    int      get_mapped_tool() const { return m_mapped_tool; }
    wxColour model_color() const { return m_model_color; }
    wxString model_type() const { return m_model_type_name; }

private:
    void style_block(wxPanel *block, const wxColour &bg, const wxString &text);
    void on_printer_half_clicked(wxMouseEvent &event);

    int            m_model_slot_index{0};
    int            m_mapped_tool{1};
    bool           m_interactive{false};
    bool           m_input_enabled{true};
    wxColour       m_model_color;
    wxString       m_model_type_name;
    ToolPickHandler m_pick_handler;

    wxPanel      *m_model_half{nullptr};
    wxPanel      *m_printer_half{nullptr};
};

/** Pre-slice print confirmation dialog (model, filament map, printer, options). */
class StartPrintDialog : public DPIDialog
{
public:
    explicit StartPrintDialog(wxWindow *parent);

    void prepare(int print_plate_idx);
    void prepare_from_storage(PrinterStoragePrintRequest request);
    std::string print_target_dev_id() const { return m_print_target_dev_id; }

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
    void update_filament_mapping_hint();
    bool has_unloaded_mapping() const;
    void assign_tools_by_color();
    void sync_filaments_then_map(bool remap);
    void clear_stale_printer_filament_ui();
    void show_tool_picker_for_slot(int model_slot, wxWindow *anchor);

    MachineObject *selected_machine() const;
    std::string    selected_machine_id() const;

    void on_refresh_printers(wxCommandEvent &event);
    void on_printer_changed(wxCommandEvent &event);
    void on_cancel(wxCommandEvent &event);
    void on_start_print(wxCommandEvent &event);
    void on_timer(wxTimerEvent &event);
    void on_task_name_edit(wxCommandEvent &event);
    void on_task_name_enter();
    void reset_print_options();
    void apply_storage_preview();
    void apply_storage_locks();
    void fill_storage_filament_slots();
    void start_print_job();
    std::string tool_map_script() const;
    std::string print_state_script() const;
    void set_sending_ui(bool sending);
    void set_send_status(const wxString &message);
    void begin_device_countdown(const std::string &dev_id);
    void on_countdown_tick(wxTimerEvent &event);
    void open_device_page_and_close();

    Plater *m_plater{nullptr};
    int     m_print_plate_idx{0};
    bool    m_storage_mode{false};
    std::string m_storage_file_path;
    std::string m_locked_machine_id;
    PrinterStoragePrintRequest m_storage_request;

    ThumbnailPanel *m_thumbnail_panel{nullptr};
    wxStaticText   *m_thumbnail_placeholder{nullptr};
    wxSimplebook   *m_task_name_switch_panel{nullptr};
    wxPanel        *m_task_name_normal_panel{nullptr};
    wxStaticText   *m_task_name_label{nullptr};
    Button         *m_task_name_edit_button{nullptr};
    TextInput      *m_task_name_input{nullptr};
    wxString        m_current_task_name;
    bool            m_is_rename_mode{false};
    wxStaticText   *m_time_label{nullptr};
    wxStaticText   *m_weight_label{nullptr};
    wxStaticText   *m_target_printer_label{nullptr};
    StaticBox      *m_preview_card{nullptr};

    std::array<StartPrintFilamentSlot *, 4> m_filament_slots{};
    wxStaticText                           *m_filament_hint{nullptr};

    ComboBox     *m_printer_combo{nullptr};
    Button       *m_refresh_button{nullptr};
    wxStaticText *m_printer_status{nullptr};
    std::array<PrinterToolSwatch *, 4> m_printer_tool_swatches{};

    wxPanel  *m_options_section{nullptr};
    CheckBox *m_bed_leveling{nullptr};
    CheckBox *m_timelapse{nullptr};
    CheckBox *m_flow_calibration{nullptr};

    Button *m_cancel_button{nullptr};
    Button *m_start_button{nullptr};
    wxPanel      *m_send_status_host{nullptr};
    wxStaticText *m_send_status{nullptr};

    std::vector<std::string> m_printer_ids;
    wxTimer                  m_refresh_timer;
    wxTimer                  m_countdown_timer;
    int                      m_countdown_left{0};
    std::string              m_print_target_dev_id;
    bool                     m_user_mapped_tools{false};
    bool                     m_filament_sync_done{false};
    unsigned                 m_filament_sync_generation{0};
    bool                     m_sending{false};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_StartPrintDialog_hpp_
