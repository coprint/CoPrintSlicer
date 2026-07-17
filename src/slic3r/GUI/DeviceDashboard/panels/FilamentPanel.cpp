#include "FilamentPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../FilamentTrackPaint.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

wxColour readable_slot_colour(const wxColour& colour, const wxColour& fallback)
{
    return readable_filament_track_colour(colour, fallback);
}

bool is_valid_loaded_filament_colour(const wxColour& colour)
{
    return colour.IsOk();
}

bool is_empty_filament_material(const wxString& material)
{
    return material.IsEmpty()
        || material.CmpNoCase(wxString::FromUTF8("Empty")) == 0
        || material.CmpNoCase(wxString::FromUTF8("N/A")) == 0;
}

class FilamentToolMapView : public wxPanel
{
public:
    using ToolHandler = std::function<void(int)>;

    explicit FilamentToolMapView(wxWindow* parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(wxSize(FromDIP(620), FromDIP(260)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::page_background());
        Bind(wxEVT_PAINT, &FilamentToolMapView::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &FilamentToolMapView::on_left_down, this);
        Bind(wxEVT_MOTION, &FilamentToolMapView::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            SetCursor(wxCursor(wxCURSOR_ARROW));
            event.Skip();
        });

        m_tool_colors = {
            wxColour(58, 139, 222),
            wxColour(80, 84, 92),
            wxColour(110, 162, 82),
            wxColour(238, 151, 42)
        };
        m_tool_materials = {
            wxString::FromUTF8("PLA"),
            wxString::FromUTF8("Empty"),
            wxString::FromUTF8("PLA"),
            wxString::FromUTF8("PLA")
        };
        reload_icons();
    }

    void set_tool_handler(ToolHandler handler) { m_tool_handler = std::move(handler); }
    void set_configure_handler(ToolHandler handler) { m_configure_handler = std::move(handler); }
    wxPoint last_configure_anchor_screen() const { return m_last_configure_anchor; }

    void apply_state(const FilamentState& state)
    {
        bool changed = false;
        for (int i = 0; i < MaxDashboardTools; ++i) {
            const ToolState& loaded = state.tools[i];

            const bool has_loaded = is_valid_loaded_filament_colour(loaded.color);
            if (m_tool_loaded[i] != has_loaded) {
                m_tool_loaded[i] = has_loaded;
                changed = true;
            }

            wxColour tool_color = has_loaded
                ? readable_slot_colour(loaded.color, m_tool_colors[i])
                : wxColour(80, 84, 92);
            if (m_tool_colors[i] != tool_color) {
                m_tool_colors[i] = tool_color;
                changed = true;
            }

            wxString material = has_loaded && !is_empty_filament_material(loaded.material)
                ? loaded.material
                : wxString::FromUTF8("Empty");
            if (m_tool_materials[i] != material) {
                m_tool_materials[i] = material;
                changed = true;
            }
        }
        if (m_selected_tool != state.selected_tool) {
            m_selected_tool = state.selected_tool;
            changed = true;
        }
        if (changed)
            Refresh();
    }

private:
    bool is_tool_empty(int tool) const
    {
        return !m_tool_loaded[tool];
    }

    wxString tool_material_label(int tool) const
    {
        return is_tool_empty(tool) ? wxString::FromUTF8("Empty") : m_tool_materials[tool];
    }

    int hit_test_tool(const wxPoint& pos) const
    {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (m_card_hit_rects[i].Contains(pos))
                return i;
        }
        return -1;
    }

    int hit_test_track(const wxPoint& pos) const
    {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (m_track_hit_rects[i].Contains(pos))
                return i;
        }
        return -1;
    }

    void on_left_down(wxMouseEvent& event)
    {
        const wxPoint pos = event.GetPosition();
        const int track_tool = hit_test_track(pos);
        if (track_tool >= 0) {
            if (m_configure_handler) {
                m_last_configure_anchor = ClientToScreen(m_track_hit_rects[track_tool].GetPosition() +
                    wxPoint(m_track_hit_rects[track_tool].GetWidth() / 2, m_track_hit_rects[track_tool].GetHeight() / 2));
                m_configure_handler(track_tool);
            }
            return;
        }

        const int tool = hit_test_tool(pos);
        if (tool >= 0) {
            if (m_tool_handler)
                m_tool_handler(tool);
            return;
        }
        event.Skip();
    }

    void on_motion(wxMouseEvent& event)
    {
        const wxPoint pos = event.GetPosition();
        SetCursor((hit_test_track(pos) >= 0 || hit_test_tool(pos) >= 0) ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
        event.Skip();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        const wxSize size = GetClientSize();
        const int canvas_w = FromDIP(610);
        const int canvas_h = FromDIP(250);
        const int ox = std::max(0, (size.x - canvas_w) / 2);
        const int oy = std::max(0, (size.y - canvas_h) / 2);

        const int card_w = FromDIP(66);
        const int card_h = FromDIP(86);
        const int track_w = FromDIP(64);
        const int track_h = FromDIP(88);
        const int card_track_gap = FromDIP(30);
        const int track_body_gap = 0;
        const int body_w = FromDIP(238);
        const int assembly_w = 2 * (card_w + card_track_gap + track_w) + body_w;
        const int layout_start = std::max(0, (canvas_w - assembly_w) / 2);

        const int card_left_x = ox + layout_start;
        const int track_left_x = card_left_x + card_w + card_track_gap;
        const int body_x = track_left_x + track_w + track_body_gap;
        const int track_right_x = body_x + body_w + track_body_gap;
        const int card_right_x = track_right_x + track_w + card_track_gap;

        const wxRect body(body_x, oy + FromDIP(20), body_w, FromDIP(200));
        draw_body(gc.get(), body);

        const wxRect card0(card_left_x, oy + FromDIP(38), card_w, card_h);
        const wxRect track0(track_left_x, oy + FromDIP(37), track_w, track_h);
        const wxRect card1(card_left_x, oy + FromDIP(150), card_w, card_h);
        const wxRect track1(track_left_x, oy + FromDIP(149), track_w, track_h);

        const wxRect track2(track_right_x, oy + FromDIP(37), track_w, track_h);
        const wxRect card2(card_right_x, oy + FromDIP(38), card_w, card_h);
        const wxRect track3(track_right_x, oy + FromDIP(149), track_w, track_h);
        const wxRect card3(card_right_x, oy + FromDIP(150), card_w, card_h);

        const std::array<wxRect, MaxDashboardTools> cards{{card0, card1, card2, card3}};
        const std::array<wxRect, MaxDashboardTools> tracks{{track0, track1, track2, track3}};

        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (is_tool_empty(i))
                draw_empty_track(gc.get(), tracks[i]);
            else
                draw_filled_track(gc.get(), tracks[i], m_tool_colors[i]);
            draw_tool_card(gc.get(), cards[i], i);
            m_card_hit_rects[i] = cards[i];
            m_track_hit_rects[i] = tracks[i];
        }
    }

    void draw_body(wxGraphicsContext* gc, const wxRect& rect)
    {
        const double x = rect.x;
        const double y = rect.y;
        const double w = rect.width;
        const double h = rect.height;
        const double arch_lift = FromDIP(8);

        wxGraphicsPath outer = gc->CreatePath();
        outer.MoveToPoint(x, y + h);
        outer.AddLineToPoint(x, y + FromDIP(30));
        outer.AddQuadCurveToPoint(x + w * 0.5, y - arch_lift, x + w, y + FromDIP(30));
        outer.AddLineToPoint(x + w, y + h);
        outer.CloseSubpath();

        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(42, 45, 48)));
        gc->FillPath(outer);

        const double inset = FromDIP(24);
        wxGraphicsPath inner = gc->CreatePath();
        inner.MoveToPoint(x + inset, y + h - FromDIP(2));
        inner.AddLineToPoint(x + inset, y + FromDIP(42));
        inner.AddQuadCurveToPoint(x + w * 0.5, y + FromDIP(24), x + w - inset, y + FromDIP(42));
        inner.AddLineToPoint(x + w - inset, y + h - FromDIP(2));
        inner.CloseSubpath();
        gc->SetBrush(wxBrush(wxColour(30, 33, 36)));
        gc->FillPath(inner);

        wxGraphicsPath shade = gc->CreatePath();
        shade.MoveToPoint(x + inset, y + FromDIP(42));
        shade.AddLineToPoint(x + inset + FromDIP(20), y + FromDIP(42));
        shade.AddLineToPoint(x + inset + FromDIP(8), y + h - FromDIP(2));
        shade.AddLineToPoint(x + inset, y + h - FromDIP(2));
        shade.CloseSubpath();
        gc->SetBrush(wxBrush(wxColour(34, 37, 40)));
        gc->FillPath(shade);
    }

    wxString tool_short_label(int tool_index) const
    {
        return wxString::Format("T%d", tool_index + 1);
    }

    void draw_tool_card(wxGraphicsContext* gc, const wxRect& rect, int tool)
    {
        const bool selected = tool == m_selected_tool;
        const wxColour bg = selected ? wxColour(205, 206, 207) : wxColour(39, 42, 47);
        const wxColour fg = selected ? wxColour(48, 48, 50) : *wxWHITE;
        const wxColour sub = selected ? wxColour(72, 74, 78) : wxColour(210, 212, 216);

        gc->SetBrush(wxBrush(bg));
        gc->SetPen(wxPen(selected ? wxColour(170, 172, 176) : wxColour(58, 61, 66), FromDIP(1)));
        gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, FromDIP(10));

        draw_text(gc, tool_short_label(tool), wxRect(rect.x, rect.y + FromDIP(14), rect.width, FromDIP(32)), fg, 17, wxFONTWEIGHT_BOLD);
        draw_text(gc, tool_material_label(tool), wxRect(rect.x, rect.y + FromDIP(48), rect.width, FromDIP(22)), sub, 10, wxFONTWEIGHT_SEMIBOLD);
    }

    void reload_icons()
    {
        m_edit_icon_bmp = create_scaled_bitmap("ams_editable_light", this, 18);
        m_add_icon_bmp  = create_scaled_bitmap("add_filament", this, 18);
    }

    void draw_filled_track(wxGraphicsContext* gc, const wxRect& track, const wxColour& color)
    {
        draw_filament_track(gc, track, 0, color, true, FilamentTrackCenter::EditIcon, m_edit_icon_bmp, this);
    }

    void draw_empty_track(wxGraphicsContext* gc, const wxRect& track)
    {
        const wxBitmap empty;
        draw_filament_track(gc, track, 0, wxColour(), false, FilamentTrackCenter::PlusSign, empty, this);
    }

    void draw_text(wxGraphicsContext* gc, const wxString& text, const wxRect& rect, const wxColour& colour, int point_size, wxFontWeight weight)
    {
        wxFont font(wxFontInfo(std::max(1, point_size))
            .Family(wxFONTFAMILY_SWISS)
            .FaceName(wxString::FromUTF8("Bahnschrift"))
            .Weight(weight));
        gc->SetFont(font, colour);
        double tw = 0.0;
        double th = 0.0;
        gc->GetTextExtent(text, &tw, &th);
        gc->DrawText(text, rect.x + std::max(0.0, (rect.width - tw) / 2.0), rect.y + std::max(0.0, (rect.height - th) / 2.0));
    }

    std::array<wxColour, MaxDashboardTools> m_tool_colors;
    std::array<wxString, MaxDashboardTools> m_tool_materials;
    std::array<bool, MaxDashboardTools> m_tool_loaded{};
    std::array<wxRect, MaxDashboardTools> m_card_hit_rects;
    std::array<wxRect, MaxDashboardTools> m_track_hit_rects;
    wxBitmap m_edit_icon_bmp;
    wxBitmap m_add_icon_bmp;
    int m_selected_tool{0};
    wxPoint m_last_configure_anchor{wxDefaultPosition};
    ToolHandler m_tool_handler;
    ToolHandler m_configure_handler;
};

void set_panel_colour(wxWindow* panel, const wxColour& colour)
{
    if (panel == nullptr || panel->GetBackgroundColour() == colour)
        return;

    if (auto* box = dynamic_cast<StaticBox*>(panel))
        box->SetBackgroundColorNormal(colour);

    panel->SetBackgroundColour(colour);
    panel->Refresh();
}

void style_action_button(Button* button)
{
    if (button == nullptr)
        return;
    button->SetMinSize(wxSize(-1, button->FromDIP(40)));
    button->SetCornerRadius(button->FromDIP(8));
    button->SetBorderWidth(0);
    button->SetBackgroundColor(StateColor(
        std::pair(wxColour(52, 55, 62), (int) StateColor::Disabled),
        std::pair(wxColour(55, 58, 66), (int) StateColor::Pressed),
        std::pair(wxColour(78, 82, 91), (int) StateColor::Hovered),
        std::pair(wxColour(65, 68, 75), (int) StateColor::Normal)));
    button->SetTextColor(StateColor(
        std::pair(wxColour(150, 154, 164), (int) StateColor::Disabled),
        std::pair(wxColour(255, 255, 255), (int) StateColor::Pressed),
        std::pair(wxColour(245, 248, 252), (int) StateColor::Hovered),
        std::pair(wxColour(220, 220, 220), (int) StateColor::Normal)));
}

wxBitmap make_white_bitmap_from_png(wxWindow* parent, const char* relative_path, const char* fallback_name, int px)
{
    const wxString path = Slic3r::GUI::from_u8(Slic3r::var(relative_path));
    wxImage image;
    if (!image.LoadFile(path, wxBITMAP_TYPE_PNG) || !image.IsOk())
        return create_scaled_bitmap(fallback_name, parent, px);

    const int size = parent->FromDIP(px);
    image.Rescale(size, size, wxIMAGE_QUALITY_BILINEAR);

    if (image.HasAlpha()) {
        unsigned char* alpha = image.GetAlpha();
        unsigned char* data = image.GetData();
        const int pixels = image.GetWidth() * image.GetHeight();
        for (int i = 0; i < pixels; ++i) {
            if (alpha[i] == 0)
                continue;
            data[i * 3 + 0] = 255;
            data[i * 3 + 1] = 255;
            data[i * 3 + 2] = 255;
        }
    } else {
        image.InitAlpha();
        unsigned char* alpha = image.GetAlpha();
        unsigned char* data = image.GetData();
        const int pixels = image.GetWidth() * image.GetHeight();
        for (int i = 0; i < pixels; ++i) {
            alpha[i] = 255;
            data[i * 3 + 0] = 255;
            data[i * 3 + 1] = 255;
            data[i * 3 + 2] = 255;
        }
    }

    return wxBitmap(image);
}

wxBitmap make_expand_arrow_icon(wxWindow* parent, int px)
{
    return make_white_bitmap_from_png(parent, "images/expandarrow.png", "replace_arrow_down", px);
}

struct PopupManageToolOption {
    int tool_index{0};
    wxString label;
    wxColour color;
};

class ManageToolDropdownPopup final : public wxPopupTransientWindow
{
public:
    using SelectHandler = std::function<void(int)>;
    using DismissHandler = std::function<void()>;

    ManageToolDropdownPopup(wxWindow* parent,
                            const std::array<PopupManageToolOption, MaxDashboardTools>& options,
                            int selected_tool,
                            SelectHandler on_select,
                            DismissHandler on_dismiss)
        : wxPopupTransientWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_on_select(std::move(on_select))
        , m_on_dismiss(std::move(on_dismiss))
    {
        SetBackgroundColour(DeviceUiStyle::page_background());

        auto* root = new wxBoxSizer(wxVERTICAL);
        for (int i = 0; i < MaxDashboardTools; ++i) {
            const auto& option = options[i];
            auto* row = new StaticBox(this, wxID_ANY);
            row->SetMinSize(wxSize(FromDIP(224), FromDIP(48)));
            row->SetCornerRadius(FromDIP(8));
            row->SetBorderWidth(1);
            row->SetBorderColorNormal(option.tool_index == selected_tool ? DeviceUiStyle::accent() : wxColour(58, 61, 68));
            row->SetBackgroundColorNormal(option.tool_index == selected_tool ? wxColour(48, 52, 58) : DeviceUiStyle::control_background());
            row->SetBackgroundColour(DeviceUiStyle::control_background());
            row->SetCursor(wxCursor(wxCURSOR_HAND));

            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* color = new wxPanel(row, wxID_ANY);
            color->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
            color->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));
            color->SetBackgroundColour(option.color.IsOk() ? option.color : *wxWHITE);
            color->SetCursor(wxCursor(wxCURSOR_HAND));
            row_sizer->Add(color, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
            row_sizer->AddSpacer(FromDIP(12));

            auto* label = new wxStaticText(row, wxID_ANY, option.label.IsEmpty() ? wxString::Format("Tool %d", i + 1) : option.label);
            label->SetForegroundColour(DeviceUiStyle::text_primary());
            label->SetCursor(wxCursor(wxCURSOR_HAND));
            {
                wxFont f = label->GetFont();
                f.SetWeight(wxFONTWEIGHT_BOLD);
                label->SetFont(f);
            }
            row_sizer->Add(label, 1, wxALIGN_CENTER_VERTICAL);
            row->SetSizer(row_sizer);

            const auto pick = [this, tool_index = option.tool_index](wxMouseEvent& event) {
                event.Skip(false);
                if (m_on_select)
                    m_on_select(tool_index);
                Dismiss();
            };
            row->Bind(wxEVT_LEFT_DOWN, pick);
            color->Bind(wxEVT_LEFT_DOWN, pick);
            label->Bind(wxEVT_LEFT_DOWN, pick);

            root->Add(row, 0, wxEXPAND);
        }

        SetSizerAndFit(root);
    }

private:
    void OnDismiss() override
    {
        wxPopupTransientWindow::OnDismiss();
        if (m_on_dismiss)
            m_on_dismiss();
    }

    SelectHandler m_on_select;
    DismissHandler m_on_dismiss;
};

FilamentPanel::FilamentPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Filament"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::page_background());

    auto* columns = new wxBoxSizer(wxHORIZONTAL);

    m_tool_map_view = new FilamentToolMapView(m_frame->content_parent());
    m_tool_map_view->set_tool_handler([this](int tool_index) {
        select_manage_tool(tool_index);
    });
    m_tool_map_view->set_configure_handler([this](int tool_index) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::ConfigureFilament;
        command.tool_index = tool_index;
        const wxPoint anchor = m_tool_map_view->last_configure_anchor_screen();
        command.screen_x = anchor.x;
        command.screen_y = anchor.y;
        dispatch(command);
    });
    columns->Add(m_tool_map_view, 1, wxEXPAND | wxRIGHT, FromDIP(18));

    auto* manage = new StaticBox(m_frame->content_parent(), wxID_ANY);
    manage->SetBackgroundColour(DeviceUiStyle::page_background());
    manage->SetBackgroundColorNormal(DeviceUiStyle::page_background());
    manage->SetBorderWidth(0);
    auto* manage_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(manage, wxID_ANY, wxString::FromUTF8("Manage Filament"));
    title->SetForegroundColour(DeviceUiStyle::text_primary());
    {
        wxFont f = title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(f);
    }
    auto* sep = new wxPanel(manage, wxID_ANY);
    sep->SetMinSize(wxSize(-1, FromDIP(1)));
    sep->SetMaxSize(wxSize(-1, FromDIP(1)));
    sep->SetBackgroundColour(DeviceUiStyle::card_border());

    m_selected_tool_box = new StaticBox(manage, wxID_ANY);
    m_selected_tool_box->SetMinSize(wxSize(FromDIP(188), FromDIP(44)));
    m_selected_tool_box->SetCornerRadius(FromDIP(8));
    m_selected_tool_box->SetBorderWidth(1);
    m_selected_tool_box->SetBorderColorNormal(wxColour(70, 73, 80));
    m_selected_tool_box->SetBackgroundColorNormal(DeviceUiStyle::control_background());
    m_selected_tool_box->SetBackgroundColour(DeviceUiStyle::control_background());
    m_selected_tool_box->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* selected_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_selected_tool_dot = new wxPanel(m_selected_tool_box, wxID_ANY);
    m_selected_tool_dot->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_selected_tool_dot->SetBackgroundColour(DeviceUiStyle::accent());
    m_selected_tool_dot->SetCursor(wxCursor(wxCURSOR_HAND));
    selected_sizer->Add(m_selected_tool_dot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    selected_sizer->AddSpacer(FromDIP(8));
    m_selected_tool = new wxStaticText(m_selected_tool_box, wxID_ANY, wxString::FromUTF8("Tool 1"));
    m_selected_tool->SetForegroundColour(DeviceUiStyle::text_primary());
    m_selected_tool->SetCursor(wxCursor(wxCURSOR_HAND));
    {
        wxFont f = m_selected_tool->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_selected_tool->SetFont(f);
    }
    selected_sizer->Add(m_selected_tool, 1, wxALIGN_CENTER_VERTICAL);
    auto* arrow_down = new wxStaticBitmap(m_selected_tool_box, wxID_ANY, make_expand_arrow_icon(m_selected_tool_box, 18));
    arrow_down->SetCursor(wxCursor(wxCURSOR_HAND));
    selected_sizer->Add(arrow_down, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    m_selected_tool_box->SetSizer(selected_sizer);

    const auto open_selected_menu = [this](wxMouseEvent& event) {
        event.Skip(false);
        toggle_selected_tool_dropdown();
    };
    m_selected_tool_box->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    m_selected_tool_dot->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    m_selected_tool->Bind(wxEVT_LEFT_DOWN, open_selected_menu);
    arrow_down->Bind(wxEVT_LEFT_DOWN, open_selected_menu);

    m_load_button = new Button(manage, wxString::FromUTF8("Load"));
    style_action_button(m_load_button);
    m_load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::LoadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    m_unload_button = new Button(manage, wxString::FromUTF8("Unload"));
    style_action_button(m_unload_button);
    m_unload_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::UnloadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    manage_sizer->Add(title, 0, wxLEFT | wxTOP, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(sep, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(12));
    manage_sizer->Add(m_selected_tool_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(10));
    manage_sizer->Add(m_load_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddSpacer(FromDIP(8));
    manage_sizer->Add(m_unload_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    manage_sizer->AddStretchSpacer(1);
    manage->SetSizer(manage_sizer);
    columns->Add(manage, 0, wxEXPAND);

    m_frame->set_content(columns);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void FilamentPanel::apply_state(const FilamentState& state)
{
    if (m_tool_map_view != nullptr)
        m_tool_map_view->apply_state(state);

    for (int i = 0; i < MaxDashboardTools; ++i) {
        m_manage_tool_options[i].tool_index = i;
        m_manage_tool_options[i].label = wxString::Format("Tool %d", i + 1);
        m_manage_tool_options[i].color = is_valid_loaded_filament_colour(state.tools[i].color)
            ? state.tools[i].color
            : wxColour();
    }

    set_selected_tool(state.selected_tool);
    const bool selected_tool_loading = state.is_loading && state.loading_tool == m_selected_tool_index;
    const bool controls_enabled = state.can_load_unload && !state.is_loading;
    if (m_load_button != nullptr) {
        m_load_button->SetLabel(selected_tool_loading ? wxString::FromUTF8("Loading...") : wxString::FromUTF8("Load"));
        m_load_button->Enable(controls_enabled);
    }
    if (m_unload_button != nullptr)
        m_unload_button->Enable(controls_enabled);
}

void FilamentPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void FilamentPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void FilamentPanel::toggle_selected_tool_dropdown()
{
    if (m_selected_tool_box == nullptr)
        return;

    if (m_tool_dropdown_popup != nullptr) {
        m_tool_dropdown_popup->Dismiss();
        m_tool_dropdown_popup = nullptr;
        return;
    }

    std::array<PopupManageToolOption, MaxDashboardTools> options;
    for (int i = 0; i < MaxDashboardTools; ++i) {
        options[i].tool_index = m_manage_tool_options[i].tool_index;
        options[i].label = m_manage_tool_options[i].label.IsEmpty() ? wxString::Format("Tool %d", i + 1) : m_manage_tool_options[i].label;
        options[i].color = m_manage_tool_options[i].color.IsOk()
            ? m_manage_tool_options[i].color
            : *wxWHITE;
    }

    m_tool_dropdown_popup = new ManageToolDropdownPopup(
        this,
        options,
        m_selected_tool_index,
        [this](int tool_index) { select_manage_tool(tool_index); },
        [this]() { m_tool_dropdown_popup = nullptr; });

    const wxPoint selector_screen_pos = m_selected_tool_box->ClientToScreen(wxPoint(0, 0));
    const int popup_x = std::max(0, selector_screen_pos.x - FromDIP(240));
    m_tool_dropdown_popup->SetPosition(wxPoint(popup_x, selector_screen_pos.y));
    m_tool_dropdown_popup->Popup(m_selected_tool_box);
}

void FilamentPanel::select_manage_tool(int tool_index)
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::SelectFilamentTool;
    command.tool_index = tool_index;
    dispatch(command);
    set_selected_tool(tool_index);

}

void FilamentPanel::set_selected_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    m_selected_tool_index = tool_index;
    if (m_selected_tool != nullptr) {
        const wxString label = wxString::Format("Tool %d", tool_index + 1);
        if (m_selected_tool->GetLabelText() != label)
            m_selected_tool->SetLabelText(label);
    }
    if (m_selected_tool_dot != nullptr) {
        const wxColour& tool_color = m_manage_tool_options[tool_index].color;
        set_panel_colour(
            m_selected_tool_dot,
            tool_color.IsOk() ? tool_color : *wxWHITE);
    }
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
