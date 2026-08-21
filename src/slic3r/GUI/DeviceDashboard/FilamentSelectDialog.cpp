#include "FilamentSelectDialog.hpp"

#include "DeviceUiStyle.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/font.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

bool is_empty_material(const wxString &material)
{
    return material.IsEmpty()
        || material.CmpNoCase(wxString::FromUTF8("Empty")) == 0
        || material.CmpNoCase(wxString::FromUTF8("N/A")) == 0;
}

void set_muted(wxStaticText *label)
{
    if (label == nullptr)
        return;
    label->SetForegroundColour(DeviceUiStyle::text_muted());
}

void set_primary(wxWindow *win)
{
    if (win == nullptr)
        return;
    win->SetForegroundColour(DeviceUiStyle::text_primary());
}

void set_bold(wxWindow *win)
{
    if (win == nullptr)
        return;
    wxFont font = win->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    win->SetFont(font);
}

const wxColour kSelectionBorder(0x65, 0x8A, 0x4D);
const wxColour kValueBorder(0x78, 0x78, 0x78);
const wxColour kChoiceRowDivider(0xA9, 0xA9, 0xA9);

class ColorSwatch : public wxPanel
{
public:
    ColorSwatch(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(wxSize(-1, FromDIP(26)));
        SetBackgroundColour(parent->GetBackgroundColour());
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ColorSwatch::on_paint, this);
    }

    void set_colour(const wxColour &colour)
    {
        m_colour = colour;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        const int pad = FromDIP(4);
        const int border = FromDIP(1);
        const int outer_w = std::max(0, size.x - border);
        const int outer_h = std::max(0, size.y - border);
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (m_colour.IsOk()) {
            dc.SetBrush(wxBrush(m_colour));
            dc.DrawRoundedRectangle(pad, pad, std::max(0, outer_w - pad * 2), std::max(0, outer_h - pad * 2), FromDIP(3));
        }
        dc.SetPen(wxPen(kValueBorder, border));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, outer_w, outer_h, FromDIP(5));
    }

    wxColour m_colour;
};

class SelectedValueBox : public wxPanel
{
public:
    SelectedValueBox(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(parent->GetBackgroundColour());
        m_label = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("--"));
        set_primary(m_label);
        set_bold(m_label);
        m_label->SetBackgroundColour(GetBackgroundColour());
        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, parent->FromDIP(4));
        SetSizer(sizer);
        Bind(wxEVT_PAINT, &SelectedValueBox::on_paint, this);
    }

    wxStaticText *label() { return m_label; }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        dc.SetPen(wxPen(kValueBorder, FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        const wxSize size = GetClientSize();
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(5));
    }

    wxStaticText *m_label{nullptr};
};

class ChoiceRow : public wxPanel
{
public:
    using ClickHandler = std::function<void()>;

    ChoiceRow(wxWindow *parent, const wxString &label)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(DeviceUiStyle::card_background());
        SetCursor(wxCursor(wxCURSOR_HAND));
        m_label = new wxStaticText(this, wxID_ANY, label);
        set_primary(m_label);
        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, parent->FromDIP(6));
        SetSizer(sizer);
        Bind(wxEVT_PAINT, &ChoiceRow::on_paint, this);

        const auto bind_click = [this](wxWindow *win) {
            win->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
                if (m_handler)
                    m_handler();
            });
            win->Bind(wxEVT_MOUSEWHEEL, [](wxMouseEvent &evt) {
                evt.ResumePropagation(wxEVENT_PROPAGATE_MAX);
                evt.Skip();
            });
        };
        bind_click(this);
        bind_click(m_label);
    }

    void set_selected(bool selected)
    {
        m_selected = selected;
        SetBackgroundColour(selected ? DeviceUiStyle::control_background() : DeviceUiStyle::card_background());
        wxFont font = m_label->GetFont();
        font.SetWeight(selected ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_label->SetFont(font);
        m_label->SetBackgroundColour(GetBackgroundColour());
        Refresh();
    }

    void set_handler(ClickHandler handler) { m_handler = std::move(handler); }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        if (m_selected) {
            dc.SetPen(wxPen(kSelectionBorder, FromDIP(2)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(4));
            return;
        }
        dc.SetPen(wxPen(kChoiceRowDivider, FromDIP(1)));
        dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
    }

    wxStaticText *m_label{nullptr};
    ClickHandler  m_handler;
    bool          m_selected{false};
};

class ColorChip : public wxPanel
{
public:
    using ClickHandler = std::function<void()>;

    ColorChip(wxWindow *parent, const wxColour &colour, const wxString &name)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(22), parent->FromDIP(22)))
        , m_colour(colour)
    {
        SetMinSize(wxSize(FromDIP(22), FromDIP(22)));
        SetMaxSize(wxSize(FromDIP(22), FromDIP(22)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(name);
        Bind(wxEVT_PAINT, &ColorChip::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            if (m_handler)
                m_handler();
        });
        Bind(wxEVT_MOUSEWHEEL, [](wxMouseEvent &evt) {
            evt.ResumePropagation(wxEVENT_PROPAGATE_MAX);
            evt.Skip();
        });
    }

    void set_selected(bool selected)
    {
        m_selected = selected;
        Refresh();
    }

    void set_handler(ClickHandler handler) { m_handler = std::move(handler); }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent() != nullptr ? GetParent()->GetBackgroundColour() : *wxWHITE));
        dc.Clear();
        const wxSize size = GetClientSize();
        const int pen_w = m_selected ? FromDIP(2) : FromDIP(1);
        dc.SetPen(wxPen(m_selected ? kSelectionBorder : DeviceUiStyle::card_border(), pen_w));
        dc.SetBrush(wxBrush(m_colour));
        const int inset = pen_w / 2;
        dc.DrawEllipse(inset, inset, size.x - pen_w, size.y - pen_w);
    }

    wxColour     m_colour;
    bool         m_selected{false};
    ClickHandler m_handler;
};

class FilamentScrolledList : public wxPanel
{
public:
    explicit FilamentScrolledList(wxWindow *parent)
        : wxPanel(parent)
    {
        SetBackgroundColour(DeviceUiStyle::card_background());
        SetMinSize(wxSize(0, 0));

        m_scrolled = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_scrolled->SetBackgroundColour(DeviceUiStyle::card_background());
        m_scrolled->SetScrollRate(0, parent->FromDIP(8));
        m_scrolled->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        m_scrolled->EnableScrolling(false, true);
        m_scrolled->SetMinSize(wxSize(0, 0));

        m_inner = new wxPanel(m_scrolled, wxID_ANY);
        m_inner->SetBackgroundColour(DeviceUiStyle::card_background());

        const int bar_w = FromDIP(10);
        m_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(bar_w, -1));
        m_bar->SetMinSize(wxSize(bar_w, -1));
        m_bar->SetMaxSize(wxSize(bar_w, -1));
        m_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_bar->SetCursor(wxCursor(wxCURSOR_HAND));
        m_bar->Hide();

        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_scrolled, 1, wxEXPAND);
        sizer->Add(m_bar, 0, wxEXPAND | wxLEFT, FromDIP(2));
        SetSizer(sizer);

        m_bar->Bind(wxEVT_PAINT, &FilamentScrolledList::on_bar_paint, this);
        m_bar->Bind(wxEVT_LEFT_DOWN, &FilamentScrolledList::on_bar_down, this);
        m_bar->Bind(wxEVT_LEFT_UP, &FilamentScrolledList::on_bar_up, this);
        m_bar->Bind(wxEVT_MOTION, &FilamentScrolledList::on_bar_move, this);
        m_bar->Bind(wxEVT_MOUSEWHEEL, &FilamentScrolledList::on_bar_wheel, this);
        m_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { m_dragging = false; });

        auto on_scroll = [this](wxScrollWinEvent &evt) {
            evt.Skip();
            update_bar();
        };
        m_scrolled->Bind(wxEVT_SCROLLWIN_TOP, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_BOTTOM, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_LINEUP, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_PAGEUP, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_scroll);
        m_scrolled->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_scroll);
        m_scrolled->Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            evt.Skip();
            const int width = std::max(0, m_scrolled->GetClientSize().GetWidth());
            const int height = m_inner->GetMinSize().GetHeight();
            if (height > 0)
                m_inner->SetSize(0, 0, width, height);
            update_bar();
        });
        const auto on_wheel = [this](wxMouseEvent &evt) {
            evt.Skip();
            CallAfter([this] { update_bar(); });
        };
        m_scrolled->Bind(wxEVT_MOUSEWHEEL, on_wheel);
        m_inner->Bind(wxEVT_MOUSEWHEEL, on_wheel);
    }

    wxWindow *content() { return m_inner; }

    void refresh()
    {
        const int width = std::max(m_scrolled->GetClientSize().GetWidth(), FromDIP(1));
        wxSizer *inner_sizer = m_inner->GetSizer();
        int height = 0;
        if (inner_sizer != nullptr) {
            height = inner_sizer->CalcMin().GetHeight();
            m_inner->SetMinSize(wxSize(width, height));
            m_inner->SetSize(0, 0, width, height);
            m_inner->Layout();
        }
        m_scrolled->SetVirtualSize(width, height);
        update_bar();
        Layout();
    }

private:
    int ppu_y() const
    {
        int x = 0;
        int y = 0;
        m_scrolled->GetScrollPixelsPerUnit(&x, &y);
        return y;
    }

    int view_pixel() const
    {
        int x = 0;
        int y = 0;
        m_scrolled->GetViewStart(&x, &y);
        return y * ppu_y();
    }

    int max_view_pixel() const
    {
        return std::max(0, virtual_h() - client_h());
    }

    int virtual_h() const
    {
        return m_inner != nullptr ? m_inner->GetMinSize().GetHeight() : 0;
    }

    int client_h() const { return m_scrolled->GetClientSize().GetHeight(); }

    wxRect thumb_rect() const
    {
        const wxSize size = m_bar->GetClientSize();
        const int vh = virtual_h();
        const int ch = client_h();
        if (vh <= ch || size.y <= 0)
            return wxRect();
        const int thumb_h = std::max(FromDIP(18), ch * size.y / vh);
        const int max_thumb = std::max(0, size.y - thumb_h);
        const int max_view = std::max(1, vh - ch);
        const int thumb_y = view_pixel() * max_thumb / max_view;
        return wxRect(0, thumb_y, size.x, thumb_h);
    }

    void scroll_to_pixel(int view_y)
    {
        view_y = std::clamp(view_y, 0, max_view_pixel());
        const int ppu = ppu_y();
        m_scrolled->Scroll(0, ppu > 0 ? view_y / ppu : 0);
        update_bar();
    }

    void update_bar()
    {
        if (m_updating)
            return;
        m_updating = true;
        const bool need = virtual_h() > client_h() && client_h() > 0;
        if (m_bar->IsShown() != need) {
            m_bar->Show(need);
            Layout();
        }
        if (need)
            m_bar->Refresh();
        m_updating = false;
    }

    void on_bar_paint(wxPaintEvent &)
    {
        wxPaintDC dc(m_bar);
        dc.SetBackground(wxBrush(DeviceUiStyle::control_background()));
        dc.Clear();
        const wxRect thumb = thumb_rect();
        if (thumb.GetHeight() <= 0)
            return;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(DeviceUiStyle::card_border()));
        dc.DrawRoundedRectangle(thumb, FromDIP(4));
    }

    void on_bar_down(wxMouseEvent &evt)
    {
        const wxRect thumb = thumb_rect();
        if (thumb.Contains(evt.GetPosition())) {
            m_dragging = true;
            m_drag_offset = evt.GetY() - thumb.GetY();
            m_bar->CaptureMouse();
        } else {
            const int page = client_h();
            scroll_to_pixel(view_pixel() + (evt.GetY() < thumb.GetY() ? -page : page));
        }
    }

    void on_bar_up(wxMouseEvent &)
    {
        if (m_bar->HasCapture())
            m_bar->ReleaseMouse();
        m_dragging = false;
    }

    void on_bar_move(wxMouseEvent &evt)
    {
        if (!m_dragging || !evt.Dragging())
            return;
        const wxRect thumb = thumb_rect();
        const int max_thumb = std::max(1, m_bar->GetClientSize().GetHeight() - thumb.GetHeight());
        const int thumb_y = std::clamp(evt.GetY() - m_drag_offset, 0, max_thumb);
        scroll_to_pixel(thumb_y * max_view_pixel() / max_thumb);
    }

    void on_bar_wheel(wxMouseEvent &evt)
    {
        const int step = std::max(1, ppu_y()) * 3;
        scroll_to_pixel(view_pixel() + (evt.GetWheelRotation() > 0 ? -step : step));
    }

    wxScrolledWindow *m_scrolled{nullptr};
    wxPanel *         m_inner{nullptr};
    wxPanel *         m_bar{nullptr};
    bool              m_dragging{false};
    bool              m_updating{false};
    int               m_drag_offset{0};
};

wxWindow *make_scrolled_list(wxWindow *parent)
{
    return new FilamentScrolledList(parent);
}

wxWindow *list_content(wxWindow *list)
{
    if (auto *host = dynamic_cast<FilamentScrolledList *>(list))
        return host->content();
    return list;
}

void refresh_scrolled(wxWindow *list)
{
    if (list == nullptr)
        return;
    if (auto *host = dynamic_cast<FilamentScrolledList *>(list)) {
        host->refresh();
        return;
    }
    if (auto *host = dynamic_cast<FilamentScrolledList *>(list->GetParent())) {
        host->refresh();
        return;
    }
    list->Layout();
}

void set_choice_selected(wxWindow *list, int selected)
{
    wxWindow *content = list_content(list);
    if (content == nullptr)
        return;
    int index = 0;
    for (wxWindow *child : content->GetChildren()) {
        if (auto *row = dynamic_cast<ChoiceRow *>(child))
            row->set_selected(index++ == selected);
    }
}

void set_color_selected(wxWindow *list, int selected)
{
    wxWindow *content = list_content(list);
    if (content == nullptr)
        return;
    int index = 0;
    for (wxWindow *child : content->GetChildren()) {
        if (auto *chip = dynamic_cast<ColorChip *>(child))
            chip->set_selected(index++ == selected);
    }
}

StaticBox *make_box(wxWindow *parent)
{
    auto *box = new StaticBox(parent, wxID_ANY);
    box->SetCornerRadius(parent->FromDIP(8));
    box->SetBorderWidth(parent->FromDIP(1));
    box->SetBorderColorNormal(DeviceUiStyle::card_border());
    box->SetBackgroundColorNormal(DeviceUiStyle::card_background());
    box->SetBackgroundColour(DeviceUiStyle::card_background());
    return box;
}

} // namespace

FilamentSelectDialog::FilamentSelectDialog(wxWindow *parent, int ui_tool, const wxString &initial_material,
    const wxColour &initial_color, const wxString &initial_brand)
    : wxDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_catalog(FilamentCatalog::load())
{
    m_brand_index = -1;
    m_type_index = -1;
    m_color_index = -1;

    if (!initial_brand.IsEmpty()) {
        for (size_t i = 0; i < m_catalog.brands.size(); ++i) {
            if (m_catalog.brands[i].CmpNoCase(initial_brand) == 0) {
                m_brand_index = static_cast<int>(i);
                break;
            }
        }
    }
    if (!is_empty_material(initial_material)) {
        for (size_t i = 0; i < m_catalog.types.size(); ++i) {
            if (m_catalog.types[i].name.CmpNoCase(initial_material) == 0) {
                m_type_index = static_cast<int>(i);
                break;
            }
        }
    }
    if (initial_color.IsOk())
        m_color_index = m_catalog.index_of_color(initial_color);

    SetBackgroundColour(DeviceUiStyle::page_background());
    build_ui(ui_tool);
    SetClientSize(FromDIP(550), FromDIP(330));
    SetMinSize(GetSize());
    SetMaxSize(GetSize());
    Layout();
    refresh_scrolled(m_brand_list);
    refresh_scrolled(m_type_list);
    refresh_scrolled(m_color_list);
    center_on_screen();
}

int FilamentSelectDialog::ShowModal()
{
    center_on_screen();
    return wxDialog::ShowModal();
}

void FilamentSelectDialog::center_on_screen()
{
    int display_index = wxDisplay::GetFromWindow(GetParent() != nullptr ? GetParent() : this);
    if (display_index == wxNOT_FOUND)
        display_index = wxDisplay::GetFromPoint(wxGetMousePosition());
    if (display_index == wxNOT_FOUND)
        display_index = 0;

    const wxRect area = wxDisplay(display_index).GetClientArea();
    const wxSize size = GetSize();
    SetPosition(wxPoint(
        area.GetX() + (area.GetWidth() - size.GetWidth()) / 2,
        area.GetY() + (area.GetHeight() - size.GetHeight()) / 2));
}

wxString FilamentSelectDialog::material() const
{
    if (m_type_index < 0 || m_type_index >= static_cast<int>(m_catalog.types.size()))
        return wxEmptyString;
    return m_catalog.types[static_cast<size_t>(m_type_index)].name;
}

wxString FilamentSelectDialog::color_hex() const
{
    if (m_color_index < 0 || m_color_index >= static_cast<int>(m_catalog.colors.size()))
        return wxEmptyString;
    const wxColour colour = m_catalog.colors[static_cast<size_t>(m_color_index)].colour;
    return wxString::Format("#%02X%02X%02X", colour.Red(), colour.Green(), colour.Blue());
}

FilamentSelection FilamentSelectDialog::selection() const
{
    FilamentSelection out;
    if (m_brand_index >= 0 && m_brand_index < static_cast<int>(m_catalog.brands.size()))
        out.brand = m_catalog.brands[static_cast<size_t>(m_brand_index)];
    out.type = material();
    if (m_color_index >= 0 && m_color_index < static_cast<int>(m_catalog.colors.size())) {
        out.color = m_catalog.colors[static_cast<size_t>(m_color_index)].name;
        out.color_hex = color_hex().Lower();
    }
    if (!out.type.IsEmpty()) {
        if (const FilamentTypeInfo *info = m_catalog.find_type(out.type)) {
            out.temp_min = info->temp_min;
            out.temp_max = info->temp_max;
            out.pressure_advance = info->pressure_advance;
        }
    }
    return out;
}

void FilamentSelectDialog::build_ui(int ui_tool)
{
    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *header = new wxPanel(this, wxID_ANY);
    header->SetBackgroundColour(DeviceUiStyle::card_header_background());
    auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new wxStaticText(header, wxID_ANY, wxString::Format("Tool %d Filament", ui_tool));
    set_primary(title);
    set_bold(title);
    title->SetBackgroundColour(DeviceUiStyle::card_header_background());
    auto *close_x = new Button(header, wxString::FromUTF8("\u00D7"));
    close_x->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    close_x->SetCornerRadius(FromDIP(8));
    close_x->SetMinSize(wxSize(FromDIP(34), FromDIP(34)));
    close_x->SetPaddingSize(wxSize(FromDIP(6), FromDIP(2)));
    close_x->SetBorderWidth(0);
    close_x->SetBackgroundColour(DeviceUiStyle::card_header_background());
    close_x->SetBackgroundColor(StateColor(
        std::pair(wxColour(220, 220, 220), (int) StateColor::Pressed),
        std::pair(wxColour(235, 235, 235), (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::card_header_background(), (int) StateColor::Normal)));
    close_x->SetTextColor(StateColor(std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Normal)));
    header_sizer->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
    header_sizer->Add(close_x, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    header->SetSizer(header_sizer);
    header->SetMinSize(wxSize(-1, FromDIP(48)));
    root->Add(header, 0, wxEXPAND);

    auto *content = new wxPanel(this, wxID_ANY);
    content->SetBackgroundColour(DeviceUiStyle::page_background());
    auto *content_sizer = new wxBoxSizer(wxVERTICAL);

    auto *columns = new wxBoxSizer(wxHORIZONTAL);

    auto add_section = [&](const wxString &caption, wxStaticText **selected_out, wxWindow **list_out) {
        StaticBox *box = make_box(content);
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        auto *caption_label = new wxStaticText(box, wxID_ANY, caption);
        set_muted(caption_label);
        auto *selected_box = new SelectedValueBox(box);
        auto *list = make_scrolled_list(box);
        sizer->Add(caption_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        sizer->Add(selected_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        sizer->Add(list, 1, wxEXPAND | wxALL, FromDIP(8));
        box->SetSizer(sizer);
        box->SetMinSize(wxSize(0, -1));
        *selected_out = selected_box->label();
        *list_out = list;
        columns->Add(box, 1, wxEXPAND);
    };

    add_section(wxString::FromUTF8("Selected Brand"), &m_brand_selected, &m_brand_list);
    columns->AddSpacer(FromDIP(10));

    add_section(wxString::FromUTF8("Type"), &m_type_selected, &m_type_list);
    columns->AddSpacer(FromDIP(10));

    {
        StaticBox *box = make_box(content);
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        auto *caption_label = new wxStaticText(box, wxID_ANY, wxString::FromUTF8("Color"));
        set_muted(caption_label);
        auto *swatch = new ColorSwatch(box);
        m_color_swatch = swatch;
        sizer->Add(caption_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        sizer->Add(swatch, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        auto *list = make_scrolled_list(box);
        m_color_list = list;
        sizer->Add(list, 1, wxEXPAND | wxALL, FromDIP(8));
        box->SetSizer(sizer);
        box->SetMinSize(wxSize(0, -1));
        columns->Add(box, 1, wxEXPAND);
    }

    content_sizer->Add(columns, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    StaticBox *info = make_box(content);
    auto *info_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto *temp_col = new wxBoxSizer(wxVERTICAL);
    auto *temp_title = new wxStaticText(info, wxID_ANY, wxString::FromUTF8("Nozzle Temperature"));
    set_muted(temp_title);
    m_temp_min = new wxStaticText(info, wxID_ANY, wxEmptyString);
    m_temp_max = new wxStaticText(info, wxID_ANY, wxEmptyString);
    set_primary(m_temp_min);
    set_primary(m_temp_max);
    temp_col->Add(temp_title, 0, wxBOTTOM, FromDIP(6));
    temp_col->Add(m_temp_min, 0, wxBOTTOM, FromDIP(2));
    temp_col->Add(m_temp_max, 0);

    auto *pa_col = new wxBoxSizer(wxVERTICAL);
    auto *pa_title = new wxStaticText(info, wxID_ANY, wxString::FromUTF8("Pressure Advance"));
    set_muted(pa_title);
    m_pa_value = new wxStaticText(info, wxID_ANY, wxEmptyString);
    set_primary(m_pa_value);
    set_bold(m_pa_value);
    pa_col->Add(pa_title, 0, wxBOTTOM, FromDIP(6));
    pa_col->Add(m_pa_value, 0);

    auto *reset = new Button(info, wxString::FromUTF8("Reset"));
    auto *confirm = new Button(info, wxString::FromUTF8("Confirm"));
    style_button(reset, false);
    style_button(confirm, true);
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(reset, 0, wxRIGHT, FromDIP(8));
    buttons->Add(confirm, 0);

    info_sizer->Add(temp_col, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
    info_sizer->Add(pa_col, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
    info_sizer->AddStretchSpacer(1);
    info_sizer->Add(buttons, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
    info->SetSizer(info_sizer);
    content_sizer->Add(info, 0, wxEXPAND | wxALL, FromDIP(12));

    content->SetSizer(content_sizer);
    root->Add(content, 1, wxEXPAND);
    SetSizer(root);

    rebuild_brand_list();
    rebuild_type_list();
    rebuild_color_list();
    refresh_info();

    close_x->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { reset_defaults(); });
    confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_brand_index < 0 || m_type_index < 0 || m_color_index < 0) {
            wxMessageDialog dlg(this,
                wxString::FromUTF8("Please select brand, type and color before confirming."),
                wxString::FromUTF8("Filament"),
                wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
            return;
        }
        EndModal(wxID_OK);
    });
}

void FilamentSelectDialog::rebuild_brand_list()
{
    wxWindow *list = list_content(m_brand_list);
    if (list == nullptr)
        return;
    list->SetSizer(nullptr, true);
    list->DestroyChildren();
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    for (size_t i = 0; i < m_catalog.brands.size(); ++i) {
        auto *row = new ChoiceRow(list, m_catalog.brands[i]);
        row->set_selected(static_cast<int>(i) == m_brand_index);
        row->set_handler([this, i]() { select_brand(static_cast<int>(i)); });
        sizer->Add(row, 0, wxEXPAND);
    }
    list->SetSizer(sizer);
    if (m_brand_index >= 0 && m_brand_index < static_cast<int>(m_catalog.brands.size()))
        m_brand_selected->SetLabel(m_catalog.brands[static_cast<size_t>(m_brand_index)]);
    else
        m_brand_selected->SetLabel(wxEmptyString);
    refresh_scrolled(m_brand_list);
}

void FilamentSelectDialog::rebuild_type_list()
{
    wxWindow *list = list_content(m_type_list);
    if (list == nullptr)
        return;
    list->SetSizer(nullptr, true);
    list->DestroyChildren();
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    for (size_t i = 0; i < m_catalog.types.size(); ++i) {
        auto *row = new ChoiceRow(list, m_catalog.types[i].name);
        row->set_selected(static_cast<int>(i) == m_type_index);
        row->set_handler([this, i]() { select_type(static_cast<int>(i)); });
        sizer->Add(row, 0, wxEXPAND);
    }
    list->SetSizer(sizer);
    if (m_type_index >= 0 && m_type_index < static_cast<int>(m_catalog.types.size()))
        m_type_selected->SetLabel(m_catalog.types[static_cast<size_t>(m_type_index)].name);
    else
        m_type_selected->SetLabel(wxEmptyString);
    refresh_scrolled(m_type_list);
}

void FilamentSelectDialog::rebuild_color_list()
{
    wxWindow *list = list_content(m_color_list);
    if (list == nullptr)
        return;
    list->SetSizer(nullptr, true);
    list->DestroyChildren();

    const int cols = 4;
    const int gap = FromDIP(6);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *row = nullptr;
    for (size_t i = 0; i < m_catalog.colors.size(); ++i) {
        if (i % static_cast<size_t>(cols) == 0) {
            row = new wxBoxSizer(wxHORIZONTAL);
            sizer->Add(row, 0, wxBOTTOM, gap);
        }
        auto *chip = new ColorChip(list, m_catalog.colors[i].colour, m_catalog.colors[i].name);
        chip->set_selected(static_cast<int>(i) == m_color_index);
        chip->set_handler([this, i]() { select_color(static_cast<int>(i)); });
        row->Add(chip, 0, wxRIGHT, gap);
    }
    list->SetSizer(sizer);
    if (auto *swatch = dynamic_cast<ColorSwatch *>(m_color_swatch)) {
        if (m_color_index >= 0 && m_color_index < static_cast<int>(m_catalog.colors.size()))
            swatch->set_colour(m_catalog.colors[static_cast<size_t>(m_color_index)].colour);
        else
            swatch->set_colour(wxColour());
    }
    refresh_scrolled(m_color_list);
}

void FilamentSelectDialog::select_brand(int index)
{
    if (index < 0 || index >= static_cast<int>(m_catalog.brands.size()))
        return;
    m_brand_index = index;
    set_choice_selected(m_brand_list, index);
    m_brand_selected->SetLabel(m_catalog.brands[static_cast<size_t>(index)]);
}

void FilamentSelectDialog::select_type(int index)
{
    if (index < 0 || index >= static_cast<int>(m_catalog.types.size()))
        return;
    m_type_index = index;
    set_choice_selected(m_type_list, index);
    m_type_selected->SetLabel(m_catalog.types[static_cast<size_t>(index)].name);
    refresh_info();
}

void FilamentSelectDialog::select_color(int index)
{
    if (index < 0 || index >= static_cast<int>(m_catalog.colors.size()))
        return;
    m_color_index = index;
    set_color_selected(m_color_list, index);
    if (auto *swatch = dynamic_cast<ColorSwatch *>(m_color_swatch))
        swatch->set_colour(m_catalog.colors[static_cast<size_t>(index)].colour);
}

void FilamentSelectDialog::refresh_info()
{
    const FilamentTypeInfo *info = m_type_index >= 0 ? m_catalog.find_type(material()) : nullptr;
    if (m_temp_min != nullptr)
        m_temp_min->SetLabel(info != nullptr ? wxString::Format("Minimum: %d\u00B0", info->temp_min) : wxString());
    if (m_temp_max != nullptr)
        m_temp_max->SetLabel(info != nullptr ? wxString::Format("Maximum: %d\u00B0", info->temp_max) : wxString());
    if (m_pa_value != nullptr)
        m_pa_value->SetLabel(info != nullptr ? wxString::Format("%.2fmm", info->pressure_advance) : wxString());
}

void FilamentSelectDialog::reset_defaults()
{
    m_brand_index = -1;
    m_type_index = -1;
    m_color_index = -1;
    rebuild_brand_list();
    rebuild_type_list();
    rebuild_color_list();
    refresh_info();
}

void FilamentSelectDialog::style_button(Button *button, bool primary)
{
    if (button == nullptr)
        return;
    button->SetStyle(primary ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    button->SetCornerRadius(FromDIP(8));
    button->SetMinSize(wxSize(FromDIP(92), FromDIP(34)));
    button->SetPaddingSize(wxSize(FromDIP(14), FromDIP(8)));
    button->SetBorderWidth(primary ? 0 : FromDIP(1));
    button->SetBackgroundColour(DeviceUiStyle::card_background());
    const wxColour fill = primary ? DeviceUiStyle::accent() : DeviceUiStyle::control_background();
    button->SetBackgroundColor(StateColor(
        std::pair(primary ? wxColour(33, 141, 97) : wxColour(224, 224, 224), (int) StateColor::Pressed),
        std::pair(primary ? wxColour(53, 198, 136) : wxColour(238, 238, 238), (int) StateColor::Hovered),
        std::pair(fill, (int) StateColor::Normal)));
    button->SetBorderColor(StateColor(std::pair(DeviceUiStyle::card_border(), (int) StateColor::Normal)));
    button->SetTextColor(StateColor(
        std::pair(primary ? *wxWHITE : DeviceUiStyle::text_primary(), (int) StateColor::Normal)));
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
