#ifndef slic3r_GUI_PrinterWebView_hpp_
#define slic3r_GUI_PrinterWebView_hpp_

#include <array>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>

#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <wx/webrequest.h>
#include <wx/webview.h>
#include "Widgets/WebView.hpp"
#include "DeviceDashboard/DeviceCommandService.hpp"
#include "DeviceDashboard/DeviceStateStore.hpp"
#include "DeviceDashboard/DeviceDashboardPage.hpp"
class wxStaticBitmap;
class wxStaticText;
class wxPopupTransientWindow;
class wxGauge;
class StaticBox;
class Button;
class ProgressBar;
namespace Slic3r {
struct BBLocalMachine;
class MachineObject;

namespace GUI {

namespace DeviceDashboard {
struct FilamentSelection;
class PrinterOfflineOverlay;
}

class CloudTaskManagerPage;
enum class PrinterWebViewTab {
    Status,
    Storage,
    PrintModels,
    Update,
    Assistant
};

class PrinterWebView : public wxPanel
{
    friend class PrinterWebViewHandler;

public:
    PrinterWebView(wxWindow *parent);
    ~PrinterWebView() override;

    void load_url(wxString &url, wxString apikey);
    bool Show(bool show) override;
    void reload();
    void update_mode();
    void toggle_printers_popup();
    void toggle_printers_popup_at(wxWindow* anchor);
    void dismiss_printers_popup();
    void set_embedded_in_monitor(bool embedded);
    void set_sidebar_visible(bool visible);
    DeviceDashboard::DeviceDashboardPage* device_dashboard_page() { return m_dashboard_page; }
    wxPanel* coprint_update_page() const { return m_update_page; }
    CloudTaskManagerPage* coprint_storage_page() const { return m_storage_page; }
    void ensure_coprint_storage_page();
    void set_coprint_storage_mode(bool print_models);
    void attach_media_pages(CloudTaskManagerPage *timelapse, CloudTaskManagerPage *models);
    void mark_printer_connecting(const std::string &dev_id);
    bool is_printer_connecting(const MachineObject *machine) const;

    enum class DeviceSessionUi {
        None,
        Connecting,
        Failed
    };
    void set_device_session_ui_handler(std::function<void(DeviceSessionUi, const wxString &, const wxString &)> handler);
    void acknowledge_device_connect_failure();
    void prompt_ip_connect();
    void reset_placeholder_selections();
    void rebuild_printers_popup();
    void rebuild_sidebar_printer_list();
    void show_sidebar_root_view();
    void show_sidebar_printers_view();
    void show_sidebar_add_printer_view();
    void select_tab(PrinterWebViewTab tab);
    void update_sidebar_selection();
    wxPanel *create_placeholder_page(wxWindow *parent, const wxString &title, const wxString &description);
    wxPanel *create_update_page(wxWindow *parent);
    void set_sidebar_user_avatar(const wxBitmap &avatar_bitmap);
    void begin_moonraker_lan_scan();
    void clear_preview_thumbnail();
    void on_thumbnail_webrequest_state(wxWebRequestEvent &evt);
    void update_preview_thumbnail(const MachineObject *obj, bool has_active_job);
    void refresh_layer_info_from_selected_machine();
    void invalidate_device_cache_and_refresh();
    void refresh_coprint_device_names(std::function<void()> on_updated = {});
    void refresh_update_page_from_selected_machine();
    void UpdateState();
    void OnClose(wxCloseEvent &evt);
    void SendAPIKey();
    void OnError(wxWebViewEvent &evt);
    void OnLoaded(wxWebViewEvent &evt);

    /** Used by Add Printer flow (dialog + LAN discovery). Returns false on failure.
     *  When run_probe is false, the caller already resolved identity off the UI thread.
     *  When run_probe is true, an unreachable host is not inserted. */
    bool finish_add_moonraker_printer(const BBLocalMachine &machine, bool use_ssl, bool run_probe = true);

    /** Probe Moonraker off the UI thread, then insert only if reachable.
     *  on_done always runs on the UI thread (unless this view is destroyed). */
    void add_moonraker_printer_async(const BBLocalMachine &machine, bool use_ssl,
                                     std::function<void(bool ok, const wxString &message)> on_done);

    void sync_model_colors_from_plater();

    /** Starts a background Moonraker filament_selections fetch; updates tool colour cache on the UI thread. */
    void sync_loaded_tool_filaments(MachineObject *obj, std::function<void()> on_done = {});
    void fetch_filament_selections(MachineObject *obj, std::function<void(bool ok)> on_done);
    void reset_loaded_tool_filaments();

    /** Cached loaded tool colour/material from Moonraker DB (after sync or device refresh). */
    bool get_loaded_tool_filament(int tool_0based, wxColour *color_out, wxString *material_out) const;
    void apply_notify_filament_changed(const std::string &dev_id, const std::string &payload);

    void handle_dashboard_command(const DeviceDashboard::DeviceCommand &command);
    void toggle_camera_timelapse();
    wxPanel * coprint_status_host() const { return m_status_page; }
    wxPanel * content_host() const { return m_content_host; }
    void show_add_printer_dialog();
    wxString sidebar_display_name_for(const MachineObject *machine) const;
    bool edit_sidebar_printer_name(MachineObject *machine);
    void show_printer_card_actions_menu(wxWindow *anchor, MachineObject *machine);

private:
    void apply_filament_tool_selection(int tool_index);
    void refresh_filament_preview_from_selected_machine();
    void apply_filament_preview_fallback();
    void reset_filament_cache_and_ui();
    void apply_loaded_filament_cache(const std::array<wxColour, 4> &colors,
                                     const std::array<wxString, 4> &materials,
                                     const std::array<wxString, 4> &brands,
                                     const std::array<std::string, 4> &item_json,
                                     const std::array<bool, 4> &has_color);
    void apply_filament_preview_rows(const std::array<wxColour, 4> &model_colors,
                                     const std::array<wxString, 4> &materials,
                                     const std::array<wxString, 4> &weights,
                                     const std::array<int, 4> &assigned_tools,
                                     const std::array<wxColour, 4> &assigned_colors);
    void update_dashboard_filament_state(const std::array<wxColour, 4> &model_colors,
                                         const std::array<wxString, 4> &materials,
                                         const std::array<wxString, 4> &weights,
                                         const std::array<int, 4> &assigned_tools,
                                         const std::array<wxColour, 4> &assigned_colors);
    void set_filament_assigned_tool(int model_slot_index, int ui_tool, bool send_mapping_command);
    void send_tool_map_command(int model_slot_index, int ui_tool);
    bool send_tool_select_command(int tool_index);
    bool send_print_control_command(bool stop_print, bool resume = false);
    bool send_klipper_gcode_script(const std::string& script);
    bool show_filament_material_dialog(bool start_load_after_save, const wxPoint& anchor_screen_pos = wxDefaultPosition);
    void prompt_and_save_filament_selection_then_load();
    void save_filament_selection_to_moonraker(int ui_tool, const DeviceDashboard::FilamentSelection &selection);
    void clear_filament_selection_from_moonraker(int ui_tool);
    void queue_coprint_filament_write(std::string body);
    void start_coprint_filament_write();
    void refresh_moonraker_status_from_selected_machine();
    void refresh_dashboard_panels(MachineObject *obj);
    void update_dashboard_connecting_overlay(MachineObject *obj);
    void retry_selected_printer_connection();
    void sync_media_page_connection_state(bool printer_ready, bool show_offline, const wxString &printer_name);
    void apply_klippy_connection_ui(MachineObject *obj);
    void refresh_connected_printer_header(MachineObject *obj);
    void refresh_printer_info_labels(MachineObject *obj);
    void refresh_camera_stream(MachineObject *obj);
    void reset_dashboard_snapshot();
    void abort_preview_thumbnail();
    void begin_filament_snapshot_fetch(MachineObject *obj);
    void reveal_dashboard_if_ready(MachineObject *obj);
    bool is_dashboard_snapshot_ready() const;
    void start_camera_stream();
    void stop_camera_stream();
    void handle_camera_webview_title(const wxString &title);
    void apply_printer_status_tool_selection(int tool_index);
    void begin_dashboard_homing();
    void clear_dashboard_homing();
    void update_dashboard_homing(bool gcode_busy, bool got_idle_timeout);
    void apply_nozzle_target_temperature(int extruder_index, int temperature);
    void apply_bed_target_temperature(int temperature);
    void prompt_ps_target_temperature(bool is_bed, int extruder_index);
    void show_toolhead_temperature_dialog(int active_extruder_index);
    void show_bed_temperature_dialog();
    void show_toolhead_fan_dialog(int active_extruder_index);
    bool send_toolhead_fan_speed_command(int tool_index, int fan_percent);
    void show_filament_load_wizard();
    void show_filament_busy_dialog(bool is_load);
    bool confirm_forget_printer();
    void post_coprint_device_name(MachineObject *machine, const std::string &device_name);
    void forget_local_printer(MachineObject *machine);
    void ensure_camera_webview_created();
    void ensure_storage_page_created();
    void update_sidebar_connect_attempt_state();
    void begin_sidebar_connect_attempt(const std::string &dev_id);
    void clear_sidebar_connect_attempt();
    struct SidebarItem {
        PrinterWebViewTab tab;
        wxPanel *panel{ nullptr };
        wxPanel *active_strip{ nullptr };
        wxStaticText *label{ nullptr };
        wxStaticText *chevron{ nullptr };
    };

    wxString m_apikey;
    bool m_apikey_sent{ false };
    wxPanel *m_assistant_page{ nullptr };
    wxWebView *m_browser{ nullptr };
    wxPanel *m_connected_printer_panel{ nullptr };
    wxStaticText *m_connected_printer_status_label{ nullptr };
    wxStaticText *m_connected_printer_logout_label{ nullptr };
    bool m_has_active_printer_connection{ false };
    wxTimer *m_layer_refresh_timer{ nullptr };
    wxWindow *m_preview_printers_button{ nullptr };
    wxPanel *m_sidebar_header_panel{ nullptr };
    wxStaticText *m_sidebar_header_back{ nullptr };
    wxStaticText *m_sidebar_header_title{ nullptr };
    wxStaticText *m_sidebar_header_add{ nullptr };
    wxPanel *m_sidebar_user_avatar_panel{ nullptr };
    wxBitmap m_sidebar_user_avatar_bitmap;
    wxPanel *m_auto_connect_scroll_track{ nullptr };
    wxScrolledWindow *m_auto_connect_list_window{ nullptr };
    std::vector<BBLocalMachine> m_discovered_moonraker_printers;
    bool m_lan_scan_in_progress{ false };
    bool m_lan_rescan_requested{ false };
    std::shared_ptr<std::atomic_bool> m_lan_scan_cancel_token;
    wxPanel *m_sidebar_printer_list_panel{ nullptr };
    wxPanel *m_sidebar_printer_list_container{ nullptr };
    wxPanel *m_sidebar_printer_scroll_track{ nullptr };
    wxBoxSizer *m_sidebar_printer_list_sizer{ nullptr };
    wxString m_sidebar_printer_list_signature;
    wxPanel *m_sidebar_add_printer_panel{ nullptr };
    wxPanel *m_sidebar_root_panel{ nullptr };
    wxBoxSizer *m_sidebar_root_sizer{ nullptr };
    int m_sidebar_add_tab_index{ 1 };
    wxStaticBitmap *m_preview_thumbnail{ nullptr };
    wxWebView *m_camera_webview{ nullptr };
    wxPanel *m_camera_webview_host{ nullptr };
    bool m_camera_webview_initialized{ false };
    bool m_camera_stream_requested{ false };
    wxStaticText *m_printer_name_value{ nullptr };
    wxStaticText *m_printer_model_value{ nullptr };
    wxStaticText *m_printer_serial_value{ nullptr };
    wxStaticText *m_printer_firmware_value{ nullptr };
    wxStaticBitmap *m_printer_photo_bitmap{ nullptr };
    std::string m_camera_machine_id;
    wxString m_camera_stream_url;
    wxString m_preview_thumbnail_url;
    wxString m_pending_thumbnail_url;
    std::string m_thumbnail_request_machine_id;
    std::string m_pending_thumbnail_machine_id;
    bool m_clear_thumbnail_after_cancel{false};
    wxPopupTransientWindow *m_printers_popup{ nullptr };
    StaticBox *m_printers_popup_panel{ nullptr };
    int m_printers_popup_max_width{ 0 };
    int m_selected_extruder_index{ 0 };
    PrinterWebViewTab m_selected_tab{ PrinterWebViewTab::Status };
    std::vector<SidebarItem> m_sidebar_items;
    wxPanel *m_storage_placeholder{ nullptr };
    int m_selected_filament_tool{ 0 };
    std::array<int, 4> m_filament_assigned_tool_mapping{ 1, 2, 3, 4 };
    std::array<wxColour, 4> m_filament_loaded_tool_colors;
    std::array<wxString, 4> m_filament_loaded_tool_materials;
    std::array<wxString, 4> m_filament_loaded_tool_brands;
    std::array<std::string, 4> m_filament_tool_item_json;
    std::array<bool, 4> m_filament_tool_has_color{};
    unsigned m_filament_fetch_generation{ 0 };
    bool m_filament_db_write_in_progress{ false };
    std::vector<std::string> m_pending_filament_posts;
    // Colors synced from the Plater at upload time — used as fallback when
    // no printer metadata is available (e.g. printer is idle after upload).
    std::array<wxColour, 4> m_plater_synced_colors;
    std::array<wxString, 4> m_plater_synced_materials;
    bool m_has_plater_synced_colors{ false };
    std::array<double, 4> m_moonraker_nozzle_current{ 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 4> m_moonraker_nozzle_target{ 0.0, 0.0, 0.0, 0.0 };
    std::array<int, 4> m_moonraker_fan_percent{ 0, 0, 0, 0 };
    std::array<bool, 4> m_moonraker_fan_available{ false, false, false, false };
    double m_moonraker_bed_current{ 0.0 };
    double m_moonraker_bed_target{ 0.0 };
    int m_moonraker_available_tool_count{ 0 };
    bool m_has_moonraker_status{ false };
    std::string m_klippy_state;
    bool m_has_moonraker_print_status{ false };
    DeviceDashboard::PrintJobState m_moonraker_print_job;
    bool m_moonraker_status_fetch_in_progress{ false };
    std::string m_moonraker_status_machine_id;
    bool m_dashboard_snapshot_ready{ false };
    bool m_filament_snapshot_ready{ false };
    bool m_filament_snapshot_fetch_in_progress{ false };
    wxString m_filament_preview_fetch_key;
    bool m_filament_preview_fetch_in_progress{ false };
    wxPanel *m_preview_menu_panel{ nullptr };
    wxPanel *m_content_host{ nullptr };
    wxPanel *m_status_page{ nullptr };
    DeviceDashboard::DeviceDashboardPage* m_dashboard_page{nullptr};
    CloudTaskManagerPage *m_storage_page{ nullptr };
    CloudTaskManagerPage *m_media_timelapse_page{ nullptr };
    CloudTaskManagerPage *m_media_models_page{ nullptr };
    DeviceDashboard::PrinterOfflineOverlay *m_update_offline_overlay{nullptr};
    bool m_embedded_in_monitor{false};
    enum class DeviceWarnAck { None, KlippyFault, Unreachable };
    DeviceWarnAck m_device_warn_ack{DeviceWarnAck::None};
    std::function<void(DeviceSessionUi, const wxString &, const wxString &)> m_device_session_ui;
    wxImage m_thumbnail_image;
    wxWebRequest m_thumbnail_web_request;
    wxStaticText *m_update_connection_badge{ nullptr };
    Button *m_update_firmware_button{ nullptr };
    wxStaticText *m_update_header_title{ nullptr };
    wxStaticText *m_update_model_value{ nullptr };
    wxPanel *m_update_page{ nullptr };
    wxStaticText *m_update_percent_value{ nullptr };
    wxStaticBitmap *m_update_printer_bitmap{ nullptr };
    ProgressBar *m_update_progress_gauge{ nullptr };
    wxTimer *m_update_progress_timer{ nullptr };
    bool m_update_sim_active{ false };
    int m_update_sim_percent{ 0 };
    wxStaticText *m_update_release_note_link{ nullptr };
    wxStaticText *m_update_serial_value{ nullptr };
    wxStaticText *m_update_status_value{ nullptr };
    wxStaticText *m_update_version_value{ nullptr };
    DeviceDashboard::DeviceStateStore m_dashboard_state_store;
    double m_axis_move_step{ 1.0 };
    bool m_homing_in_progress{ false };
    bool m_homing_saw_busy{ false };
    wxLongLong m_homing_started_ms{ 0 };
    int m_zoomFactor{ 100 };
    std::shared_ptr<int> m_lifetime_token{ std::make_shared<int>(1) };
    bool m_destroying{ false };
    int m_coprint_names_fetch_gen{ 0 };
    std::string m_last_refresh_machine_id;
    std::string m_device_serial;
    std::string m_device_os_version;
    int m_refresh_tick_counter{ 0 };
    wxLongLong m_dashboard_connect_started_ms{ 0 };

    // Sidebar printer-card connection attempt: Connecting (yellow) → Connected / Not connected (red after 15s).
    enum class SidebarConnectPhase { None, Connecting, Failed };
    std::string m_sidebar_connect_dev_id;
    wxLongLong m_sidebar_connect_started_ms{ 0 };
    SidebarConnectPhase m_sidebar_connect_phase{ SidebarConnectPhase::None };
};

} // namespace GUI
} // namespace Slic3r

#endif
