#include "PresetStepSlider.hpp"

#include "DeviceUiStyle.hpp"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

constexpr int kTrackYDip = 22;
constexpr int kSidePadDip = 22;
constexpr int kThumbRDip = 11;
constexpr int kKnotRDip = 4;

} // namespace

PresetStepSlider::PresetStepSlider(wxWindow *parent, std::vector<wxString> labels)
    : wxPanel(parent, wxID_ANY)
    , m_labels(std::move(labels))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(0xEB, 0xEB, 0xEB));
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetMinSize(wxSize(FromDIP(320), FromDIP(64)));
    Bind(wxEVT_PAINT, &PresetStepSlider::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &PresetStepSlider::on_left_down, this);
    Bind(wxEVT_MOTION, &PresetStepSlider::on_motion, this);
    Bind(wxEVT_LEFT_UP, &PresetStepSlider::on_left_up, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PresetStepSlider::on_capture_lost, this);
}

void PresetStepSlider::set_selection(int index)
{
    if (m_labels.empty())
        return;
    const int next = std::clamp(index, 0, static_cast<int>(m_labels.size()) - 1);
    if (m_selection == next)
        return;
    m_selection = next;
    Refresh();
}

void PresetStepSlider::set_change_handler(ChangeHandler handler)
{
    m_change_handler = std::move(handler);
}

wxPoint PresetStepSlider::knot_center(int index) const
{
    const int count = static_cast<int>(m_labels.size());
    const int pad = FromDIP(kSidePadDip);
    const int y = FromDIP(kTrackYDip);
    const int span = std::max(1, GetClientSize().GetWidth() - 2 * pad);
    if (count <= 1)
        return wxPoint(pad, y);
    const int x = pad + (span * index) / (count - 1);
    return wxPoint(x, y);
}

int PresetStepSlider::hit_test(const wxPoint &pos) const
{
    const int count = static_cast<int>(m_labels.size());
    if (count <= 0)
        return -1;
    int best = 0;
    int best_d = std::abs(pos.x - knot_center(0).x);
    for (int i = 1; i < count; ++i) {
        const int d = std::abs(pos.x - knot_center(i).x);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void PresetStepSlider::select_from_mouse(const wxPoint &pos)
{
    const int index = hit_test(pos);
    if (index < 0 || index == m_selection)
        return;
    m_selection = index;
    Refresh();
    if (m_change_handler)
        m_change_handler(m_selection);
}

void PresetStepSlider::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC raw(this);
    raw.SetBackground(wxBrush(GetBackgroundColour()));
    raw.Clear();
    wxGCDC dc(raw);

    const wxSize size = GetClientSize();
    const int count = static_cast<int>(m_labels.size());
    if (count <= 0 || size.x <= 0)
        return;

    const int pad = FromDIP(kSidePadDip);
    const int y = FromDIP(kTrackYDip);
    const int knot_r = FromDIP(kKnotRDip);
    const int thumb_r = FromDIP(kThumbRDip);
    const wxColour track(0x9A, 0x9A, 0x9A);
    const wxColour knot(0x8A, 0x8A, 0x8A);
    const wxColour thumb(0x5A, 0x5A, 0x5A);

    dc.SetPen(wxPen(track, FromDIP(2)));
    dc.DrawLine(pad, y, size.x - pad, y);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(knot));
    for (int i = 0; i < count; ++i) {
        const wxPoint c = knot_center(i);
        dc.DrawCircle(c, knot_r);
    }

    const wxPoint thumb_c = knot_center(m_selection);
    dc.SetBrush(wxBrush(thumb));
    dc.DrawCircle(thumb_c, thumb_r);

    dc.SetPen(wxPen(*wxWHITE, std::max(1, FromDIP(1))));
    const int line_w = FromDIP(8);
    const int gap = FromDIP(3);
    for (int i = -1; i <= 1; ++i) {
        const int ly = thumb_c.y + i * gap;
        dc.DrawLine(thumb_c.x - line_w / 2, ly, thumb_c.x + line_w / 2, ly);
    }

    wxFont font = GetFont();
    font.SetWeight(wxFONTWEIGHT_NORMAL);
    dc.SetFont(font);
    for (int i = 0; i < count; ++i) {
        const wxColour colour = DeviceUiStyle::text_primary();
        dc.SetTextForeground(colour);
        const wxSize ts = dc.GetTextExtent(m_labels[i]);
        const wxPoint c = knot_center(i);
        dc.DrawText(m_labels[i], c.x - ts.x / 2, y + thumb_r + FromDIP(6));
    }
}

void PresetStepSlider::on_left_down(wxMouseEvent &event)
{
    m_dragging = true;
    select_from_mouse(event.GetPosition());
}

void PresetStepSlider::on_motion(wxMouseEvent &event)
{
    if (m_dragging && event.LeftIsDown())
        select_from_mouse(event.GetPosition());
    else if (m_dragging && !event.LeftIsDown())
        end_drag();
}

void PresetStepSlider::on_left_up(wxMouseEvent &event)
{
    if (m_dragging)
        select_from_mouse(event.GetPosition());
    end_drag();
}

void PresetStepSlider::on_capture_lost(wxMouseCaptureLostEvent &)
{
    end_drag();
}

void PresetStepSlider::end_drag()
{
    m_dragging = false;
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
