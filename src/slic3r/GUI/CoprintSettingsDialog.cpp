#include "CoprintSettingsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "MainFrame.hpp"
#include "UnsavedChangesDialog.hpp"
#include "NetworkTestDialog.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "ICloudServiceAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <algorithm>
#include <cstdlib>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/intl.h>
#include <wx/language.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/textctrl.h>
#include <wx/translation.h>
#include <wx/valtext.h>

namespace Slic3r {
namespace GUI {

namespace {

const wxColour kLabelColour("#363636");
const wxColour kHintColour("#ACACAC");

wxStaticText *make_label(wxWindow *parent, const wxString &text, const wxFont &font, const wxColour &colour)
{
    auto *label = new wxStaticText(parent, wxID_ANY, text);
    label->SetFont(font);
    label->SetForegroundColour(colour);
    return label;
}

} // namespace

CoprintSettingsDialog::CoprintSettingsDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Preferences"), wxDefaultPosition, wxDefaultSize,
#ifdef __APPLE__
               wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxSTAY_ON_TOP
#else
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
#endif
      )
{
    SetBackgroundColour(*wxWHITE);
    SetName("coprint_preferences");
    build();
    SetMinSize(FromDIP(wxSize(560, 520)));
    SetSize(FromDIP(wxSize(620, 640)));
    if (GetParent() != nullptr)
        CentreOnParent();
    else
        Centre();
}

void CoprintSettingsDialog::build()
{
    AppConfig *config = wxGetApp().app_config;
    auto *root = new wxBoxSizer(wxVERTICAL);

    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetScrollRate(0, FromDIP(12));
    m_scroll->SetBackgroundColour(*wxWHITE);
    m_rows = new wxBoxSizer(wxVERTICAL);

    add_section(_L("General Settings"));

    fill_languages();
    m_language->Bind(wxEVT_CHOICE, &CoprintSettingsDialog::on_language, this);

    const std::vector<wxString> region_labels = {
        _L("Asia-Pacific"), _L("China"), _L("Europe"), _L("North America"), _L("Others")};
    const std::vector<std::string> region_values = {
        "Asia-Pacific", "China", "Europe", "North America", "Others"};
    m_region_sel = 0;
    const std::string region = config->get("region");
    for (size_t i = 0; i < region_values.size(); ++i) {
        if (region == region_values[i])
            m_region_sel = static_cast<int>(i);
    }
    m_region = add_choice(_L("Login region"), region_labels, m_region_sel);
    m_region->Bind(wxEVT_CHOICE, &CoprintSettingsDialog::on_region, this);

    const std::vector<wxString> unit_labels = {_L("Metric") + " (mm, g)", _L("Imperial") + " (in, oz)"};
    int unit_sel = 0;
    if (!config->get("use_inches").empty())
        unit_sel = atoi(config->get("use_inches").c_str());
    auto *units = add_choice(_L("Units"), unit_labels, unit_sel);
    units->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        config->set("use_inches", std::to_string(e.GetSelection()));
        config->save();
        e.Skip();
    });

    const std::vector<wxString> flush_labels = {_L("All"), _L("Color"), _L("None")};
    const std::vector<std::string> flush_values = {"all", "color change", "disabled"};
    int flush_sel = 0;
    const std::string flush = config->get("auto_calculate_flush");
    for (size_t i = 0; i < flush_values.size(); ++i) {
        if (flush == flush_values[i])
            flush_sel = static_cast<int>(i);
    }
    auto *flush_choice = add_choice(_L("Auto flush"), flush_labels, flush_sel);
    flush_choice->Bind(wxEVT_CHOICE, [config, flush_values](wxCommandEvent &e) {
        const int sel = e.GetSelection();
        if (sel >= 0 && sel < static_cast<int>(flush_values.size())) {
            config->set("auto_calculate_flush", flush_values[sel]);
            config->save();
        }
        e.Skip();
    });

    auto *single = add_checkbox(_L("Allow only one CoPrintSlicer instance"), config->get_bool("single_instance"));
    single->Bind(wxEVT_CHECKBOX, [config, single](wxCommandEvent &) {
        config->set_bool("single_instance", single->GetValue());
        config->save();
    });

    auto *multi = add_checkbox(_L("Multi device management") + " (" + _L("Restart Required") + ")",
                               config->get_bool("enable_multi_machine"));
    multi->Bind(wxEVT_CHECKBOX, [config, multi](wxCommandEvent &) {
        config->set_bool("enable_multi_machine", multi->GetValue());
        config->save();
    });

#if defined(_WIN32) && COPRINT_DARK_MODE_ENABLED
    auto *dark = add_checkbox(_L("Enable dark mode"), config->get("dark_color_mode") == "1");
    dark->Bind(wxEVT_CHECKBOX, [config, dark](wxCommandEvent &) {
        config->set("dark_color_mode", dark->GetValue() ? "1" : "0");
        config->save();
        wxGetApp().Update_dark_mode_flag();
#ifdef _MSW_DARK_MODE
        wxGetApp().force_colors_update();
        wxGetApp().update_ui_from_settings();
#endif
        if (wxGetApp().plater()) {
            SimpleEvent evt(EVT_GLCANVAS_COLOR_MODE_CHANGED);
            wxPostEvent(wxGetApp().plater(), evt);
        }
    });
#endif
#ifdef __linux__
    auto *win_btns = add_checkbox(_L("Use window buttons on left side") + " (" + _L("Requires restart") + ")",
                                  config->get_bool("window_buttons_on_left"));
    win_btns->Bind(wxEVT_CHECKBOX, [config, win_btns](wxCommandEvent &) {
        config->set_bool("window_buttons_on_left", win_btns->GetValue());
        config->save();
    });
#endif

    add_downloads_row();

    add_section(_L("Project"));

    const std::vector<wxString> page_labels = {_L("Home"), _L("Prepare")};
    int page_sel = 0;
    if (!config->get("default_page").empty())
        page_sel = atoi(config->get("default_page").c_str());
    auto *default_page = add_choice(_L("Default page"), page_labels, page_sel);
    default_page->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        config->set("default_page", std::to_string(e.GetSelection()));
        config->save();
        e.Skip();
    });

    auto *splash = add_checkbox(_L("Show splash screen"), config->get_bool("show_splash_screen"));
    splash->Bind(wxEVT_CHECKBOX, [config, splash](wxCommandEvent &) {
        config->set_bool("show_splash_screen", splash->GetValue());
        config->save();
    });

    const std::vector<wxString> load_labels = {
        _L("Load All"), _L("Ask When Relevant"), _L("Always Ask"), _L("Load Geometry Only")};
    const std::vector<std::string> load_values = {
        OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT,
        OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY};
    int load_sel = 0;
    const std::string load = config->get(SETTING_PROJECT_LOAD_BEHAVIOUR);
    for (size_t i = 0; i < load_values.size(); ++i) {
        if (load == load_values[i])
            load_sel = static_cast<int>(i);
    }
    auto *load_choice = add_choice(_L("Load behaviour"), load_labels, load_sel);
    load_choice->Bind(wxEVT_CHOICE, [config, load_values](wxCommandEvent &e) {
        const int sel = e.GetSelection();
        if (sel >= 0 && sel < static_cast<int>(load_values.size())) {
            config->set(SETTING_PROJECT_LOAD_BEHAVIOUR, load_values[sel]);
            config->save();
        }
        e.Skip();
    });

    auto *backup = add_checkbox(_L("Auto backup"), config->get_bool("backup_switch"));
    backup->Bind(wxEVT_CHECKBOX, [config, backup](wxCommandEvent &) {
        config->set_bool("backup_switch", backup->GetValue());
        config->save();
        std::string interval = "10";
        config->get("backup_interval", interval);
        const long seconds = interval.empty() ? 10 : std::atol(interval.c_str());
        Slic3r::set_backup_interval(backup->GetValue() ? seconds : 0);
    });

    auto *recent_models = add_checkbox(_L("Add STL/STEP files to recent files list"), config->get_bool("recent_models"));
    recent_models->Bind(wxEVT_CHECKBOX, [config, recent_models](wxCommandEvent &) {
        config->set_bool("recent_models", recent_models->GetValue());
        config->save();
    });

    wxString max_recent = wxString::FromUTF8(config->get("max_recent_count"));
    if (max_recent.empty())
        max_recent = "18";
    auto *max_recent_ctrl = add_text(_L("Maximum recent files"), max_recent);
    auto commit_max_recent = [config, max_recent_ctrl]() {
        long max = 0;
        if (!max_recent_ctrl->GetValue().ToLong(&max))
            max = 18;
        if (wxGetApp().mainframe)
            wxGetApp().mainframe->set_max_recent_count(static_cast<int>(max));
        config->set("max_recent_count", std::to_string(max));
        config->save();
    };
    max_recent_ctrl->Bind(wxEVT_TEXT_ENTER, [commit_max_recent](wxCommandEvent &e) {
        commit_max_recent();
        e.Skip();
    });
    max_recent_ctrl->Bind(wxEVT_KILL_FOCUS, [commit_max_recent](wxFocusEvent &e) {
        commit_max_recent();
        e.Skip();
    });

    auto *gcodes_warning = add_checkbox(_L("Don't warn when loading 3MF with modified G-code"),
                                        config->get_bool("no_warn_when_modified_gcodes"));
    gcodes_warning->Bind(wxEVT_CHECKBOX, [config, gcodes_warning](wxCommandEvent &) {
        config->set_bool("no_warn_when_modified_gcodes", gcodes_warning->GetValue());
        config->save();
    });

    auto *step_dialog = add_checkbox(_L("Show options when importing STEP file"),
                                     config->get_bool("enable_step_mesh_setting"));
    step_dialog->Bind(wxEVT_CHECKBOX, [config, step_dialog](wxCommandEvent &) {
        config->set_bool("enable_step_mesh_setting", step_dialog->GetValue());
        config->save();
    });

    int drc_bits = 0;
    if (!config->get("drc_bits").empty())
        drc_bits = atoi(config->get("drc_bits").c_str());
    if (drc_bits != 0 && drc_bits < DRC_BITS_MIN)
        drc_bits = DRC_BITS_MIN;
    if (drc_bits > DRC_BITS_MAX)
        drc_bits = DRC_BITS_MAX;
    auto *drc_spin = add_spin(_L("Quality level for Draco export"), drc_bits, 0, DRC_BITS_MAX);
    drc_spin->Bind(wxEVT_SPINCTRL, [config, drc_spin](wxSpinEvent &) {
        config->set("drc_bits", std::to_string(drc_spin->GetValue()));
        config->save();
    });

    auto *full_paths = add_checkbox(_L("Store full source file paths in projects"),
                                    config->get_bool("export_sources_full_pathnames"));
    full_paths->Bind(wxEVT_CHECKBOX, [config, full_paths](wxCommandEvent &) {
        config->set_bool("export_sources_full_pathnames", full_paths->GetValue());
        config->save();
    });

    auto *remember = add_checkbox(_L("Remember printer configuration"), config->get_bool("remember_printer_config"));
    remember->Bind(wxEVT_CHECKBOX, [config, remember](wxCommandEvent &) {
        config->set_bool("remember_printer_config", remember->GetValue());
        config->save();
    });

    const std::vector<wxString> group_labels = {_L("All"), _L("None"), _L("By type"), _L("By vendor")};
    int group_sel = 0;
    if (!config->get("group_filament_presets").empty())
        group_sel = atoi(config->get("group_filament_presets").c_str());
    auto *group_choice = add_choice(_L("Group user filament presets"), group_labels, group_sel);
    group_choice->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        config->set("group_filament_presets", std::to_string(e.GetSelection()));
        config->save();
        if (wxGetApp().plater())
            wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
        e.Skip();
    });

    int filament_height = 8;
    if (!config->get("filaments_area_preferred_count").empty())
        filament_height = atoi(config->get("filaments_area_preferred_count").c_str());
    auto *filament_spin = add_spin(_L("Optimize filaments area height for..."), filament_height, 8, 99);
    filament_spin->Bind(wxEVT_SPINCTRL, [config, filament_spin](wxSpinEvent &) {
        config->set("filaments_area_preferred_count", std::to_string(filament_spin->GetValue()));
        config->save();
        if (wxGetApp().plater())
            wxGetApp().plater()->sidebar().update_filaments_area_height();
    });

    auto *shared = add_checkbox(_L("Show shared profiles notification"),
                                config->get_bool("show_shared_profiles_notification"));
    shared->Bind(wxEVT_CHECKBOX, [config, shared](wxCommandEvent &) {
        config->set_bool("show_shared_profiles_notification", shared->GetValue());
        config->save();
    });

    add_section(_L("Control"));

    auto *arrange = add_checkbox(_L("Auto arrange plate after cloning"), config->get_bool("auto_arrange"));
    arrange->Bind(wxEVT_CHECKBOX, [config, arrange](wxCommandEvent &) {
        config->set_bool("auto_arrange", arrange->GetValue());
        config->save();
    });

    auto *autoslice = add_checkbox(_L("Auto slice after changes"), config->get_bool("auto_slice_after_change"));
    autoslice->Bind(wxEVT_CHECKBOX, [config, autoslice](wxCommandEvent &) {
        config->set_bool("auto_slice_after_change", autoslice->GetValue());
        config->save();
    });

    wxString delay_value = wxString::FromUTF8(config->get("auto_slice_change_delay_seconds"));
    if (delay_value.empty())
        delay_value = "1";
    auto *delay_ctrl = add_text(_L("Auto slice delay (sec)"), delay_value);
    auto commit_delay = [config, delay_ctrl]() {
        long seconds = 0;
        if (!delay_ctrl->GetValue().ToLong(&seconds) || seconds < 0)
            seconds = 0;
        const wxString sanitized = wxString::Format("%ld", seconds);
        delay_ctrl->SetValue(sanitized);
        config->set("auto_slice_change_delay_seconds", std::string(sanitized.mb_str()));
        config->save();
    };
    delay_ctrl->Bind(wxEVT_TEXT_ENTER, [commit_delay](wxCommandEvent &e) {
        commit_delay();
        e.Skip();
    });
    delay_ctrl->Bind(wxEVT_KILL_FOCUS, [commit_delay](wxFocusEvent &e) {
        commit_delay();
        e.Skip();
    });

    auto *mix_temp = add_checkbox(_L("Remove mixed temperature restriction"),
                                  config->get_bool("enable_high_low_temp_mixed_printing"));
    mix_temp->Bind(wxEVT_CHECKBOX, [config, mix_temp](wxCommandEvent &) {
        config->set_bool("enable_high_low_temp_mixed_printing", mix_temp->GetValue());
        config->save();
    });

    auto *zoom = add_checkbox(_L("Zoom to mouse position"), config->get_bool("zoom_to_mouse"));
    zoom->Bind(wxEVT_CHECKBOX, [config, zoom](wxCommandEvent &) {
        config->set_bool("zoom_to_mouse", zoom->GetValue());
        config->save();
    });

    auto *free_cam = add_checkbox(_L("Use free camera"), config->get_bool("use_free_camera"));
    free_cam->Bind(wxEVT_CHECKBOX, [config, free_cam](wxCommandEvent &) {
        config->set_bool("use_free_camera", free_cam->GetValue());
        config->save();
    });

    auto *rev_zoom = add_checkbox(_L("Reverse mouse zoom"), config->get_bool("reverse_mouse_wheel_zoom"));
    rev_zoom->Bind(wxEVT_CHECKBOX, [config, rev_zoom](wxCommandEvent &) {
        config->set_bool("reverse_mouse_wheel_zoom", rev_zoom->GetValue());
        config->save();
    });

    const std::vector<wxString> camera_labels = {_L("Default"), _L("Touchpad")};
    int camera_sel = 0;
    if (!config->get("camera_navigation_style").empty())
        camera_sel = atoi(config->get("camera_navigation_style").c_str());
    auto *camera_choice = add_choice(_L("Camera style"), camera_labels, camera_sel);
    camera_choice->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        config->set("camera_navigation_style", std::to_string(e.GetSelection()));
        config->save();
        e.Skip();
    });

    wxString orbit = wxString::FromUTF8(config->get("camera_orbit_mult"));
    if (orbit.empty())
        orbit = "1";
    auto *orbit_ctrl = add_text(_L("Orbit speed multiplier"), orbit);
    auto commit_orbit = [config, orbit_ctrl]() {
        double conv = 1.0;
        if (orbit_ctrl->GetValue().ToCDouble(&conv)) {
            if (conv < 0.05)
                conv = 0.05;
            if (conv > 2.0)
                conv = 2.0;
            const std::string strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            orbit_ctrl->SetValue(strval);
            config->set("camera_orbit_mult", strval);
            config->save();
        }
    };
    orbit_ctrl->Bind(wxEVT_TEXT_ENTER, [commit_orbit](wxCommandEvent &e) {
        commit_orbit();
        e.Skip();
    });
    orbit_ctrl->Bind(wxEVT_KILL_FOCUS, [commit_orbit](wxFocusEvent &e) {
        commit_orbit();
        e.Skip();
    });

    const std::vector<wxString> drag_labels = {_L("None"), _L("Pan"), _L("Rotate")};
    auto bind_drag = [config](wxChoice *choice, const char *key) {
        choice->Bind(wxEVT_CHOICE, [config, key](wxCommandEvent &e) {
            config->set(key, std::to_string(e.GetSelection()));
            config->save();
            e.Skip();
        });
    };
    int left_drag = 0, mid_drag = 0, right_drag = 0;
    if (!config->get("left_mouse_drag_action").empty())
        left_drag = atoi(config->get("left_mouse_drag_action").c_str());
    if (!config->get("middle_mouse_drag_action").empty())
        mid_drag = atoi(config->get("middle_mouse_drag_action").c_str());
    if (!config->get("right_mouse_drag_action").empty())
        right_drag = atoi(config->get("right_mouse_drag_action").c_str());
    bind_drag(add_choice(_L("Left Mouse Drag"), drag_labels, left_drag), "left_mouse_drag_action");
    bind_drag(add_choice(_L("Middle Mouse Drag"), drag_labels, mid_drag), "middle_mouse_drag_action");
    bind_drag(add_choice(_L("Right Mouse Drag"), drag_labels, right_drag), "right_mouse_drag_action");

    add_action(_L("Unsaved projects"), _L("Clear"), [config]() {
        config->set("save_project_choise", "");
        config->save();
    });
    add_action(_L("Unsaved presets"), _L("Clear"), [config]() {
        config->set("save_preset_choise", "");
        config->save();
    });
    add_action(_L("Synchronizing printer preset"), _L("Clear"), [config]() {
        config->erase("app", "sync_after_load_file_show_flag");
        config->save();
    });

    add_section(_L("Graphics"));

    auto *phong = add_checkbox(_L("Phong shading"), config->get_bool(SETTING_OPENGL_REALISTIC_PHONG));
    phong->Bind(wxEVT_CHECKBOX, [config, phong](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_REALISTIC_PHONG, phong->GetValue());
        config->save();
    });
    auto *ssao = add_checkbox(_L("SSAO ambient occlusion"), config->get_bool(SETTING_OPENGL_PHONG_SSAO));
    ssao->Bind(wxEVT_CHECKBOX, [config, ssao](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_PHONG_SSAO, ssao->GetValue());
        config->save();
    });
    auto *shadows = add_checkbox(_L("Shadows"), config->get_bool(SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS));
    shadows->Bind(wxEVT_CHECKBOX, [config, shadows](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS, shadows->GetValue());
        config->save();
    });
    auto *smooth = add_checkbox(_L("Smooth normals"), config->get_bool(SETTING_OPENGL_PHONG_SMOOTH_NORMALS));
    smooth->Bind(wxEVT_CHECKBOX, [config, smooth](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_PHONG_SMOOTH_NORMALS, smooth->GetValue());
        config->save();
    });

    const std::vector<wxString> msaa_labels = {_L("Disabled"), "2x", "4x", "8x", "16x"};
    const std::vector<std::string> msaa_values = {"0", "2", "4", "8", "16"};
    int msaa_sel = 0;
    const std::string msaa = config->get(SETTING_OPENGL_AA_SAMPLES);
    for (size_t i = 0; i < msaa_values.size(); ++i) {
        if (msaa == msaa_values[i])
            msaa_sel = static_cast<int>(i);
    }
    auto *msaa_choice = add_choice(_L("MSAA Multiplier"), msaa_labels, msaa_sel);
    msaa_choice->Bind(wxEVT_CHOICE, [config, msaa_values](wxCommandEvent &e) {
        const int sel = e.GetSelection();
        if (sel >= 0 && sel < static_cast<int>(msaa_values.size())) {
            config->set(SETTING_OPENGL_AA_SAMPLES, msaa_values[sel]);
            config->save();
        }
        e.Skip();
    });

    auto *fxaa = add_checkbox(_L("FXAA post-processing"), config->get_bool(SETTING_OPENGL_FXAA_ENABLED));
    fxaa->Bind(wxEVT_CHECKBOX, [config, fxaa](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_FXAA_ENABLED, fxaa->GetValue());
        config->save();
    });

    int fps_cap = 0;
    if (!config->get(SETTING_OPENGL_FPS_CAP).empty())
        fps_cap = atoi(config->get(SETTING_OPENGL_FPS_CAP).c_str());
    auto *fps_spin = add_spin(_L("FPS cap"), fps_cap, 0, 240);
    fps_spin->Bind(wxEVT_SPINCTRL, [config, fps_spin](wxSpinEvent &) {
        config->set(SETTING_OPENGL_FPS_CAP, std::to_string(fps_spin->GetValue()));
        config->save();
    });

    auto *fps_overlay = add_checkbox(_L("Show FPS overlay"), config->get_bool(SETTING_OPENGL_SHOW_FPS_OVERLAY));
    fps_overlay->Bind(wxEVT_CHECKBOX, [config, fps_overlay](wxCommandEvent &) {
        config->set_bool(SETTING_OPENGL_SHOW_FPS_OVERLAY, fps_overlay->GetValue());
        config->save();
    });

    add_section(_L("Online"));

    auto *stealth = add_checkbox(_L("Stealth mode"), config->get_bool("stealth_mode"));

    auto *hide_login = add_checkbox(_L("Hide login side panel"), config->get_bool("hide_login_side_panel"));
    hide_login->Bind(wxEVT_CHECKBOX, [config, hide_login](wxCommandEvent &) {
        config->set_bool("hide_login_side_panel", hide_login->GetValue());
        config->save();
        if (wxGetApp().mainframe && wxGetApp().mainframe->m_webview)
            wxGetApp().mainframe->m_webview->SendCloudProvidersInfo();
    });

    auto *stable = add_checkbox(_L("Check for stable updates only"), config->get_bool("check_stable_update_only"));
    stable->Bind(wxEVT_CHECKBOX, [config, stable](wxCommandEvent &) {
        config->set_bool("check_stable_update_only", stable->GetValue());
        config->save();
    });

    auto *sync = add_checkbox(_L("Auto sync user presets (Printer/Filament/Process)"),
                              config->get_bool("sync_user_preset"));
    sync->Bind(wxEVT_CHECKBOX, [config, sync](wxCommandEvent &) {
        config->set_bool("sync_user_preset", sync->GetValue());
        config->save();
        if (sync->GetValue())
            wxGetApp().start_sync_user_preset();
        else
            wxGetApp().stop_sync_user_preset();
    });

    auto *bambu = add_checkbox(_L("Enable Bambu Cloud"), config->has_cloud_provider(BBL_CLOUD_PROVIDER));
    bambu->Bind(wxEVT_CHECKBOX, [config, bambu](wxCommandEvent &) {
        if (bambu->GetValue())
            config->add_cloud_provider(BBL_CLOUD_PROVIDER);
        else
            config->remove_cloud_provider(BBL_CLOUD_PROVIDER);
        config->save();
        if (wxGetApp().mainframe && wxGetApp().mainframe->m_webview)
            wxGetApp().mainframe->m_webview->SendCloudProvidersInfo();
    });

    add_action(_L("Network test"), _L("Test"), [this]() {
        NetworkTestDialog dlg(this);
        dlg.ShowModal();
    });

    const std::vector<wxString> filament_sync_labels = {_L("Filament & Color"), _L("Color only")};
    int filament_sync_sel = 0;
    if (!config->get("sync_ams_filament_mode").empty())
        filament_sync_sel = atoi(config->get("sync_ams_filament_mode").c_str());
    auto *filament_sync = add_choice(_L("Filament sync mode"), filament_sync_labels, filament_sync_sel);
    filament_sync->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        config->set("sync_ams_filament_mode", std::to_string(e.GetSelection()));
        config->save();
        e.Skip();
    });

    auto *system_sync = add_checkbox(_L("Update built-in presets automatically."),
                                     config->get_bool("sync_system_preset"));
    system_sync->Bind(wxEVT_CHECKBOX, [config, system_sync](wxCommandEvent &) {
        config->set_bool("sync_system_preset", system_sync->GetValue());
        config->save();
    });

    auto *token = add_checkbox(_L("Use encrypted file for token storage"),
                               config->get_bool(SETTING_USE_ENCRYPTED_TOKEN_FILE));
    token->Bind(wxEVT_CHECKBOX, [config, token](wxCommandEvent &) {
        config->set_bool(SETTING_USE_ENCRYPTED_TOKEN_FILE, token->GetValue());
        config->save();
    });

    auto *plugin = add_checkbox(_L("Enable network plug-in"), config->get_bool("installed_networking"));
    plugin->Bind(wxEVT_CHECKBOX, [config, plugin](wxCommandEvent &) {
        config->set_bool("installed_networking", plugin->GetValue());
        config->save();
    });

    if (config->get_stealth_mode()) {
        bambu->Enable(false);
        sync->Enable(false);
    }
    stealth->Bind(wxEVT_CHECKBOX, [config, stealth, bambu, sync](wxCommandEvent &) {
        config->set_bool("stealth_mode", stealth->GetValue());
        config->save();
        const bool on = config->get_stealth_mode();
        bambu->Enable(!on);
        sync->Enable(!on);
        if (on)
            wxGetApp().on_stealth_mode_enter();
    });

    add_section(_L("Developer"));

    auto *dev = add_checkbox(_L("Developer mode"), config->get_bool("developer_mode"));
    dev->Bind(wxEVT_CHECKBOX, [config, dev](wxCommandEvent &) {
        config->set_bool("developer_mode", dev->GetValue());
        config->save();
    });

    auto *ams_black = add_checkbox(_L("Skip AMS blacklist check"), config->get_bool("skip_ams_blacklist_check"));
    ams_black->Bind(wxEVT_CHECKBOX, [config, ams_black](wxCommandEvent &) {
        config->set_bool("skip_ams_blacklist_check", ams_black->GetValue());
        config->save();
    });

    auto *unsupported = add_checkbox(_L("Show unsupported presets"), config->get_bool("show_unsupported_presets"));
    unsupported->Bind(wxEVT_CHECKBOX, [config, unsupported](wxCommandEvent &) {
        config->set_bool("show_unsupported_presets", unsupported->GetValue());
        config->save();
    });

    auto *keep_paint = add_checkbox(_L("Keep painted feature after mesh change"), config->get_bool("keep_painting"));
    keep_paint->Bind(wxEVT_CHECKBOX, [config, keep_paint](wxCommandEvent &) {
        config->set_bool("keep_painting", keep_paint->GetValue());
        config->save();
    });

    auto *abnormal = add_checkbox(_L("Allow Abnormal Storage"), config->get_bool("allow_abnormal_storage"));
    abnormal->Bind(wxEVT_CHECKBOX, [config, abnormal](wxCommandEvent &) {
        config->set_bool("allow_abnormal_storage", abnormal->GetValue());
        config->save();
    });

    const std::vector<wxString> log_labels = {
        _L("fatal"), _L("error"), _L("warning"), _L("info"), _L("debug"), _L("trace")};
    int log_sel = 1;
    const std::string severity = config->get("log_severity_level");
    for (unsigned i = 0; i < log_labels.size(); ++i) {
        if (severity == Slic3r::get_string_logging_level(i) || severity == into_u8(log_labels[i]))
            log_sel = static_cast<int>(i);
    }
    auto *log_choice = add_choice(_L("Log Level"), log_labels, log_sel);
    log_choice->Bind(wxEVT_CHOICE, [config](wxCommandEvent &e) {
        const std::string level = Slic3r::get_string_logging_level(e.GetSelection());
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(level));
        config->set("log_severity_level", level);
        config->save();
        e.Skip();
    });

#ifdef _WIN32
    add_section(_L("Associate"));
    auto bind_assoc = [config](wxCheckBox *box, const char *key) {
        box->Bind(wxEVT_CHECKBOX, [config, box, key](wxCommandEvent &) {
            config->set_bool(key, box->GetValue());
            config->save();
        });
    };
    bind_assoc(add_checkbox(_L("Associate 3MF files to CoPrintSlicer"), config->get_bool("associate_3mf")), "associate_3mf");
    bind_assoc(add_checkbox(_L("Associate DRC files to CoPrintSlicer"), config->get_bool("associate_drc")), "associate_drc");
    bind_assoc(add_checkbox(_L("Associate STL files to CoPrintSlicer"), config->get_bool("associate_stl")), "associate_stl");
    bind_assoc(add_checkbox(_L("Associate STEP files to CoPrintSlicer"), config->get_bool("associate_step")), "associate_step");
#endif

    m_scroll->SetSizer(m_rows);
    m_scroll->FitInside();
    root->Add(m_scroll, 1, wxEXPAND | wxALL, FromDIP(16));

    auto *buttons = CreateSeparatedButtonSizer(wxCLOSE);
    if (buttons)
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizer(root);
    Layout();
}

void CoprintSettingsDialog::add_section(const wxString &title)
{
    if (m_rows->GetItemCount() > 0)
        m_rows->AddSpacer(FromDIP(12));
    auto *header = make_label(m_scroll, title, Label::Head_14, kLabelColour);
    m_rows->Add(header, 0, wxBOTTOM, FromDIP(8));
    m_rows->Add(new wxStaticLine(m_scroll), 0, wxEXPAND | wxBOTTOM, FromDIP(8));
}

wxChoice *CoprintSettingsDialog::add_choice(const wxString &title, const std::vector<wxString> &labels, int selection)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, title, Label::Body_14, kLabelColour);
    wxArrayString items;
    for (const wxString &item : labels)
        items.Add(item);
    auto *choice = new wxChoice(m_scroll, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(200, -1)), items);
    if (selection >= 0 && selection < static_cast<int>(labels.size()))
        choice->SetSelection(selection);
    else if (!labels.empty())
        choice->SetSelection(0);
    row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(choice, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    return choice;
}

wxCheckBox *CoprintSettingsDialog::add_checkbox(const wxString &title, bool value)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, title, Label::Body_14, kLabelColour);
    auto *box = new wxCheckBox(m_scroll, wxID_ANY, wxEmptyString);
    box->SetValue(value);
    row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(box, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    return box;
}

wxTextCtrl *CoprintSettingsDialog::add_text(const wxString &title, const wxString &value)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, title, Label::Body_14, kLabelColour);
    auto *ctrl = new wxTextCtrl(m_scroll, wxID_ANY, value, wxDefaultPosition, FromDIP(wxSize(120, -1)), wxTE_PROCESS_ENTER);
    row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    return ctrl;
}

wxSpinCtrl *CoprintSettingsDialog::add_spin(const wxString &title, int value, int min, int max)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, title, Label::Body_14, kLabelColour);
    auto *spin = new wxSpinCtrl(m_scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(120, -1)),
                                wxSP_ARROW_KEYS, min, max, value);
    row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(spin, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    return spin;
}

void CoprintSettingsDialog::add_action(const wxString &title, const wxString &button_label, std::function<void()> on_click)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, title, Label::Body_14, kLabelColour);
    auto *button = new wxButton(m_scroll, wxID_ANY, button_label);
    button->Bind(wxEVT_BUTTON, [on_click](wxCommandEvent &) {
        if (on_click)
            on_click();
    });
    row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(button, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
}

void CoprintSettingsDialog::add_downloads_row()
{
    AppConfig *config = wxGetApp().app_config;
    wxString path = wxString::FromUTF8(config->get("download_path"));

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *label = make_label(m_scroll, _L("Downloads Folder"), Label::Body_14, kLabelColour);
    auto *path_label = make_label(m_scroll, path, Label::Body_14, kHintColour);
    path_label->SetMinSize(FromDIP(wxSize(180, -1)));
    path_label->Wrap(FromDIP(180));
    path_label->SetToolTip(path);

    auto *browse = new wxButton(m_scroll, wxID_ANY, _L("Browse"));
    browse->Bind(wxEVT_BUTTON, [this, config, path_label](wxCommandEvent &) {
        wxDirDialog dialog(this, _L("Choose Download Directory"), wxEmptyString, wxDD_NEW_DIR_BUTTON);
        if (dialog.ShowModal() == wxID_OK) {
            const wxString folder = dialog.GetPath();
            config->set("download_path", folder.ToUTF8().data());
            config->save();
            path_label->SetLabel(folder);
            path_label->SetToolTip(folder);
            Layout();
        }
    });

    row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(path_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    m_rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
}

void CoprintSettingsDialog::fill_languages()
{
    const wxLanguage supported[] = {
        wxLANGUAGE_ENGLISH,
        wxLANGUAGE_CHINESE_SIMPLIFIED,
        wxLANGUAGE_CHINESE,
        wxLANGUAGE_GERMAN,
        wxLANGUAGE_CZECH,
        wxLANGUAGE_FRENCH,
        wxLANGUAGE_SPANISH,
        wxLANGUAGE_SWEDISH,
        wxLANGUAGE_DUTCH,
        wxLANGUAGE_HUNGARIAN,
        wxLANGUAGE_JAPANESE,
        wxLANGUAGE_ITALIAN,
        wxLANGUAGE_KOREAN,
        wxLANGUAGE_RUSSIAN,
        wxLANGUAGE_UKRAINIAN,
        wxLANGUAGE_TURKISH,
        wxLANGUAGE_POLISH,
        wxLANGUAGE_CATALAN,
        wxLANGUAGE_PORTUGUESE_BRAZILIAN,
        wxLANGUAGE_LITHUANIAN,
        wxLANGUAGE_VIETNAMESE,
        wxLANGUAGE_THAI
    };

    std::vector<const wxLanguageInfo *> infos;
    if (const wxLanguageInfo *english = wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH))
        infos.push_back(english);

    if (wxTranslations *tr = wxTranslations::Get()) {
        const wxArrayString translations = tr->GetAvailableTranslations(SLIC3R_APP_KEY);
        for (size_t i = 0; i < translations.GetCount(); ++i) {
            const wxLanguageInfo *info = wxLocale::FindLanguageInfo(translations[i]);
            if (info == nullptr)
                continue;
            for (const wxLanguage lang : supported) {
                if (info == wxLocale::GetLanguageInfo(lang)) {
                    infos.push_back(info);
                    break;
                }
            }
        }
    }

    std::sort(infos.begin(), infos.end());
    infos.erase(std::unique(infos.begin(), infos.end()), infos.end());
    std::sort(infos.begin(), infos.end(), [](const wxLanguageInfo *a, const wxLanguageInfo *b) {
        return a->Description < b->Description;
    });

    std::vector<wxString> labels;
    m_language_codes.clear();
    m_language_sel = 0;
    const std::string current = wxGetApp().app_config->get("language");
    for (size_t i = 0; i < infos.size(); ++i) {
        labels.push_back(infos[i]->Description);
        m_language_codes.push_back(into_u8(infos[i]->CanonicalName));
        if (current == m_language_codes.back())
            m_language_sel = static_cast<int>(i);
    }
    if (m_language_sel == 0 && current.size() >= 2) {
        const std::string prefix = current.substr(0, 2);
        for (size_t i = 0; i < m_language_codes.size(); ++i) {
            if (m_language_codes[i].rfind(prefix, 0) == 0) {
                m_language_sel = static_cast<int>(i);
                break;
            }
        }
    }

    m_language = add_choice(_L("Language"), labels, m_language_sel);
}

void CoprintSettingsDialog::on_language(wxCommandEvent &e)
{
    const int sel = m_language->GetSelection();
    if (sel == m_language_sel || sel < 0 || sel >= static_cast<int>(m_language_codes.size()))
        return;

    // Same sequence as Orca Preferences: save dirty project, confirm restart,
    // then close this dialog so recreate_GUI() can rebuild MainFrame.
    if (wxGetApp().plater() != nullptr && wxGetApp().plater()->is_project_dirty()) {
        const int result = MessageDialog(this,
            _L("The current project has unsaved changes, save it before continue?"),
            wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"),
            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE).ShowModal();
        if (result == wxID_YES)
            wxGetApp().plater()->save_project();
        else if (result == wxID_CANCEL) {
            m_language->SetSelection(m_language_sel);
            return;
        }
    }

    {
        MessageDialog ask(nullptr,
                          _L("Switching the language requires application restart.\n") + "\n" + _L("Do you want to continue?"),
                          _L("Language selection"), wxICON_QUESTION | wxOK | wxCANCEL);
        if (ask.ShowModal() == wxID_CANCEL) {
            m_language->SetSelection(m_language_sel);
            return;
        }
    }

    if (!wxGetApp().check_and_keep_current_preset_changes(
            _L("Switching application language"),
            _L("Switching application language while some presets are modified."),
            ActionButtons::SAVE)) {
        m_language->SetSelection(m_language_sel);
        return;
    }

    m_language_sel = sel;
    m_pending_language = m_language_codes[sel];
    wxGetApp().app_config->set("language", m_pending_language);
    wxGetApp().app_config->save();
    m_recreate_GUI = true;

#ifdef __APPLE__
    Close();
#else
    EndModal(wxID_OK);
#endif
    e.Skip();
}

void CoprintSettingsDialog::on_region(wxCommandEvent &e)
{
    const int sel = m_region->GetSelection();
    const std::vector<std::string> values = {
        "Asia-Pacific", "China", "Europe", "North America", "Others"};
    if (sel < 0 || sel >= static_cast<int>(values.size()) || sel == m_region_sel)
        return;

    AppConfig *config = wxGetApp().app_config;
    NetworkAgent *agent = wxGetApp().getAgent();
    if (agent) {
        MessageDialog ask(this,
                          _L("Changing the region will log out your account.\n") + "\n" + _L("Do you want to continue?"),
                          _L("Region selection"), wxICON_QUESTION | wxOK | wxCANCEL);
        if (ask.ShowModal() == wxID_CANCEL) {
            m_region->SetSelection(m_region_sel);
            return;
        }
        wxGetApp().request_user_logout();
        config->set("region", values[sel]);
        agent->set_country_code(config->get_country_code());
    } else {
        config->set("region", values[sel]);
    }
    config->save();
    m_region_sel = sel;
    wxGetApp().update_publish_status();
    e.Skip();
}

} // namespace GUI
} // namespace Slic3r
