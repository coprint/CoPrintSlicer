#include "SelectCoPrintPrinterDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Config.hpp"

#include <algorithm>
#include <set>

#include <wx/sizer.h>

#include <boost/algorithm/string.hpp>

namespace Slic3r { namespace GUI {

namespace {

const wxColour kDialogBackground(255, 255, 255);
const wxColour kTextPrimary(26, 26, 26);
const wxColour kControlBackground(255, 255, 255);
const wxColour kControlBorder(223, 223, 223);

bool is_standard_coprint_preset(const Preset &preset)
{
    if (preset.is_default || preset.is_project_embedded || preset.is_external)
        return false;
    const auto *model_opt = preset.config.option<ConfigOptionString>("printer_model");
    if (model_opt == nullptr)
        return false;
    const std::string &model = model_opt->value;
    if (model != "Co Print Quadro" && model != "Co Print ChromaSet")
        return false;
    const auto *variant_opt = preset.config.option<ConfigOptionString>("printer_variant");
    if (variant_opt == nullptr || variant_opt->value.empty())
        return false;
    return preset.name == model + " " + variant_opt->value + " nozzle";
}

int model_order(const std::string &model)
{
    if (model == "Co Print Quadro")
        return 0;
    if (model == "Co Print ChromaSet")
        return 1;
    return 2;
}

double nozzle_value(const std::string &nozzle)
{
    try {
        return std::stod(nozzle);
    } catch (...) {
        return 0.0;
    }
}

} // namespace

SelectCoPrintPrinterDialog::SelectCoPrintPrinterDialog(wxWindow *parent, const std::string &preferred_preset_name)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Select Printer"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(kDialogBackground);

    collect_choices();

    auto *root = new wxBoxSizer(wxVERTICAL);
    root->SetMinSize(wxSize(FromDIP(360), -1));

    auto *message = new Label(this, _L("This 3MF was created for a different printer. Choose a Co Print printer and nozzle to open it."));
    message->SetFont(Label::Body_13);
    message->SetForegroundColour(kTextPrimary);
    message->Wrap(FromDIP(330));
    root->Add(message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto add_field = [&](const wxString &title, ComboBox *&combo) {
        auto *label = new Label(this, title);
        label->SetFont(Label::Body_13);
        label->SetForegroundColour(kTextPrimary);
        root->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(328), FromDIP(32)), 0, nullptr, wxCB_READONLY);
        style_combo(combo);
        root->Add(combo, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    };

    add_field(_L("Printer"), m_printer_combo);
    add_field(_L("Nozzle"), m_nozzle_combo);

    fill_printer_combo(preferred_preset_name);

    m_printer_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
        fill_nozzle_combo("0.4");
        update_selected_preset();
    });
    m_nozzle_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { update_selected_preset(); });

    auto *buttons = new DialogButtons(this, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        update_selected_preset();
        if (m_selected_preset_name.empty())
            return;
        EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    root->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(12));

    SetSizer(root);
    Layout();
    root->Fit(this);
    CentreOnParent();
}

void SelectCoPrintPrinterDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

void SelectCoPrintPrinterDialog::collect_choices()
{
    m_choices.clear();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    for (const Preset &preset : bundle->printers) {
        if (!is_standard_coprint_preset(preset))
            continue;
        Choice choice;
        choice.model       = preset.config.option<ConfigOptionString>("printer_model")->value;
        choice.nozzle      = preset.config.option<ConfigOptionString>("printer_variant")->value;
        choice.preset_name = preset.name;
        m_choices.push_back(std::move(choice));
    }

    std::sort(m_choices.begin(), m_choices.end(), [](const Choice &a, const Choice &b) {
        const int oa = model_order(a.model);
        const int ob = model_order(b.model);
        if (oa != ob)
            return oa < ob;
        return nozzle_value(a.nozzle) < nozzle_value(b.nozzle);
    });
}

void SelectCoPrintPrinterDialog::fill_printer_combo(const std::string &preferred_preset_name)
{
    if (m_printer_combo == nullptr)
        return;

    m_printer_combo->Clear();
    std::vector<std::string> models;
    std::set<std::string>    seen;
    for (const Choice &choice : m_choices) {
        if (seen.insert(choice.model).second)
            models.push_back(choice.model);
    }

    int selection = 0;
    std::string preferred_model;
    if (boost::algorithm::icontains(preferred_preset_name, "Quadro"))
        preferred_model = "Co Print Quadro";
    else if (boost::algorithm::icontains(preferred_preset_name, "ChromaSet"))
        preferred_model = "Co Print ChromaSet";

    for (size_t i = 0; i < models.size(); ++i) {
        m_printer_combo->Append(from_u8(models[i]));
        if (!preferred_model.empty() && models[i] == preferred_model)
            selection = static_cast<int>(i);
    }

    if (!models.empty())
        m_printer_combo->SetSelection(selection);

    fill_nozzle_combo("0.4");
    update_selected_preset();
}

void SelectCoPrintPrinterDialog::fill_nozzle_combo(const std::string &preferred_nozzle)
{
    if (m_nozzle_combo == nullptr || m_printer_combo == nullptr)
        return;

    m_nozzle_combo->Clear();
    const std::string model = into_u8(m_printer_combo->GetValue());
    std::vector<std::string> nozzles;
    for (const Choice &choice : m_choices) {
        if (choice.model != model)
            continue;
        if (std::find(nozzles.begin(), nozzles.end(), choice.nozzle) == nozzles.end())
            nozzles.push_back(choice.nozzle);
    }

    int selection = 0;
    int idx_04    = -1;
    int idx_want  = -1;
    for (size_t i = 0; i < nozzles.size(); ++i) {
        m_nozzle_combo->Append(from_u8(nozzles[i] + " mm"));
        if (nozzles[i] == "0.4")
            idx_04 = static_cast<int>(i);
        if (!preferred_nozzle.empty() && nozzles[i] == preferred_nozzle)
            idx_want = static_cast<int>(i);
    }
    if (idx_want >= 0)
        selection = idx_want;
    else if (idx_04 >= 0)
        selection = idx_04;
    if (!nozzles.empty())
        m_nozzle_combo->SetSelection(selection);
}

void SelectCoPrintPrinterDialog::update_selected_preset()
{
    m_selected_preset_name.clear();
    if (m_printer_combo == nullptr || m_nozzle_combo == nullptr)
        return;

    const std::string model = into_u8(m_printer_combo->GetValue());
    wxString nozzle_label = m_nozzle_combo->GetValue();
    nozzle_label.Replace(" mm", "");
    const std::string nozzle = into_u8(nozzle_label);

    for (const Choice &choice : m_choices) {
        if (choice.model == model && choice.nozzle == nozzle) {
            m_selected_preset_name = choice.preset_name;
            return;
        }
    }
}

void SelectCoPrintPrinterDialog::style_combo(ComboBox *combo)
{
    if (combo == nullptr)
        return;

    combo->SetCornerRadius(combo->FromDIP(6));
    combo->SetBorderColor(StateColor(std::make_pair(kControlBorder, (int) StateColor::Normal)));
    combo->SetBackgroundColor(StateColor(std::make_pair(kControlBackground, (int) StateColor::Normal)));
    combo->SetLabelColor(StateColor(std::make_pair(kTextPrimary, (int) StateColor::Normal)));

    DropDown &drop = combo->GetDropDown();
    drop.SetApplyDarkMode(false);
    drop.SetBackgroundColour(kControlBackground);
    drop.SetBorderColor(StateColor(std::make_pair(kControlBorder, (int) StateColor::Normal)));
    drop.SetTextColor(StateColor(std::make_pair(kTextPrimary, (int) StateColor::Normal)));
    drop.SetSelectorBackgroundColor(StateColor(
        std::make_pair(wxColour(232, 244, 255), (int) StateColor::Checked),
        std::make_pair(kControlBackground, (int) StateColor::Normal)));
}

}} // namespace Slic3r::GUI
