#include "MovementPanel.hpp"

#include "../DeviceUiStyle.hpp"
#include "../../Widgets/Button.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

int d(wxWindow* win, int v) { return DeviceUiStyle::dip(win, v); }
int s(int v) { return DeviceUiStyle::scaled(v); }

constexpr double DistanceOptions[] = {1.0, 5.0, 10.0, 50.0};
constexpr int kHoverDarken = 18;
constexpr int kPressDarken = 28;
const wxColour kJoystickButtonBg(0xD6, 0xD6, 0xD6);
const wxColour kJoystickButtonDisabled(0xEE, 0xEE, 0xEE);

wxColour darken(const wxColour& colour, int amount)
{
    auto ch = [amount](int v) { return std::max(0, v - amount); };
    return wxColour(ch(colour.Red()), ch(colour.Green()), ch(colour.Blue()), colour.Alpha());
}

StateColor mouse_hover_color(StateColor color)
{
    color.setTakeFocusedAsHovered(false);
    return color;
}

wxGraphicsPath z_shape_path(wxGraphicsContext* gc, double x, double y, double w, double h, bool plus)
{
    auto map = [&](double px, double py) {
        return wxPoint2DDouble(x + px * w / 66.0, y + py * h / 58.0);
    };
    auto curve = [&](wxGraphicsPath& path, double x1, double y1, double x2, double y2, double x3, double y3) {
        const auto c1 = map(x1, y1);
        const auto c2 = map(x2, y2);
        const auto p  = map(x3, y3);
        path.AddCurveToPoint(c1.m_x, c1.m_y, c2.m_x, c2.m_y, p.m_x, p.m_y);
    };

    wxGraphicsPath path = gc->CreatePath();
    if (plus) {
        const auto start = map(7.84334, 8.028);
        path.MoveToPoint(start.m_x, start.m_y);
        curve(path, 8.78254, 3.35887, 12.8843, 0, 17.647, 0);
        path.AddLineToPoint(map(47.936, 0));
        curve(path, 52.6987, 0, 56.8005, 3.35887, 57.7397, 8.028);
        path.AddLineToPoint(map(65.3833, 46.028));
        curve(path, 66.6287, 52.219, 61.8947, 58, 55.5797, 58);
        path.AddLineToPoint(map(10.0033, 58));
        curve(path, 3.68832, 58, -1.04565, 52.219, 0.199664, 46.028);
    } else {
        const auto start = map(57.7397, 49.972);
        path.MoveToPoint(start.m_x, start.m_y);
        curve(path, 56.8005, 54.6411, 52.6987, 58, 47.936, 58);
        path.AddLineToPoint(map(17.647, 58));
        curve(path, 12.8843, 58, 8.78254, 54.6411, 7.84334, 49.972);
        path.AddLineToPoint(map(0.199661, 11.972));
        curve(path, -1.04565, 5.78103, 3.68832, 0, 10.0033, 0);
        path.AddLineToPoint(map(55.5797, 0));
        curve(path, 61.8947, 0, 66.6287, 5.78103, 65.3833, 11.972);
    }
    path.CloseSubpath();
    return path;
}

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
        set_visual_size(d(this, 90), d(this, 75), s(75));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::card_background());
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &ZAxisShapeButton::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, &ZAxisShapeButton::on_enter, this);
        Bind(wxEVT_LEAVE_WINDOW, &ZAxisShapeButton::on_leave, this);
        Bind(wxEVT_LEFT_DOWN, &ZAxisShapeButton::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &ZAxisShapeButton::on_left_up, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &ZAxisShapeButton::on_capture_lost, this);
    }

    void set_click_handler(ClickHandler handler) { m_click_handler = std::move(handler); }
    void set_visual_size(int width, int height, int bitmap_dip_size)
    {
        SetMinSize(wxSize(width, height));
        SetMaxSize(wxSize(width, height));
        SetSize(wxSize(width, height));
        m_bitmap_dip_size = bitmap_dip_size;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const int press_offset = m_pressed ? d(this, 1) : 0;
        const wxSize hs = GetClientSize();
        const int shape_h = FromDIP(m_bitmap_dip_size);
        const int shape_w = static_cast<int>(std::lround(shape_h * 66.0 / 58.0));
        const double x = std::max(0, (hs.x - shape_w) / 2);
        const double y = std::max(0, (hs.y - shape_h) / 2) + press_offset;

        wxColour fill = kJoystickButtonBg;
        if (!IsEnabled())
            fill = kJoystickButtonDisabled;
        else if (m_pressed)
            fill = darken(fill, kPressDarken);
        else if (m_hovered)
            fill = darken(fill, kHoverDarken);

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(fill));
            const bool plus = m_bitmap_name == wxString::FromUTF8("rectangle_10");
            gc->DrawPath(z_shape_path(gc.get(), x, y, shape_w, shape_h, plus));
        }

        wxFont font = GetFont();
        font.SetWeight(wxFONTWEIGHT_BOLD);
        font.SetPointSize(std::max(8, s(font.GetPointSize() + 2)));
        dc.SetFont(font);
        dc.SetTextForeground(DeviceUiStyle::text_primary());

        int tw = 0;
        int th = 0;
        int descent = 0;
        dc.GetTextExtent(m_label, &tw, &th, &descent);
        const int text_x = std::max(0, (hs.x - tw) / 2);
        int text_y = (hs.y - th) / 2 + descent / 2 + press_offset;
        // Trapezoid is wider at the base; shift the glyph toward that visual center.
        if (m_bitmap_name == wxString::FromUTF8("rectangle_10"))
            text_y += hs.y / 18;
        else if (m_bitmap_name == wxString::FromUTF8("rectangle_12"))
            text_y -= hs.y / 18;
        dc.DrawText(m_label, text_x, text_y);
    }

    void on_enter(wxMouseEvent& event)
    {
        if (!IsEnabled()) {
            event.Skip();
            return;
        }
        m_hovered = true;
        Refresh();
        event.Skip();
    }

    void on_leave(wxMouseEvent& event)
    {
        if (!HasCapture())
            m_hovered = false;
        Refresh();
        event.Skip();
    }

    void on_left_down(wxMouseEvent& event)
    {
        if (!IsEnabled()) {
            event.Skip();
            return;
        }
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
        hit_rect.Inflate(d(this, 8));
        const bool clicked = m_pressed && hit_rect.Contains(event.GetPosition());
        m_pressed = false;
        m_hovered = hit_rect.Contains(event.GetPosition());
        Refresh();

        if (clicked && IsEnabled() && m_click_handler)
            m_click_handler();
        else
            event.Skip();
    }

    void on_capture_lost(wxMouseCaptureLostEvent&)
    {
        m_pressed = false;
        m_hovered = false;
        Refresh();
    }

    wxString m_bitmap_name;
    wxString m_label;
    ClickHandler m_click_handler;
    bool m_pressed{false};
    bool m_hovered{false};
    int m_bitmap_dip_size{75};
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
        SetMinSize(wxSize(square, square));
        SetMaxSize(wxSize(square, square));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::card_background());
        Bind(wxEVT_PAINT, &AxisJoystickPanel::on_paint, this);
        Bind(wxEVT_MOTION, &AxisJoystickPanel::on_motion, this);
        Bind(wxEVT_LEFT_DOWN, &AxisJoystickPanel::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &AxisJoystickPanel::on_left_up, this);
        Bind(wxEVT_LEAVE_WINDOW, &AxisJoystickPanel::on_mouse_leave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &AxisJoystickPanel::on_mouse_capture_lost, this);
    }

    int center_pos() const { return m_center_pos; }
    int center_size() const { return m_center_size; }
    void set_action_handler(ActionHandler handler) { m_action_handler = std::move(handler); }
    void set_geometry(int square, int center_size, int center_gap, int button_gap)
    {
        m_square = std::max(1, square);
        m_center_size = std::max(1, center_size);
        m_center_pos = (m_square - m_center_size) / 2;
        m_center_gap = center_gap;
        m_button_gap = button_gap;
        SetMinSize(wxSize(m_square, m_square));
        SetMaxSize(wxSize(m_square, m_square));
        SetSize(wxSize(m_square, m_square));
        Refresh();
    }

    void layout_overlay(wxWindow* home)
    {
        const int sq = live_square();
        const double scale = static_cast<double>(sq) / 300.0;
        const int center = std::max(1, static_cast<int>(std::lround(85.0 * scale)));
        const int pos = (sq - center) / 2;
        m_center_size = center;
        m_center_pos = pos;
        if (home != nullptr) {
            home->SetSize(wxRect(wxPoint(pos, pos), wxSize(center, center)));
            home->SetMinSize(wxSize(center, center));
            home->SetMaxSize(wxSize(center, center));
        }
    }

private:
    int live_square() const
    {
        // Never paint larger than the laid-out DIP size. Per-monitor DPI on Windows
        // can stretch the HWND before wx relayouts; using raw client size made the
        // pad grow and clip when the window moved to another display.
        const wxSize cs = GetClientSize();
        const int side = std::min(cs.GetWidth(), cs.GetHeight());
        int sq = std::max(1, m_square);
        if (side > 1)
            sq = std::min(sq, side);
        return sq;
    }

    struct Piece {
        std::vector<wxPoint2DDouble> points;
        wxString label;
        JoystickAction action{JoystickAction::None};
    };

    wxPoint2DDouble p(double x, double y) const
    {
        const double scale = static_cast<double>(live_square()) / 300.0;
        return {x * scale, y * scale};
    }

    std::vector<Piece> pieces() const
    {
        const double scale = static_cast<double>(live_square()) / 300.0;
        const double center_size = 85.0 * scale;
        const double center_pos = (static_cast<double>(live_square()) - center_size) / 2.0;
        const double center_left = center_pos;
        const double center_top = center_pos;
        const double center_right = center_pos + center_size;
        const double center_bottom = center_pos + center_size;
        const double cgap = 4.0 * scale;
        const double diagonal_gap = (3.0 * scale) / std::sqrt(2.0);
        const double left_inner = center_left - cgap;
        const double top_inner = center_top - cgap;
        const double right_inner = center_right + cgap;
        const double bottom_inner = center_bottom + cgap;

        return {
            {{{p(54, 22), p(246, 22), p(260, 36), {right_inner - diagonal_gap, top_inner}, {left_inner + diagonal_gap, top_inner}, p(40, 36)}}, wxString::FromUTF8("Y"), JoystickAction::YPlus},
            {{{p(22, 54), p(36, 40), {left_inner, top_inner + diagonal_gap}, {left_inner, bottom_inner - diagonal_gap}, p(36, 260), p(22, 246)}}, wxString::FromUTF8("-X"), JoystickAction::XMinus},
            {{{p(278, 54), p(264, 40), {right_inner, top_inner + diagonal_gap}, {right_inner, bottom_inner - diagonal_gap}, p(264, 260), p(278, 246)}}, wxString::FromUTF8("X"), JoystickAction::XPlus},
            {{{p(54, 278), p(246, 278), p(260, 264), {right_inner - diagonal_gap, bottom_inner}, {left_inner + diagonal_gap, bottom_inner}, p(40, 264)}}, wxString::FromUTF8("-Y"), JoystickAction::YMinus},
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

    static wxPoint2DDouble polygon_centroid(const std::vector<wxPoint2DDouble>& pts)
    {
        if (pts.empty())
            return {};
        double area2 = 0.0;
        double cx = 0.0;
        double cy = 0.0;
        const int n = static_cast<int>(pts.size());
        for (int i = 0; i < n; ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % n];
            const double cross = a.m_x * b.m_y - b.m_x * a.m_y;
            area2 += cross;
            cx += (a.m_x + b.m_x) * cross;
            cy += (a.m_y + b.m_y) * cross;
        }
        if (std::abs(area2) < 1e-6) {
            wxPoint2DDouble avg;
            for (const auto& pt : pts) {
                avg.m_x += pt.m_x;
                avg.m_y += pt.m_y;
            }
            avg.m_x /= n;
            avg.m_y /= n;
            return avg;
        }
        return {cx / (3.0 * area2), cy / (3.0 * area2)};
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

    void set_hovered_action(JoystickAction action)
    {
        if (m_hovered_action == action)
            return;
        m_hovered_action = action;
        SetCursor(action != JoystickAction::None ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
        Refresh();
    }

    void on_motion(wxMouseEvent& event)
    {
        if (!IsEnabled()) {
            set_hovered_action(JoystickAction::None);
            event.Skip();
            return;
        }
        if (!HasCapture())
            set_hovered_action(hit_test(event.GetPosition()));
        event.Skip();
    }

    void on_left_down(wxMouseEvent& event)
    {
        if (!IsEnabled()) {
            event.Skip();
            return;
        }
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
        m_hovered_action = released;
        SetCursor(released != JoystickAction::None ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
        Refresh();

        if (m_action_handler && IsEnabled() && pressed != JoystickAction::None && pressed == released)
            m_action_handler(pressed);
        else
            event.Skip();
    }

    void on_mouse_leave(wxMouseEvent& event)
    {
        if (!HasCapture()) {
            m_pressed_action = JoystickAction::None;
            set_hovered_action(JoystickAction::None);
        }
        event.Skip();
    }

    void on_mouse_capture_lost(wxMouseCaptureLostEvent&)
    {
        m_pressed_action = JoystickAction::None;
        set_hovered_action(JoystickAction::None);
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
        label_font.SetPointSize(std::max(9, s(label_font.GetPointSize() + 4)));
        label_font.SetWeight(wxFONTWEIGHT_BOLD);

        for (const auto& piece : pieces()) {
            const bool pressed = piece.action == m_pressed_action;
            const bool hovered = piece.action == m_hovered_action;
            wxColour fill = kJoystickButtonBg;
            if (!IsEnabled())
                fill = kJoystickButtonDisabled;
            else if (pressed)
                fill = darken(kJoystickButtonBg, kPressDarken);
            else if (hovered)
                fill = darken(kJoystickButtonBg, kHoverDarken);
            gc->SetBrush(wxBrush(fill));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawPath(rounded_path(gc.get(), piece.points, 15.0 * live_square() / 300.0));
            gc->SetFont(label_font, DeviceUiStyle::text_primary());
            double text_w = 0.0;
            double text_h = 0.0;
            double descent = 0.0;
            gc->GetTextExtent(piece.label, &text_w, &text_h, &descent);
            const wxPoint2DDouble center = polygon_centroid(piece.points);
            gc->DrawText(piece.label,
                center.m_x - text_w * 0.5,
                center.m_y - text_h * 0.5 + descent * 0.5);
        }
    }

    int m_square{0};
    int m_center_size{0};
    int m_center_pos{0};
    int m_center_gap{0};
    int m_button_gap{0};
    JoystickAction m_pressed_action{JoystickAction::None};
    JoystickAction m_hovered_action{JoystickAction::None};
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

    const wxColour normal_bg = filled_active && active ? wxColour(232, 232, 232) : DeviceUiStyle::control_background();
    const wxColour hover_bg = darken(normal_bg, kHoverDarken);
    const wxColour pressed_bg = darken(normal_bg, kPressDarken);
    const wxColour normal_border = active ? DeviceUiStyle::accent() : DeviceUiStyle::card_border();
    const wxColour hover_border = active ? DeviceUiStyle::accent() : wxColour(176, 176, 176);
    const wxColour text = DeviceUiStyle::text_primary();

    button->SetBorderColor(mouse_hover_color(StateColor(
        std::pair(DeviceUiStyle::card_border(), (int) StateColor::Disabled),
        std::pair(DeviceUiStyle::accent(), (int) StateColor::Pressed),
        std::pair(hover_border, (int) StateColor::Hovered),
        std::pair(normal_border, (int) StateColor::Normal))));
    button->SetTextColor(mouse_hover_color(StateColor(
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Disabled),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Pressed),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Hovered),
        std::pair(text, (int) StateColor::Normal))));
    button->SetBackgroundColor(mouse_hover_color(StateColor(
        std::pair(wxColour(240, 240, 240), (int) StateColor::Disabled),
        std::pair(pressed_bg, (int) StateColor::Pressed),
        std::pair(hover_bg, (int) StateColor::Hovered),
        std::pair(normal_bg, (int) StateColor::Normal))));
}

void set_button_enabled(Button* button, bool enabled, const wxColour& normal_bg = DeviceUiStyle::control_background(),
                        const wxColour& disabled_bg = wxColour(240, 240, 240))
{
    if (button == nullptr)
        return;

    button->Enable(enabled);
    button->SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
    button->SetBorderColor(mouse_hover_color(StateColor(
        std::pair(DeviceUiStyle::card_border(), (int) StateColor::Disabled),
        std::pair(DeviceUiStyle::accent(), (int) StateColor::Pressed),
        std::pair(wxColour(176, 176, 176), (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::card_border(), (int) StateColor::Normal))));
    button->SetTextColor(mouse_hover_color(StateColor(
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Disabled),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Pressed),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Normal))));
    button->SetBackgroundColor(mouse_hover_color(StateColor(
        std::pair(disabled_bg, (int) StateColor::Disabled),
        std::pair(darken(normal_bg, kPressDarken), (int) StateColor::Pressed),
        std::pair(darken(normal_bg, kHoverDarken), (int) StateColor::Hovered),
        std::pair(normal_bg, (int) StateColor::Normal))));
}

} // namespace

MovementPanel::MovementPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::card_background());

    wxWindow* content = this;
    auto* body = new wxBoxSizer(wxVERTICAL);

    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    m_controls_sizer = controls;
    auto* tool_col = new wxBoxSizer(wxVERTICAL);
    for (int i = 0; i < MaxDashboardTools; ++i) {
        m_tool_buttons[i] = make_tool_button(content, wxString::Format("T%d", i + 1));
        set_button_active(m_tool_buttons[i], i == 0, m_tool_button_active[i], true);
        m_tool_buttons[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            if (i >= m_available_tool_count)
                return;
            DeviceCommand command;
            command.kind = DeviceCommandKind::SelectTool;
            command.tool_index = i;
            dispatch(command);
            set_active_tool_button(i);
        });
        tool_col->Add(m_tool_buttons[i], 0, i < MaxDashboardTools - 1 ? wxBOTTOM : 0, d(this, 10));
    }
    // wxBoxSizer has no padding of its own; wrap T1–T4 with 20 DIP left/right.
    auto* tool_wrap = new wxBoxSizer(wxHORIZONTAL);
    tool_wrap->AddSpacer(d(this, 20));
    tool_wrap->Add(tool_col, 0);
    tool_wrap->AddSpacer(d(this, 20));

    const int xy_square = d(this, 300);
    const int center_size = d(this, 85);
    auto* xy_area = new AxisJoystickPanel(content, xy_square, center_size, d(this, 4), d(this, 3));
    m_xy_area = xy_area;
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

    const wxColour home_bg = kJoystickButtonBg;
    auto* center_btn = new Button(xy_area, wxString(), "home", 0, 34);
    m_center_button = center_btn;
    center_btn->SetSize(wxRect(wxPoint(xy_area->center_pos(), xy_area->center_pos()), wxSize(xy_area->center_size(), xy_area->center_size())));
    center_btn->SetMinSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetMaxSize(wxSize(xy_area->center_size(), xy_area->center_size()));
    center_btn->SetCornerRadius(FromDIP(7));
    center_btn->SetBorderWidth(0);
    center_btn->SetBackgroundColor(mouse_hover_color(StateColor(
        std::pair(kJoystickButtonDisabled, (int) StateColor::Disabled),
        std::pair(darken(home_bg, kPressDarken), (int) StateColor::Pressed),
        std::pair(darken(home_bg, kHoverDarken), (int) StateColor::Hovered),
        std::pair(home_bg, (int) StateColor::Normal))));
    // Window erase is a square; keep it the card colour so the rounded fill is visible.
    center_btn->SetBackgroundColour(DeviceUiStyle::card_background());
    center_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    center_btn->SetCanFocus(false);
    center_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::Home;
        dispatch(command);
        if (m_center_button != nullptr)
            m_center_button->Refresh();
    });

    auto* z_col = new wxBoxSizer(wxVERTICAL);

    // Z+ butonu — rectangle_10 SVG şekli üzerine +Z etiketi
    auto* z_plus_host = new ZAxisShapeButton(content, wxString::FromUTF8("rectangle_10"), wxString::FromUTF8("+Z"));
    m_z_plus_host = z_plus_host;
    z_plus_host->set_click_handler([this]() { dispatch_axis(Axis::Z, 1.0); });

    auto* z_minus_host = new ZAxisShapeButton(content, wxString::FromUTF8("rectangle_12"), wxString::FromUTF8("-Z"));
    m_z_minus_host = z_minus_host;
    z_minus_host->set_click_handler([this]() { dispatch_axis(Axis::Z, -1.0); });

    z_col->AddSpacer(d(this, 48));
    z_col->Add(z_plus_host, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, d(this, 10));
    z_col->Add(z_minus_host, 0, wxALIGN_CENTER_HORIZONTAL);

    auto* axes_row = new wxBoxSizer(wxHORIZONTAL);
    axes_row->Add(xy_area, 0);
    axes_row->AddSpacer(d(this, 20));
    axes_row->Add(z_col, 0, wxALIGN_TOP | wxTOP, d(this, 22));

    auto* distance_row = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < 4; ++i) {
        m_distance_buttons[i] = make_option_button(content, wxString::Format("%.0fmm", DistanceOptions[i]));
        set_button_active(m_distance_buttons[i], i == 0, m_distance_button_active[i]);
        const double distance = DistanceOptions[i];
        m_distance_buttons[i]->Bind(wxEVT_BUTTON, [this, distance](wxCommandEvent&) {
            DeviceCommand command;
            command.kind = DeviceCommandKind::SetMotionDistance;
            command.value = distance;
            dispatch(command);
            set_active_distance_button(distance);
        });
        distance_row->Add(m_distance_buttons[i], 0, wxALIGN_CENTER_VERTICAL | (i > 0 ? wxLEFT : 0), d(this, 8));
    }

    auto* move_col = new wxBoxSizer(wxVERTICAL);
    move_col->Add(axes_row, 0);
    move_col->AddSpacer(d(this, 12));
    move_col->Add(distance_row, 0, wxALIGN_CENTER_HORIZONTAL);

    m_status_slot = new wxPanel(content, wxID_ANY);
    m_status_slot->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* status_sizer = new wxBoxSizer(wxVERTICAL);
    m_status_slot->SetSizer(status_sizer);
    controls->Add(m_status_slot, 0, wxTOP, d(this, 18));
    controls->AddSpacer(d(this, 40));
    controls->Add(move_col, 0);
    controls->Add(tool_wrap, 0, wxTOP, d(this, 18));

    body->Add(controls, 0, wxALIGN_LEFT);
    SetSizer(body);

    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        if (m_relayout_busy)
            return;
        CallAfter([this] {
            if (!m_relayout_busy)
                relayout_joystick();
        });
    });
#if defined(__WXMSW__) && wxCHECK_VERSION(3, 1, 0)
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        event.Skip();
        CallAfter([this] { msw_rescale(); });
    });
#endif
    CallAfter([this] { msw_rescale(); });
}

void MovementPanel::apply_state(const MovementState& state)
{
    set_available_tool_count(state.available_tool_count);
    set_active_tool_button(state.selected_tool);
    set_active_distance_button(state.selected_distance_mm);
    set_controls_enabled(state.can_move);
    if (m_center_button != nullptr)
        set_button_enabled(m_center_button, state.can_move && !state.is_homing, kJoystickButtonBg, kJoystickButtonDisabled);
    refresh_selection_styles();
}

void MovementPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void MovementPanel::msw_rescale()
{
    m_last_square = -1;
    relayout_joystick();
}

void MovementPanel::relayout_joystick()
{
    if (m_relayout_busy)
        return;
    m_relayout_busy = true;

    // Logical pad size is always 300 DIP (then DeviceUiStyle 80%). Do not grow with
    // the parent or with a stale pixel MinSize from another monitor's DPI.
    const int square = d(this, 300);
    const int current = m_xy_area != nullptr ? m_xy_area->GetMinSize().GetWidth() : 0;
    if (m_last_square == square && current == square) {
        if (auto* joy = dynamic_cast<AxisJoystickPanel*>(m_xy_area))
            joy->layout_overlay(m_center_button);
        m_relayout_busy = false;
        return;
    }
    m_last_square = square;

    if (auto* joy = dynamic_cast<AxisJoystickPanel*>(m_xy_area)) {
        joy->set_geometry(square, d(this, 85), d(this, 4), d(this, 3));
        joy->layout_overlay(m_center_button);
    }
    if (m_center_button != nullptr) {
        m_center_button->SetCornerRadius(FromDIP(7));
        m_center_button->Rescale();
    }
    if (auto* z = dynamic_cast<ZAxisShapeButton*>(m_z_plus_host))
        z->set_visual_size(d(this, 90), d(this, 75), s(75));
    if (auto* z = dynamic_cast<ZAxisShapeButton*>(m_z_minus_host))
        z->set_visual_size(d(this, 90), d(this, 75), s(75));

    const wxSize tool(d(this, 92), d(this, 58));
    for (Button* button : m_tool_buttons) {
        if (button == nullptr)
            continue;
        button->SetMinSize(tool);
        button->SetMaxSize(tool);
        button->SetSize(tool);
        button->SetCornerRadius(d(this, 10));
        button->Rescale();
    }
    const wxSize distance(d(this, 73), d(this, 45));
    for (Button* button : m_distance_buttons) {
        if (button == nullptr)
            continue;
        button->SetMinSize(distance);
        button->SetCornerRadius(d(this, 8));
        button->Rescale();
    }

    Layout();
    m_relayout_busy = false;
}

Button* MovementPanel::make_tool_button(wxWindow* parent, const wxString& label)
{
    auto* button = new Button(parent, label);
    const wxSize size(d(this, 92), d(this, 58));
    button->SetMinSize(size);
    button->SetMaxSize(size);
    button->SetSize(size);
    button->SetCornerRadius(d(this, 10));
    button->SetBorderWidth(1);
    button->SetFont(wxFont(wxFontInfo(16)
        .Family(wxFONTFAMILY_SWISS)
        .FaceName(wxString::FromUTF8("Bahnschrift"))
        .Weight(wxFONTWEIGHT_SEMIBOLD)));
    button->SetCanFocus(false);
    return button;
}

Button* MovementPanel::make_option_button(wxWindow* parent, const wxString& label)
{
    auto* button = new Button(parent, label);
    button->SetMinSize(wxSize(d(this, 73), d(this, 45)));
    button->SetCornerRadius(d(this, 8));
    button->SetBorderWidth(1);
    button->SetCanFocus(false);
    return button;
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
    if (!IsEnabled() || !m_command_handler)
        return;
    m_command_handler(command);
}

void MovementPanel::set_active_tool_button(int tool_index)
{
    if (m_available_tool_count <= 0)
        m_available_tool_count = 1;
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    if (tool_index >= m_available_tool_count)
        tool_index = 0;
    if (m_selected_tool == tool_index)
        return;
    m_selected_tool = tool_index;
    for (int i = 0; i < MaxDashboardTools; ++i)
        set_button_active(m_tool_buttons[i], i == m_selected_tool, m_tool_button_active[i], true);
}

void MovementPanel::set_available_tool_count(int tool_count)
{
    const int clamped_count = std::clamp(tool_count, 1, MaxDashboardTools);
    if (m_available_tool_count == clamped_count)
        return;

    m_available_tool_count = clamped_count;
    for (int i = 0; i < MaxDashboardTools; ++i) {
        const bool enabled = i < m_available_tool_count;
        set_button_enabled(m_tool_buttons[i], enabled);
        m_tool_button_active[i] = -1;
    }

    if (m_selected_tool >= m_available_tool_count)
        m_selected_tool = 0;
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

void MovementPanel::refresh_selection_styles()
{
    if (!m_controls_enabled)
        return;

    for (int i = 0; i < MaxDashboardTools; ++i)
        m_tool_button_active[i] = -1;
    for (int i = 0; i < 4; ++i)
        m_distance_button_active[i] = -1;

    for (int i = 0; i < MaxDashboardTools; ++i) {
        if (i >= m_available_tool_count)
            continue;
        set_button_active(m_tool_buttons[i], i == m_selected_tool, m_tool_button_active[i], true);
    }
    for (int i = 0; i < 4; ++i)
        set_button_active(
            m_distance_buttons[i],
            std::abs(DistanceOptions[i] - m_selected_distance_mm) < 0.01,
            m_distance_button_active[i]);
}

void MovementPanel::set_controls_enabled(bool enabled)
{
    m_controls_enabled = enabled;
    if (m_xy_area != nullptr) {
        m_xy_area->Enable(enabled);
        m_xy_area->SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        m_xy_area->Refresh();
    }
    if (m_center_button != nullptr)
        set_button_enabled(m_center_button, enabled, kJoystickButtonBg, kJoystickButtonDisabled);
    if (m_z_plus_host != nullptr) {
        m_z_plus_host->Enable(enabled);
        m_z_plus_host->SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        m_z_plus_host->Refresh();
    }
    if (m_z_minus_host != nullptr) {
        m_z_minus_host->Enable(enabled);
        m_z_minus_host->SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        m_z_minus_host->Refresh();
    }
    for (int i = 0; i < MaxDashboardTools; ++i)
        set_button_enabled(m_tool_buttons[i], enabled && i < m_available_tool_count);
    for (int i = 0; i < 4; ++i)
        set_button_enabled(m_distance_buttons[i], enabled);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
