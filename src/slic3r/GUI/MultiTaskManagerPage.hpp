#ifndef slic3r_MultiTaskManagerPage_hpp_
#define slic3r_MultiTaskManagerPage_hpp_

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "MultiMachine.hpp"
#include "DeviceManager.hpp"
#include "TaskManager.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/TextInput.hpp"
#include <wx/image.h>
#include <wx/webrequest.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { 
namespace GUI {

namespace DeviceDashboard {
class PrinterOfflineOverlay;
}

#define CLOUD_TASK_ITEM_MAX_WIDTH 1100
#define TASK_ITEM_MAX_WIDTH    900
#define TASK_LEFT_PADDING_LEFT 15
#define TASK_LEFT_PRINTABLE    40
#define TASK_LEFT_PRO_NAME     180
#define TASK_LEFT_DEV_NAME     150
#define TASK_LEFT_PRO_STATE    170
#define TASK_LEFT_PRO_INFO     230
#define TASK_LEFT_SEND_TIME    180

class MultiTaskItem : public DeviceItem
{
public:
    MultiTaskItem(wxWindow* parent, MachineObject* obj, int type);
    ~MultiTaskItem();


    void OnEnterWindow(wxMouseEvent& evt);
    void OnLeaveWindow(wxMouseEvent& evt);
    void OnSelectedDevice(wxCommandEvent& evt);
    void OnLeftDown(wxMouseEvent& evt);
    void OnMove(wxMouseEvent& evt);

    void         paintEvent(wxPaintEvent& evt);
    void         render(wxDC& dc);
    void         doRender(wxDC& dc);
    void         DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top = 0);
    void         set_history_info(TaskStateInfo& info, const wxString& date_text, const wxString& duration_text, const wxString& status_text);
    void         on_thumbnail_request(wxWebRequestEvent& evt);
    void         post_event(wxCommandEvent&& event);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

    bool m_hover{ false };
    wxString get_left_time(int mc_left_time);
    
    ScalableBitmap m_bitmap_check_disable;
    ScalableBitmap m_bitmap_check_off;
    ScalableBitmap m_bitmap_check_on;

    int          m_sending_percent{0};
    int          m_task_type{0}; //0-local 1-cloud
    wxString     m_project_name;
    wxString     m_dev_name;
    wxString     m_history_status;
    wxString     m_history_date;
    wxString     m_history_duration;
    wxString     m_thumbnail_url;
    wxImage      m_thumbnail_image;
    wxWebRequest m_thumbnail_request;
    std::string  m_dev_id;
    TaskStateInfo* task_obj { nullptr };
    std::string  m_job_id;
    //std::string  m_sent_time;

    Button* m_button_resume{ nullptr };
    Button* m_button_cancel{ nullptr };
    Button* m_button_pause{ nullptr };
    Button* m_button_stop{ nullptr };

    void update_info();
    void onPause();
    void onResume();
    void onStop();
    void onCancel();
};

class LocalTaskManagerPage : public wxPanel
{
public:
    LocalTaskManagerPage(wxWindow* parent);
    ~LocalTaskManagerPage() {};

    void update_page();
    void refresh_user_device(bool clear = false);
    bool Show(bool show);
    void cancel_all(wxCommandEvent& evt);
    void msw_rescale();

private:
    SortItem                    m_sort;
    std::map<int, MultiTaskItem*> m_task_items;
    bool                        device_name_big{ true };
    bool                        device_state_big{ true };
    bool                        device_send_time{ true };

    wxPanel* m_main_panel{ nullptr };
    wxBoxSizer* m_main_sizer{ nullptr };
    wxBoxSizer* page_sizer{ nullptr };
    wxBoxSizer* m_sizer_task_list{ nullptr };
    wxScrolledWindow* m_task_list{ nullptr };
    wxStaticText* m_selected_num{ nullptr };

    // table head
    wxPanel* m_table_head_panel{ nullptr };
    wxBoxSizer* m_table_head_sizer{ nullptr };
    CheckBox* m_select_checkbox{ nullptr };
    Button* m_task_name{ nullptr };
    Button* m_printer_name{ nullptr };
    Button* m_status{ nullptr };
    Button* m_info{ nullptr };
    Button* m_send_time{ nullptr };
    Button* m_action{ nullptr };

    // ctrl button for all
    int m_sel_number{0};
    wxPanel* m_ctrl_btn_panel{ nullptr };
    wxBoxSizer* m_btn_sizer{ nullptr };
    Button* btn_stop_all{ nullptr };
    wxStaticText* m_sel_text{ nullptr };

    // tip when no device
    wxStaticText* m_tip_text{ nullptr };
};

struct MoonrakerModelFileView
{
    std::string   path;
    std::string   thumbnail_url;
    std::uint64_t size{ 0 };
    double        modified{ 0.0 };
    double        estimated_time_seconds{ 0.0 };
    double        filament_weight_grams{ 0.0 };
};

class CloudTaskManagerPage : public wxPanel
{
public:
    enum class MediaPresentation {
        Combined,
        TimelapseOnly,
        ModelOnly
    };

    CloudTaskManagerPage(wxWindow* parent);
    CloudTaskManagerPage(wxWindow* parent, MediaPresentation presentation);
    ~CloudTaskManagerPage();

    void update_page();
    void refresh_user_device(bool clear = false);
    void set_media_presentation(MediaPresentation presentation);
    void reload_media_models();
    void ensure_media_models_for_selected_machine();
    void invalidate_media_cache_and_reload();
    void set_allow_moonraker_fetch(bool allow);
    void set_offline_overlay_visible(bool visible, const wxString &printer_name = wxEmptyString);
    void set_offline_retry_handler(std::function<void()> handler);
    std::string utc_time_to_date(std::string utc_time);
    bool Show(bool show);
    void update_page_number();
    void start_timer();
    void on_timer(wxTimerEvent& event);

    void pause_all(wxCommandEvent& evt);
    void resume_all(wxCommandEvent& evt);
    void stop_all(wxCommandEvent& evt);

    void enable_buttons(bool enable);
    void page_num_enter_evt();

    void msw_rescale();

private:
    void set_media_mode(bool timelapse);
    void update_media_mode_tabs();
    void set_timelapse_filter(int filter);
    void update_timelapse_filter_tabs();
    void select_all_timelapse_cards();
    void refresh_moonraker_model_status();
    void render_moonraker_model_files(const std::vector<MoonrakerModelFileView>& files);
    int  model_grid_column_count() const;
    void relayout_model_file_grid();
    void sync_model_grid_overlay(bool reveal = true);
    void load_visible_model_thumbnails();
    void apply_model_file_metadata(const MoonrakerModelFileView& file);

    SortItem                    m_sort;
    bool                        device_name_big{ true };
    bool                        device_state_big{ true };
    bool                        device_send_time{ true };
    MediaPresentation           m_media_presentation{ MediaPresentation::Combined };
    bool                        m_media_timelapse_mode{ false };
    int                         m_timelapse_filter{ 0 };

    /* job_id -> sel */
    std::map <std::string, MultiTaskItem*> m_task_items;

    wxPanel* m_main_panel{ nullptr };
    wxBoxSizer* page_sizer{ nullptr };
    wxBoxSizer* m_sizer_task_list{ nullptr };
    wxBoxSizer* m_main_sizer{ nullptr };
    wxScrolledWindow* m_task_list{ nullptr };
    wxStaticText* m_selected_num{ nullptr };
    wxPanel* m_media_mode_panel{ nullptr };
    Button* m_timelapse_tab{ nullptr };
    Button* m_model_tab{ nullptr };
    Button* m_refresh_tab{ nullptr };
    wxPanel* m_timelapse_panel{ nullptr };
    wxPanel* m_timelapse_top_actions{ nullptr };
    wxScrolledWindow* m_timelapse_grid{ nullptr };
    wxStaticText* m_timelapse_date_range{ nullptr };
    Button* m_timelapse_select_all{ nullptr };
    Button* m_timelapse_select{ nullptr };
    Button* m_timelapse_all_files{ nullptr };
    Button* m_timelapse_year{ nullptr };
    Button* m_timelapse_month{ nullptr };
    wxStaticText* m_model_status_text{ nullptr };
    wxScrolledWindow* m_model_file_grid{ nullptr };
    wxGridSizer* m_model_file_grid_sizer{ nullptr };
    wxWindow* m_model_grid_scroll{ nullptr };
    std::shared_ptr<int> m_model_status_lifetime{ std::make_shared<int>(0) };
    std::map<std::string, wxImage> m_model_thumbnail_cache;
    std::string m_last_model_probe_machine_id;
    bool m_model_probe_in_flight{ false };
    bool m_last_model_probe_ok{ false };
    long long m_last_model_probe_started_ms{ 0 };
    bool m_allow_moonraker_fetch{ true };
    DeviceDashboard::PrinterOfflineOverlay *m_offline_overlay{ nullptr };

    // Flipping pages
    int                         m_current_page{ 0 };
    int                         m_total_page{0};
    int                         m_total_count{ 0 };
    int                         m_count_page_item{ 10 };
    bool                        prev{ false };
    bool                        next{ false };
    Button*                     btn_last_page{ nullptr };
    Button*                     btn_next_page{ nullptr };
    wxStaticText*               st_page_number{ nullptr };
    wxBoxSizer*                 m_flipping_page_sizer{ nullptr };
    wxBoxSizer*                 m_page_sizer{ nullptr };
    wxPanel*                    m_flipping_panel{ nullptr };
    wxTimer*                    m_flipping_timer{ nullptr };
    TextInput*                  m_page_num_input{ nullptr };
    Button*                     m_page_num_enter{ nullptr };

    // table head
    wxPanel*                    m_table_head_panel{ nullptr };
    wxBoxSizer*                 m_table_head_sizer{ nullptr };
    CheckBox*                   m_select_checkbox{ nullptr };
    Button*                     m_task_name{ nullptr };
    Button*                     m_printer_name{ nullptr };
    Button*                     m_status{ nullptr };
    Button*                     m_info{ nullptr };
    Button*                     m_send_time{ nullptr };
    Button*                     m_action{ nullptr };

    // ctrl button for all
    int                         m_sel_number;
    wxPanel*                    m_ctrl_btn_panel{ nullptr };
    wxBoxSizer*                 m_btn_sizer{ nullptr };
    Button*                     btn_pause_all{ nullptr };
    Button*                     btn_continue_all{ nullptr };
    Button*                     btn_stop_all{ nullptr };
    wxStaticText*               m_sel_text{ nullptr };

    // tip when no device
    wxStaticText*               m_tip_text{ nullptr };
    wxStaticText*               m_loading_text{ nullptr };
};

void stop_moonraker_model_file_probes();

} // namespace GUI
} // namespace Slic3r

#endif
