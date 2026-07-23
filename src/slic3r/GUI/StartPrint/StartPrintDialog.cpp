#include "StartPrintDialog.hpp"

#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../PrinterWebView.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../DeviceManager.hpp"
#include "../DeviceCore/DevManager.h"
#include "../DeviceCore/DevFilaSystem.h"
#include "../DeviceDashboard/DeviceUiStyle.hpp"
#include "../DeviceDashboard/FilamentTrackSlot.hpp"
#include "../wxExtensions.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <algorithm>
#include <thread>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/popupwin.h>
#include <wx/statbmp.h>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

using Ui = DeviceDashboard::DeviceUiStyle;
using DeviceDashboard::FilamentTrackSlot;

constexpr int kRefreshIntervalMs = 2000;

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

std::vector<FilamentInfo> filament_rows_for_plate(PartPlate *plate)
{
    if (plate != nullptr && !plate->get_slice_filaments_info().empty())
        return plate->get_slice_filaments_info();

    std::vector<FilamentInfo> rows;
    auto *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return rows;

    const auto *color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const auto *type_opt  = preset_bundle->full_config().option<ConfigOptionStrings>("filament_type");
    if (color_opt == nullptr)
        return rows;

    const size_t count = std::min<size_t>(4, color_opt->values.size());
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        FilamentInfo info;
        info.id    = static_cast<int>(i);
        info.color = color_opt->values[i];
        if (type_opt && i < type_opt->values.size())
            info.type = type_opt->values[i];
        rows.push_back(std::move(info));
    }
    return rows;
}

/** tray_id is 0-based physical tool index; UI shows T1..T4 (1-based). */
int physical_tool_for_filament(int model_slot, const FilamentInfo &info)
{
    const int slot = std::clamp(model_slot, 0, 3);
    if (info.mapping_result != MAPPING_RESULT_DEFAULT && info.tray_id >= 0 && info.tray_id <= 3)
        return info.tray_id + 1;
    if (info.tray_id >= 0 && info.tray_id <= 3 && info.tray_id == info.id)
        return info.tray_id + 1;
    return slot + 1;
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

class RoundedColorBlock : public wxPanel
{
public:
    explicit RoundedColorBlock(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(Ui::card_background());
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(Ui::card_background()));
            dc.Clear();

            wxGCDC gc(dc);
            const wxSize size = GetClientSize();
            if (size.x <= 0 || size.y <= 0)
                return;

            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(m_fill));
            gc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(8));
        });
    }

    void set_fill(const wxColour &fill)
    {
        m_fill = fill.IsOk() ? fill : Ui::control_background();
        Refresh();
    }

private:
    wxColour m_fill{Ui::control_background()};
};

struct PrinterToolInfo {
    wxColour  color{Ui::control_background()};
    wxString  material;
    bool      has_filament{false};
};

PrinterToolInfo query_printer_tool(MachineObject *obj, int tool_0based)
{
    PrinterToolInfo info;
    if (obj == nullptr || tool_0based < 0 || tool_0based > 3)
        return info;

    const std::string tray_id = std::to_string(tool_0based);
    const std::string type    = obj->get_filament_type("0", tray_id);
    if (!type.empty()) {
        info.has_filament = true;
        wxString display  = from_u8(obj->get_filament_display_type("0", tray_id));
        info.material     = display.empty() ? from_u8(type) : display;
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

void style_primary_text(wxStaticText *label, bool bold = false)
{
    if (label == nullptr)
        return;
    label->SetForegroundColour(Ui::text_primary());
    if (bold) {
        wxFont font = label->GetFont();
        font.SetWeight(wxFONTWEIGHT_BOLD);
        label->SetFont(font);
    }
}

void style_muted_text(wxStaticText *label)
{
    if (label != nullptr)
        label->SetForegroundColour(Ui::text_muted());
}

StaticBox *make_card(wxWindow *parent, const wxColour &bg, int radius_dip = -1)
{
    auto *card = new StaticBox(parent, wxID_ANY);
    const int radius = radius_dip < 0 ? Ui::card_radius() : radius_dip;
    card->SetCornerRadius(parent->FromDIP(radius));
    card->SetBorderWidth(Ui::card_border_width());
    card->SetBorderColorNormal(Ui::card_border());
    card->SetBackgroundColorNormal(bg);
    card->SetBackgroundColour(bg);
    return card;
}

wxPanel *make_mini_tool_pick_track(wxWindow *parent, int tool_1based, const PrinterToolInfo &info,
    int cell_h_dip, int track_w_dip, std::function<void()> on_pick)
{
    const int cell_h  = parent->FromDIP(cell_h_dip);
    const int track_w = parent->FromDIP(track_w_dip);

    const bool filled = info.has_filament && info.color.IsOk();
    auto *track = new FilamentTrackSlot(parent, tool_1based, info.color, filled);
    track->SetMinSize(wxSize(track_w, cell_h));
    track->SetMaxSize(wxSize(track_w, cell_h));

    const auto pick = [on_pick](wxMouseEvent &event) {
        event.Skip(false);
        on_pick();
    };
    track->Bind(wxEVT_LEFT_DOWN, pick);

    return track;
}

class StartPrintToolPickerPopup : public wxPopupTransientWindow
{
public:
    using PickHandler = std::function<void(int tool_1based)>;

    StartPrintToolPickerPopup(wxWindow *parent, MachineObject *obj, PickHandler on_pick)
        : wxPopupTransientWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_on_pick(std::move(on_pick))
    {
        SetBackgroundColour(Ui::page_background());

        const int pad         = FromDIP(12);
        const int gap         = FromDIP(10);
        const int track_w_dip = 64;
        const int cell_h_dip  = 80;

        auto *outer = new wxBoxSizer(wxVERTICAL);
        auto *frame = make_card(this, Ui::card_background(), 8);
        outer->Add(frame, 0, wxEXPAND | wxALL, pad);

        auto *grid = new wxFlexGridSizer(2, gap, gap);
        const std::array<int, 4> tool_order{{0, 2, 1, 3}};
        for (const int tool_0based : tool_order) {
            const int tool = tool_0based + 1;
            const PrinterToolInfo tool_info = query_printer_tool(obj, tool_0based);
            auto *track = make_mini_tool_pick_track(frame, tool, tool_info, cell_h_dip, track_w_dip,
                [this, tool]() {
                    if (m_on_pick)
                        m_on_pick(tool);
                    Dismiss();
                });
            grid->Add(track);
        }

        auto *frame_sizer = new wxBoxSizer(wxVERTICAL);
        frame_sizer->Add(grid, 0, wxALL, pad);
        frame->SetSizer(frame_sizer);
        SetSizerAndFit(outer);
    }

private:
    PickHandler m_on_pick;
};

void apply_dark_combo(ComboBox *combo)
{
    if (combo == nullptr)
        return;
    const wxColour bg       = Ui::control_background();
    const wxColour bg_hover = wxColour(52, 56, 62);
    combo->SetCornerRadius(combo->FromDIP(6));
    combo->SetBorderColor(StateColor(
        std::make_pair(Ui::card_border(), (int) StateColor::Disabled),
        std::make_pair(Ui::accent(), (int) StateColor::Hovered),
        std::make_pair(Ui::card_border(), (int) StateColor::Normal)));
    combo->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(35, 38, 44), (int) StateColor::Disabled),
        std::make_pair(bg_hover, (int) StateColor::Focused),
        std::make_pair(bg, (int) StateColor::Normal)));
    combo->SetLabelColor(StateColor(
        std::make_pair(Ui::text_muted(), (int) StateColor::Disabled),
        std::make_pair(Ui::text_primary(), (int) StateColor::Normal)));
}

void apply_dark_secondary_button(Button *btn)
{
    if (btn == nullptr)
        return;
    btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);
    btn->SetCornerRadius(btn->FromDIP(10));
    btn->SetBackgroundColour(Ui::page_background());
    const wxColour bg = Ui::control_background();
    btn->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(35, 38, 44), (int) StateColor::Disabled),
        std::make_pair(wxColour(52, 56, 62), (int) StateColor::Pressed),
        std::make_pair(wxColour(52, 56, 62), (int) StateColor::Hovered),
        std::make_pair(bg, (int) StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::make_pair(Ui::card_border(), (int) StateColor::Disabled),
        std::make_pair(Ui::card_border(), (int) StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::make_pair(Ui::text_muted(), (int) StateColor::Disabled),
        std::make_pair(Ui::text_primary(), (int) StateColor::Normal)));
}

void apply_dark_primary_button(Button *btn)
{
    if (btn == nullptr)
        return;
    btn->SetStyle(ButtonStyle::Confirm, ButtonType::Expanded);
    btn->SetCornerRadius(btn->FromDIP(10));
    btn->SetBackgroundColour(Ui::page_background());
    const wxColour accent = Ui::accent();
    btn->SetBackgroundColor(StateColor(
        std::make_pair(wxColour(35, 38, 44), (int) StateColor::Disabled),
        std::make_pair(wxColour(36, 150, 104), (int) StateColor::Pressed),
        std::make_pair(wxColour(52, 196, 140), (int) StateColor::Hovered),
        std::make_pair(accent, (int) StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::make_pair(Ui::card_border(), (int) StateColor::Disabled),
        std::make_pair(accent, (int) StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::make_pair(Ui::text_muted(), (int) StateColor::Disabled),
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
    row->SetBackgroundColour(Ui::card_background());
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    checkbox = new CheckBox(row);
    checkbox->SetBackgroundColour(Ui::card_background());
    auto *label = new wxStaticText(row, wxID_ANY, label_text);
    style_primary_text(label);
    sizer->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(6));
    sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
    row->SetSizer(sizer);
    return row;
}

} // namespace

StartPrintFilamentSlot::StartPrintFilamentSlot(wxWindow *parent, int model_slot_index)
    : wxPanel(parent, wxID_ANY)
    , m_model_slot_index(model_slot_index)
    , m_mapped_tool(model_slot_index + 1)
{
    SetBackgroundColour(Ui::page_background());

    const int half_h   = parent->FromDIP(40);
    const int half_gap = parent->FromDIP(4);

    auto *root = new wxBoxSizer(wxHORIZONTAL);
    SetSizer(root);

    m_row_card = make_card(this, Ui::card_background(), 8);
    m_row_card->SetMinSize(wxSize(-1, half_h * 2 + half_gap));
    m_row_card->SetBorderWidth(1);
    m_row_card->SetBorderColorNormal(Ui::card_border());

    auto *col = new wxBoxSizer(wxVERTICAL);

    m_model_half = new RoundedColorBlock(m_row_card);
    m_model_half->SetMinSize(wxSize(-1, half_h));

    auto *model_inner = new wxBoxSizer(wxHORIZONTAL);
    m_model_type = new wxStaticText(m_model_half, wxID_ANY, wxEmptyString);
    wxFont type_font = m_model_type->GetFont();
    type_font.SetPointSize(9);
    type_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_type->SetFont(type_font);
    model_inner->AddStretchSpacer();
    model_inner->Add(m_model_type, 0, wxALIGN_CENTER_VERTICAL);
    model_inner->AddStretchSpacer();
    m_model_half->SetSizer(model_inner);

    m_printer_half = new RoundedColorBlock(m_row_card);
    m_printer_half->SetMinSize(wxSize(-1, half_h));
    m_printer_half->SetCursor(wxCursor(wxCURSOR_HAND));

    auto *printer_inner = new wxBoxSizer(wxHORIZONTAL);
    m_printer_filament_icon = new wxStaticBitmap(m_printer_half, wxID_ANY,
        create_scaled_bitmap("start_print_filament_spool", m_printer_half, 18));
    m_printer_tag = new wxStaticText(m_printer_half, wxID_ANY, wxEmptyString);
    m_printer_type = new wxStaticText(m_printer_half, wxID_ANY, wxEmptyString);
    wxFont tag_font = m_printer_tag->GetFont();
    tag_font.SetPointSize(9);
    tag_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_printer_tag->SetFont(tag_font);
    m_printer_type->SetFont(type_font);
    m_printer_filament_icon->SetCursor(wxCursor(wxCURSOR_HAND));
    m_printer_tag->SetCursor(wxCursor(wxCURSOR_HAND));
    m_printer_type->SetCursor(wxCursor(wxCURSOR_HAND));
    printer_inner->AddStretchSpacer();
    printer_inner->Add(m_printer_filament_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(5));
    printer_inner->Add(m_printer_tag, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(4));
    printer_inner->Add(m_printer_type, 0, wxALIGN_CENTER_VERTICAL);
    printer_inner->AddStretchSpacer();
    m_printer_half->SetSizer(printer_inner);

    const auto bind_printer_click = [this](wxWindow *win) {
        win->Bind(wxEVT_LEFT_DOWN, &StartPrintFilamentSlot::on_printer_half_clicked, this);
    };
    bind_printer_click(m_printer_half);
    bind_printer_click(m_printer_filament_icon);
    bind_printer_click(m_printer_tag);
    bind_printer_click(m_printer_type);

    col->Add(m_model_half, 1, wxEXPAND | wxBOTTOM, half_gap);
    col->Add(m_printer_half, 1, wxEXPAND);
    m_row_card->SetSizer(col);

    root->Add(m_row_card, 1, wxEXPAND);

    const wxColour placeholder = wxColour(120, 120, 120);
    style_half(m_model_half, nullptr, m_model_type, placeholder, wxEmptyString, _L("PLA"));
    style_half(m_printer_half, m_printer_tag, m_printer_type, Ui::control_background(),
        wxString::Format(wxString::FromUTF8("▼ T%d"), m_mapped_tool), _L("Empty"));
}

void StartPrintFilamentSlot::style_half(wxPanel *half, wxStaticText *tag, wxStaticText *type_label,
    const wxColour &bg, const wxString &tag_text, const wxString &type_text)
{
    if (half == nullptr)
        return;
    if (auto *block = dynamic_cast<RoundedColorBlock *>(half))
        block->set_fill(bg);
    const wxColour text = readable_on_fill(bg);
    if (tag != nullptr) {
        if (tag_text.empty())
            tag->Hide();
        else {
            tag->SetLabel(tag_text);
            tag->SetForegroundColour(text);
            tag->SetBackgroundColour(bg);
            tag->Show();
        }
    }
    if (m_printer_filament_icon != nullptr && half == m_printer_half)
        m_printer_filament_icon->SetBackgroundColour(bg);
    if (type_label != nullptr) {
        type_label->SetLabel(type_text);
        type_label->SetForegroundColour(text);
        type_label->SetBackgroundColour(bg);
    }
    half->Refresh();
}

void StartPrintFilamentSlot::set_visible(bool visible)
{
    Show(visible);
}

void StartPrintFilamentSlot::set_model_filament(const std::string &type, const wxColour &color)
{
    const wxString type_text = from_u8(type.empty() ? "PLA" : type);
    style_half(m_model_half, nullptr, m_model_type, color, wxEmptyString, type_text);
}

void StartPrintFilamentSlot::set_mapped_tool(int mapped_tool)
{
    m_mapped_tool = std::clamp(mapped_tool, 1, 4);
    if (m_printer_tag != nullptr)
        m_printer_tag->SetLabel(wxString::Format(wxString::FromUTF8("▼ T%d"), m_mapped_tool));
}

void StartPrintFilamentSlot::update_printer_tool(const wxColour &color, const wxString &material, int tool_1based)
{
    m_mapped_tool = std::clamp(tool_1based, 1, 4);
    const wxColour fill = color.IsOk() ? color : Ui::control_background();
    style_half(m_printer_half, m_printer_tag, m_printer_type, fill,
        wxString::Format(wxString::FromUTF8("▼ T%d"), m_mapped_tool), material);
}

void StartPrintFilamentSlot::bind_tool_pick_handler(ToolPickHandler handler)
{
    m_pick_handler = std::move(handler);
}

void StartPrintFilamentSlot::on_printer_half_clicked(wxMouseEvent &event)
{
    event.Skip(false);
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
    SetBackgroundColour(Ui::page_background());
}

void StartPrintDialog::build_ui()
{
    const int margin = FromDIP(20);
    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(16));

    auto *title = new wxStaticText(this, wxID_ANY, _L("Start Print"));
    style_primary_text(title, true);
    wxFont title_font = title->GetFont();
    title_font.SetPointSize(15);
    title->SetFont(title_font);
    main_sizer->Add(title, 0, wxLEFT | wxRIGHT, margin);

    main_sizer->AddSpacer(FromDIP(6));

    auto *task_row = new wxBoxSizer(wxHORIZONTAL);
    task_row->AddSpacer(margin);
    m_task_label = new wxStaticText(this, wxID_ANY, _L("Printing task:"));
    style_muted_text(m_task_label);
    task_row->Add(m_task_label, 0, wxALIGN_CENTER_VERTICAL);
    task_row->AddSpacer(FromDIP(6));
    m_task_name_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    style_primary_text(m_task_name_label, true);
    task_row->Add(m_task_name_label, 1, wxALIGN_CENTER_VERTICAL);
    task_row->AddSpacer(margin);
    main_sizer->Add(task_row, 0, wxEXPAND);

    main_sizer->AddSpacer(FromDIP(12));

    // Job summary: thumbnail left, print stats right
    m_preview_card = make_card(this, Ui::card_background());
    auto *preview_outer = new wxBoxSizer(wxHORIZONTAL);
    preview_outer->AddSpacer(margin);
    preview_outer->Add(m_preview_card, 1, wxEXPAND);
    preview_outer->AddSpacer(margin);
    main_sizer->Add(preview_outer, 0, wxEXPAND);

    auto *preview_body = new wxBoxSizer(wxHORIZONTAL);
    const int thumb_dip = 144;
    auto *thumb_host = make_card(m_preview_card, Ui::control_background(), 8);
    thumb_host->SetMinSize(wxSize(FromDIP(thumb_dip), FromDIP(thumb_dip)));
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

    auto *stats_col = new wxBoxSizer(wxVERTICAL);
    stats_col->Add(make_info_row(m_preview_card, _L("Time"), m_time_label), 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    stats_col->Add(make_info_row(m_preview_card, _L("Filament"), m_weight_label), 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    stats_col->Add(make_info_row(m_preview_card, _L("Sliced for"), m_target_printer_label), 0, wxEXPAND);
    stats_col->AddStretchSpacer();
    preview_body->Add(stats_col, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    m_preview_card->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_preview_card->GetSizer()->Add(preview_body, 1, wxEXPAND | wxALL, FromDIP(12));

    main_sizer->AddSpacer(FromDIP(12));

    // Filament mapping section
    auto *mapping_card = make_card(this, Ui::card_background());
    auto *mapping_outer = new wxBoxSizer(wxHORIZONTAL);
    mapping_outer->AddSpacer(margin);
    mapping_outer->Add(mapping_card, 1, wxEXPAND);
    mapping_outer->AddSpacer(margin);
    main_sizer->Add(mapping_outer, 0, wxEXPAND);

    auto *mapping_sizer = new wxBoxSizer(wxVERTICAL);
    auto *mapping_title = new wxStaticText(mapping_card, wxID_ANY, _L("Filament Mapping"));
    style_primary_text(mapping_title, true);
    mapping_sizer->Add(mapping_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    mapping_sizer->AddSpacer(FromDIP(8));

    const int grid_gap = FromDIP(20);
    auto *mapping_grid = new wxFlexGridSizer(2, grid_gap, grid_gap);
    mapping_grid->AddGrowableCol(0, 1);
    mapping_grid->AddGrowableCol(1, 1);
    for (int i = 0; i < 4; ++i) {
        m_filament_slots[i] = new StartPrintFilamentSlot(mapping_card, i);
        m_filament_slots[i]->bind_tool_pick_handler([this](int model_slot, wxWindow *anchor) {
            show_tool_picker_for_slot(model_slot, anchor);
        });
        mapping_grid->Add(m_filament_slots[i], 1, wxEXPAND);
    }
    mapping_sizer->Add(mapping_grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_filament_hint = new wxStaticText(mapping_card, wxID_ANY, _L("Click the bottom row to change tool mapping."));
    m_filament_hint->SetForegroundColour(Ui::accent());
    wxFont hint_font = m_filament_hint->GetFont();
    hint_font.SetPointSize(9);
    m_filament_hint->SetFont(hint_font);
    mapping_sizer->Add(m_filament_hint, 0, wxALL, FromDIP(12));
    mapping_card->SetSizer(mapping_sizer);

    main_sizer->AddSpacer(FromDIP(12));

    // Printer selection
    auto *printer_card = make_card(this, Ui::card_background());
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
        auto *swatch = make_card(printer_card, Ui::control_background(), 4);
        swatch->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
        m_printer_tool_swatches[i] = swatch;
        printer_row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    }
    printer_row->AddSpacer(FromDIP(12));
    printer_sizer->Add(printer_row, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_printer_status = new wxStaticText(printer_card, wxID_ANY, wxEmptyString);
    printer_sizer->Add(m_printer_status, 0, wxALL, FromDIP(12));
    printer_card->SetSizer(printer_sizer);

    main_sizer->AddSpacer(FromDIP(10));

    // Print options
    auto *options_card = make_card(this, Ui::card_background());
    auto *options_outer = new wxBoxSizer(wxHORIZONTAL);
    options_outer->AddSpacer(margin);
    options_outer->Add(options_card, 1, wxEXPAND);
    options_outer->AddSpacer(margin);
    main_sizer->Add(options_outer, 0, wxEXPAND);

    auto *options_row = new wxBoxSizer(wxHORIZONTAL);
    options_row->Add(make_option_row(options_card, _L("Bed leveling"), m_bed_leveling), 0, wxRIGHT, FromDIP(18));
    options_row->Add(make_option_row(options_card, _L("Flow calibration"), m_flow_calibration), 0, wxRIGHT, FromDIP(18));
    options_row->Add(make_option_row(options_card, _L("Timelapse"), m_timelapse), 0);
    options_card->SetSizer(new wxBoxSizer(wxVERTICAL));
    options_card->GetSizer()->Add(options_row, 0, wxALL, FromDIP(10));

    m_bed_leveling->SetValue(true);
    m_timelapse->SetValue(false);
    m_flow_calibration->SetValue(false);

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
        m_cancel_button->SetBackgroundColour(Ui::page_background());
    }
    if (m_start_button) {
        m_start_button->Rescale();
        apply_dark_primary_button(m_start_button);
        m_start_button->SetBackgroundColour(Ui::page_background());
    }
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
    sync_printer_tool_colours(selected_machine());
    update_printer_status();
    update_start_button_state();
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
    m_task_name_label->SetLabel(wxFileName(filename).GetFullName());

    const auto &preset_bundle = *wxGetApp().preset_bundle;
    m_target_printer_label->SetLabel(from_u8(preset_bundle.printers.get_edited_preset().name));

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
        apply_plate_thumbnail(thumb_host, m_thumbnail_panel, m_thumbnail_placeholder, plate, 144);
    } else {
        apply_plate_thumbnail(nullptr, m_thumbnail_panel, m_thumbnail_placeholder, nullptr, 144);
    }

    char weight_buf[64];
    if (wxGetApp().app_config->get("use_inches") == "1")
        ::sprintf(weight_buf, "%.2f oz", total_weight * 0.035274);
    else
        ::sprintf(weight_buf, "%.2f g", total_weight);

    m_time_label->SetLabel(time_label);
    m_weight_label->SetLabel(wxString::FromUTF8(weight_buf));

    const std::vector<FilamentInfo> filaments = filament_rows_for_plate(plate);
    for (int i = 0; i < 4; ++i)
        m_filament_slots[i]->set_visible(false);

    for (const auto &info : filaments) {
        const int model_slot = info.id;
        if (model_slot < 0 || model_slot >= 4)
            continue;
        const int mapped_tool = physical_tool_for_filament(model_slot, info);
        m_filament_slots[model_slot]->set_model_filament(
            info.get_display_filament_type(), parse_filament_colour(info.color));
        m_filament_slots[model_slot]->set_mapped_tool(mapped_tool);
        m_filament_slots[model_slot]->set_visible(true);
    }

    refresh_filament_printer_sides();

    if (!plate_ready_for_device_print(plate)) {
        m_filament_hint->SetLabel(plate && plate->is_slice_result_valid()
            ? _L("G-code file is missing. Please slice again.")
            : _L("Slice the plate before starting a print."));
        m_filament_hint->SetForegroundColour(Ui::danger());
    } else if (plate && !plate->is_slice_result_ready_for_print()) {
        m_filament_hint->SetLabel(_L("There are slicing warnings. Review them before printing."));
        m_filament_hint->SetForegroundColour(ui_warning());
    } else {
        m_filament_hint->SetLabel(_L("Click the bottom row to change tool mapping."));
        m_filament_hint->SetForegroundColour(Ui::accent());
    }
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
    MachineObject *obj = selected_machine();
    for (int i = 0; i < 4; ++i) {
        if (m_filament_slots[i] == nullptr || !m_filament_slots[i]->IsShown())
            continue;
        const int tool = m_filament_slots[i]->get_mapped_tool();
        const PrinterToolInfo info = query_printer_tool(obj, tool - 1);
        m_filament_slots[i]->update_printer_tool(info.color, info.material, tool);
    }
}

void StartPrintDialog::show_tool_picker_for_slot(int model_slot, wxWindow *anchor)
{
    if (model_slot < 0 || model_slot >= 4 || anchor == nullptr)
        return;

    MachineObject *obj = selected_machine();
    if (obj == nullptr)
        return;

    sync_printer_tool_colours(obj);

    auto *popup = new StartPrintToolPickerPopup(this, obj, [this, model_slot](int tool) {
        if (model_slot < 0 || model_slot >= 4 || m_filament_slots[model_slot] == nullptr)
            return;
        m_filament_slots[model_slot]->set_mapped_tool(tool);
        refresh_filament_printer_sides();
    });

    const wxPoint pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().y + anchor->FromDIP(4)));
    popup->Position(pos, wxSize(0, 0));
    popup->Popup();
}

void StartPrintDialog::update_printer_status()
{
    MachineObject *obj = selected_machine();
    for (int i = 0; i < 4; ++i) {
        if (m_printer_tool_swatches[i] == nullptr)
            continue;
        const PrinterToolInfo info = query_printer_tool(obj, i);
        const wxColour tint = info.has_filament && info.color.IsOk() ? info.color : Ui::control_background();
        m_printer_tool_swatches[i]->SetBackgroundColorNormal(tint);
        m_printer_tool_swatches[i]->SetBackgroundColour(tint);
        m_printer_tool_swatches[i]->Refresh();
    }

    refresh_filament_printer_sides();

    if (obj == nullptr) {
        m_printer_status->SetLabel(_L("No printer selected."));
        m_printer_status->SetForegroundColour(Ui::danger());
        return;
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
    const bool slice_ready = plate_ready_for_device_print(plate);
    const bool printer_ready = obj && obj->is_online() && !obj->is_in_printing();
    const bool can_start = slice_ready && printer_ready;
    m_start_button->Enable(can_start);
    m_bed_leveling->Enable(printer_ready);
    m_timelapse->Enable(printer_ready);
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
    update_printer_status();
    update_start_button_state();
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
