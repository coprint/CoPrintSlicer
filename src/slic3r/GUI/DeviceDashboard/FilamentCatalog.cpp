#include "FilamentCatalog.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

wxColour colour_from_hex(const std::string &hex)
{
    std::string v = hex;
    if (!v.empty() && v.front() == '#')
        v.erase(v.begin());
    if (v.size() < 6)
        return wxColour();
    try {
        const int r = std::stoi(v.substr(0, 2), nullptr, 16);
        const int g = std::stoi(v.substr(2, 2), nullptr, 16);
        const int b = std::stoi(v.substr(4, 2), nullptr, 16);
        return wxColour(r, g, b);
    } catch (...) {
        return wxColour();
    }
}

std::string catalog_path()
{
    const boost::filesystem::path from_resources =
        boost::filesystem::path(Slic3r::resources_dir()) / "filament" / "filament.json";
    if (boost::filesystem::exists(from_resources))
        return from_resources.string();
    return Slic3r::var("filament/filament.json");
}

void prefer_brand_order(std::vector<wxString> &brands)
{
    const wxString first = wxString::FromUTF8("Co Print");
    auto it = std::find_if(brands.begin(), brands.end(), [&](const wxString &name) {
        return name.CmpNoCase(first) == 0;
    });
    if (it != brands.end() && it != brands.begin())
        std::rotate(brands.begin(), it, it + 1);
}

FilamentCatalog fallback_catalog()
{
    FilamentCatalog catalog;
    catalog.brands = {wxString::FromUTF8("Co Print"), wxString::FromUTF8("Generic")};
    const std::vector<wxString> type_names = {
        wxString::FromUTF8("PLA"), wxString::FromUTF8("PLA-CF"), wxString::FromUTF8("PETG"),
        wxString::FromUTF8("ABS"), wxString::FromUTF8("ASA"), wxString::FromUTF8("TPU")};
    for (const wxString &name : type_names)
        catalog.types.push_back({name, 190, 240, 0.04});
    catalog.colors.push_back({wxString::FromUTF8("White"), wxString::FromUTF8("#FFFFFF"), wxColour(255, 255, 255)});
    return catalog;
}

int colour_distance(const wxColour &a, const wxColour &b)
{
    if (!a.IsOk() || !b.IsOk())
        return 255 * 255 * 3;
    const int dr = a.Red() - b.Red();
    const int dg = a.Green() - b.Green();
    const int db = a.Blue() - b.Blue();
    return dr * dr + dg * dg + db * db;
}

} // namespace

FilamentCatalog FilamentCatalog::load()
{
    FilamentCatalog catalog;
    std::ifstream file(catalog_path());
    if (!file.is_open())
        return fallback_catalog();

    try {
        const nlohmann::ordered_json json = nlohmann::ordered_json::parse(file, nullptr, false, true);
        if (json.is_discarded() || !json.is_object())
            return fallback_catalog();

        if (json.contains("brands") && json["brands"].is_array()) {
            for (const auto &item : json["brands"]) {
                if (!item.is_string())
                    continue;
                catalog.brands.emplace_back(wxString::FromUTF8(item.get<std::string>()));
            }
        }
        prefer_brand_order(catalog.brands);

        if (json.contains("types") && json["types"].is_object()) {
            for (auto it = json["types"].begin(); it != json["types"].end(); ++it) {
                if (!it.value().is_object())
                    continue;
                FilamentTypeInfo info;
                info.name = wxString::FromUTF8(it.key());
                info.temp_min = it.value().value("temp_min", 190);
                info.temp_max = it.value().value("temp_max", 240);
                info.pressure_advance = it.value().value("pressure_advance", 0.04);
                catalog.types.push_back(std::move(info));
            }
        }

        if (json.contains("colors") && json["colors"].is_object()) {
            for (auto it = json["colors"].begin(); it != json["colors"].end(); ++it) {
                if (!it.value().is_string())
                    continue;
                FilamentColorInfo info;
                info.name = wxString::FromUTF8(it.key());
                const std::string hex = it.value().get<std::string>();
                info.hex = wxString::FromUTF8(hex);
                info.colour = colour_from_hex(hex);
                if (info.colour.IsOk())
                    catalog.colors.push_back(std::move(info));
            }
        }
    } catch (...) {
        return fallback_catalog();
    }

    if (catalog.brands.empty() || catalog.types.empty() || catalog.colors.empty())
        return fallback_catalog();
    return catalog;
}

const FilamentTypeInfo *FilamentCatalog::find_type(const wxString &name) const
{
    const int index = index_of_type(name);
    return index >= 0 ? &types[static_cast<size_t>(index)] : nullptr;
}

const FilamentColorInfo *FilamentCatalog::find_color(const wxColour &colour) const
{
    const int index = index_of_color(colour);
    return index >= 0 ? &colors[static_cast<size_t>(index)] : nullptr;
}

int FilamentCatalog::index_of_brand(const wxString &name) const
{
    for (size_t i = 0; i < brands.size(); ++i) {
        if (brands[i].CmpNoCase(name) == 0)
            return static_cast<int>(i);
    }
    return brands.empty() ? -1 : 0;
}

int FilamentCatalog::index_of_type(const wxString &name) const
{
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i].name.CmpNoCase(name) == 0)
            return static_cast<int>(i);
    }
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i].name.CmpNoCase(wxString::FromUTF8("PLA")) == 0)
            return static_cast<int>(i);
    }
    return types.empty() ? -1 : 0;
}

int FilamentCatalog::index_of_color(const wxColour &colour) const
{
    if (colors.empty())
        return -1;
    int best = 0;
    int best_dist = colour_distance(colors.front().colour, colour);
    for (size_t i = 0; i < colors.size(); ++i) {
        const int dist = colour_distance(colors[i].colour, colour);
        if (dist < best_dist) {
            best_dist = dist;
            best = static_cast<int>(i);
        }
        if (dist == 0)
            return static_cast<int>(i);
    }
    return best;
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
