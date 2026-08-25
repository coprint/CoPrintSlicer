#include "PrinterStatusPanel.hpp"

#include "../DeviceUiStyle.hpp"
#include "../StatusPresetPopups.hpp"
#include "../../Widgets/StaticBox.hpp"
#include "../../wxExtensions.hpp"
#include "libslic3r/Utils.hpp"
#ifdef __APPLE__
#include "../../../Utils/MacDarkMode.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/cursor.h>
#include <wx/filename.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

void set_status_font(wxWindow *win)
{
    if (win == nullptr)
        return;
    wxFont font = win->GetFont();
    font.SetPointSize(12);
    font.SetWeight(wxFONTWEIGHT_NORMAL);
    win->SetFont(font);
}

bool set_label_if_changed(wxStaticText *label, const wxString &text)
{
    if (label == nullptr || label->GetLabelText() == text)
        return false;
    label->SetLabelText(text);
    return true;
}

wxBitmap load_dashboard_icon(wxWindow *parent, const std::string &name, int dip_w, int dip_h)
{
    wxImage image;
    const wxString path = wxString::FromUTF8(Slic3r::var(name + ".png").c_str());
    const bool loaded = wxFileName::FileExists(path) && image.LoadFile(path, wxBITMAP_TYPE_PNG) && image.IsOk()
        && image.GetWidth() > 0 && image.GetHeight() > 0;
    if (!loaded) {
        wxBitmap svg = create_scaled_bitmap(name, parent, std::max(dip_w, dip_h));
        if (svg.IsOk())
            return svg;
        return wxBitmap(parent->FromDIP(dip_w), parent->FromDIP(dip_h));
    }

    double scale = 1.0;
#ifdef __APPLE__
    scale = std::max(1.0, mac_max_scaling_factor());
#elif defined(__WXMSW__)
    scale = std::max(1.0, parent->GetDPIScaleFactor());
#endif

    const int logical_w = std::max(1, parent->FromDIP(dip_w));
    const int logical_h = std::max(1, parent->FromDIP(dip_h));
#ifdef __APPLE__
    const int dst_w = std::max(1, static_cast<int>(std::lround(logical_w * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::lround(logical_h * scale)));
#else
    const int dst_w = logical_w;
    const int dst_h = logical_h;
#endif
    image.Rescale(dst_w, dst_h, wxIMAGE_QUALITY_HIGH);

#ifdef __APPLE__
    return wxBitmap(image, -1, scale);
#else
    wxBitmap bmp(image);
#ifdef __WXMSW__
    bmp.SetScaleFactor(scale);
#endif
    return bmp;
#endif
}

wxBitmap load_forward_icon(wxWindow *parent)
{
    const wxString path = wxString::FromUTF8(Slic3r::var("forwardicon.png").c_str());
    if (wxFileName::FileExists(path))
        return load_dashboard_icon(parent, "forwardicon", 15, 15);
    return create_scaled_bitmap("mall_control_forward", parent, 15);
}

wxSize glyph_size(wxWindow *win, const wxString &text)
{
    wxCoord w = 0;
    wxCoord h = 0;
    win->GetTextExtent(text, &w, &h);
    return wxSize(std::max(1, w), std::max(1, h));
}

void pin_label(wxStaticText *label)
{
    if (label == nullptr)
        return;
    const wxString text = label->GetLabelText().IsEmpty() ? wxString::FromUTF8("0") : label->GetLabelText();
    const wxSize size = glyph_size(label, text);
    label->SetMinSize(size);
    label->SetMaxSize(wxSize(-1, size.GetHeight()));
}

wxSize temp_slot_size(wxWindow *win)
{
    // Size for three digits so values like 29 / 220 are not clipped. GetTextExtent
    // can under-report before the control is shown, so keep a DIP floor.
    const wxSize glyph = glyph_size(win, wxString::FromUTF8("888"));
    const int width = std::max(glyph.GetWidth(), win->FromDIP(36)) + win->FromDIP(4);
    const int height = std::max(glyph.GetHeight(), win->FromDIP(18));
    return wxSize(width, height);
}

void pin_icon(wxStaticBitmap *icon, int dip_w, int dip_h)
{
    if (icon == nullptr)
        return;
    const wxSize size(icon->FromDIP(dip_w), icon->FromDIP(dip_h));
    icon->SetMinSize(size);
    icon->SetMaxSize(size);
}

struct TempSlotWidgets {
    wxPanel *     host{nullptr};
    wxStaticText *label{nullptr};
    wxTextCtrl *  input{nullptr};
};

TempSlotWidgets make_temp_slot(wxWindow *parent, bool editable)
{
    TempSlotWidgets slot;
    slot.host = new wxPanel(parent, wxID_ANY);
    slot.host->SetBackgroundColour(DeviceUiStyle::card_background());

    slot.label = new wxStaticText(slot.host, wxID_ANY, wxString::FromUTF8("--"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    slot.label->SetForegroundColour(DeviceUiStyle::text_primary());
    slot.label->SetBackgroundColour(DeviceUiStyle::card_background());
    set_status_font(slot.label);

    const wxSize size = temp_slot_size(slot.label);
    slot.label->SetMinSize(size);
    slot.label->SetMaxSize(size);
    slot.host->SetMinSize(size);
    slot.host->SetMaxSize(size);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer(1);
    sizer->Add(slot.label, 0, wxALIGN_CENTER);
    sizer->AddStretchSpacer(1);
    slot.host->SetSizer(sizer);

    if (editable) {
        slot.input = new wxTextCtrl(slot.host, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxTE_PROCESS_ENTER | wxTE_CENTRE | wxBORDER_NONE);
        slot.input->SetBackgroundColour(*wxWHITE);
        slot.input->SetForegroundColour(DeviceUiStyle::text_primary());
        slot.input->SetMaxLength(3);
        set_status_font(slot.input);
        slot.input->Hide();
        slot.host->Bind(wxEVT_SIZE, [input = slot.input](wxSizeEvent &event) {
            event.Skip();
            if (input == nullptr || !input->IsShown())
                return;
            const wxSize host_size = event.GetSize();
            input->SetSize(0, 0, host_size.GetWidth(), host_size.GetHeight());
        });
    }
    return slot;
}

wxWindow *make_temp_cell(wxWindow *parent, PrinterStatusPanel::TempView &view,
    const std::string &icon_name, int icon_w, int icon_h)
{
    auto *cell = new wxPanel(parent, wxID_ANY);
    cell->SetBackgroundColour(DeviceUiStyle::card_background());

    view.icon = new wxStaticBitmap(cell, wxID_ANY, load_dashboard_icon(cell, icon_name, icon_w, icon_h));
    pin_icon(view.icon, icon_w, icon_h);

    auto *texts = new wxPanel(cell, wxID_ANY);
    texts->SetBackgroundColour(DeviceUiStyle::card_background());

    const TempSlotWidgets current_slot = make_temp_slot(texts, false);
    auto *slash = new wxStaticText(texts, wxID_ANY, wxString::FromUTF8("/"));
    slash->SetForegroundColour(DeviceUiStyle::text_primary());
    slash->SetBackgroundColour(DeviceUiStyle::card_background());
    set_status_font(slash);
    pin_label(slash);

    auto *target_hit = new wxPanel(texts, wxID_ANY);
    target_hit->SetBackgroundColour(DeviceUiStyle::card_background());
    const wxCursor hand(wxCURSOR_HAND);
    target_hit->SetCursor(hand);
    const TempSlotWidgets target_slot = make_temp_slot(target_hit, true);
    target_slot.host->SetCursor(hand);
    target_slot.label->SetCursor(hand);
    auto *unit = new wxStaticText(target_hit, wxID_ANY, wxString::FromUTF8("\xC2\xB0""C"));
    unit->SetForegroundColour(DeviceUiStyle::text_primary());
    unit->SetBackgroundColour(DeviceUiStyle::card_background());
    unit->SetCursor(hand);
    set_status_font(unit);
    pin_label(unit);
    auto *target_row = new wxBoxSizer(wxHORIZONTAL);
    target_row->Add(target_slot.host, 0, wxALIGN_CENTER_VERTICAL);
    target_row->Add(unit, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, cell->FromDIP(2));
    target_hit->SetSizer(target_row);

    view.temp_current = current_slot.label;
    view.temp_slash = slash;
    view.temp_target = target_slot.label;
    view.temp_input = target_slot.input;
    view.temp_unit = unit;
    view.temp_target_hit = target_hit;

    auto *inner = new wxBoxSizer(wxHORIZONTAL);
    inner->Add(current_slot.host, 0, wxALIGN_CENTER_VERTICAL);
    inner->Add(slash, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, cell->FromDIP(2));
    inner->Add(target_hit, 0, wxALIGN_CENTER_VERTICAL);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->AddStretchSpacer(1);
    outer->Add(inner, 0, wxALIGN_LEFT);
    outer->AddStretchSpacer(1);
    texts->SetSizer(outer);

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(view.icon, 0, wxALIGN_CENTER_VERTICAL);
    row->AddSpacer(cell->FromDIP(5));
    row->Add(texts, 0, wxALIGN_CENTER_VERTICAL);
    cell->SetSizer(row);
    const int row_h = std::max(cell->FromDIP(icon_h), current_slot.host->GetMinSize().GetHeight());
    cell->SetMinSize(wxSize(-1, row_h));
    return cell;
}

wxWindow *make_action_cell(wxWindow *parent, const std::string &icon_name, int icon_w, int icon_h)
{
    auto *cell = new wxPanel(parent, wxID_ANY);
    cell->SetBackgroundColour(DeviceUiStyle::card_background());
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *icon = new wxStaticBitmap(cell, wxID_ANY, load_dashboard_icon(cell, icon_name, icon_w, icon_h));
    pin_icon(icon, icon_w, icon_h);
    auto *forward = new wxStaticBitmap(cell, wxID_ANY, load_forward_icon(cell));
    pin_icon(forward, 15, 15);
    const wxCursor hand(wxCURSOR_HAND);
    cell->SetCursor(hand);
    icon->SetCursor(hand);
    forward->SetCursor(hand);
    row->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
    row->AddSpacer(cell->FromDIP(5));
    row->Add(forward, 0, wxALIGN_CENTER_VERTICAL);
    cell->SetSizer(row);
    return cell;
}

} // namespace

PrinterStatusPanel::PrinterStatusPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::card_background());

    auto *frame = new StaticBox(this, wxID_ANY);
    frame->SetCornerRadius(FromDIP(8));
    frame->SetBorderWidth(1);
    frame->SetBorderColorNormal(wxColour(0xDF, 0xDF, 0xDF));
    frame->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    frame->SetBackgroundColour(DeviceUiStyle::card_background());

    auto *content = new wxBoxSizer(wxVERTICAL);
    const int cell_gap = FromDIP(12);
    for (int i = 0; i < MaxDashboardTools; ++i) {
        wxWindow *cell = make_temp_cell(frame, m_tools[i], "nozzleimg_" + std::to_string(i + 1), 21, 21);
        bind_temp_edit(m_tools[i], i);
        content->Add(cell, 0, wxALIGN_LEFT | (i > 0 ? wxTOP : 0), cell_gap);
    }

    wxWindow *bed_cell = make_temp_cell(frame, m_bed, "cp_bed_heating", 21, 21);
    bind_temp_edit(m_bed, -1);
    m_fan_cell = make_action_cell(frame, "cp_tool_fan", 21, 21);
    m_speed_cell = make_action_cell(frame, "speedimg", 21, 21);
    content->Add(bed_cell, 0, wxALIGN_LEFT | wxTOP, cell_gap);
    content->Add(m_fan_cell, 0, wxALIGN_LEFT | wxTOP, cell_gap);
    content->Add(m_speed_cell, 0, wxALIGN_LEFT | wxTOP, cell_gap);

    auto *padded = new wxBoxSizer(wxVERTICAL);
    padded->AddSpacer(FromDIP(22));
    padded->Add(content, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(9));
    padded->AddSpacer(FromDIP(22));
    frame->SetSizer(padded);

    auto *root = new wxBoxSizer(wxVERTICAL);
    root->Add(frame, 0, wxALIGN_LEFT);
    SetSizer(root);

    const auto bind_action = [](wxWindow *cell, std::function<void()> fn) {
        auto invoke = [cell, fn](wxMouseEvent &) {
            if (cell == nullptr || !cell->IsEnabled())
                return;
            fn();
        };
        cell->Bind(wxEVT_LEFT_DOWN, invoke);
        for (wxWindow *child : cell->GetChildren())
            child->Bind(wxEVT_LEFT_DOWN, invoke);
    };
    bind_action(m_fan_cell, [this]() { open_fan_popup(); });
    bind_action(m_speed_cell, [this]() { open_speed_popup(); });

    m_speed_popup = new PrintSpeedPopup(this);
    m_speed_popup->set_change_handler([this](int percent) {
        if (!m_speed_enabled)
            return;
        m_print_speed_percent = percent;
        if (m_print_speed_handler)
            m_print_speed_handler(percent);
    });
    m_fan_popup = new FanSpeedPopup(this);
    m_fan_popup->set_change_handler([this](int tool, int percent) {
        if (tool >= 0 && tool < MaxDashboardTools)
            m_fan_percent[tool] = percent;
        if (m_fan_speed_handler)
            m_fan_speed_handler(tool, percent);
    });
    set_speed_enabled(false);
}

void PrinterStatusPanel::bind_temp_edit(TempView &view, int tool_index)
{
    if (view.temp_target == nullptr || view.temp_input == nullptr)
        return;

    const auto bind_click = [this, tool_index](wxWindow *win) {
        if (win == nullptr)
            return;
        win->Bind(wxEVT_LEFT_DOWN, [this, win, tool_index](wxMouseEvent &) {
            if (win == nullptr || !win->IsEnabled())
                return;
            begin_target_edit(tool_index);
        });
    };
    bind_click(view.temp_target_hit);
    bind_click(view.temp_target);
    bind_click(view.temp_unit);
    bind_click(view.temp_input->GetParent());

    view.temp_input->Bind(wxEVT_TEXT_ENTER, [this, tool_index](wxCommandEvent &) { end_target_edit(tool_index, true); });
    view.temp_input->Bind(wxEVT_KILL_FOCUS, [this, tool_index](wxFocusEvent &event) {
        event.Skip();
        end_target_edit(tool_index, true);
    });
    view.temp_input->Bind(wxEVT_KEY_DOWN, [this, tool_index](wxKeyEvent &event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            end_target_edit(tool_index, false);
            return;
        }
        event.Skip();
    });
}

void PrinterStatusPanel::open_fan_popup()
{
    if (m_fan_popup == nullptr)
        return;
    m_fan_popup->set_percents(m_fan_percent);
    m_fan_popup->popup_at(m_fan_cell);
}

void PrinterStatusPanel::open_speed_popup()
{
    if (m_speed_popup == nullptr)
        return;
    m_speed_popup->set_enabled(m_speed_enabled);
    m_speed_popup->set_percent(m_print_speed_percent);
    m_speed_popup->popup_at(m_speed_cell);
}

void PrinterStatusPanel::set_speed_enabled(bool enabled)
{
    if (m_speed_enabled == enabled)
        return;
    m_speed_enabled = enabled;
    if (m_speed_popup != nullptr)
        m_speed_popup->set_enabled(enabled);
}

void PrinterStatusPanel::apply_state(const std::array<ToolState, MaxDashboardTools> &tools, const BedState &bed,
    int print_speed_percent, bool speed_enabled)
{
    bool layout_needed = false;
    int active_tool = -1;
    m_print_speed_percent = print_speed_percent;
    set_speed_enabled(speed_enabled);

    for (int i = 0; i < MaxDashboardTools; ++i) {
        const ToolState &tool = tools[i];
        if (tool.active)
            active_tool = i;
        m_tools[i].temp_available = tool.nozzle.available;
        m_tools[i].temp_last_target = static_cast<int>(tool.nozzle.target);
        m_fan_percent[i] = tool.fan.available ? tool.fan.percent : 0;
        layout_needed |= set_label_if_changed(m_tools[i].temp_current, temp_slot_text(tool.nozzle.available, tool.nozzle.current));
        if (!m_tools[i].temp_editing)
            layout_needed |= set_label_if_changed(m_tools[i].temp_target, temp_slot_text(tool.nozzle.available, tool.nozzle.target));
    }

    m_bed.temp_available = bed.temperature.available;
    m_bed.temp_last_target = static_cast<int>(bed.temperature.target);
    layout_needed |= set_label_if_changed(m_bed.temp_current, temp_slot_text(bed.temperature.available, bed.temperature.current));
    if (!m_bed.temp_editing)
        layout_needed |= set_label_if_changed(m_bed.temp_target, temp_slot_text(bed.temperature.available, bed.temperature.target));

    if (layout_needed) {
        Freeze();
        Layout();
        Thaw();
    }

    if (active_tool >= 0)
        set_active_tool(active_tool);
}

void PrinterStatusPanel::set_active_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    m_active_tool = tool_index;
}

void PrinterStatusPanel::set_tool_select_handler(ToolSelectHandler handler) { m_tool_select_handler = std::move(handler); }
void PrinterStatusPanel::set_nozzle_temp_handler(NozzleTempHandler handler) { m_nozzle_temp_handler = std::move(handler); }
void PrinterStatusPanel::set_fan_speed_handler(FanSpeedHandler handler)     { m_fan_speed_handler = std::move(handler); }
void PrinterStatusPanel::set_bed_temp_handler(BedTempHandler handler)       { m_bed_temp_handler = std::move(handler); }
void PrinterStatusPanel::set_print_speed_handler(PrintSpeedHandler handler) { m_print_speed_handler = std::move(handler); }

wxString PrinterStatusPanel::temp_slot_text(bool available, double value)
{
    if (!available)
        return wxString::FromUTF8("--");
    return wxString::Format("%d", static_cast<int>(value));
}

void PrinterStatusPanel::cancel_all_temp_edits()
{
    for (int i = 0; i < MaxDashboardTools; ++i)
        end_target_edit(i, false);
    end_target_edit(-1, false);
}

void PrinterStatusPanel::begin_target_edit(int tool_index)
{
    cancel_all_temp_edits();

    TempView *view = tool_index < 0 ? &m_bed : (tool_index < MaxDashboardTools ? &m_tools[tool_index] : nullptr);
    if (view == nullptr || view->temp_target == nullptr || view->temp_input == nullptr)
        return;

    view->temp_editing = true;
    view->temp_input->ChangeValue(view->temp_available ? wxString::Format("%d", view->temp_last_target) : wxString());
    view->temp_target->Hide();
    view->temp_input->Show();
    if (wxWindow *host = view->temp_input->GetParent()) {
        const wxSize host_size = host->GetClientSize();
        view->temp_input->SetSize(0, 0, host_size.GetWidth(), host_size.GetHeight());
        host->Layout();
    }
    Layout();
    view->temp_input->SetFocus();
    view->temp_input->SelectAll();
}

void PrinterStatusPanel::end_target_edit(int tool_index, bool commit)
{
    TempView *view = tool_index < 0 ? &m_bed : (tool_index < MaxDashboardTools ? &m_tools[tool_index] : nullptr);
    if (view == nullptr || view->temp_target == nullptr || view->temp_input == nullptr || !view->temp_editing)
        return;

    wxString raw = view->temp_input->GetValue();
    view->temp_editing = false;
    view->temp_input->Hide();
    view->temp_target->Show();
    if (wxWindow *host = view->temp_target->GetParent())
        host->Layout();
    Layout();

    if (!commit)
        return;
    raw.Trim(true);
    raw.Trim(false);
    long value = 0;
    if (!raw.ToLong(&value) || static_cast<int>(value) == view->temp_last_target)
        return;
    if (tool_index < 0) {
        if (m_bed_temp_handler)
            m_bed_temp_handler(static_cast<int>(value));
    } else if (m_nozzle_temp_handler) {
        m_nozzle_temp_handler(tool_index, static_cast<int>(value));
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
