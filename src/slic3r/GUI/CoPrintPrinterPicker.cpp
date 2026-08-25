#include "CoPrintPrinterPicker.hpp"

#include "I18N.hpp"
#include "GUI.hpp"
#include "PrinterWebView.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "slic3r/Utils/CoprintMdnsDiscovery.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#ifdef __APPLE__
#include "../Utils/MacDarkMode.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valtext.h>

namespace Slic3r {
namespace GUI {

namespace {

const wxColour kHeaderBg("#FEFFFF");
const wxColour kHeaderSel("#BFE1DE");
const wxColour kHeaderHoverBorder("#009688");
const wxColour kHeaderIdleLine("#E1E1E1");
const wxColour kOnlineBorder("#82AA6F");
const wxColour kOnlineBg("#FBFCF9");
const wxColour kOfflineBorder("#ECECEC");
const wxColour kOfflineBg("#F9FAFA");
const wxColour kOfflineSelectedBorder("#E74C3C");
const wxColour kOfflineSelectedBg("#FDF6F5");
const wxColour kDotOnline("#35AD27");
const wxColour kDotOffline("#767C84");
const wxColour kDotOfflineRed("#E74C3C");
const wxColour kConnectingBorder("#E2B93A");
const wxColour kConnectingBg("#FFF8E8");
const wxColour kDotConnecting("#F2C94C");
const wxColour kTextPrimary("#434343");
const wxColour kTextMuted(118, 124, 132);
const wxColour kAddAccent(40, 167, 69);
const wxColour kAutoCardBg("#FFFFFF");
const wxColour kAutoCardBorder("#EBEBEC");
const wxColour kAutoAddBg("#F7FAF3");
const wxColour kAutoAddBorder("#E7ECE5");
const wxColour kAutoAddText("#559432");
const wxColour kHintBorder("#E9E9EA");
const wxColour kHintBg(240, 240, 242);
const wxColour kCantFindBg("#F0F5EC");
const wxColour kCantFindBorder("#E6EDE3");
const wxColour kAddCaption("#3F3E40");

bool is_ip_input_char(wxUniChar ch)
{
    return (ch >= '0' && ch <= '9') || ch == '.';
}

wxString filter_ip_input(const wxString& value)
{
    wxString filtered;
    filtered.reserve(value.size());
    for (auto ch : value) {
        if (is_ip_input_char(ch))
            filtered += ch;
    }
    return filtered;
}

void restrict_ip_text_ctrl(wxTextCtrl* field)
{
    if (field == nullptr)
        return;
    wxArrayString allowed;
    for (char c = '0'; c <= '9'; ++c)
        allowed.Add(wxString(1, c));
    allowed.Add(".");
    wxTextValidator validator(wxFILTER_INCLUDE_CHAR_LIST);
    validator.SetIncludes(allowed);
    field->SetValidator(validator);
    field->Bind(wxEVT_CHAR, [](wxKeyEvent& evt) {
        const int key = evt.GetKeyCode();
        if (key < WXK_SPACE || key == WXK_DELETE || key > 255) {
            evt.Skip();
            return;
        }
        const wxChar uni = evt.GetUnicodeKey();
        if (uni != WXK_NONE) {
            if (is_ip_input_char(uni))
                evt.Skip();
            return;
        }
        if (is_ip_input_char(static_cast<wxUniChar>(key)))
            evt.Skip();
    });
    field->Bind(wxEVT_TEXT, [field](wxCommandEvent& evt) {
        evt.Skip();
        const wxString raw = field->GetValue();
        const wxString filtered = filter_ip_input(raw);
        if (filtered == raw)
            return;
        long from = 0;
        long to = 0;
        field->GetSelection(&from, &to);
        field->ChangeValue(filtered);
        const long pos = std::min(from, static_cast<long>(filtered.size()));
        field->SetInsertionPoint(pos);
    });
}

void apply_add_caption_style(wxStaticText* label)
{
    if (label == nullptr)
        return;
    wxFont font = Label::sysFont(11, false);
    font.SetWeight(wxFONTWEIGHT_MEDIUM);
    label->SetFont(font);
    label->SetForegroundColour(kAddCaption);
}
const int kAutoCardGapDip = 8;
const wxColour kScrollTrack("#E4E6E8");
const wxColour kScrollThumb("#7A8088");

class AutoListScrollbar : public wxPanel
{
public:
    explicit AutoListScrollbar(wxWindow* parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(wxSize(FromDIP(8), -1));
        SetMaxSize(wxSize(FromDIP(8), -1));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &AutoListScrollbar::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &AutoListScrollbar::on_down, this);
        Bind(wxEVT_LEFT_UP, &AutoListScrollbar::on_up, this);
        Bind(wxEVT_MOTION, &AutoListScrollbar::on_move, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
    }

    void attach(wxScrolledWindow* scrolled) { m_scrolled = scrolled; }

    void sync()
    {
        if (m_scrolled == nullptr)
            return;
        int x = 0;
        int y = 0;
        m_scrolled->GetViewStart(&x, &y);
        int ux = 0;
        int uy = 0;
        m_scrolled->GetScrollPixelsPerUnit(&ux, &uy);
        const int content_h = m_scrolled->GetVirtualSize().GetHeight();
        const int view_h = m_scrolled->GetClientSize().GetHeight();
        const int track_h = GetClientSize().GetHeight();
        const bool need = content_h > view_h && view_h > 0 && uy > 0;
        if (IsShown() != need) {
            Show(need);
            if (wxWindow* page = GetParent())
                page->Layout();
        }
        if (!need) {
            m_thumb_h = 0;
            return;
        }
        m_thumb_h = std::max(FromDIP(24), track_h * view_h / content_h);
        const int max_scroll = std::max(1, content_h - view_h);
        m_thumb_y = (std::max)(0, (track_h - m_thumb_h) * (y * uy) / max_scroll);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC raw_dc(this);
        wxGCDC dc(raw_dc);
        const wxColour bg = GetParent() != nullptr ? GetParent()->GetBackgroundColour() : kHeaderBg;
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
        const int bar_w = FromDIP(4);
        const int bar_x = (GetClientSize().GetWidth() - bar_w) / 2;
        const int track_h = GetClientSize().GetHeight();
        if (track_h <= 0)
            return;
        const double radius = bar_w / 2.0;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(kScrollTrack));
        dc.DrawRoundedRectangle(bar_x, 0, bar_w, track_h, radius);
        if (m_thumb_h > 0) {
            dc.SetBrush(wxBrush(kScrollThumb));
            dc.DrawRoundedRectangle(bar_x, m_thumb_y, bar_w, m_thumb_h, radius);
        }
    }

    void scroll_to_thumb_y(int thumb_y)
    {
        if (m_scrolled == nullptr)
            return;
        const int content_h = m_scrolled->GetVirtualSize().GetHeight();
        const int view_h = m_scrolled->GetClientSize().GetHeight();
        const int track_h = GetClientSize().GetHeight();
        const int max_thumb = std::max(1, track_h - m_thumb_h);
        thumb_y = std::clamp(thumb_y, 0, max_thumb);
        int ux = 0;
        int uy = 0;
        m_scrolled->GetScrollPixelsPerUnit(&ux, &uy);
        const int max_scroll = std::max(1, content_h - view_h);
        const int view_px = thumb_y * max_scroll / max_thumb;
        m_scrolled->Scroll(0, uy > 0 ? view_px / uy : 0);
        sync();
    }

    void on_down(wxMouseEvent& evt)
    {
        if (m_thumb_h <= 0)
            return;
        if (evt.GetY() >= m_thumb_y && evt.GetY() < m_thumb_y + m_thumb_h) {
            m_dragging = true;
            m_drag_off = evt.GetY() - m_thumb_y;
            CaptureMouse();
        } else {
            const int page = m_scrolled != nullptr ? m_scrolled->GetClientSize().GetHeight() : m_thumb_h;
            scroll_to_thumb_y(m_thumb_y + (evt.GetY() < m_thumb_y ? -page : page));
        }
    }

    void on_up(wxMouseEvent&)
    {
        if (HasCapture())
            ReleaseMouse();
        m_dragging = false;
    }

    void on_move(wxMouseEvent& evt)
    {
        if (!m_dragging || !evt.Dragging())
            return;
        scroll_to_thumb_y(evt.GetY() - m_drag_off);
    }

    wxScrolledWindow* m_scrolled{nullptr};
    int               m_thumb_y{0};
    int               m_thumb_h{0};
    int               m_drag_off{0};
    bool              m_dragging{false};
};

std::string friendly_host_from_address(const std::string& addr)
{
    std::string value = addr;
    const auto scheme = value.find("://");
    if (scheme != std::string::npos)
        value = value.substr(scheme + 3);
    const auto slash = value.find('/');
    if (slash != std::string::npos)
        value = value.substr(0, slash);
    if (value.size() > 5 && value.compare(value.size() - 5, 5, ":7125") == 0)
        value.resize(value.size() - 5);
    return value;
}

wxBitmap load_lightbulb_icon(wxWindow* host, int dip)
{
    wxImage img;
    if (!img.LoadFile(from_u8(Slic3r::var("lightbulb.png")), wxBITMAP_TYPE_PNG) || !img.IsOk())
        return {};

#ifdef __APPLE__
    const double scale = std::max(1.0, mac_max_scaling_factor());
    const int px = std::max(1, (int) std::lround(dip * scale));
    img.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(std::move(img), -1, scale);
#else
    const int px = std::max(1, host->FromDIP(dip));
    if (img.GetWidth() != px || img.GetHeight() != px)
        img.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(img);
#endif
}

StaticBox* build_hint_panel(wxWindow* parent, const wxString& text, const wxColour& bg, const wxColour& border, bool with_info_icon)
{
    auto* p = new StaticBox(parent, wxID_ANY);
    p->SetCornerRadius(static_cast<double>(parent->FromDIP(8)));
    p->SetBorderWidth(1);
    p->SetBorderColorNormal(border);
    p->SetBackgroundColorNormal(bg);
    p->SetBackgroundColour(bg);
    auto* s = new wxBoxSizer(wxHORIZONTAL);
    s->AddSpacer(parent->FromDIP(10));
    if (with_info_icon) {
        wxBitmap idea = load_lightbulb_icon(parent, 20);
        auto* icon = new wxStaticBitmap(p, wxID_ANY, idea);
        icon->SetMinSize(wxSize(parent->FromDIP(20), parent->FromDIP(20)));
        icon->SetBackgroundColour(bg);
        s->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, parent->FromDIP(15));
        s->AddSpacer(parent->FromDIP(8));
    }
    auto* t = new wxStaticText(p, wxID_ANY, text);
    t->SetFont(Label::sysFont(10, false));
    t->SetForegroundColour(wxColour(55, 55, 58));
    t->SetBackgroundColour(bg);
    s->Add(t, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, parent->FromDIP(15));
    s->AddSpacer(parent->FromDIP(10));
    p->SetSizer(s);
    return p;
}

wxBitmap rotate_arrow_down(const wxBitmap& src)
{
    if (!src.IsOk())
        return src;
    wxImage img = src.ConvertToImage();
    if (!img.IsOk())
        return src;
    img = img.Rotate90(true);
#ifdef __APPLE__
    return wxBitmap(img, -1, src.GetScaleFactor());
#else
    return wxBitmap(img);
#endif
}

void paint_bg(wxWindow* win, const wxColour& colour)
{
    if (win == nullptr)
        return;
    win->SetBackgroundColour(colour);
}

std::string host_without_port(std::string value)
{
    const auto scheme = value.find("://");
    if (scheme != std::string::npos)
        value = value.substr(scheme + 3);
    const auto slash = value.find('/');
    if (slash != std::string::npos)
        value = value.substr(0, slash);
    if (std::count(value.begin(), value.end(), ':') == 1) {
        const auto colon = value.rfind(':');
        if (colon != std::string::npos)
            value = value.substr(0, colon);
    }
    return value;
}

wxString display_ip_only(const MachineObject* machine)
{
    if (machine == nullptr)
        return {};
    std::string value = machine->get_dev_ip();
    if (value.empty())
        value = machine->get_dev_id();
    return from_u8(host_without_port(value));
}

bool machine_matches_mdns(const MachineObject* machine, const CoprintMdnsPrinter& printer)
{
    if (machine == nullptr || printer.ip.empty())
        return false;
    if (!printer.id.empty() &&
        (machine->get_dev_id() == printer.id || machine->get_dev_ip() == printer.id))
        return true;
    const std::string discovered = host_without_port(printer.ip);
    if (discovered.empty())
        return false;
    return host_without_port(machine->get_dev_ip()) == discovered ||
           host_without_port(machine->get_dev_id()) == discovered;
}

bool is_already_added_mdns_printer(const CoprintMdnsPrinter& printer)
{
    auto* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        return false;
    auto matches_any = [&](const std::map<std::string, MachineObject*>& machines) {
        for (const auto& entry : machines) {
            if (machine_matches_mdns(entry.second, printer))
                return true;
        }
        return false;
    };
    return matches_any(dev->get_my_machine_list()) ||
           matches_any(dev->get_local_machinelist()) ||
           machine_matches_mdns(dev->get_selected_machine(), printer);
}

bool same_machine(const MachineObject* a, const MachineObject* b)
{
    if (a == nullptr || b == nullptr)
        return false;
    return !a->get_dev_id().empty() && a->get_dev_id() == b->get_dev_id();
}

} // namespace

CoPrintPrinterPicker::CoPrintPrinterPicker(wxWindow* parent, PrinterWebView* backend)
    : wxPanel(parent, wxID_ANY)
    , m_backend(backend)
{
    SetBackgroundColour(kHeaderBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    build_header();
    sizer->Add(m_header, 0, wxEXPAND);

    m_submenu = new wxPanel(this, wxID_ANY);
    m_submenu->SetBackgroundColour(kHeaderBg);
    m_submenu_sizer = new wxBoxSizer(wxVERTICAL);
    m_submenu->SetSizer(m_submenu_sizer);
    m_submenu->Show(m_expanded);
    sizer->Add(m_submenu, 0, wxEXPAND);

    m_submenu_border = new wxPanel(this, wxID_ANY);
    m_submenu_border->SetMinSize(wxSize(-1, FromDIP(1)));
    m_submenu_border->SetMaxSize(wxSize(-1, FromDIP(1)));
    m_submenu_border->SetBackgroundColour(kHeaderIdleLine);
    m_submenu_border->Show(m_expanded);
    sizer->Add(m_submenu_border, 0, wxEXPAND);

    build_add_printer();
    sizer->Add(m_add_panel, 0, wxEXPAND);
    SetSizer(sizer);

    rebuild_list();
    apply_header_style();
    if (m_chevron != nullptr)
        m_chevron->SetBitmap(m_expanded && m_arrow_down.IsOk() ? m_arrow_down : m_arrow_right);
}

CoPrintPrinterPicker::~CoPrintPrinterPicker()
{
    m_add_mode = false;
    m_lifetime_token.reset();
    stop_mdns_discovery();
}

void CoPrintPrinterPicker::set_open_status_handler(std::function<void()> handler)
{
    m_open_status = std::move(handler);
}

void CoPrintPrinterPicker::set_status_page_active(bool active)
{
    if (m_status_page_active == active)
        return;
    m_status_page_active = active;
    apply_header_style();
}

void CoPrintPrinterPicker::build_header()
{
    m_header = new wxPanel(this, wxID_ANY);
    m_header->SetMinSize(wxSize(-1, FromDIP(46)));
    m_header->SetCursor(wxCursor(wxCURSOR_HAND));
    m_header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    paint_bg(m_header, kHeaderBg);

    m_title = new wxStaticText(m_header, wxID_ANY, _L("Printers"));
    m_title->SetFont(Label::Body_14);
    m_title->SetForegroundColour(kTextPrimary);
    m_title->SetCursor(wxCursor(wxCURSOR_HAND));

    m_add_label = new wxStaticText(m_header, wxID_ANY, _L("+Add"));
    m_add_label->SetFont(Label::Body_14);
    m_add_label->SetForegroundColour(wxColour("#4A9525"));
    m_add_label->SetCursor(wxCursor(wxCURSOR_HAND));

    m_arrow_right = create_scaled_bitmap("monitor_arrow", this, 14);
    m_arrow_down = rotate_arrow_down(m_arrow_right);
    m_chevron = new wxStaticBitmap(m_header, wxID_ANY, m_arrow_right);
    m_chevron->SetCursor(wxCursor(wxCURSOR_HAND));

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
    row->Add(m_add_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(m_chevron, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    m_header->SetSizer(row);

    auto toggle = [this](wxMouseEvent& evt) {
        evt.StopPropagation();
        toggle_submenu();
    };
    m_header->Bind(wxEVT_LEFT_DOWN, toggle);
    m_title->Bind(wxEVT_LEFT_DOWN, toggle);
    m_chevron->Bind(wxEVT_LEFT_DOWN, toggle);
    m_add_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        evt.StopPropagation();
        show_add_printer();
    });
    m_header->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_header(); });
    bind_header_hover(m_header);
    bind_header_hover(m_title);
    bind_header_hover(m_add_label);
    bind_header_hover(m_chevron);
}

void CoPrintPrinterPicker::build_add_printer()
{
    m_add_panel = new wxPanel(this, wxID_ANY);
    m_add_panel->SetBackgroundColour(kHeaderBg);
    m_add_panel->Hide();

    m_add_header = new wxPanel(m_add_panel, wxID_ANY);
    m_add_header->SetMinSize(wxSize(-1, FromDIP(46)));
    m_add_header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    paint_bg(m_add_header, kHeaderBg);

    auto* close_lbl = new wxStaticText(m_add_header, wxID_ANY, wxString::FromUTF8("\xC3\x97"));
    close_lbl->SetFont(Label::Head_16);
    close_lbl->SetForegroundColour(kTextPrimary);
    close_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    close_lbl->SetMinSize(wxSize(FromDIP(24), -1));

    auto* title = new wxStaticText(m_add_header, wxID_ANY, _L("Add Printer"));
    title->SetFont(Label::Head_14);
    title->SetForegroundColour(kTextPrimary);

    auto* header_row = new wxBoxSizer(wxHORIZONTAL);
    header_row->Add(close_lbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
    header_row->AddStretchSpacer(1);
    header_row->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    header_row->AddStretchSpacer(1);
    header_row->AddSpacer(FromDIP(38));
    m_add_header->SetSizer(header_row);
    m_add_header->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_add_header(); });
    auto close_add = [this](wxMouseEvent& evt) {
        evt.StopPropagation();
        hide_add_printer();
    };
    close_lbl->Bind(wxEVT_LEFT_DOWN, close_add);

    auto* tab_area = new wxPanel(m_add_panel, wxID_ANY);
    tab_area->SetBackgroundColour(kHeaderBg);
    auto* tab_hs = new wxBoxSizer(wxHORIZONTAL);
    const wxString tab_titles[2] = { _L("Auto Connect"), _L("IP Connect") };
    for (int i = 0; i < 2; ++i) {
        auto* cell = new wxPanel(tab_area, wxID_ANY);
        cell->SetBackgroundColour(kHeaderBg);
        auto* vs = new wxBoxSizer(wxVERTICAL);
        m_add_tab_lbl[i] = new wxStaticText(cell, wxID_ANY, tab_titles[i], wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_add_tab_lbl[i]->SetCursor(wxCursor(wxCURSOR_HAND));
        wxFont tab_font = Label::sysFont(11, false);
        tab_font.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_add_tab_lbl[i]->SetFont(tab_font);
        vs->Add(m_add_tab_lbl[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
        m_add_tab_under[i] = new wxPanel(cell, wxID_ANY);
        m_add_tab_under[i]->SetMinSize(wxSize(-1, FromDIP(2)));
        m_add_tab_under[i]->SetMaxSize(wxSize(-1, FromDIP(2)));
        vs->Add(m_add_tab_under[i], 0, wxEXPAND | wxTOP, FromDIP(6));
        cell->SetSizer(vs);
        const int idx = i;
        auto pick_tab = [this, idx](wxMouseEvent&) { apply_add_tab(idx); };
        cell->Bind(wxEVT_LEFT_DOWN, pick_tab);
        m_add_tab_lbl[i]->Bind(wxEVT_LEFT_DOWN, pick_tab);
        tab_hs->Add(cell, 1, wxEXPAND);
    }
    tab_area->SetSizer(tab_hs);

    m_add_book = new wxSimplebook(m_add_panel, wxID_ANY);
    m_add_book->SetBackgroundColour(kHeaderBg);
    m_add_book->SetMinSize(wxSize(-1, FromDIP(300)));

    auto* auto_page = new wxPanel(m_add_book, wxID_ANY);
    auto_page->SetBackgroundColour(kHeaderBg);
    auto* auto_sz = new wxBoxSizer(wxVERTICAL);
    auto* search_row = new wxBoxSizer(wxHORIZONTAL);
    auto* auto_status = new wxStaticText(auto_page, wxID_ANY, _L("Searching for printers on your network..."),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    apply_add_caption_style(auto_status);
    auto_status->SetMinSize(wxSize(1, -1));
    search_row->Add(auto_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* refresh_btn = new wxStaticBitmap(auto_page, wxID_ANY, create_scaled_bitmap("refresh", this, 16));
    refresh_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    refresh_btn->SetToolTip(_L("Refresh"));
    refresh_btn->SetBackgroundColour(kHeaderBg);
    refresh_btn->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    search_row->Add(refresh_btn, 0, wxALIGN_CENTER_VERTICAL);
    auto_sz->Add(search_row, 0, wxEXPAND | wxALL, FromDIP(8));
    m_auto_list = new wxScrolledWindow(auto_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_auto_list->SetScrollRate(0, FromDIP(8));
    m_auto_list->EnableScrolling(false, true);
    m_auto_list->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    m_auto_list->SetBackgroundColour(kHeaderBg);
    m_auto_list->SetMinSize(wxSize(-1, FromDIP(1)));
    m_auto_list->SetMaxSize(wxSize(-1, FromDIP(1)));
    m_auto_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_auto_list->SetSizer(m_auto_list_sizer);
    auto* scroll_bar = new AutoListScrollbar(auto_page);
    scroll_bar->attach(m_auto_list);
    scroll_bar->Hide();
    m_auto_scroll = scroll_bar;
    auto sync_scroll = [this]() {
        fill_auto_list_cards();
        update_auto_scrollbar();
    };
    m_auto_list->Bind(wxEVT_SIZE, [sync_scroll](wxSizeEvent& evt) {
        evt.Skip();
        sync_scroll();
    });
    const auto on_win_scroll = [this](wxScrollWinEvent& evt) {
        evt.Skip();
        update_auto_scrollbar();
    };
    m_auto_list->Bind(wxEVT_SCROLLWIN_TOP, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_BOTTOM, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_LINEUP, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_PAGEUP, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_win_scroll);
    m_auto_list->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_win_scroll);
    m_auto_list->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& evt) {
        evt.Skip();
        CallAfter([this] { update_auto_scrollbar(); });
    });
    auto* list_row = new wxBoxSizer(wxHORIZONTAL);
    list_row->Add(m_auto_list, 1, wxEXPAND);
    list_row->Add(m_auto_scroll, 0, wxEXPAND | wxLEFT, FromDIP(6));
    auto_sz->Add(list_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    auto_sz->Add(build_hint_panel(auto_page, _L("Make sure your printer is powered on\nand connected the same network."), kHintBg, kHintBorder, true), 0,
        wxEXPAND | wxALL, FromDIP(8));
    auto* try_ip = new StaticBox(auto_page, wxID_ANY);
    try_ip->SetCornerRadius(static_cast<double>(FromDIP(8)));
    try_ip->SetBorderWidth(1);
    try_ip->SetBorderColorNormal(kCantFindBorder);
    try_ip->SetBackgroundColorNormal(kCantFindBg);
    try_ip->SetBackgroundColour(kCantFindBg);
    try_ip->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* try_ip_sz = new wxBoxSizer(wxVERTICAL);
    auto* cant_find = new wxStaticText(try_ip, wxID_ANY, _L("Can't find your printer?"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    wxFont cant_font = Label::sysFont(10, false);
    cant_font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    cant_find->SetFont(cant_font);
    cant_find->SetForegroundColour(wxColour("#3B8020"));
    cant_find->SetBackgroundColour(kCantFindBg);
    cant_find->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* try_ip_lbl = new wxStaticText(try_ip, wxID_ANY, _L("Try IP Connect"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    try_ip_lbl->SetFont(Label::sysFont(10, false));
    try_ip_lbl->SetForegroundColour(wxColour("#49882A"));
    try_ip_lbl->SetBackgroundColour(kCantFindBg);
    try_ip_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* texts = new wxBoxSizer(wxVERTICAL);
    texts->Add(cant_find, 0, wxALIGN_CENTER_HORIZONTAL);
    texts->Add(try_ip_lbl, 0, wxALIGN_CENTER_HORIZONTAL);
    auto* mid = new wxBoxSizer(wxHORIZONTAL);
    mid->AddSpacer(FromDIP(20));
    mid->Add(texts, 0, wxALIGN_CENTER_VERTICAL);
    mid->AddSpacer(FromDIP(20));
    try_ip_sz->AddSpacer(FromDIP(15));
    try_ip_sz->Add(mid, 0, wxALIGN_CENTER);
    try_ip_sz->AddSpacer(FromDIP(15));
    try_ip->SetSizer(try_ip_sz);
    try_ip->Fit();
    auto go_ip = [this](wxMouseEvent&) { apply_add_tab(1); };
    try_ip->Bind(wxEVT_LEFT_DOWN, go_ip);
    cant_find->Bind(wxEVT_LEFT_DOWN, go_ip);
    try_ip_lbl->Bind(wxEVT_LEFT_DOWN, go_ip);
    auto_sz->Add(try_ip, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));
    auto_page->SetSizer(auto_sz);
    refresh_btn->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        m_mdns.request_query();
        rebuild_auto_list(true);
    });

    auto* ip_page = new wxPanel(m_add_book, wxID_ANY);
    ip_page->SetBackgroundColour(kHeaderBg);
    auto* ip_sz = new wxBoxSizer(wxVERTICAL);
    auto* prompt = new wxStaticText(ip_page, wxID_ANY, _L("Enter your printer's IP address"));
    apply_add_caption_style(prompt);
    ip_sz->Add(prompt, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto* ip_row = new wxBoxSizer(wxHORIZONTAL);
    auto* ip_shell = new StaticBox(ip_page, wxID_ANY);
    ip_shell->SetCornerRadius(static_cast<double>(FromDIP(10)));
    ip_shell->SetBorderWidth(FromDIP(1));
    ip_shell->SetBorderColorNormal(wxColour(210, 210, 215));
    ip_shell->SetBackgroundColorNormal(*wxWHITE);
    ip_shell->SetBackgroundColour(*wxWHITE);
    auto* ip_inner = new wxBoxSizer(wxHORIZONTAL);
    m_ip_field = new wxTextCtrl(ip_shell, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTE_PROCESS_ENTER);
    m_ip_field->SetBackgroundColour(*wxWHITE);
    m_ip_field->SetHint(_L("Type IP address..."));
    restrict_ip_text_ctrl(m_ip_field);
    ip_inner->Add(m_ip_field, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(8));
    ip_shell->SetSizer(ip_inner);
    ip_row->Add(ip_shell, 1, wxALIGN_CENTER_VERTICAL);

    auto* ip_add = new Button(ip_page, _L("Add"));
    m_ip_add_btn = ip_add;
    ip_add->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    wxFont ip_add_font = Label::sysFont(10, false);
    ip_add_font.SetWeight(wxFONTWEIGHT_MEDIUM);
    ip_add->SetFont(ip_add_font);
    ip_add->SetCornerRadius(FromDIP(6));
    ip_add->SetBorderWidth(1);
    ip_add->SetBorderColorNormal(kAutoAddBorder);
    ip_add->SetBackgroundColorNormal(kAutoAddBg);
    ip_add->SetBackgroundColour(kAutoAddBg);
    ip_add->SetTextColorNormal(kAutoAddText);
    ip_add->SetPaddingSize(wxSize(FromDIP(17), FromDIP(10)));
    ip_row->Add(ip_add, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    ip_sz->Add(ip_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    m_ip_status = new wxStaticText(ip_page, wxID_ANY, wxString());
    m_ip_status->SetForegroundColour(kTextMuted);
    ip_sz->Add(m_ip_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    ip_sz->Add(build_hint_panel(ip_page, _L("Make sure your printer is powered on\nand connected the same network."), kHintBg, kHintBorder, true), 0,
        wxEXPAND | wxALL, FromDIP(8));
    ip_page->SetSizer(ip_sz);
    ip_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { try_ip_add(); });
    m_ip_field->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { try_ip_add(); });

    m_add_book->AddPage(auto_page, wxString(), false);
    m_add_book->AddPage(ip_page, wxString(), false);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_add_header, 0, wxEXPAND);
    root->Add(tab_area, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));
    root->Add(m_add_book, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
    m_add_panel->SetSizer(root);
    apply_add_tab(0);
}

void CoPrintPrinterPicker::bind_header_hover(wxWindow* win)
{
    if (win == nullptr)
        return;
    win->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { set_header_hovered(true); });
    win->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        if (m_header == nullptr)
            return;
        const wxPoint pt = m_header->ScreenToClient(wxGetMousePosition());
        const wxSize size = m_header->GetSize();
        set_header_hovered(wxRect(size).Contains(pt));
    });
}

void CoPrintPrinterPicker::set_header_hovered(bool hovered)
{
    if (m_header_hovered == hovered)
        return;
    m_header_hovered = hovered;
    if (m_header != nullptr)
        m_header->Refresh();
}

void CoPrintPrinterPicker::paint_add_header()
{
    if (m_add_header == nullptr)
        return;
    wxPaintDC dc(m_add_header);
    const wxSize size = m_add_header->GetSize();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(kHeaderBg));
    dc.DrawRectangle(0, 0, size.x, size.y);
    const int line_h = FromDIP(1) > 0 ? FromDIP(1) : 1;
    dc.SetBrush(wxBrush(kHeaderIdleLine));
    dc.DrawRectangle(0, size.y - line_h, size.x, line_h);
}

void CoPrintPrinterPicker::show_add_printer()
{
    if (m_add_mode)
        return;
    m_add_mode = true;
    if (m_header != nullptr)
        m_header->Hide();
    if (m_submenu != nullptr)
        m_submenu->Hide();
    if (m_submenu_border != nullptr)
        m_submenu_border->Hide();
    if (m_add_panel != nullptr)
        m_add_panel->Show();
    start_mdns_discovery();
    rebuild_auto_list(true);
    apply_add_tab(m_add_tab);
    relayout_parents();
    CallAfter([this] {
        fill_auto_list_cards();
        update_auto_scrollbar();
    });
}

void CoPrintPrinterPicker::hide_add_printer()
{
    if (!m_add_mode)
        return;
    m_add_mode = false;
    stop_mdns_discovery();
    if (m_add_panel != nullptr)
        m_add_panel->Hide();
    if (m_header != nullptr)
        m_header->Show();
    if (m_submenu != nullptr)
        m_submenu->Show(m_expanded);
    if (m_submenu_border != nullptr)
        m_submenu_border->Show(m_expanded);
    rebuild_list();
    relayout_parents();
}

void CoPrintPrinterPicker::apply_add_tab(int idx)
{
    m_add_tab = idx;
    for (int i = 0; i < 2; ++i) {
        if (m_add_tab_lbl[i] == nullptr || m_add_tab_under[i] == nullptr)
            continue;
        const bool on = (i == idx);
        m_add_tab_lbl[i]->SetForegroundColour(on ? kAddAccent : wxColour(72, 72, 78));
        wxFont f = Label::sysFont(11, false);
        f.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_add_tab_lbl[i]->SetFont(f);
        m_add_tab_under[i]->SetBackgroundColour(on ? kAddAccent : kHeaderBg);
        m_add_tab_under[i]->Refresh();
    }
    if (m_add_book != nullptr)
        m_add_book->ChangeSelection(static_cast<size_t>(idx));
    if (m_add_panel != nullptr)
        m_add_panel->Refresh();
}

std::string CoPrintPrinterPicker::auto_list_signature() const
{
    std::string out;
    for (const CoprintMdnsPrinter& printer : m_mdns_printers) {
        if (is_already_added_mdns_printer(printer))
            continue;
        out += printer.service;
        out += '|';
        out += printer.name;
        out += '|';
        out += printer.ip;
        out += '|';
        out += std::to_string(printer.port);
        out += ';';
    }
    auto* dev = wxGetApp().getDeviceManager();
    if (dev != nullptr) {
        auto append_known = [&](const std::map<std::string, MachineObject*>& machines) {
            for (const auto& entry : machines) {
                if (entry.second == nullptr)
                    continue;
                out += '#';
                out += host_without_port(entry.second->get_dev_ip());
                out += '|';
                out += host_without_port(entry.second->get_dev_id());
            }
        };
        append_known(dev->get_my_machine_list());
        append_known(dev->get_local_machinelist());
    }
    return out;
}

void CoPrintPrinterPicker::rebuild_auto_list(bool force)
{
    if (m_auto_list_sizer == nullptr || m_auto_list == nullptr)
        return;
    const std::string signature = auto_list_signature();
    if (!force && signature == m_auto_list_signature)
        return;
    m_auto_list_signature = signature;
    m_auto_list_sizer->Clear(true);

    int visible_count = 0;
    for (const CoprintMdnsPrinter& printer : m_mdns_printers) {
        if (is_already_added_mdns_printer(printer))
            continue;
        ++visible_count;
        auto* card = new StaticBox(m_auto_list, wxID_ANY);
        card->SetCornerRadius(static_cast<double>(FromDIP(8)));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(kAutoCardBorder);
        card->SetBackgroundColorNormal(kAutoCardBg);
        card->SetBackgroundColour(kAutoCardBg);
        card->SetCursor(wxCursor(wxCURSOR_HAND));

        auto* add_btn = new Button(card, _L("Add"));
        add_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        wxFont add_font = Label::sysFont(10, false);
        add_font.SetWeight(wxFONTWEIGHT_MEDIUM);
        add_btn->SetFont(add_font);
        add_btn->SetCornerRadius(FromDIP(6));
        add_btn->SetBorderWidth(1);
        add_btn->SetBorderColorNormal(kAutoAddBorder);
        add_btn->SetBackgroundColorNormal(kAutoAddBg);
        add_btn->SetBackgroundColour(kAutoAddBg);
        add_btn->SetTextColorNormal(kAutoAddText);
        add_btn->SetPaddingSize(wxSize(FromDIP(17), FromDIP(10)));

        const int list_w = auto_list_width();
        const int pad = FromDIP(15);
        const int text_w = std::max(FromDIP(20),
            list_w - 2 * pad - FromDIP(8) - add_btn->GetBestSize().GetWidth());

        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* texts = new wxPanel(card, wxID_ANY);
        texts->SetBackgroundColour(kAutoCardBg);
        texts->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* texts_sizer = new wxBoxSizer(wxVERTICAL);
        const wxString name = from_u8(printer.name.empty() ? printer.ip : printer.name);
        auto* name_lbl = new wxStaticText(texts, wxID_ANY, name, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        wxFont name_font = Label::sysFont(13, false);
        name_font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
        name_lbl->SetFont(name_font);
        name_lbl->SetForegroundColour(kTextPrimary);
        name_lbl->SetBackgroundColour(kAutoCardBg);
        name_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        const wxString ip = from_u8(printer.ip);
        auto* ip_lbl = new wxStaticText(texts, wxID_ANY, ip, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        wxFont ip_font = Label::sysFont(10, false);
        ip_font.SetWeight(wxFONTWEIGHT_LIGHT);
        ip_lbl->SetFont(ip_font);
        ip_lbl->SetForegroundColour(kTextMuted);
        ip_lbl->SetBackgroundColour(kAutoCardBg);
        ip_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        texts_sizer->Add(name_lbl, 0, wxEXPAND);
        texts_sizer->Add(ip_lbl, 0, wxEXPAND | wxTOP, FromDIP(2));
        texts->SetSizer(texts_sizer);
        texts->SetMinSize(wxSize(text_w, -1));
        texts->SetMaxSize(wxSize(text_w, -1));
        name_lbl->SetMinSize(wxSize(text_w, -1));
        name_lbl->SetMaxSize(wxSize(text_w, -1));
        ip_lbl->SetMinSize(wxSize(text_w, -1));
        ip_lbl->SetMaxSize(wxSize(text_w, -1));
        name_lbl->SetToolTip(name);
        ip_lbl->SetToolTip(ip);
        row->Add(texts, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        row->Add(add_btn, 0, wxALIGN_CENTER_VERTICAL);

        auto* pad_sizer = new wxBoxSizer(wxVERTICAL);
        pad_sizer->Add(row, 0, wxEXPAND | wxALL, pad);
        card->SetSizer(pad_sizer);
        card->SetMinSize(wxSize(list_w, -1));
        card->SetMaxSize(wxSize(list_w, -1));

        auto add_machine = [this, printer]() { add_discovered_printer(printer); };
        auto add_from_mouse = [add_machine](wxMouseEvent& evt) {
            evt.StopPropagation();
            add_machine();
        };
        card->Bind(wxEVT_LEFT_DOWN, add_from_mouse);
        texts->Bind(wxEVT_LEFT_DOWN, add_from_mouse);
        name_lbl->Bind(wxEVT_LEFT_DOWN, add_from_mouse);
        ip_lbl->Bind(wxEVT_LEFT_DOWN, add_from_mouse);
        add_btn->Bind(wxEVT_BUTTON, [add_machine](wxCommandEvent&) { add_machine(); });

        m_auto_list_sizer->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(kAutoCardGapDip));
    }
    if (visible_count == 0) {
        const wxString empty_text = m_mdns_printers.empty()
            ? _L("No printers discovered yet. Tap Refresh.")
            : _L("No new printers found.");
        auto* empty = new wxStaticText(m_auto_list, wxID_ANY, empty_text);
        empty->SetForegroundColour(kTextMuted);
        empty->SetMinSize(wxSize(1, -1));
        empty->Wrap(std::max(FromDIP(80), auto_list_width()));
        m_auto_list_sizer->Add(empty, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    }

    int viewport = 0;
    int shown = 0;
    for (auto* item : m_auto_list_sizer->GetChildren()) {
        if (item == nullptr || item->GetWindow() == nullptr)
            continue;
        if (shown >= 2)
            break;
        viewport += item->GetWindow()->GetBestSize().GetHeight();
        viewport += FromDIP(kAutoCardGapDip);
        ++shown;
    }
    if (viewport <= 0)
        viewport = FromDIP(1);
    m_auto_list->SetMinSize(wxSize(-1, viewport));
    m_auto_list->SetMaxSize(wxSize(-1, viewport));
    fill_auto_list_cards();
    update_auto_scrollbar();
    if (m_add_mode)
        relayout_parents();
}

void CoPrintPrinterPicker::start_mdns_discovery()
{
    std::weak_ptr<int> lifetime = m_lifetime_token;
    m_mdns.start([this, lifetime](const CoprintMdnsPrinter& printer, bool lost) {
        wxGetApp().CallAfter([this, lifetime, printer, lost]() {
            if (lifetime.expired())
                return;
            on_mdns_printer(printer, lost);
        });
    });
}

void CoPrintPrinterPicker::stop_mdns_discovery()
{
    m_mdns.stop();
    m_mdns_printers.clear();
    m_auto_list_signature.clear();
}

void CoPrintPrinterPicker::on_mdns_printer(const CoprintMdnsPrinter& printer, bool lost)
{
    if (!m_add_mode)
        return;

    auto same = [&](const CoprintMdnsPrinter& existing) {
        if (!printer.service.empty() && existing.service == printer.service)
            return true;
        return !printer.ip.empty() && existing.ip == printer.ip;
    };

    if (lost) {
        const auto it = std::remove_if(m_mdns_printers.begin(), m_mdns_printers.end(), same);
        if (it == m_mdns_printers.end())
            return;
        m_mdns_printers.erase(it, m_mdns_printers.end());
        rebuild_auto_list(true);
        return;
    }

    auto it = std::find_if(m_mdns_printers.begin(), m_mdns_printers.end(), same);
    if (it != m_mdns_printers.end()) {
        if (*it == printer)
            return;
        *it = printer;
    } else {
        m_mdns_printers.push_back(printer);
    }
    std::sort(m_mdns_printers.begin(), m_mdns_printers.end(),
        [](const CoprintMdnsPrinter& a, const CoprintMdnsPrinter& b) {
            if (a.name != b.name)
                return a.name < b.name;
            return a.ip < b.ip;
        });
    rebuild_auto_list(true);
}

void CoPrintPrinterPicker::add_discovered_printer(const CoprintMdnsPrinter& printer)
{
    if (m_backend == nullptr || printer.ip.empty())
        return;
    if (is_already_added_mdns_printer(printer))
        return;

    BBLocalMachine machine;
    // `_coprintagent._tcp` advertises the CoPrint agent HTTP port (werkzeug),
    // not Moonraker. Probe Moonraker on 7125, same as IP Connect.
    machine.dev_ip = printer.ip;
    machine.dev_id = printer.id.empty() ? machine.dev_ip : printer.id;
    machine.dev_name = printer.name.empty() ? printer.ip : printer.name;
    machine.printer_type = printer.model.empty() ? std::string("Moonraker") : printer.model;

    std::weak_ptr<int> lifetime = m_lifetime_token;
    m_backend->add_moonraker_printer_async(machine, false,
        [this, lifetime](bool ok, const wxString&) {
            if (lifetime.expired() || !ok)
                return;
            hide_add_printer();
        });
}

int CoPrintPrinterPicker::auto_list_width() const
{
    if (m_auto_list != nullptr) {
        const int cw = m_auto_list->GetClientSize().GetWidth();
        if (cw > FromDIP(40))
            return cw;
        if (wxWindow* page = m_auto_list->GetParent()) {
            const int pw = page->GetClientSize().GetWidth() - FromDIP(16);
            if (pw > FromDIP(40))
                return pw;
        }
    }
    int sidebar = GetClientSize().GetWidth();
    if (sidebar <= 0)
        sidebar = FromDIP(254);
    return std::max(FromDIP(80), sidebar - FromDIP(24));
}

void CoPrintPrinterPicker::fill_auto_list_cards()
{
    if (m_auto_list == nullptr || m_auto_list_sizer == nullptr || m_filling_auto_cards)
        return;

    const int list_w = auto_list_width();
    m_filling_auto_cards = true;
    for (auto* item : m_auto_list_sizer->GetChildren()) {
        wxWindow* win = item != nullptr ? item->GetWindow() : nullptr;
        if (win == nullptr)
            continue;
        const int h = win->GetMinSize().GetHeight();
        win->SetMinSize(wxSize(list_w, h > 0 ? h : -1));
        win->SetMaxSize(wxSize(list_w, -1));
    }
    const int content_h = std::max(1, m_auto_list_sizer->GetMinSize().GetHeight());
    m_auto_list->SetVirtualSize(list_w, content_h);
    m_auto_list_sizer->SetDimension(0, 0, list_w, content_h);
    m_filling_auto_cards = false;
}

void CoPrintPrinterPicker::update_auto_scrollbar()
{
    if (auto* bar = dynamic_cast<AutoListScrollbar*>(m_auto_scroll))
        bar->sync();
}

void CoPrintPrinterPicker::try_ip_add()
{
    if (m_ip_add_busy)
        return;
    if (m_ip_status != nullptr)
        m_ip_status->SetLabelText(wxString());
    if (m_ip_field == nullptr)
        return;

    wxString ip_value = m_ip_field->GetValue();
    ip_value.Trim(true);
    ip_value.Trim(false);
    if (ip_value.empty()) {
        if (m_ip_status != nullptr)
            m_ip_status->SetLabelText(_L("IP address cannot be empty."));
        return;
    }

    if (m_backend == nullptr)
        return;

    std::string host = into_u8(ip_value);
    const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
    const std::string normalized_host = MachineObject::dev_id_from_address(host);
    std::string dev_ip = normalized_host;
    if (!has_scheme && normalized_host.find(':') == std::string::npos)
        dev_ip += ":7125";

    BBLocalMachine machine;
    machine.dev_id = dev_ip;
    machine.dev_ip = dev_ip;
    machine.dev_name = friendly_host_from_address(dev_ip);
    machine.printer_type = "Moonraker";

    m_ip_add_busy = true;
    if (m_ip_status != nullptr)
        m_ip_status->SetLabelText(_L("Connecting to printer..."));
    if (m_ip_add_btn != nullptr)
        m_ip_add_btn->Enable(false);
    if (m_ip_field != nullptr)
        m_ip_field->Enable(false);

    // Probe on a worker thread. Synchronous Moonraker HTTP on the UI thread
    // freezes the app for several seconds when the address is unreachable.
    std::weak_ptr<int> lifetime = m_lifetime_token;
    m_backend->add_moonraker_printer_async(machine, host.rfind("https://", 0) == 0,
        [this, lifetime](bool ok, const wxString &message) {
            if (lifetime.expired())
                return;
            m_ip_add_busy = false;
            if (m_ip_add_btn != nullptr)
                m_ip_add_btn->Enable(true);
            if (m_ip_field != nullptr)
                m_ip_field->Enable(true);
            if (!ok) {
                if (m_ip_status != nullptr)
                    m_ip_status->SetLabelText(message.IsEmpty() ? _L("Could not connect to the printer.") : message);
                return;
            }
            hide_add_printer();
        });
}

void CoPrintPrinterPicker::paint_header()
{
    if (m_header == nullptr)
        return;
    wxPaintDC dc(m_header);
    const wxSize size = m_header->GetSize();
    const wxColour bg = m_status_page_active ? kHeaderSel : kHeaderBg;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(bg));
    dc.DrawRectangle(0, 0, size.x, size.y);
    const int line_h = FromDIP(1) > 0 ? FromDIP(1) : 1;
    if (m_header_hovered && !m_status_page_active) {
        dc.SetPen(wxPen(kHeaderHoverBorder, line_h));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.x, size.y);
    } else if (!m_expanded && !m_header_hovered && !m_status_page_active) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(kHeaderIdleLine));
        dc.DrawRectangle(0, size.y - line_h, size.x, line_h);
    }
}

void CoPrintPrinterPicker::toggle_submenu()
{
    set_expanded(!m_expanded);
}

void CoPrintPrinterPicker::set_expanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    if (m_submenu != nullptr)
        m_submenu->Show(m_expanded);
    if (m_submenu_border != nullptr)
        m_submenu_border->Show(m_expanded && !m_add_mode);
    if (m_chevron != nullptr)
        m_chevron->SetBitmap(m_expanded && m_arrow_down.IsOk() ? m_arrow_down : m_arrow_right);
    if (m_header != nullptr)
        m_header->Refresh();
    relayout_parents();
}

void CoPrintPrinterPicker::apply_header_style()
{
    const wxColour bg = m_status_page_active ? kHeaderSel : kHeaderBg;
    paint_bg(this, bg);
    paint_bg(m_header, bg);
    paint_bg(m_title, bg);
    paint_bg(m_add_label, bg);
    paint_bg(m_chevron, bg);
    if (m_title != nullptr)
        m_title->SetFont(m_status_page_active ? Label::Head_14 : Label::Body_14);
    if (m_header != nullptr)
        m_header->Refresh();
}

void CoPrintPrinterPicker::relayout_parents()
{
    Layout();
    if (wxWindow* parent = GetParent()) {
        parent->Layout();
        if (wxWindow* grand = parent->GetParent())
            grand->Layout();
    }
}

void CoPrintPrinterPicker::schedule_rebuild_list()
{
    if (m_rebuild_queued)
        return;
    m_rebuild_queued = true;
    CallAfter([this]() {
        m_rebuild_queued = false;
        m_list_signature.clear();
        rebuild_list();
    });
}

MachineObject* CoPrintPrinterPicker::live_machine(const std::string& dev_id) const
{
    auto* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr || dev_id.empty())
        return nullptr;
    if (MachineObject* local = dev->get_local_machine(dev_id))
        return local;
    for (const auto& entry : dev->get_my_machine_list()) {
        if (entry.second != nullptr && entry.second->get_dev_id() == dev_id)
            return entry.second;
    }
    for (const auto& entry : dev->get_local_machinelist()) {
        if (entry.second != nullptr &&
            (entry.first == dev_id || entry.second->get_dev_id() == dev_id || entry.second->get_dev_ip() == dev_id))
            return entry.second;
    }
    return nullptr;
}

void CoPrintPrinterPicker::refresh_list(bool force)
{
    if (m_add_mode)
        rebuild_auto_list(force);
    if (!force) {
        const std::string signature = list_signature();
        if (signature == m_list_signature)
            return;
        schedule_rebuild_list();
        return;
    }
    m_list_signature.clear();
    rebuild_list();
}

void CoPrintPrinterPicker::update_selection()
{
    apply_header_style();
    refresh_list();
}

std::string CoPrintPrinterPicker::list_signature() const
{
    auto* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        return {};

    std::string out;
    auto append = [&](MachineObject* machine) {
        if (machine == nullptr)
            return;
        out += machine->get_dev_id();
        out += '|';
        out += machine->get_dev_name();
        out += '|';
        out += machine->get_dev_ip();
        out += '|';
        out += machine->is_online() ? '1' : '0';
        out += ';';
    };

    MachineObject* selected = dev->get_selected_machine();
    out += "sel=";
    if (selected != nullptr)
        out += selected->get_dev_id();
    out += ";conn=";
    out += (m_backend != nullptr && m_backend->is_printer_connecting(selected)) ? '1' : '0';
    out += ';';
    for (const auto& entry : dev->get_my_machine_list())
        append(entry.second);
    for (const auto& entry : dev->get_local_machinelist())
        append(entry.second);
    return out;
}

void CoPrintPrinterPicker::rebuild_list()
{
    if (m_submenu == nullptr || m_submenu_sizer == nullptr)
        return;

    m_submenu_sizer->Clear(true);

    auto* dev = wxGetApp().getDeviceManager();
    const auto my_machines = dev ? dev->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev ? dev->get_local_machinelist() : std::map<std::string, MachineObject*>();
    MachineObject* selected = dev ? dev->get_selected_machine() : nullptr;

    std::map<std::string, MachineObject*> all_by_id;
    for (const auto& entry : my_machines) {
        if (entry.second != nullptr)
            all_by_id[entry.first] = entry.second;
    }
    for (const auto& entry : local_machines) {
        if (entry.second != nullptr && all_by_id.find(entry.first) == all_by_id.end())
            all_by_id[entry.first] = entry.second;
    }
    if (selected != nullptr)
        all_by_id[selected->get_dev_id()] = selected;

    std::vector<MachineObject*> sorted;
    sorted.reserve(all_by_id.size());
    for (const auto& entry : all_by_id) {
        if (entry.second != nullptr)
            sorted.push_back(entry.second);
    }
    std::sort(sorted.begin(), sorted.end(), [](MachineObject* a, MachineObject* b) {
        if (a == nullptr || b == nullptr)
            return a != nullptr;
        return a->get_dev_name() < b->get_dev_name();
    });

    std::vector<MachineObject*> active_list;
    std::vector<MachineObject*> inactive_list;
    for (MachineObject* machine : sorted) {
        if (machine == nullptr)
            continue;
        if (same_machine(machine, selected))
            active_list.push_back(machine);
        else
            inactive_list.push_back(machine);
    }

    m_submenu_sizer->AddSpacer(FromDIP(8));
    add_section_title(_L("Active printers"));
    if (active_list.empty())
        add_empty_placeholder_card(_L("No active printers"));
    else {
        for (MachineObject* machine : active_list)
            add_printer_card(machine, true);
    }

    add_section_title(_L("Offline printers"));
    if (inactive_list.empty())
        add_empty_placeholder_card(_L("No offline printers"));
    else {
        for (MachineObject* machine : inactive_list)
            add_printer_card(machine, false);
    }
    m_submenu_sizer->AddSpacer(FromDIP(8));

    m_list_signature = list_signature();
    m_submenu->Layout();
    if (m_expanded)
        relayout_parents();
}

void CoPrintPrinterPicker::add_section_title(const wxString& text)
{
    auto* lab = new wxStaticText(m_submenu, wxID_ANY, text);
    lab->SetForegroundColour(kTextMuted);
    lab->SetFont(Label::Body_12);
    m_submenu_sizer->Add(lab, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
}

void CoPrintPrinterPicker::add_empty_placeholder_card(const wxString& text)
{
    auto* card = new StaticBox(m_submenu, wxID_ANY);
    card->SetCornerRadius(static_cast<double>(FromDIP(8)));
    card->SetBorderWidth(1);
    card->SetBorderColorNormal(kOfflineBorder);
    card->SetBackgroundColorNormal(kOfflineBg);
    card->SetBackgroundColour(kOfflineBg);

    auto* label = new wxStaticText(card, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    label->SetForegroundColour(kTextPrimary);
    label->SetBackgroundColour(kOfflineBg);

    auto* pad = new wxBoxSizer(wxVERTICAL);
    pad->Add(label, 0, wxEXPAND | wxALL, FromDIP(10));
    card->SetSizer(pad);

    m_submenu_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
}

void CoPrintPrinterPicker::add_printer_card(MachineObject* machine, bool selected)
{
    if (machine == nullptr)
        return;

    const bool online = machine->is_online();
    const bool connecting = selected && m_backend != nullptr && m_backend->is_printer_connecting(machine);
    const wxColour border = connecting ? kConnectingBorder
        : selected ? (online ? kOnlineBorder : kOfflineSelectedBorder)
                   : kOfflineBorder;
    const wxColour bg = connecting ? kConnectingBg
        : selected ? (online ? kOnlineBg : kOfflineSelectedBg)
                   : kOfflineBg;
    const wxColour dot_colour = connecting ? kDotConnecting
        : selected ? (online ? kDotOnline : kDotOfflineRed)
                   : kDotOffline;

    auto* card = new StaticBox(m_submenu, wxID_ANY);
    card->SetCornerRadius(static_cast<double>(FromDIP(8)));
    card->SetBorderWidth(1);
    card->SetBorderColorNormal(border);
    card->SetBackgroundColorNormal(bg);
    card->SetBackgroundColour(bg);
    card->SetCursor(wxCursor(wxCURSOR_HAND));

    auto* row = new wxBoxSizer(wxHORIZONTAL);

    auto* dot = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
    dot->SetForegroundColour(dot_colour);
    dot->SetBackgroundColour(bg);
    dot->SetCursor(wxCursor(wxCURSOR_HAND));
    row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    auto* texts = new wxPanel(card, wxID_ANY);
    texts->SetBackgroundColour(bg);
    texts->SetCursor(wxCursor(wxCURSOR_HAND));
    auto* texts_sizer = new wxBoxSizer(wxVERTICAL);
    wxString name = m_backend != nullptr
        ? m_backend->sidebar_display_name_for(machine)
        : from_u8(machine->get_dev_name());
    auto* name_lbl = new wxStaticText(texts, wxID_ANY, name, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    name_lbl->SetForegroundColour(kTextPrimary);
    name_lbl->SetBackgroundColour(bg);
    name_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    const wxString ip = display_ip_only(machine);
    auto* ip_lbl = new wxStaticText(texts, wxID_ANY, ip, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    ip_lbl->SetForegroundColour(kTextMuted);
    ip_lbl->SetBackgroundColour(bg);
    ip_lbl->SetFont(Label::Body_10);
    ip_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    texts_sizer->Add(name_lbl, 0, wxEXPAND);
    texts_sizer->Add(ip_lbl, 0, wxEXPAND | wxTOP, FromDIP(2));
    texts->SetSizer(texts_sizer);
    row->Add(texts, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    auto* actions = new wxPanel(card, wxID_ANY);
    actions->SetBackgroundColour(bg);
    auto* actions_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* edit_icon = new wxStaticBitmap(actions, wxID_ANY, create_scaled_bitmap("rename_edit", this, 16));
    edit_icon->SetCursor(wxCursor(wxCURSOR_HAND));
    edit_icon->SetToolTip(_L("Edit printer name"));
    auto* card_arrow = new wxStaticBitmap(actions, wxID_ANY, create_scaled_bitmap("monitor_arrow", this, 14));
    card_arrow->SetCursor(wxCursor(wxCURSOR_HAND));
    actions_sizer->Add(edit_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    actions_sizer->Add(card_arrow, 0, wxALIGN_CENTER_VERTICAL);
    actions->SetSizer(actions_sizer);
    row->Add(actions, 0, wxALIGN_CENTER_VERTICAL);

    auto* pad = new wxBoxSizer(wxVERTICAL);
    pad->Add(row, 0, wxEXPAND | wxALL, FromDIP(10));
    card->SetSizer(pad);

    const std::string dev_id = machine->get_dev_id();
    auto pick = [this, dev_id](wxMouseEvent& evt) {
        evt.StopPropagation();
        open_machine(live_machine(dev_id));
    };
    card->Bind(wxEVT_LEFT_DOWN, pick);
    dot->Bind(wxEVT_LEFT_DOWN, pick);
    texts->Bind(wxEVT_LEFT_DOWN, pick);
    name_lbl->Bind(wxEVT_LEFT_DOWN, pick);
    ip_lbl->Bind(wxEVT_LEFT_DOWN, pick);
    card_arrow->Bind(wxEVT_LEFT_DOWN, pick);
    edit_icon->Bind(wxEVT_LEFT_DOWN, [this, dev_id, edit_icon](wxMouseEvent& evt) {
        evt.StopPropagation();
        MachineObject* live = live_machine(dev_id);
        if (m_backend != nullptr && live != nullptr)
            m_backend->show_printer_card_actions_menu(edit_icon, live);
        schedule_rebuild_list();
    });

    m_submenu_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
}

void CoPrintPrinterPicker::open_machine(MachineObject* machine)
{
    if (machine == nullptr)
        return;

    auto* dev = wxGetApp().getDeviceManager();
    const std::string dev_id = machine->get_dev_id();
    if (dev != nullptr)
        dev->set_selected_machine(dev_id);
    if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
        wxGetApp().mainframe->m_monitor->select_machine(dev_id);
    if (m_backend != nullptr) {
        if (!machine->is_online())
            m_backend->mark_printer_connecting(dev_id);
        m_backend->refresh_layer_info_from_selected_machine();
    }
    if (m_open_status)
        m_open_status();
    set_status_page_active(true);
}

} // namespace GUI
} // namespace Slic3r
