#include "DeviceCardFrame.hpp"

#include "DeviceUiStyle.hpp"

#include <cmath>
#include <memory>

#include <wx/bitmap.h>
#include <wx/dcclient.h>
#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

class RoundedSectionPanel : public wxPanel
{
public:
    enum class Corners { Top, Bottom, All };

    RoundedSectionPanel(wxWindow *parent, const wxColour &fill, Corners corners, int radius,
                        const wxColour &bottom_border = wxNullColour)
        : wxPanel(parent, wxID_ANY)
        , m_fill(fill)
        , m_bottom_border(bottom_border)
        , m_corners(corners)
        , m_radius(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(fill);
        Bind(wxEVT_PAINT, &RoundedSectionPanel::on_paint, this);
    }

private:
    wxColour parent_fill() const
    {
        wxWindow *host = GetParent();
        if (host != nullptr && host->GetBackgroundColour().IsOk())
            return host->GetBackgroundColour();
        return DeviceUiStyle::page_background();
    }

    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(parent_fill()));
        dc.Clear();
        if (size.x <= 0 || size.y <= 0)
            return;

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        const double w = size.x;
        const double h = size.y;
        const double r = m_radius;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(m_fill));
        switch (m_corners) {
        case Corners::Top:
            gc->DrawRoundedRectangle(0, 0, w, h + r, r);
            break;
        case Corners::Bottom:
            gc->DrawRoundedRectangle(0, -r, w, h + r, r);
            break;
        case Corners::All:
            gc->DrawRoundedRectangle(0, 0, w, h, r);
            break;
        }

        if (m_bottom_border.IsOk() && h >= 1) {
            gc->SetPen(wxPen(m_bottom_border, FromDIP(1)));
            gc->StrokeLine(0, h - 0.5, w, h - 0.5);
        }
    }

    wxColour m_fill;
    wxColour m_bottom_border;
    Corners  m_corners;
    int      m_radius{0};
};

// Paints only the page-background wedge outside the card radius so square
// children (camera viewport, play bar) do not cover the bottom corners.
class BottomCornerMask : public wxPanel
{
public:
    enum class Side { Left, Right };

    BottomCornerMask(wxWindow *parent, Side side)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_side(side)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCanFocus(false);
        Bind(wxEVT_PAINT, &BottomCornerMask::on_paint, this);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        const int px = size.x;
        const int py = size.y;
        wxImage image(px, py, false);
        image.InitAlpha();
        unsigned char *rgb = image.GetData();
        unsigned char *alpha = image.GetAlpha();
        if (rgb == nullptr || alpha == nullptr)
            return;

        const wxColour bg = DeviceUiStyle::page_background();
        const double cx = (m_side == Side::Left) ? px : 0.0;
        const double cy = 0.0;
        const double radius = static_cast<double>(px);

        for (int y = 0; y < py; ++y) {
            for (int x = 0; x < px; ++x) {
                const int i = y * px + x;
                const double dx = (x + 0.5) - cx;
                const double dy = (y + 0.5) - cy;
                const double dist = std::sqrt(dx * dx + dy * dy);
                const double edge = dist - radius + 0.5;
                const int coverage = edge <= 0.0 ? 0 : (edge >= 1.0 ? 255 : static_cast<int>(std::lround(edge * 255.0)));
                rgb[i * 3 + 0] = bg.Red();
                rgb[i * 3 + 1] = bg.Green();
                rgb[i * 3 + 2] = bg.Blue();
                alpha[i] = static_cast<unsigned char>(coverage);
            }
        }

        dc.DrawBitmap(wxBitmap(image), 0, 0, true);
    }

    Side m_side;
};

} // namespace

DeviceCardFrame::DeviceCardFrame(wxWindow* parent, const wxString& title, int pad_horizontal, int pad_vertical,
    int content_pad_horizontal, int content_pad_top, int content_pad_bottom)
    : StaticBox(parent, wxID_ANY)
{
    const int radius = FromDIP(DeviceUiStyle::card_radius());
    SetCornerRadius(radius);
    SetBorderWidth(0);
    SetBackgroundColorNormal(DeviceUiStyle::card_background());
    SetBackgroundColour(DeviceUiStyle::page_background());

    const int pad_h = pad_horizontal >= 0 ? pad_horizontal : DeviceUiStyle::card_pad_horizontal(this);
    const int pad_v = pad_vertical >= 0 ? pad_vertical : DeviceUiStyle::card_pad_vertical(this);
    const int content_h = content_pad_horizontal >= 0 ? content_pad_horizontal : pad_h;
    const int content_top = content_pad_top >= 0 ? content_pad_top : FromDIP(10);
    const int content_bottom = content_pad_bottom >= 0 ? content_pad_bottom : pad_v;

    auto* root = new wxBoxSizer(wxVERTICAL);
    if (!title.IsEmpty()) {
        auto* header = new RoundedSectionPanel(this, DeviceUiStyle::card_header_background(),
            RoundedSectionPanel::Corners::Top, radius, DeviceUiStyle::card_header_border());
        m_header_panel = header;
        auto* header_sizer = new wxBoxSizer(wxVERTICAL);
        m_header_row = new wxBoxSizer(wxHORIZONTAL);

        m_title = new wxStaticText(m_header_panel, wxID_ANY, title);
        m_title->SetForegroundColour(DeviceUiStyle::text_primary());
        m_title->SetBackgroundColour(DeviceUiStyle::card_header_background());
        {
            wxFont font = m_title->GetFont();
            font.SetWeight(wxFONTWEIGHT_BOLD);
            m_title->SetFont(font);
        }
        m_header_row->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
        m_header_row->AddStretchSpacer(1);
        header_sizer->AddSpacer(pad_v);
        header_sizer->Add(m_header_row, 0, wxEXPAND | wxLEFT | wxRIGHT, pad_h);
        header_sizer->AddSpacer(pad_v);
        m_header_panel->SetSizer(header_sizer);
        root->Add(m_header_panel, 0, wxEXPAND);
    } else {
        root->AddSpacer(pad_v);
    }

    // Do not custom-paint this panel: a full-size wxBG_STYLE_PAINT Clear()
    // covers child buttons and kills hover. Rounded white body comes from
    // this StaticBox; padding lets the bottom corners show through.
    m_content_parent = new wxPanel(this, wxID_ANY);
    m_content_parent->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
    m_content_parent->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* padded = new wxBoxSizer(wxVERTICAL);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    if (content_top > 0)
        padded->AddSpacer(content_top);
    padded->Add(m_content_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, content_h);
    if (content_bottom > 0)
        padded->AddSpacer(content_bottom);
    m_content_parent->SetSizer(padded);
    root->Add(m_content_parent, 1, wxEXPAND);

    SetSizer(root);

    m_corner_radius = radius;
    m_bottom_left_mask = new BottomCornerMask(this, BottomCornerMask::Side::Left);
    m_bottom_right_mask = new BottomCornerMask(this, BottomCornerMask::Side::Right);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        layout_bottom_corner_masks();
        event.Skip();
    });
    layout_bottom_corner_masks();
}

void DeviceCardFrame::layout_bottom_corner_masks()
{
    const wxSize size = GetClientSize();
    const int radius = m_corner_radius;
    if (radius <= 0 || size.x <= 0 || size.y <= 0)
        return;

    if (m_bottom_left_mask != nullptr) {
        m_bottom_left_mask->SetSize(0, size.y - radius, radius, radius);
        m_bottom_left_mask->Raise();
    }
    if (m_bottom_right_mask != nullptr) {
        m_bottom_right_mask->SetSize(size.x - radius, size.y - radius, radius, radius);
        m_bottom_right_mask->Raise();
    }
}

wxWindow* DeviceCardFrame::content_parent() const
{
    return m_content_parent;
}

void DeviceCardFrame::set_title(const wxString& title)
{
    if (m_title != nullptr)
        m_title->SetLabelText(title);
}

void DeviceCardFrame::set_content(wxWindow* content)
{
    if (content == nullptr || m_content_sizer == nullptr)
        return;

    m_content_sizer->Clear(false);
    m_content_sizer->Add(content, 1, wxEXPAND);
    m_content_parent->Layout();
    layout_bottom_corner_masks();
}

void DeviceCardFrame::set_content(wxSizer* content)
{
    if (content == nullptr || m_content_sizer == nullptr)
        return;

    m_content_sizer->Clear(false);
    m_content_sizer->Add(content, 1, wxEXPAND);
    m_content_parent->Layout();
    layout_bottom_corner_masks();
}

void DeviceCardFrame::set_header_action(wxWindow* action)
{
    if (m_header_row == nullptr)
        return;

    if (m_header_action != nullptr) {
        m_header_row->Detach(m_header_action);
        m_header_action->Hide();
    }

    m_header_action = action;
    if (m_header_action != nullptr) {
        if (m_header_panel != nullptr && m_header_action->GetParent() != m_header_panel)
            m_header_action->Reparent(m_header_panel);
        m_header_action->SetBackgroundColour(DeviceUiStyle::card_header_background());
        m_header_row->Add(m_header_action, 0, wxALIGN_CENTER_VERTICAL);
        m_header_action->Show();
    }

    Layout();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
