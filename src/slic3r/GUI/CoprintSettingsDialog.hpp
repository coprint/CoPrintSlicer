#pragma once

#include <functional>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/string.h>

class wxChoice;
class wxCheckBox;
class wxStaticText;
class wxScrolledWindow;
class wxBoxSizer;
class wxTextCtrl;
class wxSpinCtrl;

namespace Slic3r {
namespace GUI {

class CoprintSettingsDialog : public wxDialog
{
public:
    explicit CoprintSettingsDialog(wxWindow *parent);

private:
    void build();
    void add_section(const wxString &title);
    wxChoice *add_choice(const wxString &title, const std::vector<wxString> &labels, int selection);
    wxCheckBox *add_checkbox(const wxString &title, bool value);
    wxTextCtrl *add_text(const wxString &title, const wxString &value);
    wxSpinCtrl *add_spin(const wxString &title, int value, int min, int max);
    void add_action(const wxString &title, const wxString &button_label, std::function<void()> on_click);
    void add_downloads_row();
    void fill_languages();
    void on_language(wxCommandEvent &);
    void on_region(wxCommandEvent &);

    wxScrolledWindow *m_scroll{nullptr};
    wxBoxSizer *m_rows{nullptr};
    wxChoice *m_language{nullptr};
    wxChoice *m_region{nullptr};
    int m_language_sel{0};
    int m_region_sel{0};
    std::vector<std::string> m_language_codes;
};

} // namespace GUI
} // namespace Slic3r
