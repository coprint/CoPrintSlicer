#include "MovementPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Button.hpp"
#include "../../Widgets/StaticBox.hpp"
#include "../../wxExtensions.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

constexpr double DistanceOptions[] = {1.0, 5.0, 10.0, 20.0};
constexpr int kToolColumnWidth = 92;
constexpr int kXYColumnWidth = 300;
constexpr int kZColumnWidth = 90;
constexpr int kOptionColumnWidth = 93;

enum class JoystickAction {
    None,
    XMinus,
    XPlus,
    YMinus,
    YPlus
};

class ZAxisShapeButton : public wxPanel
{
public:
    using ClickHandler = std::function<void()>;

    ZAxisShapeButton(wxWindow* parent, const wxString& bitmap_name, const wxString& label)
        : wxPanel(parent, wxID_ANY)
        , m_bitmap_name(bitmap_name)
        , m_label(label)
    {
        apply_size(FromDIP(90), FromDIP(75));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::page_background());
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &ZAxisShapeButton::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &ZAxisShapeButton::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &ZAxisShapeButton::on_left_up, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &ZAxisShapeButton::on_capture_lost, this);
    }

    void set_click_handler(ClickHandler handler) { m_click_handler = std::move(handler); }

    void apply_size(int width, int height)
    {
        const wxSize size(std::max(1, width), std::max(1, height));
        SetMinSize(size);
        SetMaxSize(size);
        SetSize(size);
        Refresh(false);
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const int press_offset = m_pressed ? FromDIP(1) : 0;
        const wxBitmap bmp = create_scaled_bitmap(m_bitmap_name.ToStdString(), this, 75);
        if (bmp.IsOk()) {
            const wxSize hs = GetClientSize();
            const wxSize bs = bmp.GetSize();
            dc.DrawBitmap(bmp, std::max(0, (hs.x - bs.x) / 2), std::max(0, (hs.y - bs.y) / 2) + press_offset, true);
        }

        wxFont font = GetFont();
        font.SetWeight(wxFONTWEIGHT_BOLD);
        font.SetPointSize(font.GetPointSize() + 2);
        dc.SetFont(font);
        dc.SetTextForeground(wxColour(45, 48, 55));

        int tw = 0;
        int th = 0;
        dc.GetTextExtent(m_label, &tw, &th);
        const wxSize hs = GetClientSize();
        dc.DrawText(m_label, std::max(0, (hs.x - tw) / 2), std::max(0, (hs.y - th) / 2) + press_offset);
    }

    void on_left_down(wxMouseEvent& event)
    {
        m_pressed = true;
        if (!HasCapture())
            CaptureMouse();
        Refresh();
        event.Skip(false);
    }

    void on_left_up(wxMouseEvent& event)
    {
        if (HasCapture())
            ReleaseMouse();

        wxRect hit_rect({0, 0}, GetSize());
        hit_rect.Inflate(FromDIP(8));
        const bool clicked = m_pressed && hit_rect.Contains(event.GetPosition());
        m_pressed = false;
        Refresh();

        if (clicked && m_click_handler)
            m_click_handler();
        else
            event.Skip();
    }

    void on_capture_lost(wxMouseCaptureLostEvent&)
    {
        m_pressed = false;
        Refresh();
    }

    wxString m_bitmap_name;
    wxString m_label;
    ClickHandler m_click_handler;
    bool m_pressed{false};
};

class AxisJoystickPanel : public wxPanel
{
public:
    using ActionHandler = std::function<void(JoystickAction)>;

    AxisJoystickPanel(wxWindow* parent, int square, int center_size, int center_gap, int button_gap)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(square, square))
        , m_square(square)
        , m_center_size(center_size)
        , m_center_pos((square - center_size) / 2)
        , m_center_gap(center_gap)
        , m_button_gap(button_gap)
    {
        apply_square(square, center_size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::page_background());
        Bind(wxEVT_PAINT, &AxisJoystickPanel::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &AxisJoystickPanel::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &AxisJoystickPanel::on_left_up, this);
        Bind(wxEVT_LEAVE_WINDOW, &AxisJoystickPanel::on_mouse_leave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &AxisJoystickPanel::on_mouse_capture_lost, this);
    }

    int center_pos() const { return m_center_pos; }
    int center_size() const { return m_center_size; }
    void set_action_handler(ActionHandler handler) { m_action_handler = std::move(handler); }

    void apply_square(int square, int center_size)
    {
        m_square = std::max(1, square);
        m_center_size = std::clamp(center_size, 1, m_square);
        m_center_pos = (m_square - m_center_size) / 2;
        SetMinSize(wxSize(m_square, m_square));
        SetMaxSize(wxSize(m_square, m_square));
        SetSize(wxSize(m_square, m_square));
        Refresh(false);
    }

private:
    struct Piece {
        std::vector<wxPoint2DDouble> points;
        wxString label;
        wxPoint2DDouble label_center;
        JoystickAction action{JoystickAction::None};
    };

    wxPoint2DDouble p(double x, double y) const
    {
        const double scale = static_cast<double>(m_square) / 300.0;
        return {x * scale, y * scale};
    }

    std::vector<Piece> pieces() const
    {
        const double center_left = static_cast<double>(m_center_pos);
        const double center_top = static_cast<double>(m_center_pos);
        const double center_right = static_cast<double>(m_center_pos + m_center_size);
        const double center_bottom = static_cast<double>(m_center_pos + m_center_size);
        const double cgap = static_cast<double>(m_center_gap);
        const double diagonal_gap = static_cast<double>(m_button_gap) / std::sqrt(2.0);
        const double left_inner = center_left - cgap;
        const double top_inner = center_top - cgap;
        const double right_inner = center_right + cgap;
        const double bottom_inner = center_bottom + cgap;

        return {
            {{{p(54, 22), p(246, 22), p(260, 36), {right_inner - diagonal_gap, top_inner}, {left_inner + diagonal_gap, top_inner}, p(40, 36)}}, wxString::FromUTF8("Y+"), p(150, 74), JoystickAction::YPlus},
            {{{p(22, 54), p(36, 40), {left_inner, top_inner + diagonal_gap}, {left_inner, bottom_inner - diagonal_gap}, p(36, 260), p(22, 246)}}, wxString::FromUTF8("X-"), p(68, 150), JoystickAction::XMinus},
            {{{p(278, 54), p(264, 40), {right_inner, top_inner + diagonal_gap}, {right_inner, bottom_inner - diagonal_gap}, p(264, 260), p(278, 246)}}, wxString::FromUTF8("X+"), p(232, 150), JoystickAction::XPlus},
            {{{p(54, 278), p(246, 278), p(260, 264), {right_inner - diagonal_gap, bottom_inner}, {left_inner + diagonal_gap, bottom_inner}, p(40, 264)}}, wxString::FromUTF8("Y-"), p(150, 226), JoystickAction::YMinus},
        };
    }

    wxGraphicsPath rounded_path(wxGraphicsContext* gc, const std::vector<wxPoint2DDouble>& points, double radius) const
    {
        wxGraphicsPath path = gc->CreatePath();
        const int n = static_cast<int>(points.size());
        if (n == 0)
            return path;

        auto len = [](const wxPoint2DDouble& a, const wxPoint2DDouble& b) {
            const double dx = b.m_x - a.m_x;
            const double dy = b.m_y - a.m_y;
            return std::sqrt(dx * dx + dy * dy);
        };

        std::vector<wxPoint2DDouble> before(n), after(n);
        for (int i = 0; i < n; ++i) {
            const auto& prev = points[(i - 1 + n) % n];
            const auto& curr = points[i];
            const auto& next = points[(i + 1) % n];
            const double lin = std::max(1.0, len(prev, curr));
            const double lout = std::max(1.0, len(curr, next));
            const double ri = std::min(radius, lin * 0.45);
            const double ro = std::min(radius, lout * 0.45);

            before[i] = {curr.m_x - (curr.m_x - prev.m_x) / lin * ri, curr.m_y - (curr.m_y - prev.m_y) / lin * ri};
            after[i] = {curr.m_x + (next.m_x - curr.m_x) / lout * ro, curr.m_y + (next.m_y - curr.m_y) / lout * ro};
        }

        path.MoveToPoint(before[0]);
        for (int i = 0; i < n; ++i) {
            path.AddQuadCurveToPoint(points[i].m_x, points[i].m_y, after[i].m_x, after[i].m_y);
            path.AddLineToPoint(before[(i + 1) % n]);
        }
        path.CloseSubpath();
        return path;
    }

    static bool contains_point(const std::vector<wxPoint2DDouble>& poly, const wxPoint& point)
    {
        bool inside = false;
        const double x = point.x;
        const double y = point.y;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const double xi = poly[i].m_x;
            const double yi = poly[i].m_y;
            const double xj = poly[j].m_x;
            const double yj = poly[j].m_y;
            const bool intersect = ((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi);
            if (intersect)
                inside = !inside;
        }
        return inside;
    }

    JoystickAction hit_test(const wxPoint& point) const
    {
        for (const auto& piece : pieces())
            if (contains_point(piece.points, point))
                return piece.action;
        return JoystickAction::None;
    }

    void on_left_down(wxMouseEvent& event)
    {
        m_pressed_action = hit_test(event.GetPosition());
        if (m_pressed_action != JoystickAction::None) {
            if (!HasCapture())
                CaptureMouse();
            Refresh();
            return;
        }
        event.Skip();
    }

    void on_left_up(wxMouseEvent& event)
    {
        if (HasCapture())
            ReleaseMouse();

        const JoystickAction pressed = m_pressed_action;
        const JoystickAction released = hit_test(event.GetPosition());
        m_pressed_action = JoystickAction::None;
        Refresh();

        if (m_action_handler && pressed != JoystickAction::None && pressed == released)
            m_action_handler(pressed);
        else
            event.Skip();
    }

    void on_mouse_leave(wxMouseEvent& event)
    {
        if (!HasCapture() && m_pressed_action != JoystickAction::None) {
            m_pressed_action = JoystickAction::None;
            Refresh();
        }
        event.Skip();
    }

    void on_mouse_capture_lost(wxMouseCaptureLostEvent&)
    {
        m_pressed_action = JoystickAction::None;
        Refresh();
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
        wxFont label_font = GetFont();
        label_font.SetPointSize(std::max(12, label_font.GetPointSize() + 4));
        label_font.SetWeight(wxFONTWEIGHT_BOLD);

        for (const auto& piece : pieces()) {
            const bool pressed = piece.action == m_pressed_action;
            gc->SetBrush(wxBrush(pressed ? wxColour(190, 190, 190) : wxColour(214, 214, 214)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawPath(rounded_path(gc.get(), piece.points, 20.0));
            gc->SetFont(label_font, wxColour(18, 25, 35));
            double text_w = 0.0;
            double text_h = 0.0;
            gc->GetTextExtent(piece.label, &text_w, &text_h);
            gc->DrawText(piece.label, piece.label_center.m_x - text_w * 0.5, piece.label_center.m_y - text_h * 0.5);
        }
    }

    int m_square{0};
    int m_center_size{0};
    int m_center_pos{0};
    int m_center_gap{0};
    int m_button_gap{0};
    JoystickAction m_pressed_action{JoystickAction::None};
    ActionHandler m_action_handler;
};

void set_button_active(Button* button, bool active, int8_t& cached_active, bool filled_active = false)
{
    if (button == nullptr)
        return;
    const int8_t next_state = active ? 1 : 0;
    if (cached_active == next_state)
        return;
    cached_active = next_state;

    const wxColour normal_bg = filled_active && active ? wxColour(61, 64, 68) : DeviceUiStyle::control_background();
    const wxColour hover_bg = active ? wxColour(66, 70, 76) : wxColour(53, 57, 64);
    const wxColour pressed_bg = wxColour(35, 38, 44);
    const wxColour normal_border = active ? DeviceUiStyle::accent() : DeviceUiStyle::card_border();
    const wxColour hover_border = active ? DeviceUiStyle::accent() : wxColour(82, 88, 98);
    const wxColour text = active ? DeviceUiStyle::accent() : DeviceUiStyle::text_primary();

    button->SetBorderColor(StateColor(
        std::pair(normal_border, (int) StateColor::Normal),
        std::pair(hover_border, (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::accent(), (int) StateColor::Pressed)));
    button->SetTextColor(StateColor(
        std::pair(text, (int) StateColor::Normal),
        std::pair(active ? DeviceUiStyle::accent() : DeviceUiStyle::text_primary(), (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::accent(), (int) StateColor::Pressed)));
    button->SetBackgroundColor(StateColor(
        std::pair(normal_bg, (int) StateColor::Normal),
        std::pair(hover_bg, (int) StateColor::Hovered),
        std::pair(pressed_bg, (int) StateColor::Pressed)));
}

} // namespace

MovementPanel::MovementPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Movement"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::page_background());
    wxWindow* content = m_frame->content_parent();

    auto* body = new wxBoxSizer(wxVERTICAL);
    auto* headers = new wxBoxSizer(wxHORIZONTAL);
    auto* tool_header_slot = new wxBoxSizer(wxHORIZONTAL);
    auto* xy_header_slot = new wxBoxSizer(wxHORIZONTAL);
    auto* z_header_slot = new wxBoxSizer(wxHORIZONTAL);
    auto* distance_header_slot = new wxBoxSizer(wxHORIZONTAL);
    auto* speed_header_slot = new wxBoxSizer(wxHORIZONTAL);
    tool_header_slot->AddSpacer(FromDIP(18));
    tool_header_slot->Add(make_header_label(content, wxString::FromUTF8("Tool\nSelection")), 0, wxALIGN_CENTER);
    xy_header_slot->AddSpacer(FromDIP(120));
    xy_header_slot->Add(make_header_label(content, wxString::FromUTF8("Move\nX,Y axis")), 0, wxALIGN_CENTER);
    z_header_slot->AddSpacer(FromDIP(28));
    z_header_slot->Add(make_header_label(content, wxString::FromUTF8("Move\nZ axis")), 0, wxALIGN_CENTER);
    distance_header_slot->AddSpacer(1);
    speed_header_slot->AddSpacer(1);
    headers->Add(tool_header_slot, 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(20));
    headers->Add(xy_header_slot, 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(20));
    headers->Add(z_header_slot, 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(20));
    headers->Add(distance_header_slot, 0, wxALIGN_CENTER_VERTICAL);
    headers->AddSpacer(FromDIP(40));
    headers->Add(speed_header_slot, 0, wxALIGN_CENTER_VERTICAL);
    headers->SetItemMinSize(tool_header_slot, FromDIP(kToolColumnWidth), -1);
    headers->SetItemMinSize(xy_header_slot, FromDIP(kXYColumnWidth), -1);
    headers->SetItemMinSize(z_header_slot, FromDIP(kZColumnWidth), -1);
    headers->SetItemMinSize(distance_header_slot, FromDIP(kOptionColumnWidth), -1);
    headers->SetItemMinSize(speed_header_slot, FromDIP(kOptionColumnWidth), -1);
    body->Add(headers, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(6));

    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    auto* tool_col = new wxBoxSizer(wxVERTICAL);
    for (int i = 0; i < MaxDashboardTools; ++i) {
        m_tool_buttons[i] = make_tool_button(content, wxString::Format("T%d", i + 1));
        set_button_active(m_tool_buttons[i], i == 0, m_tool_button_active[i], true);
        m_tool_buttons[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SelectTool;
            command.tool_index = i;
            dispatch(command);
            set_active_tool_button(i);
        });
        tool_col->Add(m_tool_buttons[i], 0, i < MaxDashboardTools - 1 ? wxBOTTOM : 0, FromDIP(10));
    }
    controls->Add(tool_col, 0, wxTOP, FromDIP(18));

    const int xy_square = FromDIP(300);
    const int center_size = FromDIP(85);
    auto* xy_area = new AxisJoystickPanel(content, xy_square, center_size, FromDIP(4), FromDIP(3));
    xy_area->SetCursor(wxCursor(wxCURSOR_HAND));
    xy_area->set_action_handler([this](JoystickAction action) {
        switch (action) {
        case JoystickAction::XMinus: dispatch_axis(Axis::X, -1.0); break;
        case JoystickAction::XPlus:  dispatch_axis(Axis::X,  1.0); break;
        case JoystickAction::YMinus: dispatch_axis(Axis::Y, -1.0); break;
        case JoystickAction::YPlus:  dispatch_axis(Axis::Y,  1.0); break;
        case JoystickAction::None: break;
        }
    });

    auto* center_btn = new Button(xy_area, wxString(), "home", 0, 34);
    center_btn->SetSize(wxRect(wxPoint(xy_area->center_pos(), xy_area->center_pos()), wxSize(xy_area->center_size(), xy_area->center_size())));
    center_btn->SetMinSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetMaxSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetCornerRadius(FromDIP(18));
    center_btn->SetBorderWidth(0);
    center_btn->SetBackgroundColorNormal(*wxWHITE);
    center_btn->SetBackgroundColour(DeviceUiStyle::page_background());
    center_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    center_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::Home;
        dispatch(command);
    });

    controls->AddSpacer(FromDIP(20));
    controls->Add(xy_area, 0);

    auto* z_col = new wxBoxSizer(wxVERTICAL);

    // Z+ butonu — rectangle_10 SVG şekli üzerine +Z etiketi
    auto* z_plus_host = new ZAxisShapeButton(content, wxString::FromUTF8("rectangle_10"), wxString::FromUTF8("+Z"));
    z_plus_host->set_click_handler([this]() { dispatch_axis(Axis::Z, -1.0); });

    auto* z_minus_host = new ZAxisShapeButton(content, wxString::FromUTF8("rectangle_12"), wxString::FromUTF8("-Z"));
    z_minus_host->set_click_handler([this]() { dispatch_axis(Axis::Z, 1.0); });

    z_col->AddSpacer(FromDIP(48));
    z_col->Add(z_plus_host, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(10));
    z_col->Add(z_minus_host, 0, wxALIGN_CENTER_HORIZONTAL);
    controls->AddSpacer(FromDIP(20));
    controls->Add(z_col, 0, wxALIGN_TOP | wxTOP, FromDIP(22));

    auto* distance_box = new StaticBox(content, wxID_ANY);
    distance_box->SetCornerRadius(FromDIP(8));
    distance_box->SetBorderWidth(1);
    distance_box->SetBorderColorNormal(wxColour(55, 58, 64));
    distance_box->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    distance_box->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* distance_sizer = new wxBoxSizer(wxVERTICAL);
    distance_sizer->Add(make_header_label(distance_box, wxString::FromUTF8("Motion\nDistance")), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    distance_sizer->AddSpacer(FromDIP(8));
    for (int i = 0; i < 4; ++i) {
        m_distance_buttons[i] = make_option_button(distance_box, wxString::Format("%.0fmm", DistanceOptions[i]));
        set_button_active(m_distance_buttons[i], i == 0, m_distance_button_active[i]);
        const double distance = DistanceOptions[i];
        m_distance_buttons[i]->Bind(wxEVT_BUTTON, [this, distance](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SetMotionDistance;
            command.value = distance;
            dispatch(command);
            set_active_distance_button(distance);
        });
        distance_sizer->Add(m_distance_buttons[i], 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
        distance_sizer->AddSpacer(i == 3 ? FromDIP(10) : FromDIP(6));
    }
    distance_box->SetSizer(distance_sizer);
    controls->AddSpacer(FromDIP(20));
    controls->Add(distance_box, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    auto* speed_box = new StaticBox(content, wxID_ANY);
    speed_box->SetCornerRadius(FromDIP(8));
    speed_box->SetBorderWidth(1);
    speed_box->SetBorderColorNormal(wxColour(55, 58, 64));
    speed_box->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    speed_box->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* speed_sizer = new wxBoxSizer(wxVERTICAL);
    speed_sizer->Add(make_header_label(speed_box, wxString::FromUTF8("Printing\nSpeed")), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    speed_sizer->AddSpacer(FromDIP(8));
    const std::array<std::pair<wxString, int>, 4> speeds{{
        {wxString::FromUTF8("Slow"), 50},
        {wxString::FromUTF8("Normal"), 100},
        {wxString::FromUTF8("Fast"), 125},
        {wxString::FromUTF8("Ultra"), 166}
    }};
    for (int i = 0; i < 4; ++i) {
        m_speed_buttons[i] = make_option_button(speed_box, speeds[i].first);
        set_button_active(m_speed_buttons[i], i == 1, m_speed_button_active[i]);
        const int percent = speeds[i].second;
        const SpeedPreset preset = static_cast<SpeedPreset>(i);
        m_speed_buttons[i]->Bind(wxEVT_BUTTON, [this, percent, preset](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SetPrintSpeed;
            command.value = percent;
            dispatch(command);
            set_active_speed_button(preset);
        });
        speed_sizer->Add(m_speed_buttons[i], 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
        speed_sizer->AddSpacer(i == 3 ? FromDIP(10) : FromDIP(6));
    }
    speed_box->SetSizer(speed_sizer);
    controls->AddSpacer(FromDIP(40));
    controls->Add(speed_box, 0, wxALIGN_TOP | wxTOP, FromDIP(18));

    body->Add(controls, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));
    m_frame->set_content(body);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);

    m_joystick_square = xy_square;
    m_apply_layout_scale = [xy_area, center_btn, z_plus_host, z_minus_host, this](double scale) {
        const double s = std::clamp(scale, 0.40, 1.0);
        m_layout_scale = s;

        const int square = std::max(FromDIP(96), static_cast<int>(std::lround(FromDIP(300) * s)));
        const int center = std::max(FromDIP(36), square * 85 / 300);
        xy_area->apply_square(square, center);
        center_btn->SetSize(wxRect(wxPoint(xy_area->center_pos(), xy_area->center_pos()),
                                   wxSize(xy_area->center_size(), xy_area->center_size())));
        center_btn->SetMinSize(wxSize(xy_area->center_size(), xy_area->center_size()));
        center_btn->SetMaxSize(wxSize(xy_area->center_size(), xy_area->center_size()));
        m_joystick_square = square;

        const int tool_w = std::max(FromDIP(48), static_cast<int>(std::lround(FromDIP(92) * s)));
        const int tool_h = std::max(FromDIP(32), static_cast<int>(std::lround(FromDIP(58) * s)));
        for (Button* button : m_tool_buttons) {
            if (button == nullptr)
                continue;
            button->SetMinSize(wxSize(tool_w, tool_h));
            button->SetMaxSize(wxSize(tool_w, tool_h));
            button->SetSize(wxSize(tool_w, tool_h));
        }

        const int z_w = std::max(FromDIP(48), static_cast<int>(std::lround(FromDIP(90) * s)));
        const int z_h = std::max(FromDIP(40), static_cast<int>(std::lround(FromDIP(75) * s)));
        z_plus_host->apply_size(z_w, z_h);
        z_minus_host->apply_size(z_w, z_h);

        const int opt_w = std::max(FromDIP(44), static_cast<int>(std::lround(FromDIP(73) * s)));
        const int opt_h = std::max(FromDIP(28), static_cast<int>(std::lround(FromDIP(45) * s)));
        for (Button* button : m_distance_buttons) {
            if (button == nullptr)
                continue;
            button->SetMinSize(wxSize(opt_w, opt_h));
            button->SetMaxSize(wxSize(opt_w, opt_h));
            button->SetSize(wxSize(opt_w, opt_h));
        }
        for (Button* button : m_speed_buttons) {
            if (button == nullptr)
                continue;
            button->SetMinSize(wxSize(opt_w, opt_h));
            button->SetMaxSize(wxSize(opt_w, opt_h));
            button->SetSize(wxSize(opt_w, opt_h));
        }

        Layout();
    };
}

void MovementPanel::apply_state(const MovementState& state)
{
    set_active_tool_button(state.selected_tool);
    set_active_distance_button(state.selected_distance_mm);

    SpeedPreset preset = SpeedPreset::Normal;
    if (state.print_speed_percent <= 50)
        preset = SpeedPreset::Slow;
    else if (state.print_speed_percent <= 100)
        preset = SpeedPreset::Normal;
    else if (state.print_speed_percent <= 125)
        preset = SpeedPreset::Fast;
    else
        preset = SpeedPreset::Ultra;
    set_active_speed_button(preset);
}

void MovementPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

Button* MovementPanel::make_tool_button(wxWindow* parent, const wxString& label)
{
    auto* button = new Button(parent, label);
    const wxSize size(FromDIP(92), FromDIP(58));
    button->SetMinSize(size);
    button->SetMaxSize(size);
    button->SetSize(size);
    button->SetCornerRadius(FromDIP(10));
    button->SetBorderWidth(1);
    return button;
}

Button* MovementPanel::make_option_button(wxWindow* parent, const wxString& label)
{
    auto* button = new Button(parent, label);
    button->SetMinSize(wxSize(FromDIP(73), FromDIP(45)));
    button->SetCornerRadius(FromDIP(8));
    button->SetBorderWidth(1);
    return button;
}

wxStaticText* MovementPanel::make_header_label(wxWindow* parent, const wxString& label)
{
    auto* text = new wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    text->SetForegroundColour(DeviceUiStyle::text_muted());
    wxFont font = text->GetFont();
    if (font.GetPointSize() > 1)
        font.SetPointSize(font.GetPointSize() - 1);
    text->SetFont(font);
    return text;
}

void MovementPanel::dispatch_axis(Axis axis, double direction) const
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::MoveAxis;
    command.axis = axis;
    command.value = direction * m_selected_distance_mm;
    dispatch(command);
}

void MovementPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void MovementPanel::set_active_tool_button(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    if (m_selected_tool == tool_index)
        return;
    m_selected_tool = tool_index;
    for (int i = 0; i < MaxDashboardTools; ++i)
        set_button_active(m_tool_buttons[i], i == m_selected_tool, m_tool_button_active[i], true);
}

void MovementPanel::set_active_distance_button(double distance_mm)
{
    const double new_distance = distance_mm > 0.0 ? distance_mm : 1.0;
    if (std::abs(new_distance - m_selected_distance_mm) < 0.01)
        return;
    m_selected_distance_mm = new_distance;
    for (int i = 0; i < 4; ++i)
        set_button_active(
            m_distance_buttons[i],
            std::abs(DistanceOptions[i] - m_selected_distance_mm) < 0.01,
            m_distance_button_active[i]);
}

void MovementPanel::set_active_speed_button(SpeedPreset preset)
{
    if (m_speed_preset == preset)
        return;
    m_speed_preset = preset;
    for (int i = 0; i < 4; ++i)
        set_button_active(m_speed_buttons[i], static_cast<int>(m_speed_preset) == i, m_speed_button_active[i]);
}

void MovementPanel::fit_to_height(int content_height_px)
{
    if (content_height_px <= 0 || !m_apply_layout_scale)
        return;
    // Full-size Movement is designed around a ~300 DIP joystick + chrome.
    const int design_h = FromDIP(396);
    const double scale = std::clamp(static_cast<double>(content_height_px) / static_cast<double>(design_h), 0.40, 1.0);
    if (std::abs(scale - m_layout_scale) > 0.01 || m_joystick_square <= 0)
        m_apply_layout_scale(scale);
    // Never force a min taller than the allocated Device slot (prevents clipping).
    SetMinSize(wxSize(FromDIP(280), 1));
    SetMaxSize(wxSize(-1, content_height_px));
    Layout();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
