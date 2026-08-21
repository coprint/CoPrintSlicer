#ifndef slic3r_GUI_CoPrintPrinterPicker_hpp_
#define slic3r_GUI_CoPrintPrinterPicker_hpp_

#include <wx/panel.h>

class wxStaticText;
class StaticBox;

namespace Slic3r {
namespace GUI {

class PrinterWebView;

class CoPrintPrinterPicker : public wxPanel
{
public:
    explicit CoPrintPrinterPicker(wxWindow* parent, PrinterWebView* backend);

    void refresh_list();
    void update_selection();

private:
    PrinterWebView* m_backend{nullptr};
    StaticBox*      m_pick_row{nullptr};
    wxStaticText*   m_name_label{nullptr};
    wxStaticText*   m_status_dot{nullptr};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CoPrintPrinterPicker_hpp_
