#ifndef slic3r_GUI_SelectCoPrintPrinterDialog_hpp_
#define slic3r_GUI_SelectCoPrintPrinterDialog_hpp_

#include <string>
#include <vector>

#include "GUI_Utils.hpp"

class ComboBox;

namespace Slic3r { namespace GUI {

class SelectCoPrintPrinterDialog : public DPIDialog
{
public:
    explicit SelectCoPrintPrinterDialog(wxWindow *parent, const std::string &preferred_preset_name = {});

    std::string selected_preset_name() const { return m_selected_preset_name; }

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Choice {
        std::string model;
        std::string nozzle;
        std::string preset_name;
    };

    void collect_choices();
    void fill_printer_combo(const std::string &preferred_preset_name);
    void fill_nozzle_combo(const std::string &preferred_nozzle);
    void update_selected_preset();
    void style_combo(ComboBox *combo);

    std::vector<Choice> m_choices;
    std::string         m_selected_preset_name;

    ComboBox *m_printer_combo{nullptr};
    ComboBox *m_nozzle_combo{nullptr};
};

}} // namespace Slic3r::GUI

#endif
