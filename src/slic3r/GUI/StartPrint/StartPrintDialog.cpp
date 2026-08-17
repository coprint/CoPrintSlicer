#include "StartPrintDialog.hpp"

#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../MsgDialog.hpp"
#include "../PrinterWebView.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../DeviceManager.hpp"
#include "../DeviceCore/DevManager.h"
#include "../DeviceCore/DevFilaSystem.h"
#include "../DeviceDashboard/DeviceUiStyle.hpp"
#include "../DeviceDashboard/FilamentTrackSlot.hpp"
#include "../wxExtensions.hpp"
#include "../Widgets/PopupWindow.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <wx/popupwin.h>
#include <wx/statbmp.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

using Ui = DeviceDashboard::DeviceUiStyle;
using DeviceDashboard::FilamentTrackCenter;
using DeviceDashboard::FilamentTrackSlot;

constexpr int kRefreshIntervalMs = 2000;
const wxColour kPageBackground(255, 255, 255);
const wxColour kCardBackground(255, 255, 255);
const wxColour kControlBackground(255, 255, 255);
const wxColour kCardBorder(224, 224, 224);
const wxColour kTextPrimary(26, 26, 26);
const wxColour kTextMuted(107, 114, 128);
const wxColour kDisconnectedToolFill(0x93, 0x93, 0x93);

MachineObject *find_machine_by_id(Slic3r::DeviceManager *dev_manager, const std::string &dev_id)
{
    if (dev_manager == nullptr || dev_id.empty())
        return nullptr;
    if (MachineObject *obj = dev_manager->get_my_machine(dev_id))
        return obj;
    if (MachineObject *obj = dev_manager->get_local_machine(dev_id))
        return obj;
    if (MachineObject *obj = dev_manager->get_user_machine(dev_id))
        return obj;
    return dev_manager->find_lan_machine_for_agent_messages(dev_id);
}

wxString format_printer_label(MachineObject *obj)
{
    if (obj == nullptr)
        return wxEmptyString;
    wxString name = from_u8(obj->get_dev_name());
    wxString ip = from_u8(obj->get_dev_ip());
    if (!ip.empty() && name != ip)
        return wxString::Format("%s (%s)", name, ip);
    return name.empty() ? ip : name;
}

wxColour parse_filament_colour(const std::string &hex)
{
    if (hex.empty())
        return wxColour(120, 120, 120);
    wxColour colour(hex);
    return colour.IsOk() ? colour : wxColour(120, 120, 120);
}

PartPlate *plate_for_dialog(Plater *plater, int print_plate_idx)
{
    if (plater == nullptr)
        return nullptr;
    PartPlate *plate = plater->get_partplate_list().get_plate(print_plate_idx);
    if (plate == nullptr)
        plate = plater->get_partplate_list().get_curr_plate();
    return plate;
}

bool plate_ready_for_device_print(PartPlate *plate)
{
    return plate && plate->is_slice_result_valid() && plate->is_valid_gcode_file();
}

std::string moonraker_base_url(const MachineObject *obj)
{
    if (obj == nullptr)
        return {};
    std::string host = obj->get_dev_ip();
    if (host.empty())
        return {};
    if (host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0)
        return host;
    if (host.find(':') == std::string::npos)
        host += ":7125";
    return "http://" + host;
}

void send_tool_map_sync(MachineObject *obj, int model_slot_index, int physical_tool)
{
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    const int logical_index  = std::clamp(model_slot_index, 0, 3);
    const int physical_index = std::clamp(physical_tool - 1, 0, 3);
    const std::string script = "SET_TOOL_MAP LOGICAL=" + std::to_string(logical_index) +
                               " PHYSICAL=" + std::to_string(physical_index);

    nlohmann::json payload;
    payload["script"] = script;
    Http::post(base + "/printer/gcode/script")
        .header("Content-Type", "application/json")
        .set_post_body(payload.dump())
        .timeout_connect(2)
        .timeout_max(4)
        .perform_sync();
}

std::vector<FilamentInfo> filament_rows_from_project()
{
    std::vector<FilamentInfo> rows;
    auto *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return rows;

    const auto *color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const DynamicPrintConfig full_config = preset_bundle->full_config();
    const auto *type_opt = full_config.option<ConfigOptionStrings>("filament_type");
    if (color_opt == nullptr)
        return rows;

    const size_t count = std::min<size_t>(4, color_opt->values.size());
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        FilamentInfo info;
        info.id = static_cast<int>(i);
        info.color = color_opt->values[i];
        if (type_opt && i < type_opt->values.size())
            info.type = type_opt->values[i];
        else
            info.type = "PLA";
        rows.push_back(std::move(info));
    }
    return rows;
}

std::set<int> used_model_slots_0based(PartPlate *plate)
{
    std::set<int> ids;
    if (plate == nullptr)
        return ids;

    std::vector<int> used = plate->get_used_filaments();
    if (used.empty())
        used = plate->get_extruders_without_support();
    if (used.empty())
        used = plate->get_extruders(true);

    for (int id_1based : used) {
        const int id_0based = id_1based - 1;
        if (id_0based >= 0 && id_0based < 4)
            ids.insert(id_0based);
    }
    return ids;
}

std::vector<FilamentInfo> filament_rows_for_plate(PartPlate *plate)
{
    std::vector<FilamentInfo> source;
    if (plate != nullptr && !plate->get_slice_filaments_info().empty())
        source = plate->get_slice_filaments_info();
    else
        source = filament_rows_from_project();

    const std::set<int> used = used_model_slots_0based(plate);
    if (used.empty()) {
        if (plate != nullptr && !plate->get_slice_filaments_info().empty())
            return plate->get_slice_filaments_info();
        return {};
    }

    std::map<int, FilamentInfo> by_id;
    for (const FilamentInfo &info : source)
        by_id[info.id] = info;
    if (source.empty() || by_id.size() < used.size()) {
        for (const FilamentInfo &info : filament_rows_from_project())
            if (!by_id.count(info.id))
                by_id[info.id] = info;
    }

    std::vector<FilamentInfo> rows;
    rows.reserve(used.size());
    for (int id : used) {
        auto it = by_id.find(id);
        if (it != by_id.end()) {
            rows.push_back(it->second);
            continue;
        }
        FilamentInfo info;
        info.id = id;
        info.type = "PLA";
        rows.push_back(std::move(info));
    }
    return rows;
}

wxImage thumbnail_data_to_wximage(const ThumbnailData &data)
{
    wxImage image(data.width, data.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < data.height; ++r) {
        const unsigned int rr = (data.height - 1 - r) * data.width;
        for (unsigned int c = 0; c < data.width; ++c) {
            const unsigned char *px = data.pixels.data() + 4 * (rr + c);
            image.SetRGB(static_cast<int>(c), static_cast<int>(r), px[0], px[1], px[2]);
            image.SetAlpha(static_cast<int>(c), static_cast<int>(r), px[3]);
        }
    }
    return image;
}

const ThumbnailData *pick_plate_thumbnail(const PartPlate *plate)
{
    if (plate == nullptr)
        return nullptr;
    if (plate->thumbnail_data.is_valid())
        return &plate->thumbnail_data;
    if (plate->no_light_thumbnail_data.is_valid())
        return &plate->no_light_thumbnail_data;
    if (plate->top_thumbnail_data.is_valid())
        return &plate->top_thumbnail_data;
    if (plate->pick_thumbnail_data.is_valid())
        return &plate->pick_thumbnail_data;
    return nullptr;
}

void apply_plate_thumbnail(wxWindow *host, ThumbnailPanel *panel, wxStaticText *placeholder, const PartPlate *plate, int thumb_dip)
{
    if (panel == nullptr)
        return;

    wxWindow *dip_host = host != nullptr ? host : panel;
    const int size     = dip_host->FromDIP(thumb_dip);

    const ThumbnailData *thumb = pick_plate_thumbnail(plate);
    if (thumb != nullptr && thumb->is_valid() && !thumb->pixels.empty()) {
        wxImage image = thumbnail_data_to_wximage(*thumb);
        if (image.GetWidth() > 0 && image.GetHeight() > 0 &&
            (image.GetWidth() != size || image.GetHeight() != size))
            image = image.Rescale(size, size, wxIMAGE_QUALITY_HIGH);
        panel->set_thumbnail(image);
        panel->SetMinSize(wxSize(size, size));
        panel->SetMaxSize(wxSize(size, size));
        panel->Show(true);
        if (placeholder != nullptr)
            placeholder->Show(false);
    } else {
        panel->Show(false);
        if (placeholder != nullptr) {
            placeholder->SetLabel(_L("No preview"));
            placeholder->Show(true);
        }
    }
    panel->Refresh();
    if (host != nullptr) {
        host->Layout();
        host->Refresh();
    }
}

wxColour ui_warning() { return wxColour(255, 174, 66); }

bool is_dark_fill(const wxColour &colour)
{
    const int brightness = (colour.Red() * 299 + colour.Green() * 587 + colour.Blue() * 114) / 1000;
    return brightness < 140;
}

wxColour readable_on_fill(const wxColour &fill)
{
    return is_dark_fill(fill) ? *wxWHITE : wxColour(35, 39, 46);
}

wxString filament_type_key(wxString raw)
{
    raw.Trim(true).Trim(false);
    raw.MakeUpper();
    if (raw.StartsWith("SUP."))
        raw = raw.Mid(4);
    if (raw.StartsWith("GENERIC "))
        raw = raw.Mid(8);
    if (raw.EndsWith("-S"))
        raw.RemoveLast(2);
    return raw;
}

bool filament_types_match(const wxString &a, const wxString &b)
{
    const wxString ka = filament_type_key(a);
    const wxString kb = filament_type_key(b);
    return !ka.empty() && ka == kb;
}

int colour_distance_sq(const wxColour &a, const wxColour &b)
{
    const int dr = a.Red() - b.Red();
    const int dg = a.Green() - b.Green();
    const int db = a.Blue() - b.Blue();
    return dr * dr + dg * dg + db * db;
}

class RoundedColorBlock : public wxPanel
{
public:
    enum class Corners { Top, Bottom };

    RoundedColorBlock(wxWindow *parent, Corners corners)
        : wxPanel(parent, wxID_ANY)
        , m_corners(corners)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(kCardBackground);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();

            const wxSize size = GetClientSize();
            if (size.x <= 0 || size.y <= 0)
                return;

            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc)
                return;

            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            const double inset = 0.5;
            const double x = inset;
            const double y = inset;
            const double w = size.x - 1.0;
            const double h = size.y - 1.0;
            const double r = std::min(static_cast<double>(FromDIP(7)), std::min(w, h) / 2.0);
            const bool   top = m_corners == Corners::Top;

            wxGraphicsPath path = gc->CreatePath();
            if (top) {
                path.MoveToPoint(x + r, y);
                path.AddLineToPoint(x + w - r, y);
                path.AddArcToPoint(x + w, y, x + w, y + r, r);
                path.AddLineToPoint(x + w, y + h);
                path.AddLineToPoint(x, y + h);
                path.AddLineToPoint(x, y + r);
                path.AddArcToPoint(x, y, x + r, y, r);
            } else {
                path.MoveToPoint(x, y);
                path.AddLineToPoint(x + w, y);
                path.AddLineToPoint(x + w, y + h - r);
                path.AddArcToPoint(x + w, y + h, x + w - r, y + h, r);
                path.AddLineToPoint(x + r, y + h);
                path.AddArcToPoint(x, y + h, x, y + h - r, r);
                path.AddLineToPoint(x, y);
            }
            path.CloseSubpath();
            gc->SetBrush(wxBrush(m_fill));
            gc->SetPen(wxPen(wxColour(0xDF, 0xDF, 0xDF), 1));
            gc->FillPath(path);
            gc->StrokePath(path);

            if (!m_text.empty()) {
                wxFont font = GetFont();
                font.SetPointSize(9);
                font.SetWeight(wxFONTWEIGHT_BOLD);
                gc->SetFont(font, readable_on_fill(m_fill));
                double tw = 0.0;
                double th = 0.0;
                gc->GetTextExtent(m_text, &tw, &th);
                gc->DrawText(m_text,
                    std::max(0.0, (size.x - tw) / 2.0),
                    std::max(0.0, (size.y - th) / 2.0));
            }
        });
    }

    void set_fill(const wxColour &fill)
    {
        m_fill = fill.IsOk() ? fill : kControlBackground;
        Refresh();
    }

    void set_text(const wxString &text)
    {
        m_text = text;
        Refresh();
    }

    void set_content(const wxColour &fill, const wxString &text)
    {
        m_fill = fill.IsOk() ? fill : kControlBackground;
        m_text = text;
        Refresh();
    }

private:
    Corners  m_corners{Corners::Top};
    wxColour m_fill{kControlBackground};
    wxString m_text;
};

struct PrinterToolInfo {
    wxColour  color{kControlBackground};
    wxString  material;
    bool      has_filament{false};
};

PrinterToolInfo query_printer_tool(MachineObject *obj, int tool_0based)
{
    PrinterToolInfo info;
    if (obj == nullptr || tool_0based < 0 || tool_0based > 3)
        return info;

    try {
        const std::string tray_id = std::to_string(tool_0based);
        std::string type;
        std::string display_type;
        
        try {
            type = obj->get_filament_type("0", tray_id);
        } catch (...) {
            type.clear();
        }
        
        if (!type.empty()) {
            info.has_filament = true;
            try {
                display_type = obj->get_filament_display_type("0", tray_id);
            } catch (...) {
                display_type.clear();
            }
            
            wxString display = display_type.empty() ? wxString() : from_u8(display_type);
            info.material = display.empty() ? from_u8(type) : display;
        }

        if (DevAmsTray *tray = obj->get_ams_tray("0", tray_id)) {
            const wxColour tray_color = tray->get_color();
            if (tray_color.IsOk()) {
                info.color        = tray_color;
                info.has_filament = true;
            }
        }

        if (MainFrame *frame = wxGetApp().mainframe) {
            if (PrinterWebView *printer_view = frame->m_printer_view) {
                wxColour cached;
                wxString material;
                if (printer_view->get_loaded_tool_filament(tool_0based, &cached, &material) && cached.IsOk()) {
                    info.color        = cached;
                    info.has_filament = true;
                    if (!material.empty())
                        info.material = material;
                }
            }
        }

        if (!info.has_filament)
            info.material = _L("Empty");
        else if (info.material.empty())
            info.material = wxString::FromUTF8("?");
    } catch (...) {
        info.material = _L("Empty");
        info.has_filament = false;
    }

    return info;
}

static void sync_printer_tool_colours(MachineObject *obj)
{
    if (obj == nullptr)
        return;
    if (MainFrame *frame = wxGetApp().mainframe) {
        if (PrinterWebView *printer_view = frame->m_printer_view)
            printer_view->sync_loaded_tool_filaments(obj);
    }
}

bool printer_view_has_loaded_filaments()
{
    MainFrame *frame = wxGetApp().mainframe;
    if (frame == nullptr || frame->m_printer_view == nullptr)
        return false;
    for (int i = 0; i < 4; ++i) {
        wxColour cached;
        if (frame->m_printer_view->get_loaded_tool_filament(i, &cached, nullptr) && cached.IsOk())
            return true;
    }
    return false;
}

void style_primary_text(wxStaticText *label, bool bold = false)
{
    if (label == nullptr)
        return;
    label->SetForegroundColour(kTextPrimary);
    if (bold) {
        wxFont font = label->GetFont();
        font.SetWeight(wxFONTWEIGHT_BOLD);
        label->SetFont(font);
    }
}

void style_muted_text(wxStaticText *label)
{
    if (label != nullptr)
        label->SetForegroundColour(kTextMuted);
}

StaticBox *make_card(wxWindow *parent, const wxColour &bg, int radius_dip = -1)
{
    auto *card = new StaticBox(parent, wxID_ANY);
    const int radius = radius_dip < 0 ? Ui::card_radius() : radius_dip;
    card->SetCornerRadius(parent->FromDIP(radius));
    card->SetBorderWidth(Ui::card_border_width());
    card->SetBorderColorNormal(kCardBorder);
    card->SetBackgroundColorNormal(bg);
    card->SetBackgroundColour(bg);
    return card;
}

FilamentTrackSlot *make_mini_tool_pick_track(wxWindow *parent, int tool_1based, const PrinterToolInfo &info,
    int cell_h_dip, int track_w_dip, const wxString &required_type, bool selected, std::function<void()> on_pick)
{
    const int cell_h  = parent->FromDIP(cell_h_dip);
    const int track_w = parent->FromDIP(track_w_dip);

    const bool filled = info.has_filament && info.color.IsOk();
    const bool type_ok = filled && (required_type.empty() || filament_types_match(required_type, info.material));
    const auto center = filled ? FilamentTrackCenter::ToolNumber : FilamentTrackCenter::SlashSign;
    const wxString label = filled ? info.material : wxString();
    auto *track = new FilamentTrackSlot(parent, tool_1based, info.color, filled, center, label);
    track->SetBackgroundColour(parent->GetBackgroundColour());
    track->SetMinSize(wxSize(track_w, cell_h));
    track->SetMaxSize(wxSize(track_w, cell_h));
    track->SetCursor(wxCursor(type_ok ? wxCURSOR_HAND : wxCURSOR_ARROW));
    track->set_selected(selected);
    if (filled)
        track->enable_hover(true);

    if (type_ok) {
        const auto pick = [on_pick](wxMouseEvent &event) {
            event.Skip(false);
            on_pick();
        };
        track->Bind(wxEVT_LEFT_DOWN, pick);
    }

    return track;
}

class StartPrintToolPickerPopup : public PopupWindow
{
public:
    using PickHandler = std::function<void(int tool_1based)>;

    StartPrintToolPickerPopup(wxWindow *parent, MachineObject *obj, const wxString &required_type,
        int current_tool, PickHandler on_pick)
        : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_on_pick(std::move(on_pick))
    {
        SetBackgroundColour(kPageBackground);

        const int outer_pad   = FromDIP(8);
        const int inner_pad   = FromDIP(14);
        const int min_gap     = FromDIP(16);
        const int track_w_dip = 50;
        const int cell_h_dip  = 60;

        auto *outer = new wxBoxSizer(wxVERTICAL);
        auto *frame = make_card(this, kCardBackground, 8);
        outer->Add(frame, 0, wxEXPAND | wxALL, outer_pad);

        auto *row = new wxBoxSizer(wxHORIZONTAL);
        for (int tool_0based = 0; tool_0based < 4; ++tool_0based) {
            const int tool = tool_0based + 1;
            const PrinterToolInfo tool_info = query_printer_tool(obj, tool_0based);
            auto *track = make_mini_tool_pick_track(frame, tool, tool_info, cell_h_dip, track_w_dip, required_type,
                tool == current_tool,
                [this, tool]() {
                    if (m_on_pick)
                        m_on_pick(tool);
                    Dismiss();
                });
            m_tracks.push_back(track);
            if (tool_0based > 0) {
                row->AddSpacer(min_gap);
                row->AddStretchSpacer(1);
            }
            row->Add(track, 0, wxALIGN_CENTER_VERTICAL);
        }

        auto *hint = new wxStaticText(frame, wxID_ANY, _L("Only the same filament can be selected"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER | wxST_NO_AUTORESIZE);
        hint->SetForegroundColour(Ui::danger());
        hint->SetFont(::Label::Body_14);

        auto *frame_sizer = new wxBoxSizer(wxVERTICAL);
        frame_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, inner_pad);
        frame_sizer->Add(hint, 0, wxALIGN_CENTER | wxALL, inner_pad);
        frame->SetSizer(frame_sizer);
        SetSizerAndFit(outer);

        m_hover_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
            if (IsShown())
                sync_track_hover();
        });
    }

    ~StartPrintToolPickerPopup() override { m_hover_timer.Stop(); }

    void Popup(wxWindow *focus = nullptr) override
    {
        PopupWindow::Popup(focus);
        m_hover_timer.Start(16);
        CallAfter([this] {
            if (IsShown())
                sync_track_hover();
        });
    }

    void OnDismiss() override
    {
        m_hover_timer.Stop();
        PopupWindow::OnDismiss();
    }

private:
    void sync_track_hover()
    {
        const wxPoint screen = wxGetMousePosition();
        for (FilamentTrackSlot *track : m_tracks) {
            if (track == nullptr)
                continue;
            const wxRect rect(track->ClientToScreen(wxPoint(0, 0)), track->GetSize());
            track->set_hovered(rect.Contains(screen));
        }
    }

    PickHandler m_on_pick;
    std::vector<FilamentTrackSlot *> m_tracks;
    wxTimer m_hover_timer;
};

void apply_dark_combo(ComboBox *combo)
{
    if (combo == nullptr)
        return;
    const wxColour bg       = kControlBackground;
    const wxColour bg_hover = wxColour(245, 245, 245);
    combo->SetCornerRadius(combo->FromDIP(6));
    combo->SetBorderColor(StateColor(
        std::make_pair(kCardBorder, (int) StateColor::Disabled),
        std::make_pair(Ui::accent(), (int) StateColor::Hovered),
        std::make_pair(kCardBorder, (int) StateColor::Normal)));
    combo->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(240, 240, 240), (int) StateColor::Disabled),
        std::make_pair(bg_hover, (int) StateColor::Focused),
        std::make_pair(bg, (int) StateColor::Normal)));
    combo->SetLabelColor(StateColor(
        std::make_pair(kTextMuted, (int) StateColor::Disabled),
        std::make_pair(kTextPrimary, (int) StateColor::Normal)));

    DropDown &drop = combo->GetDropDown();
    drop.SetApplyDarkMode(false);
    drop.SetBackgroundColour(bg);
    drop.SetBorderColor(StateColor(
        std::make_pair(kCardBorder, (int) StateColor::Normal)));
    drop.SetTextColor(StateColor(
        std::make_pair(kTextMuted, (int) StateColor::Disabled),
        std::make_pair(kTextPrimary, (int) StateColor::Normal)));
    drop.SetSelectorBackgroundColor(StateColor(
        std::make_pair(wxColour(232, 244, 255), (int) StateColor::Checked),
        std::make_pair(bg, (int) StateColor::Normal)));
    drop.SetSelectorBorderColor(StateColor(
        std::make_pair(Ui::accent(), (int) StateColor::Hovered),
        std::make_pair(bg, (int) StateColor::Normal)));
}

void apply_dark_secondary_button(Button *btn)
{
    if (btn == nullptr)
        return;
    btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);
    btn->SetCornerRadius(btn->FromDIP(10));
    btn->SetBackgroundColour(kPageBackground);
    const wxColour bg = kControlBackground;
    btn->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(240, 240, 240), (int) StateColor::Disabled),
        std::make_pair(wxColour(245, 245, 245), (int) StateColor::Pressed),
        std::make_pair(wxColour(245, 245, 245), (int) StateColor::Hovered),
        std::make_pair(bg, (int) StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::make_pair(kCardBorder, (int) StateColor::Disabled),
        std::make_pair(kCardBorder, (int) StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::make_pair(kTextMuted, (int) StateColor::Disabled),
        std::make_pair(kTextPrimary, (int) StateColor::Normal)));
}

void apply_dark_primary_button(Button *btn)
{
    if (btn == nullptr)
        return;
    btn->SetStyle(ButtonStyle::Confirm, ButtonType::Expanded);
    btn->SetCornerRadius(btn->FromDIP(10));
    btn->SetBackgroundColour(kPageBackground);
    const wxColour accent = Ui::accent();
    btn->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(240, 240, 240), (int) StateColor::Disabled),
        std::make_pair(wxColour(36, 150, 104), (int) StateColor::Pressed),
        std::make_pair(wxColour(52, 196, 140), (int) StateColor::Hovered),
        std::make_pair(accent, (int) StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::make_pair(kCardBorder, (int) StateColor::Disabled),
        std::make_pair(accent, (int) StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::make_pair(kTextMuted, (int) StateColor::Disabled),
        std::make_pair(*wxWHITE, (int) StateColor::Normal)));
}

wxBoxSizer *make_info_row(wxWindow *parent, const wxString &label, wxStaticText *&value_out)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *caption = new wxStaticText(parent, wxID_ANY, label);
    style_muted_text(caption);
    caption->SetMinSize(wxSize(parent->FromDIP(76), -1));
    value_out = new wxStaticText(parent, wxID_ANY, wxEmptyString);
    style_primary_text(value_out);
    row->Add(caption, 0, wxALIGN_CENTER_VERTICAL);
    row->AddSpacer(parent->FromDIP(8));
    row->Add(value_out, 1, wxALIGN_CENTER_VERTICAL);
    return row;
}

wxPanel *make_option_row(wxWindow *parent, const wxString &label_text, CheckBox *&checkbox)
{
    auto *row = new wxPanel(parent, wxID_ANY);
    row->SetBackgroundColour(kCardBackground);
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    checkbox = new CheckBox(row);
    checkbox->SetBackgroundColour(kCardBackground);
    auto *label = new wxStaticText(row, wxID_ANY, label_text);
    style_primary_text(label);
    sizer->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(6));
    sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
    row->SetSizer(sizer);
    return row;
}

} // namespace

class PrinterToolSwatch : public wxPanel
{
public:
    explicit PrinterToolSwatch(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent->GetBackgroundColour());
        Bind(wxEVT_PAINT, &PrinterToolSwatch::on_paint, this);
    }

    void set_tool(const wxColour &color, bool has_filament)
    {
        m_color        = color;
        m_has_filament = has_filament && color.IsOk();
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const double inset = 0.5;
        const double x = inset;
        const double y = inset;
        const double w = size.x - 1.0;
        const double h = size.y - 1.0;
        const double r = std::min(static_cast<double>(FromDIP(4)), std::min(w, h) / 2.0);

        const wxColour fill = m_has_filament ? m_color : *wxWHITE;
        gc->SetBrush(wxBrush(fill));
        gc->SetPen(wxPen(wxColour(0xDF, 0xDF, 0xDF), 1));
        gc->DrawRoundedRectangle(x, y, w, h, r);

        if (!m_has_filament) {
            wxFont font = GetFont();
            font.SetPointSize(8);
            font.SetWeight(wxFONTWEIGHT_BOLD);
            gc->SetFont(font, wxColour(130, 134, 140));
            double tw = 0.0;
            double th = 0.0;
            const wxString slash("/");
            gc->GetTextExtent(slash, &tw, &th);
            gc->DrawText(slash,
                std::max(0.0, (size.x - tw) / 2.0),
                std::max(0.0, (size.y - th) / 2.0));
        }
    }

    wxColour m_color;
    bool     m_has_filament{false};
};

StartPrintFilamentSlot::StartPrintFilamentSlot(wxWindow *parent, int model_slot_index)
    : wxPanel(parent, wxID_ANY)
    , m_model_slot_index(model_slot_index)
    , m_mapped_tool(model_slot_index + 1)
{
    SetBackgroundColour(kPageBackground);

    const int box_w = FromDIP(90);
    const int box_h = FromDIP(23);
    const int gap   = FromDIP(1);
    SetMinSize(wxSize(box_w, box_h * 2 + gap));
    SetMaxSize(wxSize(box_w, box_h * 2 + gap));

    auto *col = new wxBoxSizer(wxVERTICAL);

    auto *model_block = new RoundedColorBlock(this, RoundedColorBlock::Corners::Top);
    m_model_half = model_block;
    m_model_half->SetMinSize(wxSize(box_w, box_h));
    m_model_half->SetMaxSize(wxSize(box_w, box_h));

    auto *printer_block = new RoundedColorBlock(this, RoundedColorBlock::Corners::Bottom);
    m_printer_half = printer_block;
    m_printer_half->SetMinSize(wxSize(box_w, box_h));
    m_printer_half->SetMaxSize(wxSize(box_w, box_h));
    m_printer_half->SetCursor(wxCursor(wxCURSOR_HAND));
    m_printer_half->Bind(wxEVT_LEFT_DOWN, &StartPrintFilamentSlot::on_printer_half_clicked, this);

    col->Add(m_model_half, 0);
    col->AddSpacer(gap);
    col->Add(m_printer_half, 0);
    SetSizer(col);

    style_block(m_model_half, wxColour(120, 120, 120), _L("PLA"));
    style_block(m_printer_half, kDisconnectedToolFill, wxString::FromUTF8("/"));
    m_printer_half->SetCursor(wxCursor(wxCURSOR_ARROW));
}

void StartPrintFilamentSlot::style_block(wxPanel *block, const wxColour &bg, const wxString &text)
{
    if (auto *rounded = dynamic_cast<RoundedColorBlock *>(block))
        rounded->set_content(bg, text);
    else if (block != nullptr)
        block->Refresh();
}

void StartPrintFilamentSlot::set_visible(bool visible)
{
    Show(visible);
}

void StartPrintFilamentSlot::set_model_slot_index(int model_slot_index)
{
    m_model_slot_index = std::clamp(model_slot_index, 0, 3);
}

void StartPrintFilamentSlot::set_model_filament(const std::string &type, const wxColour &color)
{
    m_model_type_name = from_u8(type);
    m_model_color = color.IsOk() ? color : wxColour(120, 120, 120);
    style_block(m_model_half, m_model_color, m_model_type_name);
}

void StartPrintFilamentSlot::set_mapped_tool(int mapped_tool)
{
    m_mapped_tool = std::clamp(mapped_tool, 1, 4);
    if (!m_interactive)
        return;
    if (auto *rounded = dynamic_cast<RoundedColorBlock *>(m_printer_half))
        rounded->set_text(wxString::Format(wxString::FromUTF8("▼ T%d"), m_mapped_tool));
}

void StartPrintFilamentSlot::set_interactive(bool interactive)
{
    m_interactive = interactive;
    if (m_printer_half == nullptr)
        return;
    if (!interactive) {
        m_printer_half->SetCursor(wxCursor(wxCURSOR_ARROW));
        style_block(m_printer_half, kDisconnectedToolFill, wxString::FromUTF8("/"));
        return;
    }
    m_printer_half->SetCursor(wxCursor(wxCURSOR_HAND));
}

void StartPrintFilamentSlot::update_printer_tool(const wxColour &color, bool has_filament, int tool_1based)
{
    m_mapped_tool = std::clamp(tool_1based, 1, 4);
    if (!m_interactive) {
        style_block(m_printer_half, kDisconnectedToolFill, wxString::FromUTF8("/"));
        return;
    }
    if (!has_filament) {
        style_block(m_printer_half, ui_warning(), wxString::FromUTF8("EF"));
        return;
    }
    const wxColour fill = color.IsOk() ? color : kControlBackground;
    style_block(m_printer_half, fill,
        wxString::Format(wxString::FromUTF8("▼ T%d"), m_mapped_tool));
}

void StartPrintFilamentSlot::bind_tool_pick_handler(ToolPickHandler handler)
{
    m_pick_handler = std::move(handler);
}

void StartPrintFilamentSlot::on_printer_half_clicked(wxMouseEvent &event)
{
    event.Skip(false);
    if (!m_interactive)
        return;
    if (m_pick_handler)
        m_pick_handler(m_model_slot_index, m_printer_half);
}

StartPrintDialog::StartPrintDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Start Print"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_refresh_timer(this)
{
    m_plater = wxGetApp().plater();
    build_ui();
    bind_events();
    SetBackgroundColour(kPageBackground);
}

void StartPrintDialog::build_ui()
{
    const int margin = FromDIP(20);
    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(16));

    m_task_name_switch_panel = new wxSimplebook(this);
    m_task_name_switch_panel->SetMinSize(wxSize(-1, FromDIP(28)));
    m_task_name_switch_panel->SetBackgroundColour(kPageBackground);

    m_task_name_normal_panel = new wxPanel(m_task_name_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_task_name_normal_panel->SetBackgroundColour(kPageBackground);
    auto *title_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_task_name_label = new wxStaticText(m_task_name_normal_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_task_name_label->SetForegroundColour(*wxBLACK);
    wxFont title_font = m_task_name_label->GetFont();
    title_font.SetPointSize(15);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_task_name_label->SetFont(title_font);
    m_task_name_label->SetMaxSize(wxSize(FromDIP(420), -1));
    m_task_name_edit_button = new Button(m_task_name_normal_panel, "", "rename_edit", wxBORDER_NONE, FromDIP(13));
    m_task_name_edit_button->SetBackgroundColor(*wxWHITE);
    m_task_name_edit_button->SetBackgroundColour(*wxWHITE);
    title_sizer->AddStretchSpacer();
    title_sizer->Add(m_task_name_label, 0, wxALIGN_CENTER_VERTICAL);
    title_sizer->Add(m_task_name_edit_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    title_sizer->AddStretchSpacer();
    m_task_name_normal_panel->SetSizer(title_sizer);

    auto *task_name_edit_panel = new wxPanel(m_task_name_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    task_name_edit_panel->SetBackgroundColour(kPageBackground);
    auto *edit_sizer = new wxBoxSizer(wxVERTICAL);
    m_task_name_input = new ::TextInput(task_name_edit_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_task_name_input->GetTextCtrl()->SetFont(::Label::Body_13);
    edit_sizer->Add(m_task_name_input, 1, wxEXPAND);
    task_name_edit_panel->SetSizer(edit_sizer);

    m_task_name_switch_panel->AddPage(m_task_name_normal_panel, wxEmptyString, true);
    m_task_name_switch_panel->AddPage(task_name_edit_panel, wxEmptyString, false);

    auto *title_outer = new wxBoxSizer(wxHORIZONTAL);
    title_outer->Add(m_task_name_switch_panel, 1, wxEXPAND);
    main_sizer->Add(title_outer, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    main_sizer->AddSpacer(FromDIP(12));

    // Job summary: thumbnail left, print stats right
    m_preview_card = make_card(this, kCardBackground);
    m_preview_card->SetBorderWidth(0);
    auto *preview_outer = new wxBoxSizer(wxHORIZONTAL);
    preview_outer->AddSpacer(margin);
    preview_outer->Add(m_preview_card, 1, wxEXPAND);
    preview_outer->AddSpacer(margin);
    main_sizer->Add(preview_outer, 0, wxEXPAND);

    auto *preview_body = new wxBoxSizer(wxHORIZONTAL);
    const int thumb_dip = 90;
    auto *thumb_host = make_card(m_preview_card, kControlBackground, 8);
    thumb_host->SetMinSize(wxSize(FromDIP(thumb_dip), FromDIP(thumb_dip)));
    thumb_host->SetMaxSize(wxSize(FromDIP(thumb_dip), FromDIP(thumb_dip)));
    auto *thumb_stack = new wxBoxSizer(wxVERTICAL);
    m_thumbnail_panel = new ThumbnailPanel(thumb_host, wxID_ANY, wxDefaultPosition,
        wxSize(FromDIP(thumb_dip), FromDIP(thumb_dip)));
    m_thumbnail_placeholder = new wxStaticText(thumb_host, wxID_ANY, _L("No preview"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    style_muted_text(m_thumbnail_placeholder);
    thumb_stack->AddStretchSpacer();
    thumb_stack->Add(m_thumbnail_panel, 0, wxALIGN_CENTER);
    thumb_stack->Add(m_thumbnail_placeholder, 0, wxALIGN_CENTER);
    thumb_stack->AddStretchSpacer();
    thumb_host->SetSizer(thumb_stack);
    preview_body->Add(thumb_host, 0, wxALIGN_CENTER_VERTICAL);

    preview_body->AddSpacer(FromDIP(16));

    auto *stats_host = new wxPanel(m_preview_card);
    stats_host->SetBackgroundColour(kCardBackground);
    stats_host->SetMinSize(wxSize(-1, FromDIP(90)));
    stats_host->SetMaxSize(wxSize(-1, FromDIP(90)));
    auto *stats_col = new wxBoxSizer(wxVERTICAL);
    stats_col->Add(make_info_row(stats_host, _L("Time"), m_time_label), 1, wxEXPAND);
    stats_col->Add(make_info_row(stats_host, _L("Filament"), m_weight_label), 1, wxEXPAND);
    stats_col->Add(make_info_row(stats_host, _L("Sliced for"), m_target_printer_label), 1, wxEXPAND);
    stats_host->SetSizer(stats_col);
    preview_body->Add(stats_host, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    m_preview_card->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_preview_card->GetSizer()->Add(preview_body, 1, wxEXPAND | wxALL, FromDIP(12));

    main_sizer->AddSpacer(FromDIP(12));

    // Printer selection
    auto *printer_card = make_card(this, kCardBackground);
    printer_card->SetBorderWidth(0);
    auto *printer_card_outer = new wxBoxSizer(wxHORIZONTAL);
    printer_card_outer->AddSpacer(margin);
    printer_card_outer->Add(printer_card, 1, wxEXPAND);
    printer_card_outer->AddSpacer(margin);
    main_sizer->Add(printer_card_outer, 0, wxEXPAND);

    auto *printer_sizer = new wxBoxSizer(wxVERTICAL);
    auto *printer_title_row = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_title = new wxStaticText(printer_card, wxID_ANY, _L("Printer"));
    style_primary_text(printer_title, true);
    printer_title_row->Add(printer_title, 0, wxALIGN_CENTER_VERTICAL);
    printer_title_row->AddStretchSpacer();
    m_refresh_button = new Button(printer_card, _L("Refresh"));
    m_refresh_button->SetMinSize(wxSize(FromDIP(72), FromDIP(26)));
    apply_dark_secondary_button(m_refresh_button);
    printer_title_row->Add(m_refresh_button, 0, wxALIGN_CENTER_VERTICAL);
    printer_sizer->Add(printer_title_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *printer_row = new wxBoxSizer(wxHORIZONTAL);
    printer_row->AddSpacer(FromDIP(12));
    m_printer_combo = new ComboBox(printer_card, wxID_ANY, wxEmptyString, wxDefaultPosition,
        wxSize(-1, FromDIP(30)), 0, nullptr, wxCB_READONLY);
    apply_dark_combo(m_printer_combo);
    printer_row->Add(m_printer_combo, 1, wxALIGN_CENTER_VERTICAL);
    printer_row->AddSpacer(FromDIP(8));
    for (int i = 0; i < 4; ++i) {
        auto *swatch = new PrinterToolSwatch(printer_card);
        swatch->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
        swatch->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));
        m_printer_tool_swatches[i] = swatch;
        printer_row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    }
    printer_row->AddSpacer(FromDIP(12));
    printer_sizer->Add(printer_row, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_printer_status = new wxStaticText(printer_card, wxID_ANY, wxEmptyString);
    printer_sizer->Add(m_printer_status, 0, wxALL, FromDIP(12));
    printer_card->SetSizer(printer_sizer);

    main_sizer->AddSpacer(FromDIP(12));

    // Filament mapping section stays visible even when no printer is selected.
    auto *mapping_card = make_card(this, kCardBackground);
    mapping_card->SetBorderWidth(0);
    auto *mapping_outer = new wxBoxSizer(wxHORIZONTAL);
    mapping_outer->AddSpacer(margin);
    mapping_outer->Add(mapping_card, 1, wxEXPAND);
    mapping_outer->AddSpacer(margin);
    main_sizer->Add(mapping_outer, 0, wxEXPAND);

    auto *mapping_sizer = new wxBoxSizer(wxVERTICAL);
    auto *mapping_title = new wxStaticText(mapping_card, wxID_ANY, _L("Filament"));
    style_primary_text(mapping_title, true);
    mapping_sizer->Add(mapping_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    mapping_sizer->AddSpacer(FromDIP(8));

    auto *mapping_row = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < 4; ++i) {
        m_filament_slots[i] = new StartPrintFilamentSlot(mapping_card, i);
        m_filament_slots[i]->bind_tool_pick_handler([this](int model_slot, wxWindow *anchor) {
            show_tool_picker_for_slot(model_slot, anchor);
        });
        mapping_row->Add(m_filament_slots[i], 0, wxALIGN_CENTER_VERTICAL | (i > 0 ? wxLEFT : 0),
            i > 0 ? FromDIP(8) : 0);
    }
    mapping_sizer->Add(mapping_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_filament_hint = new wxStaticText(mapping_card, wxID_ANY, _L("Click the bottom row to change tool mapping."));
    m_filament_hint->SetForegroundColour(Ui::accent());
    wxFont hint_font = m_filament_hint->GetFont();
    hint_font.SetPointSize(9);
    m_filament_hint->SetFont(hint_font);
    mapping_sizer->Add(m_filament_hint, 0, wxALL, FromDIP(12));
    mapping_card->SetSizer(mapping_sizer);

    main_sizer->AddSpacer(FromDIP(10));

    m_options_section = new wxPanel(this, wxID_ANY);
    m_options_section->SetBackgroundColour(kPageBackground);
    auto *options_outer_sizer = new wxBoxSizer(wxVERTICAL);

    // Print options
    auto *options_card = make_card(m_options_section, kCardBackground);
    options_card->SetBorderWidth(0);
    auto *options_outer = new wxBoxSizer(wxHORIZONTAL);
    options_outer->AddSpacer(margin);
    options_outer->Add(options_card, 1, wxEXPAND);
    options_outer->AddSpacer(margin);
    options_outer_sizer->Add(options_outer, 0, wxEXPAND);

    auto *options_row = new wxBoxSizer(wxHORIZONTAL);
    options_row->Add(make_option_row(options_card, _L("Bed leveling"), m_bed_leveling), 0, wxRIGHT, FromDIP(18));
    options_row->Add(make_option_row(options_card, _L("Flow calibration"), m_flow_calibration), 0, wxRIGHT, FromDIP(18));
    options_row->Add(make_option_row(options_card, _L("Timelapse"), m_timelapse), 0);
    options_card->SetSizer(new wxBoxSizer(wxVERTICAL));
    options_card->GetSizer()->Add(options_row, 0, wxALL, FromDIP(10));

    m_bed_leveling->SetValue(true);
    m_timelapse->SetValue(false);
    m_flow_calibration->SetValue(false);

    m_options_section->SetSizer(options_outer_sizer);
    main_sizer->Add(m_options_section, 0, wxEXPAND);

    main_sizer->AddStretchSpacer();

    auto *footer = new wxBoxSizer(wxHORIZONTAL);
    footer->AddStretchSpacer();
    m_cancel_button = new Button(this, _L("Cancel"));
    m_cancel_button->SetMinSize(wxSize(FromDIP(100), FromDIP(34)));
    apply_dark_secondary_button(m_cancel_button);
    m_start_button = new Button(this, _L("Start Print"));
    m_start_button->SetMinSize(wxSize(FromDIP(120), FromDIP(34)));
    apply_dark_primary_button(m_start_button);
    footer->Add(m_cancel_button, 0, wxRIGHT, FromDIP(10));
    footer->Add(m_start_button, 0, wxRIGHT, margin);
    main_sizer->Add(footer, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

    SetSizer(main_sizer);
    SetMinSize(wxSize(FromDIP(540), FromDIP(580)));
}

void StartPrintDialog::bind_events()
{
    m_refresh_button->Bind(wxEVT_BUTTON, &StartPrintDialog::on_refresh_printers, this);
    m_printer_combo->Bind(wxEVT_COMBOBOX, &StartPrintDialog::on_printer_changed, this);
    m_cancel_button->Bind(wxEVT_BUTTON, &StartPrintDialog::on_cancel, this);
    m_start_button->Bind(wxEVT_BUTTON, &StartPrintDialog::on_start_print, this);
    m_refresh_timer.Bind(wxEVT_TIMER, &StartPrintDialog::on_timer, this);

    m_task_name_edit_button->Bind(wxEVT_BUTTON, &StartPrintDialog::on_task_name_edit, this);
    m_task_name_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { on_task_name_enter(); });
    m_task_name_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
        if (!m_task_name_input->HasFocus() && !m_task_name_label->HasFocus())
            on_task_name_enter();
        else
            event.Skip();
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
        if (event.GetKeyCode() == WXK_ESCAPE && m_is_rename_mode) {
            m_is_rename_mode = false;
            m_task_name_switch_panel->SetSelection(0);
            m_task_name_label->SetLabel(m_current_task_name);
            m_task_name_normal_panel->Layout();
            return;
        }
        event.Skip();
    });
}

void StartPrintDialog::on_task_name_edit(wxCommandEvent &event)
{
    (void) event;
    m_is_rename_mode = true;
    m_task_name_input->GetTextCtrl()->SetValue(m_current_task_name);
    m_task_name_switch_panel->SetSelection(1);
    m_task_name_input->GetTextCtrl()->SetFocus();
    m_task_name_input->GetTextCtrl()->SetInsertionPointEnd();
}

void StartPrintDialog::on_task_name_enter()
{
    if (!m_is_rename_mode)
        return;
    m_is_rename_mode = false;

    wxString new_file_name = m_task_name_input->GetTextCtrl()->GetValue();

    wxString temp;
    int      space_run = 0;
    for (auto ch : new_file_name) {
        if (ch == wxString::FromUTF8("\x20")) {
            ++space_run;
            if (space_run == 1)
                temp += ch;
        } else {
            space_run = 0;
            temp += ch;
        }
    }
    new_file_name = temp;

    enum { Valid, NoValid };
    int      valid_type = Valid;
    wxString info_line;

    const char *unusable_symbols = "<>[]:/\\|?*\"";
    const std::string unusable_suffix = PresetCollection::get_suffix_modified();
    for (size_t i = 0; i < std::strlen(unusable_symbols); ++i) {
        if (new_file_name.find_first_of(unusable_symbols[i]) != wxString::npos) {
            info_line  = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            valid_type = NoValid;
            break;
        }
    }

    if (valid_type == Valid && new_file_name.find(from_u8(unusable_suffix)) != wxString::npos) {
        info_line  = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        valid_type = NoValid;
    }

    if (valid_type == Valid && new_file_name.empty()) {
        info_line  = _L("The name is not allowed to be empty.");
        valid_type = NoValid;
    }

    if (valid_type == Valid && new_file_name.find_first_of(' ') == 0) {
        info_line  = _L("The name is not allowed to start with space character.");
        valid_type = NoValid;
    }

    if (valid_type == Valid && new_file_name.find_last_of(' ') == new_file_name.length() - 1) {
        info_line  = _L("The name is not allowed to end with space character.");
        valid_type = NoValid;
    }

    if (valid_type == Valid && new_file_name.size() >= 100) {
        info_line  = _L("The name length exceeds the limit.");
        valid_type = NoValid;
    }

    if (valid_type != Valid) {
        MessageDialog msg_window(this, info_line, "", wxICON_WARNING | wxOK);
        msg_window.ShowModal();
        m_task_name_switch_panel->SetSelection(0);
        m_task_name_label->SetLabel(m_current_task_name);
        m_task_name_normal_panel->Layout();
        return;
    }

    m_current_task_name = new_file_name;
    m_task_name_switch_panel->SetSelection(0);
    m_task_name_label->SetLabel(m_current_task_name);
    m_task_name_normal_panel->Layout();
}

void StartPrintDialog::prepare(int print_plate_idx)
{
    m_print_plate_idx = print_plate_idx;
}

void StartPrintDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    if (m_refresh_button) {
        m_refresh_button->Rescale();
        apply_dark_secondary_button(m_refresh_button);
    }
    if (m_cancel_button) {
        m_cancel_button->Rescale();
        apply_dark_secondary_button(m_cancel_button);
        m_cancel_button->SetBackgroundColour(kPageBackground);
    }
    if (m_start_button) {
        m_start_button->Rescale();
        apply_dark_primary_button(m_start_button);
        m_start_button->SetBackgroundColour(kPageBackground);
    }
    if (m_task_name_edit_button)
        m_task_name_edit_button->Rescale();
    if (m_task_name_input)
        m_task_name_input->Rescale();
    Fit();
    Refresh();
    (void) suggested_rect;
}

int StartPrintDialog::ShowModal()
{
    reset_print_options();

    if (m_plater) {
        PartPlate *plate = plate_for_dialog(m_plater, m_print_plate_idx);
        if (plate && pick_plate_thumbnail(plate) == nullptr)
            m_plater->update_all_plate_thumbnails(false);
    }
    refresh_from_plate();
    refresh_printer_list();
    m_user_mapped_tools = false;
    m_filament_sync_done = false;
    if (printer_view_has_loaded_filaments()) {
        m_filament_sync_done = true;
        assign_tools_by_color();
        update_printer_status();
    }
    update_start_button_state();
    sync_filaments_then_map(true);
    Layout();
    Fit();
    CenterOnParent();
    m_refresh_timer.Start(kRefreshIntervalMs);
    const int result = DPIDialog::ShowModal();
    m_refresh_timer.Stop();
    return result;
}

void StartPrintDialog::reset_print_options()
{
    if (m_bed_leveling)
        m_bed_leveling->SetValue(true);
    if (m_flow_calibration)
        m_flow_calibration->SetValue(false);
    if (m_timelapse)
        m_timelapse->SetValue(false);
}

void StartPrintDialog::refresh_from_plate()
{
    if (m_plater == nullptr)
        return;

    PartPlate *plate = plate_for_dialog(m_plater, m_print_plate_idx);

    wxString filename = m_plater->get_export_gcode_filename(wxEmptyString, true);
    if (filename.empty())
        filename = _L("Untitled");
    m_current_task_name = wxFileName(filename).GetFullName();
    if (m_task_name_label != nullptr)
        m_task_name_label->SetLabel(m_current_task_name);
    if (m_task_name_switch_panel != nullptr)
        m_task_name_switch_panel->SetSelection(0);
    m_is_rename_mode = false;

    if (m_target_printer_label != nullptr) {
        try {
            auto *preset_bundle = wxGetApp().preset_bundle;
            if (preset_bundle != nullptr) {
                const auto &edited_preset = preset_bundle->printers.get_edited_preset();
                // Safely access the name field
                std::string preset_name;
                try {
                    if (!edited_preset.name.empty()) {
                        preset_name = edited_preset.name;
                    }
                } catch (...) {
                    preset_name.clear();
                }
                
                if (!preset_name.empty()) {
                    m_target_printer_label->SetLabel(from_u8(preset_name));
                } else {
                    m_target_printer_label->SetLabel(_L("Unknown"));
                }
            } else {
                m_target_printer_label->SetLabel(_L("Unknown"));
            }
        } catch (...) {
            m_target_printer_label->SetLabel(_L("Unknown"));
        }
    }

    wxString time_label = _L("—");
    double   total_weight = 0.0;
    if (plate != nullptr) {
        if (plate->get_slice_result()) {
            const auto &stats = plate->get_slice_result()->print_statistics;
            const float slice_time = stats.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
            if (slice_time > 0.f)
                time_label = from_u8(short_time(get_time_dhms(slice_time)));
        }

        if (Print *plate_print = plate->fff_print()) {
            const PrintStatistics &print_stats = plate_print->print_statistics();
            if (print_stats.total_weight > 0.0)
                total_weight = print_stats.total_weight;
            if (time_label == _L("—") && !print_stats.estimated_normal_print_time.empty())
                time_label = from_u8(print_stats.estimated_normal_print_time);
        }

        wxWindow *thumb_host = m_thumbnail_panel ? m_thumbnail_panel->GetParent() : nullptr;
        apply_plate_thumbnail(thumb_host, m_thumbnail_panel, m_thumbnail_placeholder, plate, 90);
    } else {
        apply_plate_thumbnail(nullptr, m_thumbnail_panel, m_thumbnail_placeholder, nullptr, 90);
    }

    char weight_buf[64];
    if (wxGetApp().app_config->get("use_inches") == "1")
        ::sprintf(weight_buf, "%.2f oz", total_weight * 0.035274);
    else
        ::sprintf(weight_buf, "%.2f g", total_weight);

    if (m_time_label != nullptr)
        m_time_label->SetLabel(time_label);
    if (m_weight_label != nullptr)
        m_weight_label->SetLabel(wxString::FromUTF8(weight_buf));

    // Önce tüm slot'ları gizle
    for (int i = 0; i < 4; ++i) {
        if (m_filament_slots[i] != nullptr)
            m_filament_slots[i]->set_visible(false);
    }

    const std::vector<FilamentInfo> filaments = filament_rows_for_plate(plate);
    int ui_slot = 0;
    for (const FilamentInfo &info : filaments) {
        if (ui_slot >= 4)
            break;
        if (m_filament_slots[ui_slot] == nullptr)
            break;

        std::string display_type = info.type;
        if (info.type == "PLA-S")
            display_type = "Sup.PLA";
        else if (info.type == "PA-S")
            display_type = "Sup.PA";
        else if (info.type == "ABS-S")
            display_type = "Sup.ABS";

        m_filament_slots[ui_slot]->set_model_slot_index(info.id);
        m_filament_slots[ui_slot]->set_model_filament(display_type, parse_filament_colour(info.color));
        m_filament_slots[ui_slot]->set_mapped_tool(ui_slot + 1);
        m_filament_slots[ui_slot]->set_visible(true);
        ++ui_slot;
    }

    if (m_filament_slots[0] != nullptr && m_filament_slots[0]->GetParent() != nullptr)
        m_filament_slots[0]->GetParent()->Layout();
}

void StartPrintDialog::refresh_printer_list()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager)
        dev_manager->start_refresher();

    const std::string previous_id = selected_machine_id();

    m_printer_combo->Clear();
    m_printer_ids.clear();
    std::vector<std::pair<std::string, wxString>> entries;

    if (dev_manager) {
        const auto append_machines = [&](const std::map<std::string, MachineObject *> &machines) {
            for (const auto &it : machines) {
                if (it.second == nullptr)
                    continue;
                const auto already_added = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
                    return entry.first == it.first;
                });
                if (already_added != entries.end())
                    continue;
                entries.emplace_back(it.first, format_printer_label(it.second));
            }
        };
        append_machines(dev_manager->get_my_machine_list());
        append_machines(dev_manager->get_local_machinelist());
    }

    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.second.CmpNoCase(b.second) < 0;
    });

    int selected_index = wxNOT_FOUND;
    for (size_t i = 0; i < entries.size(); ++i) {
        m_printer_ids.push_back(entries[i].first);
        m_printer_combo->Append(entries[i].second);
        if (!previous_id.empty() && entries[i].first == previous_id)
            selected_index = static_cast<int>(i);
    }

    MachineObject *current = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (selected_index == wxNOT_FOUND && current != nullptr) {
        const std::string current_id = current->get_dev_id();
        for (size_t i = 0; i < m_printer_ids.size(); ++i) {
            if (m_printer_ids[i] == current_id) {
                selected_index = static_cast<int>(i);
                break;
            }
        }
    }

    if (selected_index != wxNOT_FOUND)
        m_printer_combo->SetSelection(selected_index);
    else if (!m_printer_ids.empty())
        selected_index = 0;

    if (selected_index != wxNOT_FOUND && selected_index < static_cast<int>(m_printer_ids.size()))
        m_printer_combo->SetSelection(selected_index);

    if (dev_manager && selected_index != wxNOT_FOUND && selected_index < static_cast<int>(m_printer_ids.size()))
        dev_manager->set_selected_machine(m_printer_ids[selected_index]);
}

MachineObject *StartPrintDialog::selected_machine() const
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager == nullptr)
        return nullptr;

    int selection = m_printer_combo->GetSelection();
    if (selection < 0 && !m_printer_ids.empty())
        selection = 0;
    if (selection >= 0 && selection < static_cast<int>(m_printer_ids.size()))
        return find_machine_by_id(dev_manager, m_printer_ids[selection]);

    return dev_manager->get_selected_machine();
}

std::string StartPrintDialog::selected_machine_id() const
{
    MachineObject *obj = selected_machine();
    return obj ? obj->get_dev_id() : std::string();
}

void StartPrintDialog::refresh_filament_printer_sides()
{
    try {
        MachineObject *obj = selected_machine();
        const bool printer_connected = obj != nullptr && obj->is_online();
        for (int i = 0; i < 4; ++i) {
            if (m_filament_slots[i] == nullptr || !m_filament_slots[i]->IsShown())
                continue;
            m_filament_slots[i]->set_interactive(printer_connected);
            const int tool = m_filament_slots[i]->get_mapped_tool();
            if (!printer_connected) {
                m_filament_slots[i]->update_printer_tool(wxNullColour, false, tool);
                continue;
            }
            try {
                const PrinterToolInfo info = query_printer_tool(obj, tool - 1);
                m_filament_slots[i]->update_printer_tool(info.color, info.has_filament, tool);
            } catch (...) {
                m_filament_slots[i]->update_printer_tool(wxNullColour, false, tool);
            }
        }
    } catch (...) {
        // Silently fail if the whole function crashes
    }
    update_filament_mapping_hint();
    update_start_button_state();
}

bool StartPrintDialog::has_unloaded_mapping() const
{
    MachineObject *obj = selected_machine();
    for (int i = 0; i < 4; ++i) {
        if (m_filament_slots[i] == nullptr || !m_filament_slots[i]->IsShown())
            continue;
        if (obj == nullptr)
            return true;
        const PrinterToolInfo info = query_printer_tool(obj, m_filament_slots[i]->get_mapped_tool() - 1);
        if (!info.has_filament)
            return true;
    }
    return false;
}

void StartPrintDialog::update_filament_mapping_hint()
{
    if (m_filament_hint == nullptr)
        return;

    MachineObject *obj = selected_machine();
    if (obj == nullptr || !obj->is_online()) {
        m_filament_hint->SetLabel(_L("Connect a printer to map filaments."));
        m_filament_hint->SetForegroundColour(kTextMuted);
        return;
    }

    if (!m_filament_sync_done) {
        m_filament_hint->SetLabel(_L("Loading printer filaments..."));
        m_filament_hint->SetForegroundColour(kTextMuted);
        return;
    }

    PartPlate *plate = plate_for_dialog(m_plater, m_print_plate_idx);
    if (!plate_ready_for_device_print(plate)) {
        m_filament_hint->SetLabel(plate && plate->is_slice_result_valid()
            ? _L("G-code file is missing. Please slice again.")
            : _L("Slice the plate before starting a print."));
        m_filament_hint->SetForegroundColour(Ui::danger());
        return;
    }
    if (plate && !plate->is_slice_result_ready_for_print()) {
        m_filament_hint->SetLabel(_L("There are slicing warnings. Review them before printing."));
        m_filament_hint->SetForegroundColour(ui_warning());
        return;
    }
    if (has_unloaded_mapping()) {
        m_filament_hint->SetLabel(_L("Empty filament (EF). Map each color to a loaded tool before starting."));
        m_filament_hint->SetForegroundColour(ui_warning());
        return;
    }
    m_filament_hint->SetLabel(_L("Click the bottom row to change tool mapping."));
    m_filament_hint->SetForegroundColour(Ui::accent());
}

void StartPrintDialog::assign_tools_by_color()
{
    std::vector<int> visible;
    for (int i = 0; i < 4; ++i) {
        if (m_filament_slots[i] != nullptr && m_filament_slots[i]->IsShown())
            visible.push_back(i);
    }

    MachineObject *obj = selected_machine();
    if (obj == nullptr) {
        for (int slot : visible)
            m_filament_slots[slot]->set_mapped_tool(slot + 1);
        return;
    }

    std::array<PrinterToolInfo, 4> tools{};
    for (int t = 0; t < 4; ++t)
        tools[t] = query_printer_tool(obj, t);

    bool used[4] = {false, false, false, false};

    auto tool_loaded = [&](int t) {
        return tools[t].has_filament && tools[t].color.IsOk();
    };

    auto pick_best = [&](int slot, bool match_type, bool loaded_only) {
        const wxColour model_c = m_filament_slots[slot]->model_color();
        const wxString model_type = m_filament_slots[slot]->model_type();
        int best = -1;
        int best_d = std::numeric_limits<int>::max();
        for (int t = 0; t < 4; ++t) {
            if (used[t])
                continue;
            if (loaded_only && !tool_loaded(t))
                continue;
            if (!loaded_only && tool_loaded(t))
                continue;
            if (match_type && !model_type.empty() && !filament_types_match(model_type, tools[t].material))
                continue;
            const int d = model_c.IsOk() && tool_loaded(t) ? colour_distance_sq(model_c, tools[t].color) : t;
            if (d < best_d) {
                best_d = d;
                best = t;
            }
        }
        return best;
    };

    for (int slot : visible) {
        int best = pick_best(slot, true, true);
        if (best < 0)
            best = pick_best(slot, false, true);
        if (best < 0)
            best = pick_best(slot, false, false);
        if (best < 0)
            best = slot;
        used[best] = true;
        m_filament_slots[slot]->set_mapped_tool(best + 1);
    }
}

void StartPrintDialog::sync_filaments_then_map(bool remap)
{
    wxWeakRef<StartPrintDialog> weak(this);
    auto after = [weak, remap]() {
        StartPrintDialog *dlg = weak.get();
        if (dlg == nullptr)
            return;
        dlg->m_filament_sync_done = true;
        if (remap && !dlg->m_user_mapped_tools)
            dlg->assign_tools_by_color();
        dlg->refresh_filament_printer_sides();
        dlg->update_printer_status();
        dlg->update_start_button_state();
    };

    MachineObject *obj = selected_machine();
    if (MainFrame *frame = wxGetApp().mainframe) {
        if (PrinterWebView *printer_view = frame->m_printer_view) {
            printer_view->sync_loaded_tool_filaments(obj, after);
            return;
        }
    }
    after();
}

void StartPrintDialog::show_tool_picker_for_slot(int model_slot, wxWindow *anchor)
{
    if (model_slot < 0 || model_slot >= 4 || anchor == nullptr)
        return;

    MachineObject *obj = selected_machine();
    if (obj == nullptr)
        return;

    sync_printer_tool_colours(obj);

    wxString required_type;
    int current_tool = 0;
    for (StartPrintFilamentSlot *slot : m_filament_slots) {
        if (slot != nullptr && slot->IsShown() && slot->model_slot_index() == model_slot) {
            required_type = slot->model_type();
            current_tool  = slot->get_mapped_tool();
            break;
        }
    }

    auto *popup = new StartPrintToolPickerPopup(this, obj, required_type, current_tool, [this, model_slot](int tool) {
        for (StartPrintFilamentSlot *slot : m_filament_slots) {
            if (slot == nullptr || !slot->IsShown() || slot->model_slot_index() != model_slot)
                continue;
            slot->set_mapped_tool(tool);
            m_user_mapped_tools = true;
            break;
        }
        refresh_filament_printer_sides();
    });

    const wxPoint pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().y + anchor->FromDIP(4)));
    popup->Position(pos, wxSize(0, 0));
    popup->Popup();
}

void StartPrintDialog::update_printer_status()
{
    MachineObject *obj = selected_machine();
    
    // Yazıcı yoksa durum mesajını göster ve çık
    if (obj == nullptr) {
        // Yazıcı tool swatches'lerini varsayılan renge ayarla
        for (int i = 0; i < 4; ++i) {
            if (m_printer_tool_swatches[i] == nullptr)
                continue;
            m_printer_tool_swatches[i]->set_tool(wxNullColour, false);
        }
        
        m_printer_status->SetLabel(_L("No printer selected."));
        m_printer_status->SetForegroundColour(Ui::danger());
        return;
    }
    
    // Yazıcı varsa tool bilgilerini güncelle
    if (m_filament_sync_done) {
        for (int i = 0; i < 4; ++i) {
            if (m_printer_tool_swatches[i] == nullptr)
                continue;
            const PrinterToolInfo info = query_printer_tool(obj, i);
            m_printer_tool_swatches[i]->set_tool(info.color, info.has_filament);
        }
        refresh_filament_printer_sides();
    }

    if (!obj->is_online()) {
        m_printer_status->SetLabel(_L("Printer is offline."));
        m_printer_status->SetForegroundColour(Ui::danger());
        return;
    }

    if (obj->is_in_printing()) {
        m_printer_status->SetLabel(_L("Printer is busy, unable to initiate printing"));
        m_printer_status->SetForegroundColour(ui_warning());
        return;
    }

    m_printer_status->SetLabel(_L("Printer is ready."));
    m_printer_status->SetForegroundColour(Ui::accent());
}

void StartPrintDialog::update_start_button_state()
{
    PartPlate *plate = plate_for_dialog(m_plater, m_print_plate_idx);
    MachineObject *obj = selected_machine();
    if (m_start_button == nullptr)
        return;
    const bool slice_ready = plate_ready_for_device_print(plate);
    const bool printer_connected = obj && obj->is_online();
    const bool printer_ready = printer_connected && !obj->is_in_printing();
    const bool mapping_ready = m_filament_sync_done && !has_unloaded_mapping();
    const bool can_start = slice_ready && printer_ready && mapping_ready;
    m_start_button->Enable(can_start);
    if (m_options_section != nullptr && m_options_section->IsShown() != printer_connected) {
        m_options_section->Show(printer_connected);
        Layout();
        Refresh();
    }
    if (m_bed_leveling)
        m_bed_leveling->Enable(printer_ready);
    if (m_timelapse)
        m_timelapse->Enable(printer_ready);
    if (m_flow_calibration)
        m_flow_calibration->Enable(printer_ready);
}

void StartPrintDialog::on_refresh_printers(wxCommandEvent &)
{
    refresh_printer_list();
    update_printer_status();
    update_start_button_state();
}

void StartPrintDialog::on_printer_changed(wxCommandEvent &)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    const int selection = m_printer_combo->GetSelection();
    if (dev_manager && selection >= 0 && selection < static_cast<int>(m_printer_ids.size()))
        dev_manager->set_selected_machine(m_printer_ids[selection]);
    m_user_mapped_tools = false;
    m_filament_sync_done = false;
    sync_filaments_then_map(true);
}

void StartPrintDialog::on_cancel(wxCommandEvent &)
{
    EndModal(wxID_CANCEL);
}

void StartPrintDialog::on_start_print(wxCommandEvent &)
{
    PartPlate *plate = plate_for_dialog(m_plater, m_print_plate_idx);
    if (!plate_ready_for_device_print(plate)) {
        show_error(this, _L("Slice the plate before starting a print."));
        return;
    }

    MachineObject *obj = selected_machine();
    if (obj == nullptr || !obj->is_online() || obj->is_in_printing()) {
        show_error(this, _L("Selected printer is not ready."));
        return;
    }

    if (has_unloaded_mapping()) {
        show_error(this, _L("Empty filament (EF). Map each color to a loaded tool before starting."));
        return;
    }

    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager)
        dev_manager->set_selected_machine(obj->get_dev_id());

    NetworkAgent *agent = wxGetApp().getAgent();
    if (agent == nullptr) {
        show_error(this, _L("Printer network agent is not available."));
        return;
    }

    agent->connect_printer(obj->get_dev_id(), obj->get_dev_ip(), "", obj->get_access_code(), obj->local_use_ssl);

    for (int i = 0; i < 4; ++i) {
        if (!m_filament_slots[i]->IsShown())
            continue;
        send_tool_map_sync(obj, m_filament_slots[i]->model_slot_index(), m_filament_slots[i]->get_mapped_tool());
    }

    PrintParams params;
    params.dev_id           = obj->get_dev_id();
    params.dev_ip           = obj->get_dev_ip();
    params.dev_name         = obj->get_dev_name();
    params.connection_type  = obj->connection_type();
    params.dst_file         = plate->get_gcode_filename();
    params.task_name        = m_task_name_label->GetLabel().utf8_string();
    params.project_name     = params.task_name;
    params.plate_index      = m_print_plate_idx + 1;
    params.task_bed_leveling     = m_bed_leveling->GetValue();
    params.task_flow_cali        = m_flow_calibration->GetValue();
    params.task_record_timelapse = m_timelapse->GetValue();
    params.password              = obj->get_access_code();
    params.use_ssl_for_ftp       = obj->local_use_ssl_for_ftp;
    params.use_ssl_for_mqtt      = obj->local_use_ssl;

    m_start_button->Enable(false);
    m_cancel_button->Enable(false);

    std::thread([agent, params, weak_dlg = wxWeakRef<StartPrintDialog>(this)]() {
        const int result = agent->start_local_print(params, nullptr, nullptr);
        wxGetApp().CallAfter([weak_dlg, result]() {
            StartPrintDialog *dlg = weak_dlg.get();
            if (dlg == nullptr)
                return;
            if (result != 0)
                show_error(dlg, _L("Failed to start print. Check the printer connection and try again."));
            dlg->EndModal(result == 0 ? wxID_OK : wxID_CANCEL);
        });
    }).detach();
}

void StartPrintDialog::on_timer(wxTimerEvent &)
{
    update_printer_status();
    update_start_button_state();
}

}} // namespace Slic3r::GUI
