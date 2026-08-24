#include "PresetStepSlider.hpp"

#include "DeviceUiStyle.hpp"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

constexpr int kTrackYDip = 22;
constexpr int kSidePadDip = 22;
constexpr int kThumbRDip = 11;
constexpr int kKnotWDip = 1;
constexpr int kKnotHDip = 4;
constexpr int kTrackHDip = 4;

} // namespace

PresetStepSlider::PresetStepSlider(wxWindow *parent, std::vector<wxString> labels)
    : wxPanel(parent, wxID_ANY)
    , m_labels(std::move(labels))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(255, 255, 255));
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetMinSize(wxSize(FromDIP(360), FromDIP(64)));
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

void PresetStepSlider::set_enabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!m_enabled)
        m_dragging = false;
    SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

int PresetStepSlider::side_pad() const
{
    const int min_pad = FromDIP(kSidePadDip);
    if (m_labels.empty())
        return min_pad;
    wxClientDC dc(const_cast<PresetStepSlider *>(this));
    wxFont font = GetFont();
    font.SetWeight(wxFONTWEIGHT_NORMAL);
    dc.SetFont(font);
    const int first = dc.GetTextExtent(m_labels.front()).GetWidth();
    const int last = dc.GetTextExtent(m_labels.back()).GetWidth();
    const int extra = FromDIP(4);
    return std::max({min_pad, first / 2 + extra, last / 2 + extra});
}

wxPoint PresetStepSlider::knot_center(int index) const
{
    const int count = static_cast<int>(m_labels.size());
    const int pad = side_pad();
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
    if (!m_enabled)
        return;
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

    const int pad = side_pad();
    const int y = FromDIP(kTrackYDip);
    const int knot_w = FromDIP(kKnotWDip);
    const int knot_h = FromDIP(kKnotHDip);
    const int track_h = FromDIP(kTrackHDip);
    const int thumb_r = FromDIP(kThumbRDip);
    const int border_w = std::max(1, FromDIP(1));
    const wxColour track(0xEF, 0xEE, 0xEE);
    const wxColour knot(0x8A, 0x8A, 0x8A);
    const wxColour thumb_fill(255, 255, 255);
    const wxColour thumb_border(0xE1, 0xE1, 0xE1);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(track));
    dc.DrawRectangle(pad, y - track_h / 2, std::max(1, size.x - 2 * pad), track_h);

    dc.SetBrush(wxBrush(knot));
    for (int i = 0; i < count; ++i) {
        const wxPoint c = knot_center(i);
        dc.DrawRectangle(c.x - knot_w / 2, y - knot_h / 2, knot_w, knot_h);
    }

    const wxPoint thumb_c = knot_center(m_selection);
    dc.SetPen(wxPen(thumb_border, border_w));
    dc.SetBrush(wxBrush(thumb_fill));
    dc.DrawCircle(thumb_c, thumb_r);

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
    if (!m_enabled)
        return;
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
