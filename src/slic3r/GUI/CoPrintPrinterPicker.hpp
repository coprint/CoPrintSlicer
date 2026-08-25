#ifndef slic3r_GUI_CoPrintPrinterPicker_hpp_
#define slic3r_GUI_CoPrintPrinterPicker_hpp_

#include <functional>
#include <memory>
#include <string>

#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/string.h>

class wxBoxSizer;
class wxSimplebook;
class wxStaticBitmap;
class wxStaticText;
class wxScrolledWindow;
class wxTextCtrl;
class Button;
class StaticBox;

namespace Slic3r {
class MachineObject;
namespace GUI {

class PrinterWebView;

class CoPrintPrinterPicker : public wxPanel
{
public:
    explicit CoPrintPrinterPicker(wxWindow* parent, PrinterWebView* backend);

    void set_open_status_handler(std::function<void()> handler);
    void set_status_page_active(bool active);
    void refresh_list(bool force = false);
    void update_selection();
    void show_add_printer();
    void hide_add_printer();

private:
    void build_header();
    void build_add_printer();
    void bind_header_hover(wxWindow* win);
    void set_header_hovered(bool hovered);
    void paint_header();
    void paint_add_header();
    void toggle_submenu();
    void set_expanded(bool expanded);
    void apply_header_style();
    void relayout_parents();
    void rebuild_list();
    void schedule_rebuild_list();
    void add_section_title(const wxString& text);
    void add_empty_placeholder_card(const wxString& text);
    void add_printer_card(MachineObject* machine, bool selected);
    void open_machine(MachineObject* machine);
    MachineObject* live_machine(const std::string& dev_id) const;
    void apply_add_tab(int idx);
    void rebuild_auto_list(bool force = false);
    void fill_auto_list_cards();
    void update_auto_scrollbar();
    int  auto_list_width() const;
    std::string auto_list_signature() const;
    void try_ip_add();
    std::string list_signature() const;

    PrinterWebView*         m_backend{nullptr};
    std::function<void()>   m_open_status;
    wxPanel*                m_header{nullptr};
    wxStaticText*           m_title{nullptr};
    wxStaticText*           m_add_label{nullptr};
    wxStaticBitmap*         m_chevron{nullptr};
    wxBitmap                m_arrow_right;
    wxBitmap                m_arrow_down;
    wxPanel*                m_submenu{nullptr};
    wxBoxSizer*             m_submenu_sizer{nullptr};
    wxPanel*                m_submenu_border{nullptr};
    wxPanel*                m_add_panel{nullptr};
    wxPanel*                m_add_header{nullptr};
    wxStaticText*           m_add_tab_lbl[2]{nullptr, nullptr};
    wxPanel*                m_add_tab_under[2]{nullptr, nullptr};
    wxSimplebook*           m_add_book{nullptr};
    wxScrolledWindow*       m_auto_list{nullptr};
    wxPanel*                m_auto_scroll{nullptr};
    wxBoxSizer*             m_auto_list_sizer{nullptr};
    wxTextCtrl*             m_ip_field{nullptr};
    Button*                 m_ip_add_btn{nullptr};
    wxStaticText*           m_ip_status{nullptr};
    bool                    m_expanded{true};
    bool                    m_header_hovered{false};
    bool                    m_status_page_active{true};
    bool                    m_add_mode{false};
    bool                    m_rebuild_queued{false};
    bool                    m_filling_auto_cards{false};
    bool                    m_ip_add_busy{false};
    int                     m_add_tab{0};
    std::shared_ptr<int>    m_lifetime_token{std::make_shared<int>(1)};
    std::string             m_list_signature;
    std::string             m_auto_list_signature;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CoPrintPrinterPicker_hpp_
