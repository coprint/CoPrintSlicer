#pragma once

#include "FilamentCatalog.hpp"

#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/string.h>

class wxStaticText;
class wxWindow;
class Button;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

struct FilamentSelection {
    wxString brand;
    wxString type;
    wxString color;
    wxString color_hex;
    int      temp_min{0};
    int      temp_max{0};
    double   pressure_advance{0};
};

class FilamentSelectDialog : public wxDialog
{
public:
    FilamentSelectDialog(wxWindow *parent, int ui_tool, const wxString &initial_material,
        const wxColour &initial_color, const wxString &initial_brand = wxEmptyString);

    int ShowModal() override;
    wxString material() const;
    wxString color_hex() const;
    FilamentSelection selection() const;

private:
    void build_ui(int ui_tool);
    void rebuild_brand_list();
    void rebuild_type_list();
    void rebuild_color_list();
    void select_brand(int index);
    void select_type(int index);
    void select_color(int index);
    void refresh_info();
    void reset_defaults();
    void style_button(Button *button, bool primary);
    void center_on_screen();

    FilamentCatalog m_catalog;
    int m_brand_index{0};
    int m_type_index{0};
    int m_color_index{0};

    wxStaticText *m_brand_selected{nullptr};
    wxStaticText *m_type_selected{nullptr};
    wxWindow *    m_color_swatch{nullptr};
    wxWindow *    m_brand_list{nullptr};
    wxWindow *    m_type_list{nullptr};
    wxWindow *    m_color_list{nullptr};
    wxStaticText *m_temp_min{nullptr};
    wxStaticText *m_temp_max{nullptr};
    wxStaticText *m_pa_value{nullptr};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
