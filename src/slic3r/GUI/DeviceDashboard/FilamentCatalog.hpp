#pragma once

#include <vector>

#include <wx/colour.h>
#include <wx/string.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

struct FilamentTypeInfo {
    wxString name;
    int      temp_min{190};
    int      temp_max{240};
    double   pressure_advance{0.04};
};

struct FilamentColorInfo {
    wxString name;
    wxString hex;
    wxColour colour;
};

struct FilamentCatalog {
    std::vector<wxString>           brands;
    std::vector<FilamentTypeInfo>   types;
    std::vector<FilamentColorInfo>  colors;

    static FilamentCatalog load();

    const FilamentTypeInfo *find_type(const wxString &name) const;
    const FilamentColorInfo *find_color(const wxColour &colour) const;
    int index_of_brand(const wxString &name) const;
    int index_of_type(const wxString &name) const;
    int index_of_color(const wxColour &colour) const;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
