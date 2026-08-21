#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/MultiTaskManagerPage.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevLamp.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceDashboard/services/DashboardStateAdapter.hpp"
#include "slic3r/GUI/DeviceDashboard/DeviceUiStyle.hpp"
#include "slic3r/GUI/DeviceDashboard/DeviceDashboardPage.hpp"
#include "slic3r/GUI/DeviceDashboard/panels/CameraPanel.hpp"
#include "slic3r/GUI/DeviceDashboard/panels/FilamentPanel.hpp"
#include "slic3r/GUI/DeviceDashboard/panels/PrinterStatusPanel.hpp"
#include "slic3r/GUI/DeviceDashboard/panels/PrintStatusPanel.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ProgressBar.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <wx/filename.h>
#include <wx/dcbuffer.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/textctrl.h>
#include <wx/dialog.h>
#include <wx/button.h>
#include <wx/colordlg.h>
#include <wx/combobox.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/file.h>
#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/popupwin.h>
#include <wx/simplebook.h>
#include <algorithm>
#include <cctype>
#include <wx/artprov.h>
#include <wx/scrolwin.h>

#include <string>
#include <wx/graphics.h>
#include <wx/dcgraph.h>
#include <wx/event.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <limits>
#include <iomanip>
#include <sstream>
#include <vector>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

static bool set_text_if_changed(wxStaticText *label, const wxString &text)
{
    if (label == nullptr || label->GetLabelText() == text)
        return false;
    label->SetLabelText(text);
    return true;
}

class SidebarScrollbar : public wxPanel
{
public:
    explicit SidebarScrollbar(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(wxSize(FromDIP(8), -1));
        SetMaxSize(wxSize(FromDIP(8), -1));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC raw_dc(this);
            wxGCDC dc(raw_dc);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            if (m_thumb_height <= 0)
                return;

            const int thumb_w = FromDIP(4);
            const int thumb_x = (GetClientSize().GetWidth() - thumb_w) / 2;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#7A8088")));
            dc.DrawRoundedRectangle(thumb_x, m_thumb_y, thumb_w, m_thumb_height, thumb_w / 2.0);
        });
    }

    void set_thumb(int y, int height)
    {
        m_thumb_y = y;
        m_thumb_height = height;
        Refresh();
    }

private:
    int m_thumb_y{ 0 };
    int m_thumb_height{ 0 };
};

static void update_sidebar_scrollbar(wxScrolledWindow *scrolled, wxPanel *track, wxWindow *dip_source)
{
    if (scrolled == nullptr || track == nullptr || dip_source == nullptr)
        return;

    int x = 0, y = 0;
    scrolled->GetViewStart(&x, &y);
    int ux = 0, uy = 0;
    scrolled->GetScrollPixelsPerUnit(&ux, &uy);

    const int content_height = scrolled->GetVirtualSize().GetHeight();
    const int viewport_height = scrolled->GetClientSize().GetHeight();
    const int track_height = track->GetClientSize().GetHeight();
    if (content_height <= viewport_height || track_height <= 0 || uy <= 0) {
        track->Hide();
        return;
    }

    track->Show();
    const int thumb_height = (std::max)(dip_source->FromDIP(34), track_height * viewport_height / content_height);
    const int max_scroll_px = (std::max)(1, content_height - viewport_height);
    const int scroll_px = y * uy;
    const int thumb_y = (track_height - thumb_height) * scroll_px / max_scroll_px;
    if (auto *custom_scrollbar = dynamic_cast<SidebarScrollbar *>(track))
        custom_scrollbar->set_thumb(thumb_y, thumb_height);
    else
        track->Refresh();
}

static bool looks_like_network_identifier(const std::string &value)
{
    if (value.empty())
        return false;

    // Manual-IP additions used to seed the display name from the address itself.
    // Treat those values as technical identifiers rather than user-facing names.
    const auto has_scheme = value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
    const auto colon_pos  = value.find(':');
    const auto dot_count  = std::count(value.begin(), value.end(), '.');
    const bool ipv4ish    = dot_count == 3 &&
                         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                             return std::isdigit(ch) || ch == '.' || ch == ':';
                         });
    return has_scheme || colon_pos != std::string::npos || ipv4ish;
}

static std::string to_lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static bool is_generic_coprint_product_name(const std::string &name)
{
    const std::string lower = to_lower_ascii(name);
    return lower == "co print quadro" ||
           lower == "co-print quadro" ||
           lower == "co_print_quadro" ||
           lower == "co print chromaset" ||
           lower == "co-print chromaset" ||
           lower == "co_print_chromaset";
}

static bool is_placeholder_printer_name(const std::string &name)
{
    const std::string lower = to_lower_ascii(name);
    return lower.empty() ||
           lower == "unknown" ||
           lower == "unknown printer" ||
           lower == "moonraker printer" ||
           lower == "moonraker" ||
           is_generic_coprint_product_name(name) ||
           looks_like_network_identifier(name);
}

static std::string friendly_host_from_address(const std::string &addr)
{
    std::string value = addr;
    const auto scheme_pos = value.find("://");
    if (scheme_pos != std::string::npos)
        value = value.substr(scheme_pos + 3);
    const auto slash_pos = value.find('/');
    if (slash_pos != std::string::npos)
        value = value.substr(0, slash_pos);
    if (value.size() > 5 && value.compare(value.size() - 5, 5, ":7125") == 0)
        value.resize(value.size() - 5);
    return value;
}

static bool hostname_looks_like_quadro(const std::string &lower)
{
    // Hostnames such as "quadorya" do not contain the substring "quadro".
    return lower.find("quadro") != std::string::npos ||
           lower.find("quadorya") != std::string::npos ||
           lower.find("quado") != std::string::npos;
}

static bool apply_coprint_name_from_hostname(const std::string &raw_name, std::string &dev_name, std::string &printer_type)
{
    const std::string lower = to_lower_ascii(raw_name);
    if (hostname_looks_like_quadro(lower)) {
        dev_name = "Co Print Quadro";
        printer_type = "Co_Print_Quadro";
        return true;
    }
    if (lower.find("chromaset") != std::string::npos ||
        lower.find("chromahead") != std::string::npos ||
        lower.find("chroma") != std::string::npos) {
        dev_name = "Co Print ChromaSet";
        printer_type = "Co_Print_ChromaSet";
        return true;
    }
    return false;
}

static int coprint_tool_count_override(const MachineObject *machine)
{
    if (machine == nullptr)
        return 0;

    const std::string identity = to_lower_ascii(
        machine->get_dev_name() + " " +
        machine->printer_type + " " +
        into_u8(machine->get_printer_type_display_str()));

    if (hostname_looks_like_quadro(identity))
        return DeviceDashboard::MaxDashboardTools;
    if (identity.find("chromaset") != std::string::npos ||
        identity.find("chroma set") != std::string::npos)
        return 1;

    return 0;
}

static int coprint_tool_count_from_info_json(const nlohmann::json &info)
{
    if (!info.is_object())
        return 0;

    nlohmann::json payload = info;
    if (payload.contains("result") && payload["result"].is_object())
        payload = payload["result"];

    std::string identity;
    for (const char *key : {"manufacturer", "model", "device_name", "printer_type", "name"}) {
        if (payload.contains(key) && payload[key].is_string()) {
            if (!identity.empty())
                identity += ' ';
            identity += payload[key].get<std::string>();
        }
    }
    identity = to_lower_ascii(identity);

    if (hostname_looks_like_quadro(identity))
        return DeviceDashboard::MaxDashboardTools;
    if (identity.find("chromaset") != std::string::npos ||
        identity.find("chroma set") != std::string::npos ||
        identity.find("chroma") != std::string::npos)
        return 1;

    return 0;
}

static bool coprint_identity_from_info_json(const nlohmann::json &info, std::string &dev_name, std::string &printer_type)
{
    if (!info.is_object())
        return false;

    nlohmann::json payload = info;
    if (payload.contains("result") && payload["result"].is_object())
        payload = payload["result"];

    const std::string manufacturer = payload.contains("manufacturer") && payload["manufacturer"].is_string()
        ? payload["manufacturer"].get<std::string>() : std::string();
    const std::string model = payload.contains("model") && payload["model"].is_string()
        ? payload["model"].get<std::string>() : std::string();
    const std::string device_name = payload.contains("device_name") && payload["device_name"].is_string()
        ? payload["device_name"].get<std::string>() : std::string();
    const std::string type_field = payload.contains("printer_type") && payload["printer_type"].is_string()
        ? payload["printer_type"].get<std::string>() : std::string();
    const std::string identity = to_lower_ascii(
        manufacturer + " " + model + " " + device_name + " " + type_field);

    if (hostname_looks_like_quadro(identity) ||
        identity.find("quadro") != std::string::npos) {
        printer_type = "Co_Print_Quadro";
        if (!device_name.empty() && !is_placeholder_printer_name(device_name))
            dev_name = device_name;
        else
            dev_name.clear();
        return true;
    }
    if (identity.find("chromaset") != std::string::npos ||
        identity.find("chroma set") != std::string::npos ||
        identity.find("chromahead") != std::string::npos ||
        identity.find("chroma") != std::string::npos) {
        printer_type = "Co_Print_ChromaSet";
        if (!device_name.empty() && !is_placeholder_printer_name(device_name))
            dev_name = device_name;
        else
            dev_name.clear();
        return true;
    }

    return false;
}

struct LocalIpv4Subnet {
    uint32_t network{ 0 };
    uint32_t broadcast{ 0 };
};

static uint32_t ipv4_to_uint(const std::string &ip)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address_v4(ip, ec);
    return ec ? 0 : address.to_uint();
}

static std::string uint_to_ipv4(uint32_t value)
{
    return boost::asio::ip::address_v4(value).to_string();
}

static std::vector<LocalIpv4Subnet> local_ipv4_subnets()
{
    std::vector<LocalIpv4Subnet> subnets;
    std::unordered_set<uint64_t> seen;
#ifdef _WIN32
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW) {
        std::vector<unsigned char> buffer(size);
        auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, addresses, &size) == NO_ERROR) {
            for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp)
                    continue;
                for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr == nullptr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    const auto *sockaddr = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
                    const uint32_t ip = ntohl(sockaddr->sin_addr.s_addr);
                    if ((ip >> 24) == 127 || ip == 0)
                        continue;
                    const uint32_t prefix = unicast->OnLinkPrefixLength;
                    if (prefix == 0 || prefix > 30)
                        continue;
                    const uint32_t mask = prefix < 24 ? 0xffffff00u : (prefix == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix)));
                    const uint32_t network = ip & mask;
                    const uint32_t broadcast = prefix < 24 ? (network | 0x000000ffu) : (network | ~mask);
                    const uint64_t key = (static_cast<uint64_t>(network) << 32) | broadcast;
                    if (seen.insert(key).second)
                        subnets.push_back({ network, broadcast });
                }
            }
        }
    }
#else
    try {
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver(io);
        const auto results = resolver.resolve(boost::asio::ip::host_name(), "");
        for (const auto &entry : results) {
            const auto address = entry.endpoint().address();
            if (!address.is_v4() || address.is_loopback())
                continue;
            const uint32_t ip = address.to_v4().to_uint();
            const uint32_t network = ip & 0xffffff00u;
            const uint32_t broadcast = network | 0x000000ffu;
            const uint64_t key = (static_cast<uint64_t>(network) << 32) | broadcast;
            if (seen.insert(key).second)
                subnets.push_back({ network, broadcast });
        }
    } catch (...) {
    }
#endif
    return subnets;
}

/** Normalize user-entered Moonraker addresses.
 *  Fixes typos like "192.168.1..150", strips schemes, and appends :7125. */
static bool sanitize_moonraker_address(std::string input, std::string &out_host_port, std::string &error_message)
{
    auto trim = [](std::string &s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
    };
    trim(input);
    if (input.empty()) {
        error_message = "IP address cannot be empty.";
        return false;
    }

    const auto scheme_pos = input.find("://");
    if (scheme_pos != std::string::npos)
        input = input.substr(scheme_pos + 3);
    const auto slash_pos = input.find('/');
    if (slash_pos != std::string::npos)
        input = input.substr(0, slash_pos);
    trim(input);

    // Collapse accidental double/triple dots from typing ("192.168.1..150").
    for (;;) {
        const auto pos = input.find("..");
        if (pos == std::string::npos)
            break;
        input.replace(pos, 2, ".");
    }
    // Trim dangling dots around the host part.
    while (!input.empty() && input.front() == '.')
        input.erase(input.begin());

    std::string host = input;
    std::string port = "7125";
    const auto colon_pos = input.rfind(':');
    if (colon_pos != std::string::npos && std::count(input.begin(), input.end(), ':') == 1) {
        host = input.substr(0, colon_pos);
        port = input.substr(colon_pos + 1);
        trim(host);
        trim(port);
    }
    while (!host.empty() && host.back() == '.')
        host.pop_back();

    if (host.empty()) {
        error_message = "IP address cannot be empty.";
        return false;
    }
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        error_message = "Invalid printer port.";
        return false;
    }

    // Validate IPv4 shape when the host looks numeric.
    const bool looks_ipv4 = std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isdigit(ch) || ch == '.';
    });
    if (looks_ipv4) {
        std::vector<std::string> octets;
        std::string current;
        for (char ch : host) {
            if (ch == '.') {
                octets.push_back(current);
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        octets.push_back(current);
        if (octets.size() != 4) {
            error_message = "Invalid IP address.";
            return false;
        }
        for (const auto &octet : octets) {
            if (octet.empty() || octet.size() > 3 ||
                !std::all_of(octet.begin(), octet.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                error_message = "Invalid IP address.";
                return false;
            }
            const int value = std::atoi(octet.c_str());
            if (value < 0 || value > 255) {
                error_message = "Invalid IP address.";
                return false;
            }
        }
    }

    out_host_port = host + ":" + port;
    return true;
}

static bool probe_moonraker_host(const std::string &ip, BBLocalMachine &machine)
{
    auto moonraker_base_url = [](std::string host) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(host);
        const auto scheme_pos = host.find("://");
        if (scheme_pos != std::string::npos)
            host = host.substr(scheme_pos + 3);
        const auto slash_pos = host.find('/');
        if (slash_pos != std::string::npos)
            host = host.substr(0, slash_pos);
        trim(host);
        if (host.find(':') == std::string::npos)
            host += ":7125";
        return "http://" + host;
    };

    const std::string base_url = moonraker_base_url(ip);

    auto fetch_json = [&](const std::string &path) -> nlohmann::json {
        std::string body;
        bool success = false;
        auto http = Http::get(base_url + path);
        // Keep probe snappy: callers may run this on a worker thread, but discovery
        // paths still need to finish quickly when a printer is offline.
        http.timeout_connect(1)
            .timeout_max(2)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200) {
                    body = std::move(response);
                    success = true;
                }
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
        if (!success)
            return nlohmann::json();

        auto json = nlohmann::json::parse(body, nullptr, false, true);
        if (json.is_discarded())
            return nlohmann::json();
        return json.contains("result") ? json["result"] : json;
    };

    auto extract_hostname = [](const nlohmann::json &payload) -> std::string {
        if (!payload.is_object())
            return {};
        if (payload.contains("machine_name") && payload["machine_name"].is_string())
            return payload["machine_name"].get<std::string>();
        if (payload.contains("hostname") && payload["hostname"].is_string())
            return payload["hostname"].get<std::string>();
        if (payload.contains("system_info") && payload["system_info"].is_object() &&
            payload["system_info"].contains("hostname") && payload["system_info"]["hostname"].is_string())
            return payload["system_info"]["hostname"].get<std::string>();
        return {};
    };

    machine.dev_ip = base_url.substr(std::strlen("http://"));
    machine.dev_id = machine.dev_ip;
    machine.dev_name = ip;
    machine.printer_type = "Moonraker";

    bool any_endpoint = false;

    // Prefer CoPrint identity endpoint first, then hostname hints, then tool layout.
    const auto coprint_info = fetch_json("/machine/coprint/info");
    if (coprint_info.is_object()) {
        any_endpoint = true;
        std::string detected_name;
        std::string detected_type;
        if (coprint_identity_from_info_json(coprint_info, detected_name, detected_type)) {
            if (!detected_type.empty())
                machine.printer_type = detected_type;
            if (!detected_name.empty() && !is_placeholder_printer_name(detected_name))
                machine.dev_name = detected_name;
        }
    }

    for (const char *path : {"/server/info", "/printer/info", "/machine/system_info"}) {
        const auto info = fetch_json(path);
        if (!info.is_object())
            continue;
        any_endpoint = true;
        const std::string host_name = extract_hostname(info);
        if (host_name.empty())
            continue;
        if (!is_placeholder_printer_name(host_name))
            machine.dev_name = host_name;
        std::string mapped_name;
        std::string mapped_type;
        if (apply_coprint_name_from_hostname(host_name, mapped_name, mapped_type))
            machine.printer_type = mapped_type;
        break;
    }

    const auto objects_info = fetch_json("/printer/objects/list");
    nlohmann::json objects_payload = objects_info;
    if (objects_payload.is_object()) {
        any_endpoint = true;
        if (objects_payload.contains("result") && objects_payload["result"].is_object())
            objects_payload = objects_payload["result"];
        if (objects_payload.contains("objects") && objects_payload["objects"].is_array()) {
            bool has_extruder = false;
            bool has_extra_hotend = false;
            for (const auto &object_name : objects_payload["objects"]) {
                if (!object_name.is_string())
                    continue;
                const std::string name = object_name.get<std::string>();
                if (name == "extruder")
                    has_extruder = true;
                else if (name == "extruder1" || name == "extruder2" || name == "extruder3")
                    has_extra_hotend = true;
            }
            if (has_extruder && !has_extra_hotend)
                machine.printer_type = "Co_Print_ChromaSet";
            else if (has_extra_hotend)
                machine.printer_type = "Co_Print_Quadro";
        }
    }

    return any_endpoint;
}

static bool is_coprint_discovered_machine(const BBLocalMachine &machine)
{
    const std::string normalized_name = to_lower_ascii(machine.dev_name);
    return normalized_name == "co-print" || normalized_name == "coprint";
}


namespace {

/** CoPrint printers sidebar / list icon: resources/images/cprint_printer_nav.png */
constexpr const char *k_cprint_printer_nav_bitmap = "cprint_printer_nav";

wxBitmap safe_scaled_bitmap(wxWindow *win, const std::string &name, int dip, const char *fallback = "tool_temperature_popup")
{
    for (const char *candidate : {name.c_str(), fallback}) {
        try {
            wxBitmap bmp = create_scaled_bitmap(candidate, win, dip);
            if (bmp.IsOk())
                return bmp;
        } catch (const std::exception &) {
        }
    }
    return wxBitmap(win->FromDIP(dip), win->FromDIP(dip));
}

std::string dark_printer_thumbnail_name(const std::string &name)
{
    if (name.empty())
        return "printer_thumbnail_dark";

    constexpr const char *png_suffix = "_png";
    if (name.size() > std::strlen(png_suffix) &&
        name.compare(name.size() - std::strlen(png_suffix), std::strlen(png_suffix), png_suffix) == 0) {
        return name.substr(0, name.size() - std::strlen(png_suffix)) + "_dark";
    }

    return name + "_dark";
}

wxBitmap create_dark_printer_thumbnail(wxWindow *win, const std::string &name, int dip)
{
    const std::array<std::string, 3> candidates = {
        dark_printer_thumbnail_name(name),
        "printer_thumbnail_dark",
        name.empty() ? std::string("printer_thumbnail") : name
    };

    for (const std::string &candidate : candidates) {
        try {
            wxBitmap bmp = create_scaled_bitmap(candidate, win, dip);
            if (bmp.IsOk())
                return bmp;
        } catch (const std::exception &) {
        }
    }
    return wxBitmap(win->FromDIP(dip), win->FromDIP(dip));
}

wxBitmap create_quadro_printer_thumbnail(wxWindow *win, int dip)
{
    try {
        wxBitmap bmp = create_scaled_bitmap("quadro_icon", win, dip);
        if (bmp.IsOk())
            return bmp;
    } catch (const std::exception &) {
    }

    return create_dark_printer_thumbnail(win, "printer_thumbnail", dip);
}

std::vector<wxString> moonraker_camera_stream_urls(MachineObject *obj)
{
    if (obj == nullptr || !obj->is_online())
        return {};

    const std::string ip = obj->get_dev_ip();
    if (ip.empty())
        return {};

    // Common Mainsail/Crowsnest webcam endpoints. Mainsail often proxies the
    // stream under /webcam, while some installs expose mjpg-streamer directly.
    return {
        wxString::Format("http://%s/webcam/?action=stream", ip),
        wxString::Format("http://%s/webcam?action=stream", ip),
        wxString::Format("http://%s/webcam/stream", ip),
        wxString::Format("http://%s/webcam/video", ip),
        wxString::Format("http://%s:8080/?action=stream", ip),
        wxString::Format("http://%s:8080/webcam/?action=stream", ip)
    };
}

wxString html_escape(wxString text)
{
    text.Replace("&", "&amp;");
    text.Replace("\"", "&quot;");
    text.Replace("<", "&lt;");
    text.Replace(">", "&gt;");
    return text;
}

wxString js_escape(wxString text)
{
    text.Replace("\\", "\\\\");
    text.Replace("'", "\\'");
    text.Replace("\r", "");
    text.Replace("\n", "");
    return text;
}

wxImage image_from_thumbnail_data(const ThumbnailData &data)
{
    if (!data.is_valid())
        return wxImage();

    wxImage image(data.width, data.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < data.height; ++r) {
        const unsigned int rr = (data.height - 1 - r) * data.width;
        for (unsigned int c = 0; c < data.width; ++c) {
            const unsigned char *px = data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }
    return image;
}

wxString normalize_camera_stream_url(wxString source, MachineObject *obj)
{
    source.Trim(true);
    source.Trim(false);
    if (source.IsEmpty())
        return wxString();

    if (source.StartsWith("/")) {
        const std::string ip = obj != nullptr ? obj->get_dev_ip() : std::string();
        return ip.empty() ? wxString() : wxString::Format("http://%s%s", ip, source);
    }

    if (source.Find("://") != wxNOT_FOUND)
        return source;

    if (source.Find("/") == wxNOT_FOUND)
        return "http://" + source + "/webcam/?action=stream";

    return "http://" + source;
}

bool is_http_url(const wxString &source)
{
    return source.StartsWith("http://") || source.StartsWith("https://");
}

wxString normalize_local_file_url(wxString source)
{
    if (source.StartsWith("file://"))
        source = source.Mid(7);
#ifdef _WIN32
    if (source.StartsWith("/") && source.length() > 2 && source[2] == ':')
        source = source.Mid(1);
#endif
    source.Replace("%20", " ");
    return source;
}

wxImage scale_preview_thumbnail(wxImage image, int max_width, int max_height)
{
    if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return {};

    const double width_ratio = static_cast<double>(max_width) / static_cast<double>(image.GetWidth());
    const double height_ratio = static_cast<double>(max_height) / static_cast<double>(image.GetHeight());
    const double scale_ratio = std::min(width_ratio, height_ratio);
    const int scaled_width = std::max(1, static_cast<int>(image.GetWidth() * scale_ratio));
    const int scaled_height = std::max(1, static_cast<int>(image.GetHeight() * scale_ratio));
    return image.Scale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH);
}

wxSize preview_thumbnail_target_size(const wxStaticBitmap *bitmap, int fallback_w, int fallback_h)
{
    if (bitmap == nullptr)
        return wxSize(fallback_w, fallback_h);
    wxSize size = bitmap->GetMinSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = bitmap->GetSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = wxSize(fallback_w, fallback_h);
    return size;
}

std::vector<wxString> configured_camera_stream_urls(MachineObject *obj)
{
    std::vector<wxString> urls;
    if (obj != nullptr) {
        for (const std::string &stream_url : obj->camera_stream_urls) {
            const wxString url = normalize_camera_stream_url(from_u8(stream_url), obj);
            if (!url.IsEmpty() && std::find(urls.begin(), urls.end(), url) == urls.end())
                urls.push_back(url);
        }
    }

    if (wxGetApp().app_config != nullptr && wxGetApp().app_config->get("camera", "enable_custom_source") == "true") {
        const wxString custom_source = from_u8(wxGetApp().app_config->get("camera", "custom_source"));
        const wxString custom_url = normalize_camera_stream_url(custom_source, obj);
        if (!custom_url.IsEmpty() && std::find(urls.begin(), urls.end(), custom_url) == urls.end())
            urls.push_back(custom_url);
    }

    if (urls.empty()) {
        for (const wxString &url : moonraker_camera_stream_urls(obj)) {
            if (std::find(urls.begin(), urls.end(), url) == urls.end())
                urls.push_back(url);
        }
    }
    return urls;
}

wxString camera_stream_page(const std::vector<wxString> &stream_urls)
{
    wxString urls_js;
    for (const wxString &url : stream_urls) {
        if (url.IsEmpty())
            continue;
        if (!urls_js.IsEmpty())
            urls_js += ",";
        urls_js += "'" + js_escape(url) + "'";
    }

    return "<!doctype html><html><head><meta charset='utf-8'>"
           "<style>"
           "html,body{margin:0;width:100%;height:100%;background:#000;overflow:hidden;}"
           "body{display:flex;align-items:center;justify-content:center;color:#b8bec8;font-family:Arial,sans-serif;}"
           "#camera-stream{width:100%;height:100%;object-fit:contain;background:#000;}"
           ".message{display:none;position:absolute;inset:0;align-items:center;justify-content:center;background:#000;}"
           "body.loading .message,body.failed .message{display:flex;}"
           "</style></head><body class='loading'>"
           "<img id='camera-stream' alt='Camera stream'>"
           "<div class='message'>Camera stream unavailable</div>"
           "<script>"
           "const urls=[" + urls_js + "];"
           "let index=0;"
           "const img=document.getElementById('camera-stream');"
           "const msg=document.querySelector('.message');"
           "function withCacheBuster(url){return url+(url.indexOf('?')>=0?'&':'?')+'_='+(Date.now());}"
           "function fail(){document.title='coprint-camera-fail';document.body.className='failed';msg.textContent='Camera stream unavailable';}"
           "function next(){"
           "if(index>=urls.length){fail();return;}"
           "document.body.className='loading';"
           "document.title='coprint-camera-loading';"
           "msg.textContent='Trying camera stream...';"
           "img.src=withCacheBuster(urls[index++]);"
           "}"
           "img.onload=function(){document.title='coprint-camera-ok';document.body.className='';};"
           "img.onerror=function(){setTimeout(next,250);};"
           "if(urls.length){next();}else{fail();}"
           "</script></body></html>";
}

// Printer status mini-cards: dark header â€” rounded top corners only; bottom edge straight (separator).
class PsCardHeaderPanel : public wxPanel
{
public:
    PsCardHeaderPanel(wxWindow *parent, const wxColour &fill, double corner_radius)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_fill(fill)
        , m_corner_radius(corner_radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(fill);
        SetDoubleBuffered(true);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
        Bind(wxEVT_PAINT, &PsCardHeaderPanel::on_paint, this);
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxRect rect = GetClientRect();
        if (rect.width <= 0 || rect.height <= 0)
            return;

        const double x = rect.x;
        const double y = rect.y;
        const double w = rect.width;
        const double h = rect.height;
        const double r = std::min(m_corner_radius, std::min(w * 0.5, h * 0.5));

        dc.SetBackground(wxBrush(m_fill));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            static constexpr double kPi = 3.14159265358979323846;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->SetBrush(wxBrush(m_fill));
            gc->SetPen(*wxTRANSPARENT_PEN);

            // Top-left and top-right rounded only; bottom edge of header stays straight.
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(x + r, y);
            path.AddLineToPoint(x + w - r, y);
            path.AddArc(x + w - r, y + r, r, -kPi / 2.0, 0.0, false);
            path.AddLineToPoint(x + w, y + h);
            path.AddLineToPoint(x, y + h);
            path.AddLineToPoint(x, y + r);
            path.AddArc(x + r, y + r, r, kPi, 1.5 * kPi, false);
            path.CloseSubpath();
            gc->FillPath(path);
            return;
        }

        const int ri = std::max(1, static_cast<int>(r + 0.5));
        const int cap_hi = std::min(ri * 2, rect.height);
        dc.SetBrush(wxBrush(m_fill));
        dc.SetPen(wxPen(m_fill, 1));
        dc.DrawRoundedRectangle(rect.x, rect.y, rect.width, cap_hi, ri);
        if (rect.height > cap_hi)
            dc.DrawRectangle(rect.x, rect.y + cap_hi, rect.width, rect.height - cap_hi);
    }

    wxColour m_fill;
    double   m_corner_radius;
};

wxString layer_value_text(int layer)
{
    return layer < 0 ? wxString("N/A") : wxString::Format("%d", layer);
}

std::array<wxColour, 4> default_filament_preview_colors()
{
    return {
        wxColour(214, 181, 46),
        wxColour(57, 145, 212),
        wxColour(217, 101, 43),
        wxColour(164, 207, 42)
    };
}

std::array<wxString, 4> default_filament_preview_materials()
{
    return { wxString("PLA"), wxString("PLA"), wxString("PLA"), wxString("PLA") };
}

std::array<wxString, 4> default_filament_preview_weights()
{
    return { wxString("14.3g"), wxString("14.3g"), wxString("14.3g"), wxString("14.3g") };
}

std::array<int, 4> default_filament_preview_assigned_tools()
{
    return { 1, 2, 3, 4 };
}

wxColour colour_from_hex(const std::string &hex, const wxColour &fallback)
{
    std::string v = hex;
    if (!v.empty() && v.front() == '#')
        v.erase(v.begin());
    if (v.size() < 6)
        return fallback;
    try {
        const int r = std::stoi(v.substr(0, 2), nullptr, 16);
        const int g = std::stoi(v.substr(2, 2), nullptr, 16);
        const int b = std::stoi(v.substr(4, 2), nullptr, 16);
        return wxColour(r, g, b);
    } catch (...) {
        return fallback;
    }
}

wxString hex_from_colour(const wxColour &color)
{
    return wxString::Format("#%02X%02X%02X", color.Red(), color.Green(), color.Blue());
}

struct FilamentMetadataResult {
    std::array<wxColour, 4> model_colors;
    std::array<wxString, 4> materials;
    std::array<wxString, 4> weights;
    bool colors_parsed{false};
};

struct LoadedFilamentResult {
    std::array<wxColour, 4> assigned_colors;
    std::array<wxString, 4> materials;
    std::array<bool, 4> tool_has_color{};
    bool has_colors{false};
};

static nlohmann::json parse_json_body(const std::string &body)
{
    if (body.empty())
        return {};
    auto parsed = nlohmann::json::parse(body, nullptr, false, true);
    return parsed.is_discarded() ? nlohmann::json{} : parsed;
}

static wxArrayString split_metadata_string(const std::string &raw)
{
    wxString s = wxString::FromUTF8(raw);
    const wxChar sep = s.Find(';') != wxNOT_FOUND ? ';' : ',';
    wxArrayString parts = wxSplit(s, sep);
    for (auto &part : parts) {
        part.Trim(true).Trim(false);
        if (part.StartsWith("\"") && part.EndsWith("\"") && part.length() >= 2)
            part = part.Mid(1, part.length() - 2);
    }
    return parts;
}

static FilamentMetadataResult parse_filament_metadata(nlohmann::json metadata)
{
    FilamentMetadataResult result;
    result.model_colors = default_filament_preview_colors();
    result.materials    = default_filament_preview_materials();
    result.weights      = default_filament_preview_weights();

    if (metadata.contains("result"))
        metadata = metadata["result"];
    if (!metadata.is_object())
        return result;

    if (auto it = metadata.find("filament_colors"); it != metadata.end()) {
        if (it->is_array()) {
            for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i)
                if ((*it)[i].is_string()) {
                    result.model_colors[i] = colour_from_hex((*it)[i].get<std::string>(), result.model_colors[i]);
                    result.colors_parsed = true;
                }
        } else if (it->is_string()) {
            auto colors = split_metadata_string(it->get<std::string>());
            for (size_t i = 0; i < std::min<size_t>(4, colors.size()); ++i) {
                result.model_colors[i] = colour_from_hex(into_u8(colors[i]), result.model_colors[i]);
                result.colors_parsed = true;
            }
        }
    }

    if (auto it = metadata.find("filament_weights"); it != metadata.end()) {
        if (it->is_array()) {
            for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i)
                if ((*it)[i].is_number())
                    result.weights[i] = wxString::Format("%.1fg", (*it)[i].get<double>());
        } else if (it->is_string()) {
            auto parts = split_metadata_string(it->get<std::string>());
            for (size_t i = 0; i < std::min<size_t>(4, parts.size()); ++i) {
                double v = 0.0;
                if (parts[i].ToDouble(&v))
                    result.weights[i] = wxString::Format("%.1fg", v);
            }
        } else if (auto total = metadata.find("filament_weight_total");
                   total != metadata.end() && total->is_number()) {
            const double each = total->get<double>() / 4.0;
            for (auto &w : result.weights)
                w = wxString::Format("%.1fg", each);
        }
    }

    if (auto it = metadata.find("filament_type"); it != metadata.end()) {
        if (it->is_array()) {
            for (size_t i = 0; i < std::min<size_t>(4, it->size()); ++i)
                if ((*it)[i].is_string())
                    result.materials[i] = wxString::FromUTF8((*it)[i].get<std::string>());
        } else if (it->is_string()) {
            const std::string raw = it->get<std::string>();
            auto parsed_types = nlohmann::json::parse(raw, nullptr, false, true);
            if (!parsed_types.is_discarded() && parsed_types.is_array()) {
                for (size_t i = 0; i < std::min<size_t>(4, parsed_types.size()); ++i)
                    if (parsed_types[i].is_string())
                        result.materials[i] = wxString::FromUTF8(parsed_types[i].get<std::string>());
            } else {
                auto parts = split_metadata_string(raw);
                for (size_t i = 0; i < std::min<size_t>(4, parts.size()); ++i)
                    if (!parts[i].empty())
                        result.materials[i] = parts[i];
            }
        }
    }

    return result;
}

bool is_valid_loaded_filament_colour(const wxColour &colour);

static LoadedFilamentResult parse_filament_db(nlohmann::json db)
{
    LoadedFilamentResult result;
    result.materials.fill(wxString::FromUTF8("Empty"));

    if (db.contains("result"))
        db = db["result"];
    if (db.is_object() && db.contains("value"))
        db = db["value"];
    if (db.is_object() && db.contains("filament_selections"))
        db = db["filament_selections"];
    if (!db.is_object())
        return result;

    for (int ui_tool = 1; ui_tool <= 4; ++ui_tool) {
        const std::string key_name = "toolhead_" + std::to_string(ui_tool);
        const std::string zero_key = "toolhead_" + std::to_string(ui_tool - 1);
        const nlohmann::json *tool_ptr = nullptr;
        if (db.contains(key_name) && db[key_name].is_object())
            tool_ptr = &db[key_name];
        else if (db.contains(zero_key) && db[zero_key].is_object())
            tool_ptr = &db[zero_key];
        if (tool_ptr == nullptr)
            continue;
        const auto &tool = *tool_ptr;
        if (auto it = tool.find("color_hex"); it != tool.end() && it->is_string()) {
            const wxColour parsed = colour_from_hex(it->get<std::string>(), wxColour());
            if (is_valid_loaded_filament_colour(parsed)) {
                result.assigned_colors[ui_tool - 1] = parsed;
                result.tool_has_color[ui_tool - 1] = true;
                result.has_colors = true;
            }
        }
        if (auto it = tool.find("type"); it != tool.end() && it->is_string())
            result.materials[ui_tool - 1] = wxString::FromUTF8(it->get<std::string>());
    }

    return result;
}

bool is_valid_loaded_filament_colour(const wxColour &colour)
{
    return colour.IsOk();
}

bool is_empty_filament_material(const wxString &material)
{
    return material.IsEmpty()
        || material.CmpNoCase(wxString::FromUTF8("Empty")) == 0
        || material.CmpNoCase(wxString::FromUTF8("N/A")) == 0;
}

bool looks_like_hex_colour(wxString value)
{
    value.Trim(true);
    value.Trim(false);
    if (value.StartsWith("#"))
        value = value.Mid(1);
    if (value.length() != 6)
        return false;
    const std::string utf8 = into_u8(value);
    if (utf8.size() != 6)
        return false;
    for (const unsigned char ch : utf8) {
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}

wxString short_filament_type_from_preset(const wxString &preset)
{
    const wxString upper = preset.Upper();
    if (upper.Contains("PETG"))
        return "PETG";
    if (upper.Contains("ABS"))
        return "ABS";
    if (upper.Contains("ASA"))
        return "ASA";
    if (upper.Contains("TPU"))
        return "TPU";
    if (upper.Contains("PA"))
        return "PA";
    return "PLA";
}

std::pair<int, int> nozzle_temperature_range_for_material(const wxString &material)
{
    const wxString upper = material.Upper();
    if (upper.Contains("PETG"))
        return {260, 220};
    if (upper.Contains("ABS"))
        return {280, 240};
    if (upper.Contains("ASA"))
        return {280, 240};
    if (upper.Contains("TPU"))
        return {240, 210};
    if (upper.Contains("PA"))
        return {300, 260};
    return {240, 190};
}

class RoundedSelect : public StaticBox
{
public:
    RoundedSelect(wxWindow *parent, const wxArrayString &choices, int selection, const wxColour &bg,
                  const wxColour &border, const wxColour &text, const wxColour &muted, int radius)
        : StaticBox(parent, wxID_ANY)
        , m_choices(choices)
        , m_bg(bg)
        , m_border(border)
        , m_text(text)
        , m_muted(muted)
    {
        SetMinSize(wxSize(FromDIP(250), FromDIP(40)));
        SetCornerRadius(radius);
        SetBorderWidth(FromDIP(1));
        SetBorderColorNormal(border);
        SetBackgroundColorNormal(bg);
        SetBackgroundColour(parent->GetBackgroundColour());
        SetCursor(wxCursor(wxCURSOR_HAND));

        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        m_value = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_value->SetForegroundColour(text);
        m_arrow = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("v"));
        m_arrow->SetForegroundColour(muted);
        sizer->Add(m_value, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
        sizer->Add(m_arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        SetSizer(sizer);

        SetSelection(selection);
        Bind(wxEVT_LEFT_DOWN, &RoundedSelect::on_left_down, this);
        m_value->Bind(wxEVT_LEFT_DOWN, &RoundedSelect::on_left_down, this);
        m_arrow->Bind(wxEVT_LEFT_DOWN, &RoundedSelect::on_left_down, this);
    }

    void SetSelection(int selection)
    {
        if (m_choices.IsEmpty())
            return;
        m_selection = std::clamp(selection, 0, static_cast<int>(m_choices.size()) - 1);
        m_value->SetLabel(m_choices[m_selection]);
        Layout();
    }

    wxString GetValue() const
    {
        return !m_choices.IsEmpty() && m_selection >= 0 ? m_choices[m_selection] : wxString();
    }

    void SetChangeHandler(std::function<void()> handler)
    {
        m_change_handler = std::move(handler);
    }

private:
    void on_left_down(wxMouseEvent &)
    {
        if (m_choices.IsEmpty())
            return;

        wxMenu menu;
        constexpr int base_id = wxID_HIGHEST + 4300;
        for (size_t i = 0; i < m_choices.size(); ++i)
            menu.Append(base_id + static_cast<int>(i), m_choices[i]);

        const int result = GetPopupMenuSelectionFromUser(menu, wxPoint(0, GetSize().y + FromDIP(4)));
        if (result < base_id)
            return;

        const int next_selection = result - base_id;
        if (next_selection < 0 || next_selection >= static_cast<int>(m_choices.size()))
            return;

        SetSelection(next_selection);
        if (m_change_handler)
            m_change_handler();
    }

    wxArrayString m_choices;
    int m_selection{0};
    wxStaticText *m_value{nullptr};
    wxStaticText *m_arrow{nullptr};
    wxColour m_bg;
    wxColour m_border;
    wxColour m_text;
    wxColour m_muted;
    std::function<void()> m_change_handler;
};

class RoundedValueBox : public StaticBox
{
public:
    RoundedValueBox(wxWindow *parent, const wxString &value, const wxSize &min_size, const wxColour &bg,
                    const wxColour &border, const wxColour &text, int radius, const wxString &suffix = wxEmptyString)
        : StaticBox(parent, wxID_ANY)
    {
        SetMinSize(min_size);
        SetCornerRadius(radius);
        SetBorderWidth(FromDIP(1));
        SetBorderColorNormal(border);
        SetBackgroundColorNormal(bg);
        SetBackgroundColour(parent->GetBackgroundColour());

        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        m_value = new wxStaticText(this, wxID_ANY, value);
        m_value->SetForegroundColour(text);
        sizer->Add(m_value, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
        if (!suffix.IsEmpty()) {
            auto *suffix_label = new wxStaticText(this, wxID_ANY, suffix);
            suffix_label->SetForegroundColour(text);
            sizer->Add(suffix_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        }
        SetSizer(sizer);
    }

    void SetValue(const wxString &value)
    {
        if (m_value == nullptr)
            return;
        m_value->SetLabel(value);
        Layout();
    }

private:
    wxStaticText *m_value{nullptr};
};

class RoundedColorSwatch : public wxWindow
{
public:
    RoundedColorSwatch(wxWindow *parent, const wxColour &colour, const wxColour &border, int radius)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(32), parent->FromDIP(32)))
        , m_colour(colour)
        , m_border(border)
        , m_radius(radius)
    {
        SetMinSize(wxSize(FromDIP(32), FromDIP(32)));
        SetMaxSize(wxSize(FromDIP(32), FromDIP(32)));
        SetBackgroundColour(parent->GetBackgroundColour());
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &RoundedColorSwatch::on_paint, this);
    }

    void SetColour(const wxColour &colour)
    {
        m_colour = colour;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC raw_dc(this);
        wxGCDC dc(raw_dc);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(wxPen(m_border, FromDIP(1)));
        dc.SetBrush(wxBrush(m_colour.IsOk() ? m_colour : wxColour(214, 45, 214)));
        const wxSize size = GetClientSize();
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, m_radius);
    }

    wxColour m_colour;
    wxColour m_border;
    int m_radius{8};
};

class FilamentMaterialDialog : public wxDialog
{
public:
    FilamentMaterialDialog(wxWindow *parent, int ui_tool, const wxString &initial_material, const wxColour &initial_color)
        : wxDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_colour(initial_color.IsOk() ? initial_color : wxColour(214, 45, 214))
    {
        namespace Style = DeviceDashboard;

        const wxColour page_bg = Style::DeviceUiStyle::card_background();
        const wxColour card_bg = Style::DeviceUiStyle::card_background();
        const wxColour control_bg = Style::DeviceUiStyle::control_background();
        const wxColour border = Style::DeviceUiStyle::card_border();
        const wxColour text = Style::DeviceUiStyle::text_primary();
        const wxColour muted = Style::DeviceUiStyle::text_muted();
        const wxColour accent = Style::DeviceUiStyle::accent();
        const int shell_radius = FromDIP(18);
        const int control_radius = FromDIP(14);
        const int button_radius = FromDIP(14);

        SetBackgroundColour(page_bg);

        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *shell = new StaticBox(this, wxID_ANY);
        shell->SetCornerRadius(shell_radius);
        shell->SetBorderWidth(FromDIP(1));
        shell->SetBorderColorNormal(border);
        shell->SetBackgroundColorNormal(page_bg);
        shell->SetBackgroundColour(page_bg);
        auto *outer = new wxBoxSizer(wxVERTICAL);

        auto *header = new wxPanel(shell, wxID_ANY);
        header->SetBackgroundColour(card_bg);
        auto *header_sizer = new wxBoxSizer(wxVERTICAL);

        auto *title_row = new wxBoxSizer(wxHORIZONTAL);
        auto *title = new wxStaticText(header, wxID_ANY, wxString::Format("Tool %d Filament", ui_tool));
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title_font.SetPointSize(title_font.GetPointSize() + 2);
        title->SetFont(title_font);
        title->SetForegroundColour(text);
        title_row->Add(title, 1, wxALIGN_CENTER_VERTICAL);

        auto *close_x = new Button(header, wxString::FromUTF8("\u00D7"));
        close_x->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        close_x->SetCornerRadius(FromDIP(8));
        close_x->SetMinSize(wxSize(FromDIP(34), FromDIP(34)));
        close_x->SetPaddingSize(wxSize(FromDIP(6), FromDIP(2)));
        wxFont close_font = close_x->GetFont();
        close_font.SetPointSize(close_font.GetPointSize() + 4);
        close_font.SetWeight(wxFONTWEIGHT_BOLD);
        close_x->SetFont(close_font);
        close_x->SetBackgroundColour(card_bg);
        close_x->SetBackgroundColor(StateColor(
            std::pair(wxColour(220, 220, 220), (int) StateColor::Pressed),
            std::pair(wxColour(235, 235, 235), (int) StateColor::Hovered),
            std::pair(card_bg, (int) StateColor::Normal)));
        close_x->SetBorderWidth(0);
        close_x->SetTextColor(StateColor(std::pair(text, (int) StateColor::Normal)));
        title_row->Add(close_x, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

        auto *subtitle = new wxStaticText(header, wxID_ANY, wxString::FromUTF8("Edit material, color, and flow settings from the CoPrint device panel"));
        subtitle->SetForegroundColour(muted);

        auto *accent_line = new StaticBox(header, wxID_ANY);
        accent_line->SetMinSize(wxSize(-1, FromDIP(3)));
        accent_line->SetMaxSize(wxSize(-1, FromDIP(3)));
        accent_line->SetCornerRadius(FromDIP(2));
        accent_line->SetBorderWidth(0);
        accent_line->SetBackgroundColorNormal(accent);
        accent_line->SetBackgroundColour(accent);

        header_sizer->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        header_sizer->Add(subtitle, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(20));
        header_sizer->Add(accent_line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));
        header->SetSizer(header_sizer);
        outer->Add(header, 0, wxEXPAND);

        auto *body = new wxPanel(shell, wxID_ANY);
        body->SetBackgroundColour(page_bg);
        auto *body_sizer = new wxBoxSizer(wxVERTICAL);

        auto *grid = new wxFlexGridSizer(0, 2, FromDIP(12), FromDIP(16));
        grid->AddGrowableCol(1, 1);

        auto add_label = [&](const wxString &label_text) {
            auto *label = new wxStaticText(body, wxID_ANY, label_text);
            label->SetForegroundColour(muted);
            grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            return label;
        };

        add_label(wxString::FromUTF8("Filament"));
        wxArrayString presets;
        presets.Add("PLA");
        presets.Add("PETG");
        presets.Add("ABS");
        presets.Add("ASA");
        presets.Add("TPU");
        const wxString initial_type = is_empty_filament_material(initial_material) ? wxString("PLA") : initial_material;
        const int initial_selection = initial_type.CmpNoCase("PLA") == 0 ? 0 : std::max(0, presets.Index(initial_type, false));
        m_filament_combo = new RoundedSelect(body, presets, initial_selection == wxNOT_FOUND ? 0 : initial_selection,
                                             control_bg, border, text, muted, control_radius);
        grid->Add(m_filament_combo, 1, wxEXPAND);

        add_label(wxString::FromUTF8("Color"));
        m_colour_swatch = new RoundedColorSwatch(body, m_colour, border, FromDIP(10));
        m_colour_swatch->SetCursor(wxCursor(wxCURSOR_HAND));
        grid->Add(m_colour_swatch, 0, wxALIGN_CENTER_VERTICAL);

        add_label(wxString::FromUTF8("Nozzle\nTemp."));
        auto *temp_row = new wxBoxSizer(wxHORIZONTAL);
        auto *max_col = new wxBoxSizer(wxVERTICAL);
        auto *min_col = new wxBoxSizer(wxVERTICAL);
        auto *max_label = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("max"));
        auto *min_label = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("min"));
        max_label->SetForegroundColour(muted);
        min_label->SetForegroundColour(muted);
        max_col->Add(max_label, 0, wxBOTTOM, FromDIP(4));
        min_col->Add(min_label, 0, wxBOTTOM, FromDIP(4));
        m_temp_max = new RoundedValueBox(body, wxEmptyString, wxSize(FromDIP(88), FromDIP(38)), control_bg, border, text, control_radius, wxString::FromUTF8("C"));
        m_temp_min = new RoundedValueBox(body, wxEmptyString, wxSize(FromDIP(88), FromDIP(38)), control_bg, border, text, control_radius, wxString::FromUTF8("C"));
        max_col->Add(m_temp_max, 0);
        min_col->Add(m_temp_min, 0);
        temp_row->Add(max_col, 0, wxRIGHT, FromDIP(10));
        temp_row->Add(min_col, 0);
        grid->Add(temp_row, 0, wxEXPAND);

        body_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));

        auto *calibration = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("Flow Dynamics Calibration"));
        wxFont cal_font = calibration->GetFont();
        cal_font.SetWeight(wxFONTWEIGHT_BOLD);
        calibration->SetFont(cal_font);
        calibration->SetForegroundColour(accent);
        body_sizer->Add(calibration, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

        auto *pa_grid = new wxFlexGridSizer(0, 2, FromDIP(10), FromDIP(16));
        pa_grid->AddGrowableCol(1, 1);
        auto *pa_label = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("PA Profile"));
        pa_label->SetForegroundColour(muted);
        pa_grid->Add(pa_label, 0, wxALIGN_CENTER_VERTICAL);
        wxArrayString pa_profiles;
        pa_profiles.Add("Default");
        m_pa_combo = new RoundedSelect(body, pa_profiles, 0, control_bg, border, text, muted, control_radius);
        pa_grid->Add(m_pa_combo, 1, wxEXPAND);
        auto *factor_label = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("Factor K"));
        factor_label->SetForegroundColour(muted);
        pa_grid->Add(factor_label, 0, wxALIGN_CENTER_VERTICAL);
        m_factor_k = new wxStaticText(body, wxID_ANY, wxString::FromUTF8("0.020"));
        m_factor_k->SetForegroundColour(text);
        pa_grid->Add(m_factor_k, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        body_sizer->Add(pa_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_confirm = new Button(body, wxString::FromUTF8("Confirm"));
        m_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        style_dialog_button(m_confirm, true, accent, control_bg, border, text, page_bg, button_radius);
        m_reset = new Button(body, wxString::FromUTF8("Reset"));
        m_reset->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        style_dialog_button(m_reset, false, accent, control_bg, border, text, page_bg, button_radius);
        m_close = new Button(body, wxString::FromUTF8("Close"));
        m_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        style_dialog_button(m_close, false, accent, control_bg, border, text, page_bg, button_radius);
        buttons->AddStretchSpacer(1);
        buttons->Add(m_confirm, 0, wxRIGHT, FromDIP(14));
        buttons->Add(m_reset, 0, wxRIGHT, FromDIP(14));
        buttons->Add(m_close, 0);
        body_sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(26));
        body->SetSizer(body_sizer);
        outer->Add(body, 0, wxEXPAND);

        shell->SetSizer(outer);
        root->Add(shell, 1, wxEXPAND | wxALL, FromDIP(2));
        SetSizerAndFit(root);
        SetMinSize(GetSize());
        apply_rounded_window_shape(shell_radius);

        m_colour_swatch->SetToolTip(wxString::FromUTF8("Select color"));

        m_filament_combo->SetChangeHandler([this]() { update_temperature_fields(); });
        m_colour_swatch->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { choose_colour(); });
        m_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
        m_reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_filament_combo->SetSelection(0);
            m_colour = wxColour(214, 45, 214);
            m_colour_swatch->SetColour(m_colour);
            m_factor_k->SetLabel(wxString::FromUTF8("0.020"));
            update_temperature_fields();
        });
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        close_x->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        update_temperature_fields();

        BOOST_LOG_TRIVIAL(info) << "PrinterWebView: filament material dialog opened for T" << ui_tool;
    }

    wxString material() const
    {
        return short_filament_type_from_preset(m_filament_combo->GetValue());
    }

    wxString color_hex() const
    {
        return hex_from_colour(m_colour);
    }

private:
    void style_dialog_button(Button *button, bool primary, const wxColour &accent, const wxColour &control_bg,
                             const wxColour &border, const wxColour &text, const wxColour &page_bg, int radius)
    {
        if (button == nullptr)
            return;

        button->SetCornerRadius(radius);
        button->SetMinSize(wxSize(FromDIP(92), FromDIP(34)));
        button->SetPaddingSize(wxSize(FromDIP(14), FromDIP(8)));
        button->SetBackgroundColour(page_bg);
        button->SetBorderWidth(FromDIP(1));
        button->SetBackgroundColor(StateColor(
            std::pair(primary ? wxColour(33, 141, 97) : wxColour(37, 40, 46), (int) StateColor::Pressed),
            std::pair(primary ? wxColour(53, 198, 136) : wxColour(53, 57, 65), (int) StateColor::Hovered),
            std::pair(primary ? accent : control_bg, (int) StateColor::Normal)));
        button->SetBorderColor(StateColor(
            std::pair(primary ? accent : wxColour(76, 82, 92), (int) StateColor::Hovered),
            std::pair(primary ? accent : border, (int) StateColor::Normal)));
        button->SetTextColor(StateColor(
            std::pair(wxColour(255, 255, 255), (int) StateColor::Hovered),
            std::pair(primary ? wxColour(255, 255, 255) : text, (int) StateColor::Normal)));
    }

    void apply_rounded_window_shape(int radius)
    {
        const wxSize size = GetSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        wxBitmap mask(size.x, size.y);
        wxMemoryDC dc(mask);
        dc.SetBackground(wxBrush(*wxBLACK));
        dc.Clear();
        {
            wxGCDC gc(dc);
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(*wxWHITE));
            gc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
        }
        dc.SelectObject(wxNullBitmap);
        wxRegion region(mask, *wxBLACK);
        SetShape(region);
    }

    void update_temperature_fields()
    {
        const auto [max_temp, min_temp] = nozzle_temperature_range_for_material(material());
        m_temp_max->SetValue(wxString::Format("%d", max_temp));
        m_temp_min->SetValue(wxString::Format("%d", min_temp));
    }

    void choose_colour()
    {
        static wxColourData data;
        data.SetChooseFull(true);
        data.SetColour(m_colour);
        wxColourDialog dialog(this, &data);
        if (dialog.ShowModal() != wxID_OK)
            return;
        data = dialog.GetColourData();
        m_colour = data.GetColour();
        m_colour_swatch->SetColour(m_colour);
    }

    RoundedSelect *m_filament_combo{nullptr};
    RoundedSelect *m_pa_combo{nullptr};
    RoundedValueBox *m_temp_max{nullptr};
    RoundedValueBox *m_temp_min{nullptr};
    wxStaticText *m_factor_k{nullptr};
    RoundedColorSwatch *m_colour_swatch{nullptr};
    Button *m_confirm{nullptr};
    Button *m_reset{nullptr};
    Button *m_close{nullptr};
    wxColour m_colour;
};

void position_dialog_near_anchor(wxDialog& dialog, wxWindow *fallback_parent, const wxPoint& anchor_screen_pos)
{
    if (anchor_screen_pos == wxDefaultPosition)
        return;

    const wxSize size = dialog.GetSize();
    wxPoint pos(anchor_screen_pos.x - size.x / 2, anchor_screen_pos.y - size.y - 16);

    int display_index = wxDisplay::GetFromPoint(anchor_screen_pos);
    if (display_index == wxNOT_FOUND && fallback_parent != nullptr)
        display_index = wxDisplay::GetFromWindow(fallback_parent);
    wxRect area = display_index != wxNOT_FOUND ? wxDisplay(display_index).GetClientArea() : wxRect(wxPoint(0, 0), wxGetDisplaySize());

    const int min_x = area.GetLeft() + 8;
    const int min_y = area.GetTop() + 8;
    const int max_x = std::max(min_x, area.GetRight() - size.x - 8);
    const int max_y = std::max(min_y, area.GetBottom() - size.y - 8);
    pos.x = std::clamp(pos.x, min_x, max_x);
    pos.y = std::clamp(pos.y, min_y, max_y);
    dialog.SetPosition(pos);
}

std::string url_encode_component(const wxString &text)
{
    const std::string input = into_u8(text);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0f]);
        }
    }
    return out;
}

std::string url_encode_path_preserving_slashes(std::string path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() * 3);
    for (unsigned char ch : path) {
        if (ch == '\\')
            ch = '/';

        if (ch == '/' || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0f]);
        }
    }
    return out;
}

std::string moonraker_base_url(const MachineObject *obj)
{
    if (obj == nullptr)
        return {};
    std::string host = obj->get_dev_ip();
    if (host.empty())
        host = obj->get_dev_id();
    if (host.empty())
        return {};
    if (host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0)
        return host;
    if (host.find(':') == std::string::npos)
        host += ":7125";
    return "http://" + host;
}

wxString moonraker_thumbnail_url_from_metadata(const nlohmann::json &metadata, const std::string &base)
{
    if (base.empty() || !metadata.is_object() || !metadata.contains("thumbnails") || !metadata["thumbnails"].is_array())
        return wxString();

    const nlohmann::json *best_thumbnail = nullptr;
    int best_score = -1;
    for (const auto &thumbnail : metadata["thumbnails"]) {
        if (!thumbnail.is_object())
            continue;

        bool has_path = false;
        for (const char *key : {"relative_path", "path", "filename"}) {
            if (thumbnail.contains(key) && thumbnail[key].is_string() && !thumbnail[key].get<std::string>().empty()) {
                has_path = true;
                break;
            }
        }
        if (!has_path)
            continue;

        int score = 0;
        if (thumbnail.contains("width") && thumbnail["width"].is_number_integer() &&
            thumbnail.contains("height") && thumbnail["height"].is_number_integer()) {
            score = thumbnail["width"].get<int>() * thumbnail["height"].get<int>();
        } else if (thumbnail.contains("size") && thumbnail["size"].is_number_integer()) {
            score = thumbnail["size"].get<int>();
        }

        if (best_thumbnail == nullptr || score > best_score) {
            best_thumbnail = &thumbnail;
            best_score = score;
        }
    }

    if (best_thumbnail == nullptr)
        return wxString();

    std::string relative_path;
    for (const char *key : {"relative_path", "path", "filename"}) {
        if (best_thumbnail->contains(key) && (*best_thumbnail)[key].is_string()) {
            relative_path = (*best_thumbnail)[key].get<std::string>();
            if (!relative_path.empty())
                break;
        }
    }

    for (char &ch : relative_path) {
        if (ch == '\\')
            ch = '/';
    }
    while (!relative_path.empty() && relative_path.front() == '/')
        relative_path.erase(relative_path.begin());
    if (relative_path.rfind("gcodes/", 0) == 0)
        relative_path.erase(0, 7);

    if (relative_path.empty())
        return wxString();

    return from_u8(base + "/server/files/gcodes/" + url_encode_path_preserving_slashes(relative_path));
}

int active_tool_index_from_moonraker_extruder(const std::string &extruder)
{
    if (extruder == "extruder")
        return 0;
    static const std::string prefix = "extruder";
    if (extruder.rfind(prefix, 0) != 0 || extruder.size() <= prefix.size())
        return -1;

    const std::string suffix = extruder.substr(prefix.size());
    if (suffix.size() != 1 || !std::isdigit(static_cast<unsigned char>(suffix[0])))
        return -1;

    const int index = suffix[0] - '0';
    return index >= 1 && index < DeviceDashboard::MaxDashboardTools ? index : -1;
}

bool moonraker_fan_percent_from_json(const nlohmann::json &fan_obj, int &out_percent)
{
    if (!fan_obj.is_object())
        return false;

    if (fan_obj.contains("speed") && fan_obj["speed"].is_number()) {
        const double speed = fan_obj["speed"].get<double>();
        const int pwm = speed <= 1.0
            ? static_cast<int>(speed * 255.0 + 0.5)
            : static_cast<int>(speed + 0.5);
        out_percent = std::clamp(static_cast<int>(std::round(pwm / 2.55)), 0, 100);
        return true;
    }

    if (fan_obj.contains("power") && fan_obj["power"].is_number()) {
        const int pwm = static_cast<int>(fan_obj["power"].get<double>() * 255.0 + 0.5);
        out_percent = std::clamp(static_cast<int>(std::round(pwm / 2.55)), 0, 100);
        return true;
    }

    return false;
}

wxString clean_moonraker_print_filename(wxString name)
{
    name.Trim(true);
    name.Trim(false);
    if (name.empty() || name == "N/A")
        return wxString();

    name.Replace("\\", "/");
    if (name.StartsWith("file://"))
        name = name.Mid(7);

    const wxString lower = name.Lower();
    const int gcodes_pos = lower.Find("/gcodes/");
    if (gcodes_pos != wxNOT_FOUND)
        name = name.Mid(gcodes_pos + 8);
    else if (lower.StartsWith("gcodes/"))
        name = name.Mid(7);
    else if (wxFileName(name).IsAbsolute())
        name = wxFileName(name).GetFullName();

    while (name.StartsWith("/"))
        name = name.Mid(1);
    return name;
}

bool moonraker_json_number_value(const nlohmann::json &object, std::initializer_list<const char *> keys, double &value)
{
    if (!object.is_object())
        return false;
    for (const char *key : keys) {
        if (object.contains(key) && object[key].is_number()) {
            value = object[key].get<double>();
            return true;
        }
    }
    return false;
}

int moonraker_compute_total_estimate_seconds(int progress_percent, int estimated_total_seconds, double print_duration_seconds,
                                             const MachineObject *obj)
{
    if (estimated_total_seconds > 0)
        return estimated_total_seconds;

    const int progress = std::clamp(progress_percent, 0, 100);
    if (progress > 0 && print_duration_seconds > 0.0)
        return std::max(0, static_cast<int>(std::round(print_duration_seconds * 100.0 / progress)));

    if (obj != nullptr && obj->slice_info != nullptr && obj->slice_info->prediction > 0)
        return obj->slice_info->prediction;
    if (obj != nullptr && obj->subtask_ != nullptr && obj->subtask_->slice_info.prediction > 0)
        return obj->subtask_->slice_info.prediction;

    return -1;
}

int moonraker_compute_remaining_seconds(int progress_percent, int estimated_total_seconds, double print_duration_seconds,
                                        int current_layer, int total_layers, int mc_left_time_seconds)
{
    if (mc_left_time_seconds > 0)
        return mc_left_time_seconds;

    const int progress = std::clamp(progress_percent, 0, 100);
    if (progress >= 100)
        return -1;

    if (estimated_total_seconds > 0) {
        if (print_duration_seconds > 0.0)
            return std::max(0, estimated_total_seconds - static_cast<int>(std::round(print_duration_seconds)));
        if (progress <= 0)
            return estimated_total_seconds;
        return std::max(0, static_cast<int>(std::round(
            estimated_total_seconds * (100.0 - progress) / 100.0)));
    }

    if (progress <= 0)
        return -1;

    if (print_duration_seconds > 0.0) {
        const double estimated_total = print_duration_seconds * 100.0 / progress;
        return std::max(0, static_cast<int>(std::round(estimated_total - print_duration_seconds)));
    }

    if (current_layer > 0 && total_layers > current_layer && print_duration_seconds > 0.0) {
        return std::max(0, static_cast<int>(std::round(
            print_duration_seconds * (total_layers - current_layer) / static_cast<double>(current_layer))));
    }

    return -1;
}

wxString active_file_name_text(const MachineObject *obj);

void moonraker_finalize_print_job(DeviceDashboard::PrintJobState &job, const MachineObject *obj)
{
    if (!job.has_active_job)
        return;

    if (job.file_name.IsEmpty()) {
        const wxString fallback = clean_moonraker_print_filename(active_file_name_text(obj));
        if (!fallback.IsEmpty())
            job.file_name = fallback;
        else if (obj != nullptr && obj->slice_info != nullptr) {
            if (!obj->slice_info->gcode_name.empty())
                job.file_name = clean_moonraker_print_filename(from_u8(obj->slice_info->gcode_name));
            else if (!obj->slice_info->title.empty())
                job.file_name = clean_moonraker_print_filename(from_u8(obj->slice_info->title));
        }
    }

    if (job.current_layer <= 0 && obj != nullptr && obj->curr_layer > 0)
        job.current_layer = obj->curr_layer;
    if (job.total_layers <= 0 && obj != nullptr && obj->total_layers > 0)
        job.total_layers = obj->total_layers;

    if (job.thumbnail_url.IsEmpty() && obj != nullptr && obj->slice_info != nullptr)
        job.thumbnail_url = from_u8(obj->slice_info->thumbnail_url);
}

double rgb_distance(const wxColour &a, const wxColour &b)
{
    const double dr = static_cast<double>(a.Red()) - static_cast<double>(b.Red());
    const double dg = static_cast<double>(a.Green()) - static_cast<double>(b.Green());
    const double db = static_cast<double>(a.Blue()) - static_cast<double>(b.Blue());
    return std::sqrt(dr * dr + dg * dg + db * db);
}

std::array<int, 4> nearest_unique_tool_assignment(const std::array<wxColour, 4> &model_colors,
                                                  const std::array<wxColour, 4> &tool_colors)
{
    std::array<int, 4> tools { 1, 2, 3, 4 };
    std::array<int, 4> best = tools;
    double best_cost = std::numeric_limits<double>::infinity();
    std::sort(tools.begin(), tools.end());
    do {
        double cost = 0.0;
        for (int i = 0; i < 4; ++i)
            cost += rgb_distance(model_colors[i], tool_colors[tools[i] - 1]);
        if (cost < best_cost) {
            best_cost = cost;
            best = tools;
        }
    } while (std::next_permutation(tools.begin(), tools.end()));
    return best;
}

wxString active_file_name_text(const MachineObject *obj)
{
    if (obj == nullptr)
        return "N/A";

    if (!obj->subtask_name.empty())
        return from_u8(obj->subtask_name);

    if (!obj->m_gcode_file.empty())
        return from_u8(wxFileName(obj->m_gcode_file).GetFullName().utf8_string());

    return "N/A";
}

wxString active_file_metadata_path(const MachineObject *obj)
{
    if (obj == nullptr)
        return wxString();

    wxString path;
    if (!obj->m_gcode_file.empty())
        path = from_u8(obj->m_gcode_file);
    else if (!obj->subtask_name.empty())
        path = from_u8(obj->subtask_name);

    path.Trim(true);
    path.Trim(false);
    if (path.empty() || path == "N/A")
        return wxString();

    path.Replace("\\", "/");
    if (path.StartsWith("file://"))
        path = path.Mid(7);

    const wxString lower = path.Lower();
    int gcodes_pos = lower.Find("/gcodes/");
    if (gcodes_pos != wxNOT_FOUND)
        path = path.Mid(gcodes_pos + 8);
    else if (lower.StartsWith("gcodes/"))
        path = path.Mid(7);
    else if (wxFileName(path).IsAbsolute())
        path = wxFileName(path).GetFullName();

    while (path.StartsWith("/"))
        path = path.Mid(1);
    return path;
}

wxString remaining_minutes_text(int remaining_seconds)
{
    if (remaining_seconds < 0)
        return "N/A";
    const int minutes = (remaining_seconds + 59) / 60;
    return wxString::Format("%dm", minutes);
}
} // namespace

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
    SetBackgroundColour(wxColour("#EEEEEF"));
    m_filament_loaded_tool_materials.fill(wxString::FromUTF8("Empty"));

    auto *main_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *preview_menu_panel = new wxPanel(this, wxID_ANY);
    m_preview_menu_panel = preview_menu_panel;
    preview_menu_panel->SetBackgroundColour(wxColour(255, 255, 255));
    preview_menu_panel->SetMinSize(wxSize(FromDIP(298), FromDIP(360)));
    preview_menu_panel->SetMaxSize(wxSize(FromDIP(298), -1));
    auto *preview_menu_sizer = new wxBoxSizer(wxVERTICAL);

    {
        auto *header_panel = new wxPanel(preview_menu_panel, wxID_ANY);
        m_sidebar_header_panel = header_panel;
        header_panel->SetBackgroundColour(wxColour(255, 255, 255));
        header_panel->SetMinSize(wxSize(-1, FromDIP(50)));
        header_panel->SetMaxSize(wxSize(-1, FromDIP(50)));
        auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_sidebar_header_back = new wxStaticText(header_panel, wxID_ANY, wxString::FromUTF8("\xC3\x97"));
        m_sidebar_header_back->SetForegroundColour(wxColour("#232527"));
        m_sidebar_header_back->SetCursor(wxCursor(wxCURSOR_HAND));
        {
            wxFont f = m_sidebar_header_back->GetFont();
            f.SetPointSize(16);
            m_sidebar_header_back->SetFont(f);
        }
        m_sidebar_header_back->Hide();
        header_sizer->Add(m_sidebar_header_back, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        header_sizer->AddStretchSpacer(1);
        auto *printers_label = new wxStaticText(header_panel, wxID_ANY, "Printers");
        m_sidebar_header_title = printers_label;
        printers_label->SetForegroundColour(wxColour("#232527"));
        {
            wxFont f = printers_label->GetFont();
            f.SetPointSize(14);
            f.SetWeight(wxFONTWEIGHT_BOLD);
            printers_label->SetFont(f);
        }
        header_sizer->Add(printers_label, 0, wxALIGN_CENTER_VERTICAL);
        header_sizer->AddStretchSpacer(1);
        auto *printers_add = new wxStaticText(header_panel, wxID_ANY, "+ Add");
        m_sidebar_header_add = printers_add;
        printers_add->SetForegroundColour(wxColour(74, 149, 37));
        {
            wxFont f = printers_add->GetFont();
            f.SetPointSize(12);
            printers_add->SetFont(f);
        }
        printers_add->SetCursor(wxCursor(wxCURSOR_HAND));
        header_sizer->Add(printers_add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        header_panel->SetSizer(header_sizer);
        m_preview_printers_button = header_panel;
        printers_add->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
            evt.StopPropagation();
            show_sidebar_add_printer_view();
        });
        m_sidebar_header_back->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
            evt.StopPropagation();
            if (m_sidebar_add_printer_panel != nullptr && m_sidebar_add_printer_panel->IsShown())
                show_sidebar_printers_view();
            else
                show_sidebar_root_view();
        });
        header_panel->Hide();
        preview_menu_sizer->Add(header_panel, 0, wxEXPAND);
    }

    m_sidebar_root_panel = new wxPanel(preview_menu_panel, wxID_ANY);
    m_sidebar_root_panel->SetBackgroundColour(wxColour(255, 255, 255));
    m_sidebar_root_sizer = new wxBoxSizer(wxVERTICAL);
    m_sidebar_root_panel->SetSizer(m_sidebar_root_sizer);
    preview_menu_sizer->Add(m_sidebar_root_panel, 1, wxEXPAND);

    m_sidebar_add_printer_panel = new wxPanel(preview_menu_panel, wxID_ANY);
    m_sidebar_add_printer_panel->SetBackgroundColour(wxColour(255, 255, 255));
    m_sidebar_add_printer_panel->Hide();
    preview_menu_sizer->Add(m_sidebar_add_printer_panel, 1, wxEXPAND);

    m_sidebar_printer_list_container = new wxPanel(m_sidebar_root_panel, wxID_ANY);
    m_sidebar_printer_list_container->SetBackgroundColour(wxColour(255, 255, 255));
    auto *printer_list_row = new wxBoxSizer(wxHORIZONTAL);

    m_sidebar_printer_list_panel = new wxScrolledWindow(m_sidebar_printer_list_container, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_sidebar_printer_list_panel->SetBackgroundColour(wxColour(255, 255, 255));
    m_sidebar_printer_list_panel->SetMinSize(wxSize(-1, FromDIP(390)));
    m_sidebar_printer_list_panel->SetMaxSize(wxSize(-1, FromDIP(390)));
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel)) {
        scrolled->SetScrollRate(0, FromDIP(8));
        scrolled->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    }
    m_sidebar_printer_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_sidebar_printer_list_panel->SetSizer(m_sidebar_printer_list_sizer);
    printer_list_row->Add(m_sidebar_printer_list_panel, 1, wxEXPAND);

    m_sidebar_printer_scroll_track = new SidebarScrollbar(m_sidebar_printer_list_container);
    printer_list_row->Add(m_sidebar_printer_scroll_track, 0, wxEXPAND | wxLEFT, FromDIP(4));

    auto update_printer_scrollbar = [this]() {
        update_sidebar_scrollbar(
            dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel),
            m_sidebar_printer_scroll_track,
            this);
    };
    auto on_printer_scroll = [update_printer_scrollbar](wxScrollWinEvent &evt) {
        evt.Skip();
        update_printer_scrollbar();
    };
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel)) {
        scrolled->Bind(wxEVT_SCROLLWIN_TOP, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_BOTTOM, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_LINEUP, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_PAGEUP, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_printer_scroll);
        scrolled->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_printer_scroll);
        scrolled->Bind(wxEVT_MOUSEWHEEL, [update_printer_scrollbar](wxMouseEvent &evt) {
            evt.Skip();
            update_printer_scrollbar();
        });
        scrolled->Bind(wxEVT_SIZE, [update_printer_scrollbar](wxSizeEvent &evt) {
            evt.Skip();
            update_printer_scrollbar();
        });
    }

    m_sidebar_printer_list_container->SetSizer(printer_list_row);
    m_sidebar_printer_list_container->Hide();

    auto add_sidebar_nav_row = [this](wxWindow *parent,
                                      wxBoxSizer *parent_sizer,
                                      const wxString &label,
                                      const std::string &icon_name,
                                      const std::function<void()> &on_activate) {
        auto *row = new wxPanel(parent, wxID_ANY);
        row->SetBackgroundColour(wxColour(255, 255, 255));
        row->SetMinSize(wxSize(-1, FromDIP(37)));
        row->SetMaxSize(wxSize(-1, FromDIP(37)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *sz = new wxBoxSizer(wxHORIZONTAL);
        sz->AddSpacer(FromDIP(18));
        const bool force_white_icon =
            icon_name == "device_sidebar_timelapse_clapperboard" ||
            icon_name == "device_sidebar_print_models";
        auto *icon = new wxStaticBitmap(
            row,
            wxID_ANY,
            force_white_icon ? create_scaled_bitmap(icon_name, row, 16, false, "#232527")
                             : create_scaled_bitmap(icon_name, row, 14));
        auto *text = new wxStaticText(row, wxID_ANY, label);
        text->SetForegroundColour(wxColour("#232527"));
        auto *chev = new wxStaticText(row, wxID_ANY, wxString::FromUTF8("\xE2\x80\xBA"));
        chev->SetForegroundColour(wxColour("#B7BCC2"));
        chev->SetMinSize(wxSize(FromDIP(18), -1));
        auto chev_font = chev->GetFont();
        chev_font.SetPointSize(chev_font.GetPointSize() + 1);
        chev->SetFont(chev_font);
        if (label == _L("Printers"))
            chev->SetName("sidebar_printers_chevron");
        sz->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
        sz->AddSpacer(FromDIP(10));
        sz->Add(text, 1, wxALIGN_CENTER_VERTICAL);
        sz->Add(chev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        row->SetSizer(sz);
        auto on_click = [on_activate](wxMouseEvent &) { on_activate(); };
        row->Bind(wxEVT_LEFT_DOWN, on_click);
        icon->Bind(wxEVT_LEFT_DOWN, on_click);
        text->Bind(wxEVT_LEFT_DOWN, on_click);
        chev->Bind(wxEVT_LEFT_DOWN, on_click);
        parent_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    };

    m_sidebar_root_sizer->AddSpacer(FromDIP(10));
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Printers"), k_cprint_printer_nav_bitmap,
                        [this]() { show_sidebar_printers_view(); });
    m_sidebar_root_sizer->Add(m_sidebar_printer_list_container, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    // Explicit Device entry so users can leave System Upgrade / Media pages.
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Device"), "tab_monitor_active",
                        [this]() { select_tab(PrinterWebViewTab::Status); });
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("System Upgrade"), "monitor_upgrade_online",
                        [this]() { select_tab(PrinterWebViewTab::Update); });
    auto *sidebar_divider = new wxPanel(m_sidebar_root_panel, wxID_ANY);
    sidebar_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    sidebar_divider->SetMaxSize(wxSize(-1, FromDIP(1)));
    sidebar_divider->SetBackgroundColour(wxColour("#C7C7C7"));
    m_sidebar_root_sizer->Add(sidebar_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Timelapse"), "device_sidebar_timelapse_clapperboard",
                        [this]() { select_tab(PrinterWebViewTab::Storage); });
    add_sidebar_nav_row(m_sidebar_root_panel, m_sidebar_root_sizer, _L("Print Models"), "device_sidebar_print_models",
                        [this]() { select_tab(PrinterWebViewTab::PrintModels); });
    preview_menu_sizer->AddStretchSpacer(1);
    preview_menu_panel->SetSizer(preview_menu_sizer);

    m_printers_popup = new wxPopupTransientWindow(this, wxBORDER_NONE);
    m_printers_popup_panel = new StaticBox(m_printers_popup, wxID_ANY);
    m_printers_popup_panel->SetCornerRadius(0);
    m_printers_popup_panel->SetBorderWidth(FromDIP(2));
    m_printers_popup_panel->SetBorderColorNormal(wxColour("#C7C7C7"));
    m_printers_popup_panel->SetBackgroundColorNormal(wxColour(255, 255, 255));
    m_printers_popup_panel->SetBackgroundColour(wxColour(255, 255, 255));
    auto *popup_sizer = new wxBoxSizer(wxVERTICAL);
    popup_sizer->Add(m_printers_popup_panel, 1, wxEXPAND);
    m_printers_popup->SetSizer(popup_sizer);
    rebuild_printers_popup();
    rebuild_sidebar_printer_list();
    show_sidebar_printers_view();


    auto *content_host = new wxPanel(this, wxID_ANY);
    content_host->SetBackgroundColour(wxColour("#EEEEEF"));
    auto *content_host_sizer = new wxBoxSizer(wxVERTICAL);

    m_status_page = new wxPanel(content_host, wxID_ANY);
    m_status_page->SetBackgroundColour(wxColour("#EEEEEF"));
    auto *status_page_sizer = new wxBoxSizer(wxVERTICAL);

    m_dashboard_page = new DeviceDashboard::DeviceDashboardPage(m_status_page);
    m_dashboard_page->set_command_handler([this](const DeviceDashboard::DeviceCommand& command) {
        handle_dashboard_command(command);
    });
    m_preview_thumbnail = m_dashboard_page->thumbnail_widget();
    m_camera_webview_host = m_dashboard_page->camera_webview_host();

    m_dashboard_page->set_camera_refresh_handler([this]() {
        if (m_dashboard_page == nullptr || m_dashboard_page->camera_panel() == nullptr)
            return;
        const auto state = m_dashboard_page->camera_panel()->load_state();
        if (state == DeviceDashboard::CameraLoadState::Live ||
            state == DeviceDashboard::CameraLoadState::Initializing)
            start_camera_stream();
    });
    m_dashboard_page->set_camera_play_handler([this]() {
        if (m_dashboard_page == nullptr || m_dashboard_page->camera_panel() == nullptr)
            return;
        const auto state = m_dashboard_page->camera_panel()->load_state();
        if (state == DeviceDashboard::CameraLoadState::Live ||
            state == DeviceDashboard::CameraLoadState::Initializing)
            stop_camera_stream();
        else
            start_camera_stream();
    });
    m_dashboard_page->set_camera_timelapse_handler([this]() { toggle_camera_timelapse(); });

    if (m_camera_webview_host != nullptr) {
        m_camera_webview_host->SetMinSize(wxSize(FromDIP(420), FromDIP(236)));
        m_camera_webview_host->SetBackgroundColour(wxColour(0, 0, 0));
        if (wxWindow *const viewport = m_camera_webview_host->GetParent()) {
            viewport->Bind(wxEVT_SIZE,
                [this, last_w = -1, last_h = -1](wxSizeEvent &event) mutable {
                    event.Skip();
                    const wxSize sz = event.GetSize();
                    if (sz.x == last_w && sz.y == last_h) return;
                    last_w = sz.x; last_h = sz.y;
                    auto *win = event.GetEventObject() ? dynamic_cast<wxWindow*>(event.GetEventObject()) : nullptr;
                    if (win) win->Freeze();
                    if (m_dashboard_page != nullptr)
                        m_dashboard_page->update_camera_host_responsive_size();
                    if (win) win->Thaw();
                });
        }
    }
    {
        std::weak_ptr<int> lifetime = m_lifetime_token;
        wxGetApp().CallAfter([this, lifetime]() {
            if (lifetime.expired() || m_destroying || m_dashboard_page == nullptr)
                return;
            m_dashboard_page->update_camera_host_responsive_size();
        });
    }

    m_dashboard_page->set_printer_status_handlers(
        [this](int idx) { apply_printer_status_tool_selection(idx); },
        [this](int idx, int temp) { apply_nozzle_target_temperature(idx, temp); },
        [this](int tool, int percent) { send_toolhead_fan_speed_command(tool, percent); },
        [this](int temp) { apply_bed_target_temperature(temp); });

    status_page_sizer->Add(m_dashboard_page, 1, wxEXPAND);
    m_status_page->SetSizer(status_page_sizer);

    m_storage_placeholder = new wxPanel(content_host, wxID_ANY);
    m_storage_placeholder->SetBackgroundColour(wxColour("#EEEEEF"));
    m_storage_placeholder->Hide();
    m_update_page = create_update_page(content_host);
    m_assistant_page = create_placeholder_page(content_host, "Asistan", "Asistan paneli icin gecici yer tutucu.");

    content_host_sizer->Add(m_status_page, 1, wxEXPAND);
    content_host_sizer->Add(m_storage_placeholder, 1, wxEXPAND);
    content_host_sizer->Add(m_update_page, 1, wxEXPAND);
    content_host_sizer->Add(m_assistant_page, 1, wxEXPAND);
    content_host->SetSizer(content_host_sizer);

    main_sizer->Add(preview_menu_panel, 0, wxEXPAND | wxTOP, FromDIP(5));
    main_sizer->AddSpacer(FromDIP(20));
    main_sizer->Add(content_host, 1, wxEXPAND);
    SetSizer(main_sizer);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        event.Skip();
        Layout();
        if (wxWindow *parent = GetParent())
            parent->Layout();
    });
    select_tab(PrinterWebViewTab::Status);

    m_browser = nullptr;
    m_zoomFactor = 100;
    Bind(wxEVT_WEBREQUEST_STATE, &PrinterWebView::on_thumbnail_webrequest_state, this);
    m_layer_refresh_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_layer_info_from_selected_machine(); }, m_layer_refresh_timer->GetId());
    m_layer_refresh_timer->Start(1000);
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
    Bind(wxEVT_SHOW, [this](wxShowEvent &ev) {
        ev.Skip();
    });
 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    m_destroying = true;
    if (m_lan_scan_cancel_token)
        m_lan_scan_cancel_token->store(true);
    m_lifetime_token.reset();
    dismiss_printers_popup();
    if (m_thumbnail_web_request.IsOk())
        m_thumbnail_web_request.Cancel();
    if (m_layer_refresh_timer != nullptr) {
        m_layer_refresh_timer->Stop();
        delete m_layer_refresh_timer;
        m_layer_refresh_timer = nullptr;
    }
    if (m_update_progress_timer != nullptr) {
        m_update_progress_timer->Stop();
        delete m_update_progress_timer;
        m_update_progress_timer = nullptr;
    }
    SetEvtHandlerEnabled(false);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    // Legacy Orca monitor surface. CoPrint uses the native dashboard below; keep
    // this stub so upstream callers can pass credentials without recreating WebView.
    (void) url;
    m_apikey = apikey;
    return;
}

bool PrinterWebView::Show(bool show)
{
    if (show && !m_destroying)
        refresh_layer_info_from_selected_machine();
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    // Native CoPrint dashboard does not have a browser page to reload.
    return;
}

void PrinterWebView::update_mode()
{
    refresh_layer_info_from_selected_machine();
    return;
}

void PrinterWebView::toggle_printers_popup_at(wxWindow* anchor)
{
    if (m_printers_popup == nullptr || anchor == nullptr)
        return;

    int width = anchor->GetSize().GetWidth();
    if (width < 1 && anchor->GetParent() != nullptr)
        width = anchor->GetParent()->GetSize().GetWidth();
    m_printers_popup_max_width = (std::max)(1, width);

    rebuild_printers_popup();

    if (m_printers_popup->IsShown()) {
        m_printers_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().GetHeight()));
    m_printers_popup->Position(screen_pos, wxSize(0, 0));
    m_printers_popup->Popup(anchor);
}

void PrinterWebView::set_embedded_in_monitor(bool embedded)
{
    m_embedded_in_monitor = embedded;
    set_sidebar_visible(!embedded);
}

void PrinterWebView::set_sidebar_visible(bool visible)
{
    if (m_preview_menu_panel != nullptr)
        m_preview_menu_panel->Show(visible);
    Layout();
}

void PrinterWebView::ensure_coprint_storage_page()
{
    ensure_storage_page_created();
}

void PrinterWebView::set_coprint_storage_mode(bool print_models)
{
    ensure_storage_page_created();
    if (m_storage_page != nullptr) {
        m_storage_page->set_media_presentation(
            print_models ? CloudTaskManagerPage::MediaPresentation::ModelOnly
                         : CloudTaskManagerPage::MediaPresentation::TimelapseOnly);
        m_storage_page->refresh_user_device();
        m_storage_page->update_page();
    }
}

void PrinterWebView::toggle_printers_popup()
{
    if (m_printers_popup == nullptr || m_preview_printers_button == nullptr)
        return;

    const int width = m_preview_printers_button->GetSize().GetWidth();
    if (width > 0)
        m_printers_popup_max_width = width;

    rebuild_printers_popup();

    if (m_printers_popup->IsShown()) {
        m_printers_popup->Dismiss();
        return;
    }

    const wxPoint screen_pos = m_preview_printers_button->ClientToScreen(wxPoint(0, m_preview_printers_button->GetSize().GetHeight()));
    m_printers_popup->Position(screen_pos, wxSize(0, 0));
    m_printers_popup->Popup(m_preview_printers_button);
}

void PrinterWebView::dismiss_printers_popup()
{
    if (m_printers_popup != nullptr && m_printers_popup->IsShown())
        m_printers_popup->Dismiss();
}

void PrinterWebView::reset_placeholder_selections()
{
    m_selected_extruder_index = 0;
    m_selected_filament_tool = 0;
    apply_filament_tool_selection(0);
    Layout();
}

bool PrinterWebView::finish_add_moonraker_printer(const BBLocalMachine &machine, bool use_ssl, bool run_probe)
{
    BBLocalMachine machine_to_add = machine;

    // Always sanitize identity before insert/connect — broken IPs like
    // "192.168.1..150" make connection hang for a long time.
    {
        std::string sanitized;
        std::string sanitize_error;
        const std::string raw = !machine_to_add.dev_ip.empty() ? machine_to_add.dev_ip : machine_to_add.dev_id;
        if (!sanitize_moonraker_address(raw, sanitized, sanitize_error)) {
            wxMessageBox(from_u8(sanitize_error), _L("IP Connect"), wxOK | wxICON_ERROR, this);
            return false;
        }
        machine_to_add.dev_id = sanitized;
        machine_to_add.dev_ip = sanitized;
    }

    if (run_probe) {
        BBLocalMachine detected_machine;
        if (probe_moonraker_host(machine_to_add.dev_ip, detected_machine)) {
            // Prefer probed hostname over the IP seed; never keep a generic product label.
            if (!detected_machine.dev_name.empty() &&
                !is_generic_coprint_product_name(detected_machine.dev_name))
                machine_to_add.dev_name = detected_machine.dev_name;
            if (!detected_machine.printer_type.empty())
                machine_to_add.printer_type = detected_machine.printer_type;
            if (!detected_machine.dev_id.empty()) {
                std::string sanitized_id;
                std::string unused;
                if (sanitize_moonraker_address(detected_machine.dev_id, sanitized_id, unused))
                    machine_to_add.dev_id = sanitized_id;
            }
            if (!detected_machine.dev_ip.empty()) {
                std::string sanitized_ip;
                std::string unused;
                if (sanitize_moonraker_address(detected_machine.dev_ip, sanitized_ip, unused))
                    machine_to_add.dev_ip = sanitized_ip;
            }
        }
    }

    // Final hostname / type fallback if probe only returned a Moonraker placeholder.
    if (machine_to_add.printer_type == "Moonraker" || is_placeholder_printer_name(machine_to_add.dev_name)) {
        std::string mapped_name;
        std::string mapped_type = machine_to_add.printer_type;
        if (apply_coprint_name_from_hostname(machine_to_add.dev_name, mapped_name, mapped_type))
            machine_to_add.printer_type = mapped_type;
    }

    auto *dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager == nullptr) {
        wxMessageBox("Device manager hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    auto *agent = wxGetApp().getAgent();
    if (agent == nullptr) {
        wxMessageBox("Network agent hazir degil.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    const auto current_printer_agent = agent->get_printer_agent();
    const bool needs_moonraker_agent = current_printer_agent == nullptr ||
        current_printer_agent->get_agent_info().id != "moonraker";
    if (needs_moonraker_agent) {
        auto moonraker_agent = NetworkAgentFactory::create_printer_agent_by_id(
            "moonraker", agent->get_cloud_agent(), Slic3r::data_dir());
        if (moonraker_agent == nullptr) {
            wxMessageBox("Moonraker network agent baslatilamadi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
            return false;
        }
        agent->set_printer_agent(moonraker_agent);
    }

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);
        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);
        if (std::count(value.begin(), value.end(), ':') == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }
        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    if (wxGetApp().app_config != nullptr) {
        auto clear_forgotten = [&](const std::string &key) {
            if (!key.empty())
                wxGetApp().app_config->erase("forgotten_lan_machines", key);
        };
        const std::string host = normalize_host(!machine_to_add.dev_ip.empty() ? machine_to_add.dev_ip : machine_to_add.dev_id);
        clear_forgotten(host);
        clear_forgotten(machine_to_add.dev_id);
        clear_forgotten(machine_to_add.dev_ip);
        clear_forgotten(normalize_host(machine_to_add.dev_id));
        clear_forgotten(normalize_host(machine_to_add.dev_ip));
    }

    MachineObject *obj = dev_manager->insert_local_device(machine_to_add, "lan", "free", "", "", true);
    if (obj == nullptr) {
        wxMessageBox("Yazici yerel cihaz listesine eklenemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }

    // Ensure runtime object carries the probed display name (insert may reuse an old object).
    if (!machine_to_add.dev_name.empty())
        obj->set_dev_name(machine_to_add.dev_name);
    if (!machine_to_add.printer_type.empty())
        obj->printer_type = machine_to_add.printer_type;
    DeviceManager::update_local_machine(*obj);

    obj->local_use_ssl = use_ssl;
    if (!dev_manager->set_selected_machine(machine_to_add.dev_id)) {
        wxMessageBox("Yazici secilemedi.", "IP Adresi ile Baglan", wxOK | wxICON_ERROR, this);
        return false;
    }
    m_has_active_printer_connection = true;
    obj->command_request_push_all(true);

    dismiss_printers_popup();
    rebuild_printers_popup();
    refresh_layer_info_from_selected_machine();
    Layout();
    return true;
}

void PrinterWebView::show_printer_card_actions_menu(wxWindow *anchor, MachineObject *machine)
{
    if (anchor == nullptr || machine == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
    const bool can_remove = local_machines.find(machine->get_dev_id()) != local_machines.end();

    wxMenu menu;
    menu.Append(1, _L("Edit printer"));
    if (can_remove) {
        auto *forget_item = new wxMenuItem(&menu, 2, _L("Forget printer"));
        forget_item->SetBitmap(create_scaled_bitmap("device_sidebar_bin", this, 16));
        menu.Append(forget_item);
    }

    const wxPoint screen_pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().GetHeight()));
    const int sel = GetPopupMenuSelectionFromUser(menu, screen_pos);
    if (sel == 1) {
        if (edit_sidebar_printer_name(machine)) {
            rebuild_printers_popup();
            rebuild_sidebar_printer_list();
            refresh_layer_info_from_selected_machine();
            Layout();
        }
    } else if (sel == 2 && can_remove) {
        if (confirm_forget_printer())
            forget_local_printer(machine);
    }
}

bool PrinterWebView::edit_sidebar_printer_name(MachineObject *machine)
{
    if (machine == nullptr)
        return false;

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(*wxWHITE);

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *frame = new StaticBox(&dlg, wxID_ANY);
    frame->SetCornerRadius(FromDIP(12));
    frame->SetBorderWidth(1);
    frame->SetBorderColorNormal(wxColour("#C7C7C7"));
    frame->SetBackgroundColorNormal(*wxWHITE);
    frame->SetBackgroundColour(*wxWHITE);

    auto *content = new wxBoxSizer(wxVERTICAL);
    auto *title = new wxStaticText(frame, wxID_ANY, _L("Edit printer name"));
    title->SetForegroundColour(wxColour("#232527"));
    wxFont tf = title->GetFont();
    tf.SetPointSize(11);
    tf.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(tf);
    content->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto *hint = new wxStaticText(frame, wxID_ANY, _L("Printer display name"));
    hint->SetForegroundColour(wxColour("#767C84"));
    content->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto *name_input = new ::TextInput(frame, from_u8(machine->get_dev_name()), "", "", wxDefaultPosition,
                                       wxSize(FromDIP(280), FromDIP(36)), wxTE_PROCESS_ENTER);
    name_input->SetBackgroundColour(*wxWHITE);
    content->Add(name_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *cancel = new Button(frame, _L("Cancel"));
    cancel->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    auto *save = new Button(frame, _L("Save"));
    save->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    save->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    buttons->AddStretchSpacer(1);
    buttons->Add(cancel, 0);
    buttons->AddSpacer(FromDIP(10));
    buttons->Add(save, 0);
    content->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(20));

    frame->SetSizer(content);
    root->Add(frame, 1, wxEXPAND | wxALL, FromDIP(1));
    dlg.SetSizer(root);
    dlg.Fit();
    dlg.CentreOnParent();

    bool accepted = false;
    auto accept = [&]() {
        wxTextCtrl *ctrl = name_input->GetTextCtrl();
        if (ctrl == nullptr)
            return;
        wxString v = ctrl->GetValue();
        v.Trim(true);
        v.Trim(false);
        if (v.empty())
            return;
        const std::string new_name = into_u8(v);
        auto *dev_manager = wxGetApp().getDeviceManager();
        if (machine->is_lan_mode_printer()) {
            machine->set_dev_name(new_name);
            DeviceManager::update_local_machine(*machine);
        } else if (dev_manager != nullptr) {
            dev_manager->modify_device_name(machine->get_dev_id(), new_name);
            machine->set_dev_name(new_name);
        } else {
            machine->set_dev_name(new_name);
        }
        accepted = true;
        dlg.EndModal(wxID_OK);
    };

    cancel->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    save->Bind(wxEVT_BUTTON, [accept](wxCommandEvent &) { accept(); });
    if (wxTextCtrl *ctrl = name_input->GetTextCtrl()) {
        ctrl->Bind(wxEVT_TEXT_ENTER, [accept](wxCommandEvent &) { accept(); });
        ctrl->SetFocus();
        ctrl->SelectAll();
    }

    dlg.ShowModal();
    return accepted;
}

bool PrinterWebView::confirm_forget_printer()
{
    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(*wxWHITE);

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *frame = new StaticBox(&dlg, wxID_ANY);
    frame->SetCornerRadius(FromDIP(12));
    frame->SetBorderWidth(1);
    frame->SetBorderColorNormal(wxColour("#C7C7C7"));
    frame->SetBackgroundColorNormal(*wxWHITE);
    frame->SetBackgroundColour(*wxWHITE);

    auto *content = new wxBoxSizer(wxVERTICAL);
    auto *title = new wxStaticText(frame, wxID_ANY, _L("Are you sure to delete this printer?"));
    title->SetForegroundColour(wxColour("#232527"));
    wxFont tf = title->GetFont();
    tf.SetPointSize(11);
    tf.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(tf);
    content->Add(title, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, FromDIP(20));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *cancel = new Button(frame, _L("Cancel"));
    cancel->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    auto *forget = new Button(frame, _L("Forget"));
    forget->SetMinSize(wxSize(FromDIP(88), FromDIP(34)));
    forget->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    buttons->Add(cancel, 0);
    buttons->AddSpacer(FromDIP(10));
    buttons->Add(forget, 0);
    content->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(20));

    frame->SetSizer(content);
    root->Add(frame, 1, wxEXPAND);
    dlg.SetSizerAndFit(root);
    dlg.CentreOnParent();

    cancel->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    forget->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_OK); });
    return dlg.ShowModal() == wxID_OK;
}

void PrinterWebView::forget_local_printer(MachineObject *machine)
{
    if (machine == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    const std::string dev_id = machine->get_dev_id();
    const std::string dev_ip = machine->get_dev_ip();

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };

        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);

        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);

        const auto question_pos = value.find('?');
        if (question_pos != std::string::npos)
            value = value.substr(0, question_pos);

        const auto at_pos = value.find('@');
        if (at_pos != std::string::npos)
            value = value.substr(at_pos + 1);

        const auto colon_count = std::count(value.begin(), value.end(), ':');
        if (colon_count == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }

        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    auto same_printer = [&](const std::string &key, const std::string &entry_dev_id, const std::string &entry_dev_ip) {
        if ((!dev_id.empty() && (key == dev_id || entry_dev_id == dev_id || entry_dev_ip == dev_id)) ||
            (!dev_ip.empty() && (key == dev_ip || entry_dev_id == dev_ip || entry_dev_ip == dev_ip)))
            return true;

        const std::string target_host = !dev_ip.empty() ? normalize_host(dev_ip) : normalize_host(dev_id);
        if (target_host.empty())
            return false;

        return normalize_host(key) == target_host ||
               normalize_host(entry_dev_id) == target_host ||
               normalize_host(entry_dev_ip) == target_host;
    };

    MachineObject *selected_machine = dev_manager != nullptr ? dev_manager->get_selected_machine() : nullptr;
    if (selected_machine != nullptr &&
        same_printer(selected_machine->get_dev_id(), selected_machine->get_dev_id(), selected_machine->get_dev_ip())) {
        selected_machine->disconnect();
        selected_machine->set_online_state(false);
        selected_machine->reset();
        m_has_active_printer_connection = false;
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine("");
    }

    std::vector<std::string> keys_to_erase;
    auto add_key = [&keys_to_erase](const std::string &key) {
        if (std::find(keys_to_erase.begin(), keys_to_erase.end(), key) == keys_to_erase.end())
            keys_to_erase.push_back(key);
    };
    // Always try to drop the legacy empty-key corrupt entry.
    add_key(std::string());
    add_key(dev_id);
    add_key(dev_ip);
    const std::string forgotten_name = machine->get_dev_name();

    if (wxGetApp().app_config != nullptr) {
        const std::string forgotten_host = normalize_host(!dev_ip.empty() ? dev_ip : dev_id);
        auto mark_forgotten = [&](const std::string &key) {
            if (!key.empty())
                wxGetApp().app_config->set("forgotten_lan_machines", key, std::string("1"));
        };
        mark_forgotten(forgotten_host);
        mark_forgotten(dev_id);
        mark_forgotten(dev_ip);
        mark_forgotten(normalize_host(dev_id));
        mark_forgotten(normalize_host(dev_ip));
        const auto saved_machines = wxGetApp().app_config->get_local_machines();
        for (const auto &entry : saved_machines) {
            const BBLocalMachine &local = entry.second;
            if (same_printer(entry.first, local.dev_id, local.dev_ip) ||
                entry.first.empty() ||
                (local.dev_id.empty() && local.dev_ip.empty()) ||
                (!forgotten_name.empty() && local.dev_name == forgotten_name &&
                 (local.dev_id.empty() || local.dev_ip.empty()))) {
                add_key(entry.first);
                add_key(local.dev_id);
                add_key(local.dev_ip);
            }
        }
    }

    std::vector<MachineObject *> objects_to_delete;
    if (dev_manager != nullptr) {
        const auto local_machines = dev_manager->get_local_machinelist();
        for (const auto &entry : local_machines) {
            MachineObject *local = entry.second;
            if (local == nullptr)
                continue;
            if (same_printer(entry.first, local->get_dev_id(), local->get_dev_ip()) ||
                entry.first.empty() ||
                (local->get_dev_id().empty() && local->get_dev_ip().empty()) ||
                local == machine ||
                (!forgotten_name.empty() && local->get_dev_name() == forgotten_name &&
                 (local->get_dev_id().empty() || local->get_dev_ip().empty()))) {
                add_key(entry.first);
                add_key(local->get_dev_id());
                add_key(local->get_dev_ip());
                if (std::find(objects_to_delete.begin(), objects_to_delete.end(), local) == objects_to_delete.end())
                    objects_to_delete.push_back(local);
            }
        }
    }

    for (const std::string &key : keys_to_erase) {
        if (wxGetApp().app_config != nullptr)
            wxGetApp().app_config->erase_local_machine(key);
        if (dev_manager != nullptr)
            dev_manager->erase_local_machine(key);
    }

    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save();

    for (MachineObject *obj : objects_to_delete) {
        if (obj != nullptr) {
            obj->disconnect();
            delete obj;
        }
    }

    // Destroying sidebar cards while the trash button is still handling the click
    // can abort the refresh; rebuild after the event unwinds.
    m_sidebar_printer_list_signature.clear();
    std::weak_ptr<int> lifetime = m_lifetime_token;
    wxGetApp().CallAfter([this, lifetime]() {
        if (lifetime.expired() || m_destroying)
            return;
        rebuild_printers_popup();
        rebuild_sidebar_printer_list();
        refresh_layer_info_from_selected_machine();
        Layout();
    });
}

void PrinterWebView::show_add_printer_dialog()
{
    dismiss_printers_popup();

    struct AddPrinterDialog : public wxDialog
    {
        void apply_tab_selection(int idx)
        {
            m_tab_index = idx;
            const wxColour k_accent(40, 167, 69);
            for (int i = 0; i < 2; ++i) {
                if (m_tab_lbl[i] == nullptr || m_tab_under[i] == nullptr)
                    continue;
                const bool on = (i == idx);
                m_tab_lbl[i]->SetForegroundColour(on ? k_accent : wxColour(72, 72, 78));
                wxFont f = m_tab_lbl[i]->GetFont();
                f.SetWeight(on ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
                m_tab_lbl[i]->SetFont(f);
                m_tab_under[i]->SetBackgroundColour(on ? k_accent : wxColour(255, 255, 255));
            }
            if (m_book != nullptr)
                m_book->ChangeSelection(static_cast<size_t>(idx));
            Refresh();
        }

        explicit AddPrinterDialog(wxWindow *parent, PrinterWebView *owner)
            : wxDialog(parent, wxID_ANY, _L("Add Printer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
            , m_owner(owner)
        {
            SetBackgroundColour(wxColour(255, 255, 255));
            const wxColour k_accent(40, 167, 69);
            auto *root = new wxBoxSizer(wxVERTICAL);

            m_tab_area = new wxPanel(this, wxID_ANY);
            m_tab_area->SetBackgroundColour(wxColour(255, 255, 255));
            auto *tab_hs = new wxBoxSizer(wxHORIZONTAL);
            const wxString tab_titles[2] = { _L("Auto Connect"), _L("IP Connect") };
            for (int i = 0; i < 2; ++i) {
                auto *cell = new wxPanel(m_tab_area, wxID_ANY);
                cell->SetBackgroundColour(wxColour(255, 255, 255));
                auto *vs = new wxBoxSizer(wxVERTICAL);
                m_tab_lbl[i] = new wxStaticText(cell, wxID_ANY, tab_titles[i]);
                m_tab_lbl[i]->SetCursor(wxCursor(wxCURSOR_HAND));
                vs->Add(m_tab_lbl[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
                m_tab_under[i] = new wxPanel(cell, wxID_ANY);
                m_tab_under[i]->SetMinSize(wxSize(-1, FromDIP(2)));
                m_tab_under[i]->SetMaxSize(wxSize(-1, FromDIP(2)));
                vs->Add(m_tab_under[i], 0, wxEXPAND | wxTOP, FromDIP(6));
                cell->SetSizer(vs);
                const int idx = i;
                auto pick_tab = [this, idx](wxMouseEvent &) { apply_tab_selection(idx); };
                cell->Bind(wxEVT_LEFT_DOWN, pick_tab);
                m_tab_lbl[i]->Bind(wxEVT_LEFT_DOWN, pick_tab);
                tab_hs->Add(cell, 1, wxEXPAND);
            }
            m_tab_area->SetSizer(tab_hs);
            root->Add(m_tab_area, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

            m_book = new wxSimplebook(this, wxID_ANY);
            m_book->SetBackgroundColour(wxColour(255, 255, 255));

            m_auto_page = new wxPanel(m_book, wxID_ANY);
            m_auto_page->SetBackgroundColour(wxColour(255, 255, 255));
            auto *auto_sz = new wxBoxSizer(wxVERTICAL);
            auto *search_row = new wxBoxSizer(wxHORIZONTAL);
            m_auto_status = new wxStaticText(m_auto_page, wxID_ANY, _L("Searching for printers on your network..."));
            search_row->Add(m_auto_status, 1, wxALIGN_CENTER_VERTICAL);
            auto *refresh_btn = new wxButton(m_auto_page, wxID_ANY, _L("Refresh"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            search_row->Add(refresh_btn, 0, wxALIGN_CENTER_VERTICAL);
            auto_sz->Add(search_row, 0, wxEXPAND | wxALL, FromDIP(10));
            m_auto_list = new wxScrolledWindow(m_auto_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
            m_auto_list->SetScrollRate(FromDIP(8), FromDIP(8));
            m_auto_list->SetBackgroundColour(wxColour(255, 255, 255));
            m_auto_list_sizer = new wxBoxSizer(wxVERTICAL);
            m_auto_list->SetSizer(m_auto_list_sizer);
            auto_sz->Add(m_auto_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
            auto_sz->Add(build_hint_panel(m_auto_page, _L("Make sure your printer is powered on and connected to the same network."), false, true), 0,
                wxEXPAND | wxALL, FromDIP(10));
            auto_sz->Add(build_hint_panel(m_auto_page, _L("Can't find your printer? Try IP Connect."), true, false), 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
            m_auto_page->SetSizer(auto_sz);

            m_ip_page = new wxPanel(m_book, wxID_ANY);
            m_ip_page->SetBackgroundColour(wxColour(255, 255, 255));
            auto *ip_sz = new wxBoxSizer(wxVERTICAL);
            ip_sz->Add(new wxStaticText(m_ip_page, wxID_ANY, _L("Enter your printer's IP address")), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

            auto *ip_shell = new StaticBox(m_ip_page, wxID_ANY);
            ip_shell->SetCornerRadius(static_cast<double>(FromDIP(10)));
            ip_shell->SetBorderWidth(FromDIP(1));
            ip_shell->SetBorderColorNormal(wxColour(210, 210, 215));
            ip_shell->SetBackgroundColorNormal(wxColour(255, 255, 255));
            ip_shell->SetBackgroundColour(wxColour(255, 255, 255));
            auto *ip_inner = new wxBoxSizer(wxHORIZONTAL);
            m_ip_field = new wxTextCtrl(ip_shell, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            m_ip_field->SetBackgroundColour(wxColour(255, 255, 255));
            m_ip_field->SetHint(_L("Type IP address..."));
            ip_inner->Add(m_ip_field, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));
            auto *ip_add = new wxButton(ip_shell, wxID_ANY, _L("Add"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            ip_add->SetBackgroundColour(wxColour(232, 246, 238));
            ip_add->SetForegroundColour(k_accent);
            ip_inner->Add(ip_add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            ip_shell->SetSizer(ip_inner);
            ip_sz->Add(ip_shell, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

            auto *ip_stat_row = new wxBoxSizer(wxHORIZONTAL);
            m_ip_status = new wxStaticText(m_ip_page, wxID_ANY, wxString());
            m_ip_status->SetForegroundColour(wxColour(72, 72, 76));
            ip_stat_row->Add(m_ip_status, 0, wxALIGN_CENTER_VERTICAL);
            ip_stat_row->AddStretchSpacer(1);
            auto *ip_refresh = new wxButton(m_ip_page, wxID_ANY, wxString::FromUTF8("\xE2\x86\xBB"), wxDefaultPosition, wxSize(FromDIP(32), FromDIP(28)), wxBU_EXACTFIT);
            ip_refresh->SetToolTip(_L("Retry"));
            ip_stat_row->Add(ip_refresh, 0, wxALIGN_CENTER_VERTICAL);
            ip_sz->Add(ip_stat_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
            ip_sz->Add(build_hint_panel(m_ip_page, _L("Make sure your printer is powered on and connected to the same network."), false, true), 0,
                wxEXPAND | wxALL, FromDIP(10));
            m_ip_page->SetSizer(ip_sz);

            m_book->AddPage(m_auto_page, wxString(), false);
            m_book->AddPage(m_ip_page, wxString(), false);
            root->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

            SetSizer(root);

            refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { rebuild_auto_list(); });
            ip_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { try_ip_add(); });
            m_ip_field->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { try_ip_add(); });
            ip_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { try_ip_add(); });
            SetSize(wxSize(FromDIP(380), FromDIP(420)));
            rebuild_auto_list();
            apply_tab_selection(1);
        }

        wxPanel *build_hint_panel(wxWindow *parent, const wxString &text, bool green_bg, bool with_info_icon = false)
        {
            auto *p = new wxPanel(parent, wxID_ANY);
            p->SetBackgroundColour(green_bg ? wxColour(220, 248, 230) : wxColour(240, 240, 242));
            auto *s = new wxBoxSizer(wxHORIZONTAL);
            s->AddSpacer(FromDIP(8));
            if (with_info_icon) {
                wxBitmap tip = wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_CMN_DIALOG, wxSize(FromDIP(18), FromDIP(18)));
                if (tip.IsOk())
                    s->Add(new wxStaticBitmap(p, wxID_ANY, tip), 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(8));
                else
                    s->AddSpacer(FromDIP(18));
                s->AddSpacer(FromDIP(6));
            }
            auto *t = new wxStaticText(p, wxID_ANY, text);
            t->SetForegroundColour(wxColour(55, 55, 58));
            t->Wrap(FromDIP(300));
            s->Add(t, 1, wxALL, FromDIP(10));
            p->SetSizer(s);
            return p;
        }

        void rebuild_auto_list()
        {
            if (m_auto_list_sizer == nullptr || m_owner == nullptr)
                return;
            m_auto_list_sizer->Clear(true);
            auto *dev_manager = wxGetApp().getDeviceManager();
            if (dev_manager != nullptr)
                dev_manager->start_refresher();

            const auto locals = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
            const wxColour green(40, 167, 69);
            for (const auto &it : locals) {
                MachineObject *obj = it.second;
                if (obj == nullptr)
                    continue;
                auto *row = new wxPanel(m_auto_list, wxID_ANY);
                row->SetBackgroundColour(wxColour(255, 255, 255));
                row->SetCursor(wxCursor(wxCURSOR_HAND));
                auto *hs = new wxBoxSizer(wxHORIZONTAL);
                hs->AddSpacer(FromDIP(8));
                hs->Add(new wxStaticBitmap(row, wxID_ANY, create_scaled_bitmap(k_cprint_printer_nav_bitmap, m_owner, 20)), 0, wxALIGN_CENTER_VERTICAL);
                hs->AddSpacer(FromDIP(8));
                auto *vs = new wxBoxSizer(wxVERTICAL);
                auto *name = new wxStaticText(row, wxID_ANY, from_u8(obj->get_dev_name()));
                wxFont nf = name->GetFont();
                nf.SetWeight(wxFONTWEIGHT_BOLD);
                name->SetFont(nf);
                vs->Add(name, 0);
                wxString ip = from_u8(obj->get_dev_ip());
                if (!ip.empty())
                    vs->Add(new wxStaticText(row, wxID_ANY, ip), 0);
                hs->Add(vs, 1, wxALIGN_CENTER_VERTICAL);
                auto *add_b = new wxButton(row, wxID_ANY, _L("Add"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                add_b->SetForegroundColour(green);
                hs->Add(add_b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
                row->SetSizer(hs);
                const std::string dev_id = obj->get_dev_id();
                const std::string dev_ip = obj->get_dev_ip();
                const std::string dev_name = obj->get_dev_name();
                const std::string printer_type = obj->printer_type.empty() ? std::string("Moonraker") : obj->printer_type;
                const bool local_use_ssl = obj->local_use_ssl;
                add_b->Bind(wxEVT_BUTTON, [this, dev_id, dev_ip, dev_name, printer_type, local_use_ssl](wxCommandEvent &) {
                    BBLocalMachine machine;
                    machine.dev_id = dev_id;
                    machine.dev_ip = dev_ip;
                    machine.dev_name = dev_name;
                    machine.printer_type = printer_type;
                    if (m_owner != nullptr && m_owner->finish_add_moonraker_printer(machine, local_use_ssl))
                        EndModal(wxID_OK);
                });
                m_auto_list_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            }
            if (locals.empty()) {
                auto *empty = new wxStaticText(m_auto_list, wxID_ANY, _L("No printers discovered yet. Tap Refresh."));
                m_auto_list_sizer->Add(empty, 0, wxALL, FromDIP(8));
            }
            m_auto_list->FitInside();
            m_auto_list->Layout();
        }

        void try_ip_add()
        {
            if (m_ip_status != nullptr)
                m_ip_status->SetLabelText(wxString());
            wxString ip_value = m_ip_field->GetValue();
            ip_value.Trim(true);
            ip_value.Trim(false);
            if (ip_value.empty()) {
                wxMessageBox(_L("IP address cannot be empty."), _L("IP Connect"), wxOK | wxICON_WARNING, this);
                return;
            }

            std::string host = into_u8(ip_value);
            const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
            const std::string normalized_host = MachineObject::dev_id_from_address(host);
            std::string dev_ip = normalized_host;
            if (!has_scheme && normalized_host.find(':') == std::string::npos)
                dev_ip += ":7125";
            const std::string dev_id = dev_ip;

            BBLocalMachine machine;
            machine.dev_id = dev_id;
            machine.dev_ip = dev_ip;
            machine.dev_name = friendly_host_from_address(dev_ip);
            machine.printer_type = "Moonraker";

            if (m_ip_status != nullptr)
                m_ip_status->SetLabelText(_L("Connecting to printer..."));
            if (m_owner != nullptr && m_owner->finish_add_moonraker_printer(machine, host.rfind("https://", 0) == 0))
                EndModal(wxID_OK);
        }

        PrinterWebView *m_owner{ nullptr };
        wxSimplebook *m_book{ nullptr };
        wxPanel *m_tab_area{ nullptr };
        wxStaticText *m_tab_lbl[2]{ nullptr, nullptr };
        wxPanel *m_tab_under[2]{ nullptr, nullptr };
        int m_tab_index{ 0 };
        wxPanel *m_auto_page{ nullptr };
        wxPanel *m_ip_page{ nullptr };
        wxStaticText *m_auto_status{ nullptr };
        wxScrolledWindow *m_auto_list{ nullptr };
        wxBoxSizer *m_auto_list_sizer{ nullptr };
        wxTextCtrl *m_ip_field{ nullptr };
        wxStaticText *m_ip_status{ nullptr };
    };

    AddPrinterDialog dlg(this, this);
    dlg.ShowModal();
}

void PrinterWebView::prompt_ip_connect()
{
    wxTextEntryDialog ip_dialog(this, "Yazicinin IP adresini veya Moonraker adresini girin.", "IP Adresi ile Baglan");
    if (ip_dialog.ShowModal() != wxID_OK)
        return;

    wxString ip_value = ip_dialog.GetValue();
    ip_value.Trim(true);
    ip_value.Trim(false);
    if (ip_value.empty()) {
        wxMessageBox("IP adresi bos olamaz.", "IP Adresi ile Baglan", wxOK | wxICON_WARNING, this);
        return;
    }

    std::string host = into_u8(ip_value);
    const bool has_scheme = host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0;
    const std::string normalized_host = MachineObject::dev_id_from_address(host);
    std::string dev_ip = normalized_host;
    if (!has_scheme && normalized_host.find(':') == std::string::npos)
        dev_ip += ":7125";
    const std::string dev_id = dev_ip;

    BBLocalMachine machine;
    machine.dev_id = dev_id;
    machine.dev_ip = dev_ip;
    machine.dev_name = friendly_host_from_address(dev_ip);
    machine.printer_type = "Moonraker";

    finish_add_moonraker_printer(machine, host.rfind("https://", 0) == 0);
}

void PrinterWebView::rebuild_printers_popup()
{
    if (m_printers_popup == nullptr || m_printers_popup_panel == nullptr)
        return;

    if (auto *old_sizer = m_printers_popup_panel->GetSizer()) {
        m_printers_popup_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_printers_popup_panel->DestroyChildren();

    m_printers_popup_panel->SetBackgroundColour(wxColour(255, 255, 255));
    const int popup_width = m_printers_popup_max_width > 0 ? m_printers_popup_max_width : FromDIP(220);
    m_printers_popup_panel->SetMinSize(wxSize(popup_width, -1));
    m_printers_popup_panel->SetMaxSize(wxSize(popup_width, -1));

    auto *printers_popup_sizer = new wxBoxSizer(wxVERTICAL);
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *selected_machine = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const auto my_machines = dev_manager ? dev_manager->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();

    const wxColour k_green(40, 167, 69);
    const wxColour k_yellow(wxColour("#F2C94C"));
    const wxColour k_red(wxColour("#E74C3C"));
    const wxColour k_muted(120, 120, 120);
    const wxColour k_card_border(232, 232, 232);

    printers_popup_sizer->AddSpacer(FromDIP(10));

    auto select_machine_fn = [this, dev_manager](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const std::string dev_id = machine->get_dev_id();
        MachineObject *current = dev_manager != nullptr ? dev_manager->get_selected_machine() : nullptr;
        const bool already_selected = current != nullptr && current->get_dev_id() == dev_id;
        dismiss_printers_popup();
        if (already_selected && machine->is_online()) {
            m_has_active_printer_connection = true;
            refresh_layer_info_from_selected_machine();
            return;
        }
        // Apply selection synchronously so Status reads the same machine the
        // picker just chose. MonitorPanel::select_machine only queues an event.
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine(dev_id);
        if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
            wxGetApp().mainframe->m_monitor->select_machine(dev_id);
        m_has_active_printer_connection = machine->is_online();
        refresh_layer_info_from_selected_machine();
    };

    std::map<std::string, MachineObject*> all_by_id;
    for (const auto &entry : my_machines) {
        if (entry.second != nullptr)
            all_by_id[entry.first] = entry.second;
    }
    for (const auto &entry : local_machines) {
        if (entry.second != nullptr && all_by_id.find(entry.first) == all_by_id.end())
            all_by_id[entry.first] = entry.second;
    }
    if (selected_machine != nullptr)
        all_by_id[selected_machine->get_dev_id()] = selected_machine;

    std::vector<MachineObject *> sorted;
    sorted.reserve(all_by_id.size());
    for (const auto &entry : all_by_id) {
        if (entry.second != nullptr)
            sorted.push_back(entry.second);
    }
    std::sort(sorted.begin(), sorted.end(), [](MachineObject *a, MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return a != nullptr;
        return a->get_dev_name() < b->get_dev_name();
    });

    std::vector<MachineObject *> online_list;
    std::vector<MachineObject *> offline_list;
    for (MachineObject *m : sorted) {
        if (m == nullptr)
            continue;
        if (m->is_online())
            online_list.push_back(m);
        else
            offline_list.push_back(m);
    }

    auto add_section_title = [&](const wxString &text) {
        auto *lab = new wxStaticText(m_printers_popup_panel, wxID_ANY, text);
        lab->SetForegroundColour(k_muted);
        wxFont f = lab->GetFont();
        f.SetPointSize((std::max)(8, f.GetPointSize() - 1));
        lab->SetFont(f);
        printers_popup_sizer->Add(lab, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    };

    auto add_printer_card = [&](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const bool online = machine->is_online();
        const bool active = online && selected_machine != nullptr &&
                            selected_machine->get_dev_id() == machine->get_dev_id();
        wxColour dot_colour = k_red;
        if (online)
            dot_colour = k_green;
        else if (machine->is_connecting())
            dot_colour = k_yellow;

        auto *card = new StaticBox(m_printers_popup_panel, wxID_ANY);
        card->SetMinSize(wxSize(-1, FromDIP(36)));
        card->SetCornerRadius(static_cast<double>(FromDIP(8)));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(active ? k_green : k_card_border);
        card->SetBackgroundColorNormal(wxColour(255, 255, 255));
        card->SetBackgroundColour(wxColour(255, 255, 255));
        card->SetCursor(wxCursor(wxCURSOR_HAND));

        auto *hs = new wxBoxSizer(wxHORIZONTAL);
        hs->AddSpacer(FromDIP(10));
        auto *dot = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
        dot->SetForegroundColour(dot_colour);
        hs->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto *name_lbl = new wxStaticText(card, wxID_ANY, sidebar_display_name_for(machine), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        name_lbl->SetForegroundColour(wxColour(28, 28, 28));
        hs->Add(name_lbl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *edit_icon = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("rename_edit", this, 16));
        edit_icon->SetCursor(wxCursor(wxCURSOR_HAND));
        edit_icon->SetToolTip(_L("Edit printer name"));
        hs->Add(edit_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto *forget_icon = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("unbind", this, 16));
        forget_icon->SetCursor(wxCursor(wxCURSOR_HAND));
        forget_icon->SetToolTip(_L("Forget printer"));
        hs->Add(forget_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        auto *card_outer = new wxBoxSizer(wxVERTICAL);
        card_outer->Add(hs, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
        card->SetSizer(card_outer);

        auto pick = [select_machine_fn, machine](wxMouseEvent &) { select_machine_fn(machine); };
        card->Bind(wxEVT_LEFT_DOWN, pick);
        name_lbl->Bind(wxEVT_LEFT_DOWN, pick);
        dot->Bind(wxEVT_LEFT_DOWN, pick);
        edit_icon->Bind(wxEVT_LEFT_DOWN, [this, machine](wxMouseEvent &evt) {
            evt.StopPropagation();
            if (edit_sidebar_printer_name(machine)) {
                rebuild_printers_popup();
                rebuild_sidebar_printer_list();
                Layout();
            }
        });
        forget_icon->Bind(wxEVT_LEFT_DOWN, [this, machine](wxMouseEvent &evt) {
            evt.StopPropagation();
            if (confirm_forget_printer())
                forget_local_printer(machine);
        });
        printers_popup_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    };

    if (!online_list.empty()) {
        add_section_title(_L("Active printers"));
        for (MachineObject *m : online_list)
            add_printer_card(m);
    }
    if (!offline_list.empty()) {
        add_section_title(_L("Offline printers"));
        for (MachineObject *m : offline_list)
            add_printer_card(m);
    }
    if (sorted.empty()) {
        auto *empty = new wxStaticText(m_printers_popup_panel, wxID_ANY, _L("No printers found. Use + Add or add by IP."));
        empty->SetForegroundColour(k_muted);
        printers_popup_sizer->Add(empty, 0, wxALL, FromDIP(14));
    }

    auto *add_printer_wrap = new StaticBox(m_printers_popup_panel, wxID_ANY);
    add_printer_wrap->SetCornerRadius(static_cast<double>(FromDIP(10)));
    add_printer_wrap->SetBorderWidth(FromDIP(1));
    add_printer_wrap->SetBorderStyle(wxPENSTYLE_SHORT_DASH);
    add_printer_wrap->SetBorderColorNormal(wxColour(200, 200, 204));
    add_printer_wrap->SetBackgroundColorNormal(wxColour(255, 255, 255));
    add_printer_wrap->SetBackgroundColour(wxColour(255, 255, 255));
    add_printer_wrap->SetMinSize(wxSize(-1, FromDIP(40)));
    add_printer_wrap->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *add_printer_sizer = new wxBoxSizer(wxHORIZONTAL);
    add_printer_sizer->AddStretchSpacer(1);
    auto *add_printer_lbl = new wxStaticText(add_printer_wrap, wxID_ANY, _L("+ Add Printer"));
    add_printer_lbl->SetForegroundColour(k_green);
    add_printer_lbl->SetCursor(wxCursor(wxCURSOR_HAND));
    add_printer_sizer->Add(add_printer_lbl, 0, wxALIGN_CENTER_VERTICAL);
    add_printer_sizer->AddStretchSpacer(1);
    add_printer_wrap->SetSizer(add_printer_sizer);
    const auto open_add_printer = [this](wxMouseEvent &) { show_add_printer_dialog(); };
    add_printer_wrap->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    add_printer_lbl->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    printers_popup_sizer->Add(add_printer_wrap, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    printers_popup_sizer->AddSpacer(FromDIP(12));

    m_printers_popup_panel->SetSizer(printers_popup_sizer);
    printers_popup_sizer->Fit(m_printers_popup_panel);
    m_printers_popup_panel->Layout();

    const wxSize popup_size(popup_width, m_printers_popup_panel->GetBestSize().GetHeight());
    m_printers_popup_panel->SetSize(popup_size);
    m_printers_popup->SetClientSize(popup_size);
    m_printers_popup->SetSize(popup_size);
    m_printers_popup->SetMaxSize(wxSize(popup_width, -1));
    m_printers_popup->Layout();
    rebuild_sidebar_printer_list();
}

wxString PrinterWebView::sidebar_display_name_for(const MachineObject *machine) const
{
    if (machine == nullptr)
        return _L("Unknown Printer");

    const std::string dev_name = machine->get_dev_name();
    const std::string dev_ip   = machine->get_dev_ip();
    const bool placeholder_name = is_placeholder_printer_name(dev_name);

    if (!placeholder_name && !dev_name.empty())
        return from_u8(dev_name);

    const std::string host = friendly_host_from_address(!dev_ip.empty() ? dev_ip : machine->get_dev_id());
    if (!host.empty())
        return from_u8(host);

    if (!dev_name.empty() && !is_generic_coprint_product_name(dev_name))
        return from_u8(dev_name);

    return _L("Unknown Printer");
}

void PrinterWebView::show_sidebar_root_view()
{
    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Show();
    if (m_sidebar_printer_list_container != nullptr)
        m_sidebar_printer_list_container->Show();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Show();
    if (m_sidebar_add_printer_panel != nullptr)
        m_sidebar_add_printer_panel->Hide();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Hide();
    if (m_sidebar_root_panel != nullptr) {
        if (auto *chev = dynamic_cast<wxStaticText *>(wxWindow::FindWindowByName("sidebar_printers_chevron", m_sidebar_root_panel)))
            chev->SetLabelText(wxString::FromUTF8("\xE2\x8C\x84"));
    }
    rebuild_sidebar_printer_list();
    Layout();
}

void PrinterWebView::set_sidebar_user_avatar(const wxBitmap &avatar_bitmap)
{
    m_sidebar_user_avatar_bitmap = avatar_bitmap;
    if (m_sidebar_user_avatar_panel != nullptr)
        m_sidebar_user_avatar_panel->Refresh();
}

void PrinterWebView::begin_moonraker_lan_scan()
{
    if (m_destroying)
        return;
    if (m_lan_scan_in_progress) {
        m_lan_rescan_requested = true;
        return;
    }

    m_lan_scan_in_progress = true;
    m_lan_rescan_requested = false;
    m_discovered_moonraker_printers.clear();

    if (m_lan_scan_cancel_token)
        m_lan_scan_cancel_token->store(true);
    m_lan_scan_cancel_token = std::make_shared<std::atomic_bool>(false);
    auto cancel_token = m_lan_scan_cancel_token;
    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, cancel_token]() {
        const auto subnets = local_ipv4_subnets();
        std::vector<std::string> candidates;
        for (const auto &subnet : subnets) {
            for (uint32_t host = subnet.network + 1; host < subnet.broadcast; ++host)
                candidates.push_back(uint_to_ipv4(host));
        }

        std::atomic<size_t> next_index{ 0 };
        const size_t worker_count = (std::min<size_t>)(32, (std::max<size_t>)(1, candidates.size()));
        std::vector<std::thread> workers;
        std::vector<BBLocalMachine> discovered;
        std::mutex discovered_mutex;
        workers.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back([&]() {
                while (true) {
                    if (lifetime.expired() || cancel_token->load())
                        break;
                    const size_t index = next_index.fetch_add(1);
                    if (index >= candidates.size())
                        break;
                    BBLocalMachine machine;
                    if (probe_moonraker_host(candidates[index], machine)) {
                        if (!is_coprint_discovered_machine(machine))
                            continue;

                        bool added = false;
                        std::lock_guard<std::mutex> lock(discovered_mutex);
                        const auto duplicate = std::find_if(
                            discovered.begin(),
                            discovered.end(),
                            [&](const BBLocalMachine &existing) { return existing.dev_ip == machine.dev_ip; });
                        if (duplicate == discovered.end()) {
                            discovered.push_back(machine);
                            added = true;
                        }
                        if (added) {
                            wxGetApp().CallAfter([this, lifetime, cancel_token, machine]() {
                                if (lifetime.expired() || m_destroying || cancel_token->load())
                                    return;
                                const auto duplicate = std::find_if(
                                    m_discovered_moonraker_printers.begin(),
                                    m_discovered_moonraker_printers.end(),
                                    [&](const BBLocalMachine &existing) { return existing.dev_ip == machine.dev_ip; });
                                if (duplicate == m_discovered_moonraker_printers.end()) {
                                    m_discovered_moonraker_printers.push_back(machine);
                                    std::sort(m_discovered_moonraker_printers.begin(), m_discovered_moonraker_printers.end(), [](const BBLocalMachine &a, const BBLocalMachine &b) {
                                        return a.dev_name < b.dev_name;
                                    });
                                    if (m_sidebar_add_printer_panel != nullptr && m_sidebar_add_printer_panel->IsShown() &&
                                        m_sidebar_add_tab_index == 0)
                                        show_sidebar_add_printer_view();
                                }
                            });
                        }
                    }
                }
            });
        }
        for (auto &worker : workers)
            worker.join();

        wxGetApp().CallAfter([this, lifetime, cancel_token, discovered = std::move(discovered)]() mutable {
            if (lifetime.expired() || m_destroying || cancel_token->load())
                return;
            discovered.erase(
                std::remove_if(discovered.begin(), discovered.end(), [](const BBLocalMachine &machine) { return !is_coprint_discovered_machine(machine); }),
                discovered.end());
            std::sort(discovered.begin(), discovered.end(), [](const BBLocalMachine &a, const BBLocalMachine &b) {
                return a.dev_name < b.dev_name;
            });
            m_discovered_moonraker_printers = std::move(discovered);
            m_lan_scan_in_progress = false;
            if (m_lan_scan_cancel_token == cancel_token)
                m_lan_scan_cancel_token.reset();
            if (m_sidebar_add_printer_panel != nullptr && m_sidebar_add_printer_panel->IsShown() &&
                m_sidebar_add_tab_index == 0)
                show_sidebar_add_printer_view();
            if (m_lan_rescan_requested)
                begin_moonraker_lan_scan();
        });
    }).detach();
}

void PrinterWebView::show_sidebar_printers_view()
{
    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Show();
    if (m_sidebar_printer_list_container != nullptr)
        m_sidebar_printer_list_container->Show();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Show();
    if (m_sidebar_add_printer_panel != nullptr)
        m_sidebar_add_printer_panel->Hide();
    if (m_sidebar_header_back != nullptr)
        m_sidebar_header_back->Hide();
    if (m_sidebar_header_title != nullptr)
        m_sidebar_header_title->SetLabelText(_L("Printers"));
    if (m_sidebar_header_add != nullptr)
        m_sidebar_header_add->Hide();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Hide();
    if (m_sidebar_root_panel != nullptr) {
        if (auto *chev = dynamic_cast<wxStaticText *>(wxWindow::FindWindowByName("sidebar_printers_chevron", m_sidebar_root_panel)))
            chev->SetLabelText(wxString::FromUTF8("\xE2\x8C\x84"));
    }
    rebuild_sidebar_printer_list();
    CallAfter([this]() {
        update_sidebar_scrollbar(
            dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel),
            m_sidebar_printer_scroll_track,
            this);
    });
    Layout();
}

void PrinterWebView::show_sidebar_add_printer_view()
{
    if (m_sidebar_add_printer_panel == nullptr)
        return;

    if (m_sidebar_root_panel != nullptr)
        m_sidebar_root_panel->Hide();
    if (m_sidebar_printer_list_container != nullptr)
        m_sidebar_printer_list_container->Hide();
    if (m_sidebar_printer_list_panel != nullptr)
        m_sidebar_printer_list_panel->Hide();
    m_sidebar_add_printer_panel->Show();
    if (m_sidebar_header_back != nullptr)
        m_sidebar_header_back->Show();
    if (m_sidebar_header_title != nullptr)
        m_sidebar_header_title->SetLabelText(_L("Add Printer"));
    if (m_sidebar_header_add != nullptr)
        m_sidebar_header_add->Hide();
    if (m_sidebar_header_panel != nullptr)
        m_sidebar_header_panel->Show();

    if (auto *old_sizer = m_sidebar_add_printer_panel->GetSizer()) {
        m_sidebar_add_printer_panel->SetSizer(nullptr, false);
        delete old_sizer;
    }
    m_sidebar_add_printer_panel->DestroyChildren();
    auto *root = new wxBoxSizer(wxVERTICAL);

    const wxColour k_text("#232527");
    const wxColour k_muted("#767C84");
    const wxColour k_green("#35AD27");
    const wxColour k_card(*wxWHITE);
    const wxColour k_border("#C7C7C7");

    m_sidebar_add_tab_index = std::clamp(m_sidebar_add_tab_index, 0, 1);

    auto *tabs = new wxFlexGridSizer(1, 2, 0, 0);
    tabs->AddGrowableCol(0, 1);
    tabs->AddGrowableCol(1, 1);
    const std::array<wxString, 2> tab_names = { _L("Auto Connect"), _L("IP Connect") };
    for (size_t i = 0; i < tab_names.size(); ++i) {
        auto *tab = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, tab_names[i], wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        tab->SetForegroundColour(static_cast<int>(i) == m_sidebar_add_tab_index ? k_green : k_text);
        tab->SetCursor(wxCursor(wxCURSOR_HAND));
        {
            wxFont f = tab->GetFont();
            f.SetPointSize(8);
            tab->SetFont(f);
        }
        tabs->Add(tab, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        tab->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent &) {
            m_sidebar_add_tab_index = static_cast<int>(i);
            show_sidebar_add_printer_view();
        });
    }
    root->Add(tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *tab_line = new wxPanel(m_sidebar_add_printer_panel, wxID_ANY);
    tab_line->SetMinSize(wxSize(-1, FromDIP(1)));
    tab_line->SetBackgroundColour(k_border);
    root->Add(tab_line, 0, wxEXPAND | wxTOP, FromDIP(8));

    wxStaticText *status = nullptr;
    wxTextCtrl *ip_input = nullptr;
    Button *add_btn = nullptr;

    if (m_sidebar_add_tab_index == 0) {
        auto *search_row = new wxBoxSizer(wxHORIZONTAL);
        auto *searching = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Searching for printers on your network..."));
        searching->SetForegroundColour(k_text);
        search_row->Add(searching, 1, wxALIGN_CENTER_VERTICAL);
        auto *refresh = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, wxString::FromUTF8("\xE2\x9F\xB3"));
        refresh->SetForegroundColour(k_text);
        refresh->SetCursor(wxCursor(wxCURSOR_HAND));
        search_row->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        if (m_discovered_moonraker_printers.empty() && !m_lan_scan_in_progress)
            begin_moonraker_lan_scan();

        auto *auto_list_row = new wxBoxSizer(wxHORIZONTAL);
        auto *auto_list = new wxScrolledWindow(m_sidebar_add_printer_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_auto_connect_list_window = auto_list;
        auto_list->SetBackgroundColour(*wxWHITE);
        auto_list->SetScrollRate(0, FromDIP(8));
        auto_list->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        auto_list->SetMinSize(wxSize(-1, FromDIP(150)));
        auto *auto_list_sizer = new wxBoxSizer(wxVERTICAL);
        auto_list->SetSizer(auto_list_sizer);
        auto_list_row->Add(auto_list, 1, wxEXPAND);

        auto *scroll_track = new SidebarScrollbar(m_sidebar_add_printer_panel);
        m_auto_connect_scroll_track = scroll_track;
        auto_list_row->Add(scroll_track, 0, wxEXPAND | wxLEFT, FromDIP(4));

        auto rebuild_auto_cards = [this, auto_list, auto_list_sizer, k_text, k_muted, k_card, k_border]() {
            if (m_discovered_moonraker_printers.empty()) {
                auto *empty = new wxStaticText(auto_list, wxID_ANY,
                                               m_lan_scan_in_progress ? _L("Scanning your network...") : _L("No printers discovered yet."));
                empty->SetForegroundColour(k_muted);
                auto_list_sizer->Add(empty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
                auto_list->FitInside();
                auto_list->Layout();
                return;
            }
            for (const auto &machine_info : m_discovered_moonraker_printers) {
                auto *card = new StaticBox(auto_list, wxID_ANY);
                card->SetCornerRadius(FromDIP(8));
                card->SetBorderWidth(1);
                card->SetBorderColorNormal(k_border);
                card->SetBackgroundColorNormal(k_card);
                card->SetBackgroundColour(k_card);
                card->SetMinSize(wxSize(-1, FromDIP(72)));
                auto *row = new wxBoxSizer(wxHORIZONTAL);
                row->AddSpacer(FromDIP(12));
                auto *copy = new wxBoxSizer(wxVERTICAL);
                auto *name = new wxStaticText(card, wxID_ANY, from_u8(machine_info.dev_name));
                name->SetForegroundColour(k_text);
                wxFont nf = name->GetFont();
                nf.SetPointSize(9);
                nf.SetWeight(wxFONTWEIGHT_BOLD);
                name->SetFont(nf);
                copy->Add(name, 0);
                const std::string display_ip = machine_info.dev_ip.substr(0, machine_info.dev_ip.find(':'));
                auto *ip = new wxStaticText(card, wxID_ANY, from_u8(display_ip));
                ip->SetForegroundColour(k_muted);
                {
                    wxFont ipf = ip->GetFont();
                    ipf.SetPointSize((std::max)(1, ipf.GetPointSize() - 2));
                    ip->SetFont(ipf);
                }
                copy->Add(ip, 0);
                row->Add(copy, 1, wxALIGN_CENTER_VERTICAL);
                auto *add = new Button(card, _L("Add"));
                add->SetMinSize(wxSize(FromDIP(58), FromDIP(32)));
                row->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
                card->SetSizer(row);
                add->Bind(wxEVT_BUTTON, [this, machine_info](wxCommandEvent &) {
                    if (finish_add_moonraker_printer(machine_info, false))
                        show_sidebar_printers_view();
                });
                auto_list_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
            }
            auto_list->FitInside();
            auto_list->Layout();
        };
        rebuild_auto_cards();
        auto update_custom_scrollbar = [this]() {
            update_sidebar_scrollbar(m_auto_connect_list_window, m_auto_connect_scroll_track, this);
        };
        auto on_scroll = [update_custom_scrollbar](wxScrollWinEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        };
        auto_list->Bind(wxEVT_SCROLLWIN_TOP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_BOTTOM, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_LINEUP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_PAGEUP, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_scroll);
        auto_list->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_scroll);
        auto_list->Bind(wxEVT_MOUSEWHEEL, [update_custom_scrollbar](wxMouseEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        });
        auto_list->Bind(wxEVT_SIZE, [update_custom_scrollbar](wxSizeEvent &evt) {
            evt.Skip();
            update_custom_scrollbar();
        });
        root->Add(auto_list_row, 1, wxEXPAND | wxTOP, FromDIP(4));
        CallAfter(update_custom_scrollbar);
        refresh->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            m_discovered_moonraker_printers.clear();
            begin_moonraker_lan_scan();
            show_sidebar_add_printer_view();
        });
    } else if (m_sidebar_add_tab_index == 1) {
        auto *prompt = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Enter your printer's IP address"));
        prompt->SetForegroundColour(k_text);
        {
            wxFont f = prompt->GetFont();
            f.SetPointSize((std::max)(1, f.GetPointSize() - 2));
            prompt->SetFont(f);
        }
        root->Add(prompt, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto *input_row = new wxBoxSizer(wxHORIZONTAL);
        ip_input = new wxTextCtrl(m_sidebar_add_printer_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        ip_input->SetName("sidebar_add_printer_ip");
        ip_input->SetHint(_L("Type IP address..."));
        ip_input->SetMinSize(wxSize(-1, FromDIP(32)));
        {
            wxFont f = ip_input->GetFont();
            f.SetPointSize((std::max)(1, f.GetPointSize() - 2));
            ip_input->SetFont(f);
        }
        input_row->Add(ip_input, 1, wxRIGHT, FromDIP(8));
        add_btn = new Button(m_sidebar_add_printer_panel, _L("Add"));
        add_btn->SetName("sidebar_add_printer_button");
        add_btn->SetMinSize(wxSize(FromDIP(58), FromDIP(32)));
        input_row->Add(add_btn, 0);
        root->Add(input_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        status = new wxStaticText(m_sidebar_add_printer_panel, wxID_ANY, _L("Connecting the printer..."));
        status->SetName("sidebar_add_printer_status");
        status->SetForegroundColour(k_muted);
        status->Hide();
        root->Add(status, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }

    auto *hint_box = new StaticBox(m_sidebar_add_printer_panel, wxID_ANY);
    hint_box->SetCornerRadius(FromDIP(8));
    hint_box->SetBorderWidth(1);
    hint_box->SetBorderColorNormal(k_border);
    hint_box->SetBackgroundColorNormal(k_card);
    hint_box->SetBackgroundColour(k_card);
    hint_box->SetMinSize(wxSize(-1, FromDIP(56)));
    auto *hint_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *hint_icon = new wxStaticBitmap(hint_box, wxID_ANY, create_scaled_bitmap("device_sidebar_idea", this, 20));
    hint_sz->Add(hint_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    hint_sz->AddSpacer(FromDIP(10));
    auto *hint = new wxStaticText(hint_box, wxID_ANY, _L("Make sure your printer is powered on\nand connected the same network."));
    hint->SetForegroundColour(k_text);
    {
        wxFont f = hint->GetFont();
        f.SetPointSize((std::max)(1, f.GetPointSize() - 1));
        hint->SetFont(f);
    }
    hint_sz->Add(hint, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(12));
    hint_box->SetSizer(hint_sz);
    root->Add(hint_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    if (m_sidebar_add_tab_index == 0) {
        auto *fallback = new StaticBox(m_sidebar_add_printer_panel, wxID_ANY);
        fallback->SetCornerRadius(FromDIP(8));
        fallback->SetBorderWidth(1);
        fallback->SetBorderColorNormal(wxColour("#B7D8B0"));
        fallback->SetBackgroundColorNormal(wxColour("#F3F8F1"));
        fallback->SetBackgroundColour(wxColour("#F3F8F1"));
        fallback->SetCursor(wxCursor(wxCURSOR_HAND));
        auto *fallback_sizer = new wxBoxSizer(wxVERTICAL);
        auto *title = new wxStaticText(fallback, wxID_ANY, _L("Can't find your printer?"));
        title->SetForegroundColour(k_green);
        wxFont tf = title->GetFont();
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
        auto *sub = new wxStaticText(fallback, wxID_ANY, _L("Try IP Connect"));
        sub->SetForegroundColour(k_green);
        fallback_sizer->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
        fallback_sizer->Add(sub, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(10));
        fallback->SetSizer(fallback_sizer);
        auto go_ip = [this](wxMouseEvent &) {
            m_sidebar_add_tab_index = 1;
            show_sidebar_add_printer_view();
        };
        fallback->Bind(wxEVT_LEFT_DOWN, go_ip);
        title->Bind(wxEVT_LEFT_DOWN, go_ip);
        sub->Bind(wxEVT_LEFT_DOWN, go_ip);
        root->Add(fallback, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }

    auto submit_ip = [this, ip_input, status, add_btn](wxCommandEvent &) {
        if (ip_input == nullptr || status == nullptr)
            return;
        wxString value = ip_input->GetValue();
        value.Trim(true);
        value.Trim(false);
        if (value.empty()) {
            status->Show();
            status->SetLabelText(_L("IP address cannot be empty."));
            m_sidebar_add_printer_panel->Layout();
            return;
        }

        std::string host = into_u8(value);
        std::string sanitized;
        std::string sanitize_error;
        if (!sanitize_moonraker_address(host, sanitized, sanitize_error)) {
            status->Show();
            status->SetLabelText(from_u8(sanitize_error));
            m_sidebar_add_printer_panel->Layout();
            return;
        }

        BBLocalMachine machine;
        machine.dev_id = sanitized;
        machine.dev_ip = sanitized;
        machine.dev_name = "Unknown Printer";
        machine.printer_type = "Moonraker";

        status->Show();
        status->SetLabelText(_L("Connecting the printer..."));
        if (add_btn != nullptr)
            add_btn->Enable(false);
        if (ip_input != nullptr)
            ip_input->Enable(false);
        m_sidebar_add_printer_panel->Layout();

        // Probe off the UI thread — synchronous Moonraker HTTP used to freeze the app
        // for many seconds when a printer was slow or partially unreachable.
        const bool use_ssl = host.rfind("https://", 0) == 0;
        std::weak_ptr<int> lifetime = m_lifetime_token;
        std::thread([this, lifetime, machine, use_ssl]() {
            BBLocalMachine resolved = machine;
            BBLocalMachine detected;
            const bool probed = probe_moonraker_host(machine.dev_ip, detected);
            if (probed) {
                if (!detected.dev_name.empty())
                    resolved.dev_name = detected.dev_name;
                if (!detected.printer_type.empty())
                    resolved.printer_type = detected.printer_type;
                std::string sanitized_detected;
                std::string unused;
                if (!detected.dev_ip.empty() && sanitize_moonraker_address(detected.dev_ip, sanitized_detected, unused)) {
                    resolved.dev_ip = sanitized_detected;
                    resolved.dev_id = sanitized_detected;
                } else if (!detected.dev_id.empty() && sanitize_moonraker_address(detected.dev_id, sanitized_detected, unused)) {
                    resolved.dev_ip = sanitized_detected;
                    resolved.dev_id = sanitized_detected;
                }
            }

            wxGetApp().CallAfter([this, lifetime, resolved, use_ssl, probed]() {
                if (lifetime.expired() || m_destroying)
                    return;

                wxWindow *status_ctrl = m_sidebar_add_printer_panel
                    ? m_sidebar_add_printer_panel->FindWindowByName("sidebar_add_printer_status")
                    : nullptr;
                wxWindow *add_btn_ctrl = m_sidebar_add_printer_panel
                    ? m_sidebar_add_printer_panel->FindWindowByName("sidebar_add_printer_button")
                    : nullptr;
                wxWindow *ip_input_ctrl = m_sidebar_add_printer_panel
                    ? m_sidebar_add_printer_panel->FindWindowByName("sidebar_add_printer_ip")
                    : nullptr;

                auto show_fail = [&](const wxString &message) {
                    if (status_ctrl != nullptr) {
                        status_ctrl->Show();
                        if (auto *label = dynamic_cast<wxStaticText *>(status_ctrl))
                            label->SetLabelText(message);
                        if (m_sidebar_add_printer_panel != nullptr)
                            m_sidebar_add_printer_panel->Layout();
                    }
                    if (add_btn_ctrl != nullptr)
                        add_btn_ctrl->Enable(true);
                    if (ip_input_ctrl != nullptr)
                        ip_input_ctrl->Enable(true);
                };

                // Do not insert unreachable printers as "Unknown" — that leaves the UI
                // stuck on Connecting... against a bad/forgotten address.
                if (!probed) {
                    show_fail(_L("Could not connect to the printer."));
                    return;
                }

                const bool ok = finish_add_moonraker_printer(resolved, use_ssl, false);
                if (ok) {
                    begin_sidebar_connect_attempt(resolved.dev_id);
                    show_sidebar_printers_view();
                } else {
                    show_fail(_L("Could not connect to the printer."));
                    return;
                }

                if (add_btn_ctrl != nullptr)
                    add_btn_ctrl->Enable(true);
                if (ip_input_ctrl != nullptr)
                    ip_input_ctrl->Enable(true);
            });
        }).detach();
    };
    if (add_btn != nullptr)
        add_btn->Bind(wxEVT_BUTTON, submit_ip);
    if (ip_input != nullptr)
        ip_input->Bind(wxEVT_TEXT_ENTER, submit_ip);

    m_sidebar_add_printer_panel->SetSizer(root);
    m_sidebar_add_printer_panel->Layout();
    Layout();
}

void PrinterWebView::clear_sidebar_connect_attempt()
{
    m_sidebar_connect_dev_id.clear();
    m_sidebar_connect_started_ms = 0;
    m_sidebar_connect_phase = SidebarConnectPhase::None;
}

void PrinterWebView::begin_sidebar_connect_attempt(const std::string &dev_id)
{
    if (dev_id.empty()) {
        clear_sidebar_connect_attempt();
        return;
    }
    m_sidebar_connect_dev_id = dev_id;
    m_sidebar_connect_started_ms = wxGetUTCTimeMillis();
    m_sidebar_connect_phase = SidebarConnectPhase::Connecting;
}

void PrinterWebView::update_sidebar_connect_attempt_state()
{
    if (m_sidebar_connect_phase == SidebarConnectPhase::None || m_sidebar_connect_dev_id.empty())
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *machine = nullptr;
    if (dev_manager != nullptr) {
        machine = dev_manager->get_selected_machine();
        if (machine == nullptr || machine->get_dev_id() != m_sidebar_connect_dev_id) {
            machine = nullptr;
            for (const auto &entry : dev_manager->get_my_machine_list()) {
                if (entry.second != nullptr && entry.second->get_dev_id() == m_sidebar_connect_dev_id) {
                    machine = entry.second;
                    break;
                }
            }
            if (machine == nullptr) {
                for (const auto &entry : dev_manager->get_local_machinelist()) {
                    if (entry.second != nullptr && entry.second->get_dev_id() == m_sidebar_connect_dev_id) {
                        machine = entry.second;
                        break;
                    }
                }
            }
        }
    }

    if (machine != nullptr && machine->is_online()) {
        clear_sidebar_connect_attempt();
        m_sidebar_printer_list_signature.clear();
        rebuild_sidebar_printer_list();
        return;
    }

    if (m_sidebar_connect_phase == SidebarConnectPhase::Connecting &&
        (wxGetUTCTimeMillis() - m_sidebar_connect_started_ms) >= 15000) {
        m_sidebar_connect_phase = SidebarConnectPhase::Failed;
        m_sidebar_printer_list_signature.clear();
        rebuild_sidebar_printer_list();
    }
}

void PrinterWebView::rebuild_sidebar_printer_list()
{
    if (m_sidebar_printer_list_panel == nullptr || m_sidebar_printer_list_sizer == nullptr)
        return;

    auto *dev_manager = wxGetApp().getDeviceManager();
    const auto my_machines = dev_manager ? dev_manager->get_my_machine_list() : std::map<std::string, MachineObject*>();
    const auto local_machines = dev_manager ? dev_manager->get_local_machinelist() : std::map<std::string, MachineObject*>();
    auto *selected_machine = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    auto normalize_host = [](std::string value) {
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(value);
        const auto scheme_pos = value.find("://");
        if (scheme_pos != std::string::npos)
            value = value.substr(scheme_pos + 3);
        const auto slash_pos = value.find('/');
        if (slash_pos != std::string::npos)
            value = value.substr(0, slash_pos);
        const auto question_pos = value.find('?');
        if (question_pos != std::string::npos)
            value = value.substr(0, question_pos);
        const auto at_pos = value.find('@');
        if (at_pos != std::string::npos)
            value = value.substr(at_pos + 1);
        if (std::count(value.begin(), value.end(), ':') == 1) {
            const auto colon_pos = value.rfind(':');
            if (colon_pos != std::string::npos)
                value = value.substr(0, colon_pos);
        }
        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    auto same_machine_identity = [&](const MachineObject *a, const MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return false;
        if ((!a->get_dev_id().empty() && a->get_dev_id() == b->get_dev_id()) ||
            (!a->get_dev_ip().empty() && a->get_dev_ip() == b->get_dev_ip()) ||
            (!a->get_dev_id().empty() && a->get_dev_id() == b->get_dev_ip()) ||
            (!a->get_dev_ip().empty() && a->get_dev_ip() == b->get_dev_id()))
            return true;
        const std::string a_host = normalize_host(!a->get_dev_ip().empty() ? a->get_dev_ip() : a->get_dev_id());
        const std::string b_host = normalize_host(!b->get_dev_ip().empty() ? b->get_dev_ip() : b->get_dev_id());
        return !a_host.empty() && a_host == b_host;
    };

    auto has_local_machine_record = [&](const MachineObject *machine) {
        if (machine == nullptr)
            return false;
        for (const auto &entry : local_machines) {
            MachineObject *local = entry.second;
            if (local != nullptr && same_machine_identity(machine, local))
                return true;
            const std::string target_host = normalize_host(!machine->get_dev_ip().empty() ? machine->get_dev_ip() : machine->get_dev_id());
            if (!target_host.empty() &&
                (normalize_host(entry.first) == target_host ||
                 (local != nullptr && normalize_host(local->get_dev_id()) == target_host) ||
                 (local != nullptr && normalize_host(local->get_dev_ip()) == target_host)))
                return true;
        }
        return false;
    };

    std::map<std::string, MachineObject*> all_by_id;
    for (const auto &entry : my_machines)
        if (entry.second != nullptr && (!entry.second->is_lan_mode_printer() || has_local_machine_record(entry.second)))
            all_by_id[entry.first] = entry.second;
    for (const auto &entry : local_machines)
        if (entry.second != nullptr && all_by_id.find(entry.first) == all_by_id.end())
            all_by_id[entry.first] = entry.second;
    if (selected_machine != nullptr &&
        (!selected_machine->is_lan_mode_printer() || has_local_machine_record(selected_machine)))
        all_by_id[selected_machine->get_dev_id()] = selected_machine;

    auto machine_display_rank = [&](MachineObject *machine) {
        if (machine == nullptr)
            return -1;
        int score = 0;
        if (!machine->get_dev_ip().empty())
            score += 100;
        if (!is_placeholder_printer_name(machine->get_dev_name()))
            score += 50;
        if (machine->is_online())
            score += 10;
        if (selected_machine != nullptr && selected_machine->get_dev_id() == machine->get_dev_id())
            score += 5;
        return score;
    };

    // Collapse same-host duplicates (IP insert + SSDP announce with a different id).
    std::vector<MachineObject *> deduped;
    for (const auto &entry : all_by_id) {
        MachineObject *candidate = entry.second;
        if (candidate == nullptr)
            continue;
        bool merged = false;
        for (MachineObject *&kept : deduped) {
            if (!same_machine_identity(kept, candidate))
                continue;
            if (machine_display_rank(candidate) > machine_display_rank(kept))
                kept = candidate;
            merged = true;
            break;
        }
        if (!merged)
            deduped.push_back(candidate);
    }

    // Drop pure ghost cards: no IP and placeholder name when anything else exists.
    if (deduped.size() > 1) {
        deduped.erase(std::remove_if(deduped.begin(), deduped.end(),
                                     [](MachineObject *machine) {
                                         return machine != nullptr &&
                                                machine->get_dev_ip().empty() &&
                                                is_placeholder_printer_name(machine->get_dev_name());
                                     }),
                      deduped.end());
    }

    std::vector<MachineObject *> online_list;
    std::vector<MachineObject *> offline_list;
    for (MachineObject *machine : deduped) {
        if (machine == nullptr)
            continue;
        (machine->is_online() ? online_list : offline_list).push_back(machine);
    }
    auto by_name = [](MachineObject *a, MachineObject *b) {
        if (a == nullptr || b == nullptr)
            return a != nullptr;
        return a->get_dev_name() < b->get_dev_name();
    };
    std::sort(online_list.begin(), online_list.end(), by_name);
    std::sort(offline_list.begin(), offline_list.end(), by_name);

    wxString next_signature;
    auto append_machine_signature = [&](const char *section, const std::vector<MachineObject *> &machines) {
        next_signature += wxString::FromUTF8(section);
        next_signature += "|";
        for (MachineObject *machine : machines) {
            if (machine == nullptr)
                continue;
            next_signature += from_u8(machine->get_dev_id());
            next_signature += "\x1F";
            next_signature += sidebar_display_name_for(machine);
            next_signature += "\x1F";
            next_signature += from_u8(machine->get_dev_ip());
            next_signature += "\x1F";
            next_signature += (machine->is_online() ? "1" : "0");
            next_signature += "\x1F";
            next_signature += has_local_machine_record(machine) ? "1" : "0";
            next_signature += "\x1F";
            next_signature += (selected_machine != nullptr && selected_machine->get_dev_id() == machine->get_dev_id() ? "1" : "0");
            next_signature += "\x1F";
            if (machine->get_dev_id() == m_sidebar_connect_dev_id) {
                if (m_sidebar_connect_phase == SidebarConnectPhase::Connecting)
                    next_signature += "C";
                else if (m_sidebar_connect_phase == SidebarConnectPhase::Failed)
                    next_signature += "F";
                else
                    next_signature += "N";
            } else {
                next_signature += "-";
            }
            next_signature += "\n";
        }
    };
    append_machine_signature("online", online_list);
    append_machine_signature("offline", offline_list);
    if (next_signature == m_sidebar_printer_list_signature)
        return;
    m_sidebar_printer_list_signature = next_signature;

    m_sidebar_printer_list_sizer->Clear(true);

    const wxColour k_text("#232527");
    const wxColour k_muted("#767C84");
    const wxColour k_green("#35AD27");
    const wxColour k_yellow("#F2C94C");
    const wxColour k_red("#E74C3C");
    const wxColour k_card_bg(*wxWHITE);
    const wxColour k_card_border("#C7C7C7");

    auto add_section_title = [&](const wxString &text) {
        auto *label = new wxStaticText(m_sidebar_printer_list_panel, wxID_ANY, text);
        label->SetForegroundColour(k_muted);
        {
            wxFont f = label->GetFont();
            f.SetPointSize(10);
            label->SetFont(f);
        }
        m_sidebar_printer_list_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    };

    auto *add_printer_top_card = new StaticBox(m_sidebar_printer_list_panel, wxID_ANY);
    add_printer_top_card->SetMinSize(wxSize(-1, FromDIP(60)));
    add_printer_top_card->SetMaxSize(wxSize(-1, FromDIP(60)));
    add_printer_top_card->SetCornerRadius(FromDIP(8));
    add_printer_top_card->SetBorderWidth(1);
    add_printer_top_card->SetBorderStyle(wxPENSTYLE_SHORT_DASH);
    add_printer_top_card->SetBorderColorNormal(wxColour("#4B8C43"));
    add_printer_top_card->SetBackgroundColorNormal(k_card_bg);
    add_printer_top_card->SetBackgroundColour(k_card_bg);
    add_printer_top_card->SetCursor(wxCursor(wxCURSOR_HAND));

    auto *add_printer_row = new wxBoxSizer(wxHORIZONTAL);
    add_printer_row->AddStretchSpacer(1);
    auto *add_printer_label = new wxStaticText(add_printer_top_card, wxID_ANY, _L("+ Add Printer"));
    add_printer_label->SetForegroundColour(k_green);
    {
        wxFont f = add_printer_label->GetFont();
        f.SetPointSize(11);
        f.SetWeight(wxFONTWEIGHT_BOLD);
        add_printer_label->SetFont(f);
    }
    add_printer_row->Add(add_printer_label, 0, wxALIGN_CENTER_VERTICAL);
    add_printer_row->AddStretchSpacer(1);
    add_printer_top_card->SetSizer(add_printer_row);
    const auto open_add_printer = [this](wxMouseEvent &) { show_sidebar_add_printer_view(); };
    add_printer_top_card->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    add_printer_label->Bind(wxEVT_LEFT_DOWN, open_add_printer);
    m_sidebar_printer_list_sizer->Add(add_printer_top_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto select_machine_fn = [this, dev_manager](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const std::string dev_id = machine->get_dev_id();
        const bool was_online = machine->is_online();
        const bool already_selected =
            dev_manager != nullptr &&
            dev_manager->get_selected_machine() != nullptr &&
            dev_manager->get_selected_machine()->get_dev_id() == dev_id;

        // Already the active online printer: open device UI without reconnecting.
        // Re-select used to force LAN disconnect/reconnect and freeze the app.
        if (already_selected && was_online) {
            clear_sidebar_connect_attempt();
            m_has_active_printer_connection = true;
            refresh_layer_info_from_selected_machine();
            return;
        }

        if (!was_online) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": reconnecting offline sidebar machine "
                                    << machine->get_dev_id();
            begin_sidebar_connect_attempt(dev_id);
            machine->connect(machine->local_use_ssl);
        } else {
            clear_sidebar_connect_attempt();
        }
        // Update the real selected machine synchronously so the sidebar can reflect
        // the device page that is actually active, even before MonitorPanel's queued
        // notification is processed.
        if (dev_manager != nullptr)
            dev_manager->set_selected_machine(dev_id);
        if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->m_monitor != nullptr)
            wxGetApp().mainframe->m_monitor->select_machine(dev_id);
        m_has_active_printer_connection = was_online;
        if (was_online) {
            machine->command_request_push_all(true);
        } else {
            // connect() is asynchronous — request a push once the connection is ready
            // by scheduling it slightly after the connect attempt starts.
            const std::string reconnect_dev_id = machine->get_dev_id();
            wxGetApp().CallAfter([dev_manager, reconnect_dev_id]() {
                MachineObject *current = dev_manager ? dev_manager->get_selected_machine() : nullptr;
                if (current != nullptr && current->get_dev_id() == reconnect_dev_id && current->is_online())
                    current->command_request_push_all(true);
            });
        }
        refresh_layer_info_from_selected_machine();
        // Rebuild after the click event unwinds. Destroying the card tree while one
        // of its children is still handling the event can lead to use-after-free crashes.
        wxGetApp().CallAfter([this, token = std::weak_ptr<int>(m_lifetime_token)]() {
            if (token.expired() || m_destroying)
                return;
            rebuild_sidebar_printer_list();
        });
    };

    auto open_machine_device_page_fn = [this, select_machine_fn](MachineObject *machine) {
        select_machine_fn(machine);
        // Defer tab switch until after this click finishes: the sidebar rebuild
        // destroys the card/chevron that fired the event.
        wxGetApp().CallAfter([this, token = std::weak_ptr<int>(m_lifetime_token)]() {
            if (token.expired() || m_destroying)
                return;
            select_tab(PrinterWebViewTab::Status);
            show_sidebar_printers_view();
        });
    };

    auto add_printer_card = [&](MachineObject *machine) {
        if (machine == nullptr)
            return;
        const bool online = machine->is_online();
        const bool can_forget = true;
        const bool selected = selected_machine != nullptr &&
                              selected_machine->get_dev_id() == machine->get_dev_id();
        const bool active = online && selected;
        const bool is_connect_target = !m_sidebar_connect_dev_id.empty() &&
                                       machine->get_dev_id() == m_sidebar_connect_dev_id;
        const bool show_connecting = is_connect_target &&
                                     m_sidebar_connect_phase == SidebarConnectPhase::Connecting;
        const bool show_not_connected = is_connect_target &&
                                        m_sidebar_connect_phase == SidebarConnectPhase::Failed;

        wxString status_text;
        wxColour status_colour;
        wxColour dot_colour;
        if (show_connecting) {
            status_text = _L("Connecting...");
            status_colour = k_yellow;
            dot_colour = k_yellow;
        } else if (show_not_connected) {
            status_text = _L("Not connected");
            status_colour = k_red;
            dot_colour = k_red;
        } else if (active) {
            status_text = _L("Connected");
            status_colour = k_green;
            dot_colour = k_green;
        } else if (online) {
            status_text = _L("Active");
            status_colour = k_green;
            dot_colour = k_green;
        } else {
            status_text = _L("Not active");
            status_colour = k_muted;
            dot_colour = wxColour("#767C84");
        }
        auto *card = new StaticBox(m_sidebar_printer_list_panel, wxID_ANY);
        card->SetMinSize(wxSize(-1, FromDIP(82)));
        card->SetMaxSize(wxSize(-1, FromDIP(82)));
        card->SetCornerRadius(FromDIP(8));
        card->SetBorderWidth(1);
        card->SetBorderColorNormal(selected ? wxColour("#4B8C43") : k_card_border);
        card->SetBackgroundColorNormal(selected ? wxColour("#F3F8F1") : k_card_bg);
        card->SetBackgroundColour(selected ? wxColour("#F3F8F1") : k_card_bg);
        card->SetCursor(wxCursor(wxCURSOR_HAND));

        auto *outer = new wxBoxSizer(wxHORIZONTAL);
        outer->AddSpacer(FromDIP(12));

        auto *content = new wxBoxSizer(wxVERTICAL);
        auto *status_row = new wxBoxSizer(wxHORIZONTAL);
        auto *dot = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
        dot->SetForegroundColour(dot_colour);
        auto *status = new wxStaticText(card, wxID_ANY, status_text);
        status->SetForegroundColour(status_colour);
        {
            wxFont f = status->GetFont();
            f.SetPointSize(7);
            status->SetFont(f);
        }
        status_row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        status_row->Add(status, 0, wxALIGN_CENTER_VERTICAL);
        content->Add(status_row, 0);

        // Real device name: this comes from MachineObject / DeviceManager, not from UI mock text.
        auto *name = new wxStaticText(card, wxID_ANY, sidebar_display_name_for(machine), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        name->SetForegroundColour(k_text);
        wxFont nf = name->GetFont();
        nf.SetPointSize(11);
        nf.SetWeight(wxFONTWEIGHT_BOLD);
        name->SetFont(nf);
        content->Add(name, 0, wxEXPAND);

        auto *ip = new wxStaticText(card, wxID_ANY, from_u8(machine->get_dev_ip()));
        ip->SetForegroundColour(k_muted);
        {
            wxFont f = ip->GetFont();
            f.SetPointSize(9);
            ip->SetFont(f);
        }
        content->Add(ip, 0);
        outer->Add(content, 1, wxALIGN_CENTER_VERTICAL);

        if (active && !show_connecting && !show_not_connected) {
            auto *check = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap("device_sidebar_connected", this, 15));
            outer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        }

        auto *actions = new wxPanel(card, wxID_ANY);
        actions->SetMinSize(wxSize(FromDIP(can_forget ? 68 : 32), FromDIP(34)));
        actions->SetMaxSize(wxSize(FromDIP(can_forget ? 68 : 32), FromDIP(34)));
        actions->SetBackgroundColour(selected ? wxColour("#F3F8F1") : k_card_bg);
        auto *actions_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto flash_connecting = [card, dot, status, k_yellow]() {
            dot->SetForegroundColour(k_yellow);
            status->SetLabel(_L("Connecting..."));
            status->SetForegroundColour(k_yellow);
            card->Layout();
            card->Refresh();
            card->Update();
        };

        auto open_rename = [this, machine](wxMouseEvent &evt) {
            evt.StopPropagation();
            if (edit_sidebar_printer_name(machine)) {
                rebuild_printers_popup();
                // Rebuild after the click handler finishes — this card is destroyed.
                wxGetApp().CallAfter([this, token = std::weak_ptr<int>(m_lifetime_token)]() {
                    if (token.expired() || m_destroying)
                        return;
                    rebuild_sidebar_printer_list();
                    refresh_layer_info_from_selected_machine();
                    Layout();
                });
            }
        };

        if (can_forget) {
            auto *forget_area = new StaticBox(actions, wxID_ANY);
            forget_area->SetMinSize(wxSize(FromDIP(30), FromDIP(30)));
            forget_area->SetMaxSize(wxSize(FromDIP(30), FromDIP(30)));
            forget_area->SetCornerRadius(FromDIP(15));
            forget_area->SetBorderWidth(1);
            forget_area->SetBorderColorNormal(wxColour("#C7C7C7"));
            forget_area->SetBackgroundColorNormal(selected ? wxColour("#F3F8F1") : k_card_bg);
            forget_area->SetBackgroundColour(selected ? wxColour("#F3F8F1") : k_card_bg);
            forget_area->SetCursor(wxCursor(wxCURSOR_HAND));
            auto *forget_sizer = new wxBoxSizer(wxVERTICAL);
            forget_sizer->AddStretchSpacer(1);
            auto *forget_icon = new wxStaticBitmap(
                forget_area,
                wxID_ANY,
                create_scaled_bitmap("unbind", this, 16));
            forget_icon->SetCursor(wxCursor(wxCURSOR_HAND));
            forget_sizer->Add(forget_icon, 0, wxALIGN_CENTER_HORIZONTAL);
            forget_sizer->AddStretchSpacer(1);
            forget_area->SetSizer(forget_sizer);
            auto forget_printer = [this, machine](wxMouseEvent &evt) {
                evt.StopPropagation();
                if (confirm_forget_printer())
                    forget_local_printer(machine);
            };
            forget_area->Bind(wxEVT_LEFT_DOWN, forget_printer);
            forget_icon->Bind(wxEVT_LEFT_DOWN, forget_printer);
            actions_sizer->Add(forget_area, 0, wxALIGN_CENTER_VERTICAL);
            actions_sizer->AddSpacer(FromDIP(6));
        }

        // Circular edit button — opens rename popup (same chrome as forget).
        auto *rename_area = new StaticBox(actions, wxID_ANY);
        rename_area->SetMinSize(wxSize(FromDIP(30), FromDIP(30)));
        rename_area->SetMaxSize(wxSize(FromDIP(30), FromDIP(30)));
        rename_area->SetCornerRadius(FromDIP(15));
        rename_area->SetBorderWidth(1);
        rename_area->SetBorderColorNormal(wxColour("#C7C7C7"));
        rename_area->SetBackgroundColorNormal(selected ? wxColour("#F3F8F1") : k_card_bg);
        rename_area->SetBackgroundColour(selected ? wxColour("#F3F8F1") : k_card_bg);
        rename_area->SetCursor(wxCursor(wxCURSOR_HAND));
        rename_area->SetToolTip(_L("Edit printer name"));
        auto *rename_sizer = new wxBoxSizer(wxVERTICAL);
        rename_sizer->AddStretchSpacer(1);
        auto *rename_icon = new wxStaticBitmap(
            rename_area,
            wxID_ANY,
            create_scaled_bitmap("rename_edit", this, 16));
        rename_icon->SetCursor(wxCursor(wxCURSOR_HAND));
        rename_icon->SetToolTip(_L("Edit printer name"));
        rename_sizer->Add(rename_icon, 0, wxALIGN_CENTER_HORIZONTAL);
        rename_sizer->AddStretchSpacer(1);
        rename_area->SetSizer(rename_sizer);
        rename_area->Bind(wxEVT_LEFT_DOWN, open_rename);
        rename_icon->Bind(wxEVT_LEFT_DOWN, open_rename);
        actions_sizer->Add(rename_area, 0, wxALIGN_CENTER_VERTICAL);
        actions->SetSizer(actions_sizer);
        outer->Add(actions, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        card->SetSizer(outer);
        auto pick = [open_machine_device_page_fn, machine, flash_connecting, online](wxMouseEvent &) {
            // Only show Connecting for offline printers — flashing it on an already
            // connected card left a yellow "Bağlanıyor..." over the green checkmark.
            if (!online)
                flash_connecting();
            open_machine_device_page_fn(machine);
        };
        card->Bind(wxEVT_LEFT_DOWN, pick);
        dot->Bind(wxEVT_LEFT_DOWN, pick);
        status->Bind(wxEVT_LEFT_DOWN, pick);
        name->Bind(wxEVT_LEFT_DOWN, pick);
        ip->Bind(wxEVT_LEFT_DOWN, pick);
        m_sidebar_printer_list_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxTOP, FromDIP(12));
    };

    if (!online_list.empty()) {
        add_section_title(_L("Active printers"));
        for (auto *machine : online_list)
            add_printer_card(machine);
    }
    if (!offline_list.empty()) {
        add_section_title(_L("Offline printers"));
        for (auto *machine : offline_list)
            add_printer_card(machine);
    }

    m_sidebar_printer_list_panel->Layout();
    if (auto *scrolled = dynamic_cast<wxScrolledWindow *>(m_sidebar_printer_list_panel)) {
        scrolled->FitInside();
        scrolled->SetMinSize(wxSize(-1, FromDIP(390)));
        scrolled->SetMaxSize(wxSize(-1, FromDIP(390)));
        update_sidebar_scrollbar(scrolled, m_sidebar_printer_scroll_track, this);
    }
    if (m_sidebar_printer_list_container != nullptr)
        m_sidebar_printer_list_container->Layout();
    if (m_sidebar_printer_list_panel->GetParent() != nullptr)
        m_sidebar_printer_list_panel->GetParent()->Layout();
}



void PrinterWebView::apply_filament_preview_rows(const std::array<wxColour, 4> &model_colors,
                                                 const std::array<wxString, 4> &materials,
                                                 const std::array<wxString, 4> &weights,
                                                 const std::array<int, 4> &assigned_tools,
                                                 const std::array<wxColour, 4> &assigned_colors)
{
    update_dashboard_filament_state(model_colors, materials, weights, assigned_tools, assigned_colors);

    for (int i = 0; i < 4; ++i) {
        const int ui_tool = assigned_tools[i] >= 1 && assigned_tools[i] <= 4 ? assigned_tools[i] : i + 1;
        m_filament_assigned_tool_mapping[i] = ui_tool;
    }

    // Keep the dashboard's selected tool state stable after Moonraker DB colors arrive.
    apply_filament_tool_selection(m_selected_filament_tool);
}

void PrinterWebView::update_dashboard_filament_state(const std::array<wxColour, 4> &model_colors,
                                                     const std::array<wxString, 4> &materials,
                                                     const std::array<wxString, 4> &weights,
                                                     const std::array<int, 4> &assigned_tools,
                                                     const std::array<wxColour, 4> &assigned_colors)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const bool can_load_unload = obj != nullptr && obj->is_online() && !obj->is_in_printing();

    m_dashboard_state_store.update([&](DeviceDashboard::DeviceDashboardState &state) {
        for (int i = 0; i < 4; ++i) {
            const int ui_tool = assigned_tools[i] >= 1 && assigned_tools[i] <= 4 ? assigned_tools[i] : i + 1;
            const int tool_index = ui_tool - 1;

            state.filament.model_colors[i] = model_colors[i];
            state.filament.model_materials[i] = materials[i].empty() ? wxString("PLA") : materials[i];
            state.filament.model_weights[i] = weights[i].empty() ? wxString("--") : weights[i];
            state.filament.model_slot_to_tool[i] = tool_index;
            state.filament.assigned_colors[i] = assigned_colors[i];

            state.filament.tools[i].index = i;
            state.filament.tools[i].label = wxString::Format("T%d", i + 1);
            if (m_filament_tool_has_color[i]) {
                state.filament.tools[i].color = m_filament_loaded_tool_colors[i];
                if (m_filament_loaded_tool_materials[i].empty() || m_filament_loaded_tool_materials[i] == "N/A")
                    state.filament.tools[i].material = wxString::FromUTF8("Empty");
                else
                    state.filament.tools[i].material = m_filament_loaded_tool_materials[i];
            } else {
                state.filament.tools[i].color = wxColour();
                state.filament.tools[i].material = wxString::FromUTF8("Empty");
            }
        }
        state.filament.selected_tool = std::clamp(m_selected_filament_tool, 0, 3);
        state.filament.can_load_unload = can_load_unload;
    });

    if (m_dashboard_page != nullptr)
        m_dashboard_page->filament_panel()->apply_state(m_dashboard_state_store.state().filament);
}

void PrinterWebView::set_filament_assigned_tool(int model_slot_index, int ui_tool, bool send_mapping_command)
{
    if (model_slot_index < 0 || model_slot_index >= 4)
        return;
    if (ui_tool < 1 || ui_tool > 4)
        ui_tool = model_slot_index + 1;

    m_filament_assigned_tool_mapping[model_slot_index] = ui_tool;

    m_dashboard_state_store.update([&](DeviceDashboard::DeviceDashboardState &state) {
        state.filament.model_slot_to_tool[model_slot_index] = ui_tool - 1;
        state.filament.assigned_colors[model_slot_index] = m_filament_loaded_tool_colors[ui_tool - 1];
    });
    if (m_dashboard_page != nullptr)
        m_dashboard_page->filament_panel()->apply_state(m_dashboard_state_store.state().filament);

    if (send_mapping_command)
        send_tool_map_command(model_slot_index, ui_tool);
}

void PrinterWebView::send_tool_map_command(int model_slot_index, int ui_tool)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    const int logical_index = std::max(0, std::min(3, model_slot_index));
    const int physical_index = std::max(0, std::min(3, ui_tool - 1));
    const std::string script = "SET_TOOL_MAP LOGICAL=" + std::to_string(logical_index) +
                               " PHYSICAL=" + std::to_string(physical_index);

    BOOST_LOG_TRIVIAL(info) << "PrinterWebView: sending tool map command: " << script;

    std::thread([base, script]() {
        nlohmann::json payload;
        payload["script"] = script;
        Http::post(base + "/printer/gcode/script")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: SET_TOOL_MAP status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: SET_TOOL_MAP failed status=" << status << " error=" << error;
            })
            .perform_sync();
    }).detach();
}

bool PrinterWebView::send_tool_select_command(int tool_index)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return false;
    if (obj->is_in_printing()) {
        BOOST_LOG_TRIVIAL(info) << "PrinterWebView: ignoring tool select while printing";
        return true;
    }

    const int macro_index = std::max(0, std::min(DeviceDashboard::MaxDashboardTools - 1, tool_index));
    const std::string script = "T" + std::to_string(macro_index);

    BOOST_LOG_TRIVIAL(info) << "PrinterWebView: sending tool select command: " << script;

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, script]() {
        nlohmann::json payload;
        payload["script"] = script;
        unsigned response_status = 0;
        Http::post(base + "/printer/gcode/script")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string, unsigned status) {
                response_status = status;
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: " << script << " status=" << status;
            })
            .on_error([&](std::string, std::string error, unsigned status) {
                response_status = status;
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: " << script << " failed status=" << status << " error=" << error;
            })
            .perform_sync();

        if (response_status >= 200 && response_status < 300)
            wxGetApp().CallAfter([this, lifetime]() {
                if (lifetime.expired() || m_destroying)
                    return;
                refresh_moonraker_status_from_selected_machine();
            });
    }).detach();

    return true;
}

bool PrinterWebView::send_print_control_command(bool stop_print)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return false;

    const std::string action          = stop_print ? "CANCEL_PRINT" : "PAUSE";
    const std::string endpoint        = stop_print ? "/printer/print/cancel" : "/printer/print/pause";
    const std::string fallback_script = action;
    BOOST_LOG_TRIVIAL(info) << "PrinterWebView: sending print control command: " << action << " via " << endpoint;

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, endpoint, fallback_script, action]() {
        auto post_json = [](const std::string& url, const nlohmann::json& payload, unsigned& response_status) {
            bool request_ok = false;
            Http::post(url)
                .header("Content-Type", "application/json")
                .set_post_body(payload.dump())
                .timeout_connect(2)
                .timeout_max(8)
                .on_complete([&](std::string, unsigned status) {
                    response_status = status;
                    request_ok = status >= 200 && status < 300;
                })
                .on_error([&](std::string, std::string error, unsigned status) {
                    response_status = status;
                    BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: print control request failed status=" << status << " error=" << error;
                })
                .perform_sync();
            return request_ok;
        };

        unsigned response_status = 0;
        bool success = post_json(base + endpoint, nlohmann::json::object(), response_status);
        BOOST_LOG_TRIVIAL(info) << "PrinterWebView: " << action << " endpoint status=" << response_status;

        if (!success) {
            nlohmann::json fallback_payload;
            fallback_payload["script"] = fallback_script;
            response_status = 0;
            success = post_json(base + "/printer/gcode/script", fallback_payload, response_status);
            BOOST_LOG_TRIVIAL(info) << "PrinterWebView: " << action << " fallback status=" << response_status;
        }

        if (success)
            wxGetApp().CallAfter([this, lifetime]() {
                if (lifetime.expired() || m_destroying)
                    return;
                refresh_moonraker_status_from_selected_machine();
            });
    }).detach();

    return true;
}

void PrinterWebView::apply_filament_preview_fallback()
{
    // If we have colors synced from the Plater (set at upload time), use them
    // instead of full defaults so that the Model Colors section keeps showing
    // the uploaded model's colors even while the printer is idle.
    if (m_has_plater_synced_colors) {
        apply_filament_preview_rows(
            m_plater_synced_colors,
            m_plater_synced_materials,
            default_filament_preview_weights(),
            default_filament_preview_assigned_tools(),
            m_plater_synced_colors);
        return;
    }
    const auto colors = default_filament_preview_colors();
    apply_filament_preview_rows(
        colors,
        default_filament_preview_materials(),
        default_filament_preview_weights(),
        default_filament_preview_assigned_tools(),
        colors);
}

void PrinterWebView::sync_model_colors_from_plater()
{
    auto model_colors = default_filament_preview_colors();
    auto materials    = default_filament_preview_materials();
    bool got_colors   = false;

    // Preferred source: slice_filaments_info from the current plate.
    // This contains the ACTUAL colors embedded in the sliced gcode, which
    // reflect the model's real color groups â€” not just the filament preset colors.
    if (auto *plater = wxGetApp().plater()) {
        PartPlate *plate = plater->get_partplate_list().get_curr_plate();
        if (plate != nullptr && !plate->get_slice_filaments_info().empty()) {
            for (const auto &fi : plate->get_slice_filaments_info()) {
                const int idx = fi.id; // 0-based extruder index
                if (idx >= 0 && idx < 4) {
                    if (!fi.color.empty())
                        model_colors[idx] = colour_from_hex(fi.color, model_colors[idx]);
                    if (!fi.type.empty())
                        materials[idx] = wxString::FromUTF8(fi.type);
                    got_colors = true;
                }
            }
        }
    }

    // Fallback: filament_colour from the project config (preset colors).
    if (!got_colors) {
        auto *preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle == nullptr)
            return;
        const DynamicPrintConfig &project_config = preset_bundle->project_config;
        const auto *color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
        // Store full_config to prevent dangling pointer (full_config() returns temporary by value)
        const DynamicPrintConfig full_config = preset_bundle->full_config();
        const auto *type_opt = full_config.option<ConfigOptionStrings>("filament_type");
        if (color_opt == nullptr || color_opt->values.empty())
            return;
        for (size_t i = 0; i < std::min<size_t>(4, color_opt->values.size()); ++i) {
            const std::string &hex = color_opt->values[i];
            if (!hex.empty()) {
                model_colors[i] = colour_from_hex(hex, model_colors[i]);
                got_colors = true;
            }
        }
        if (type_opt != nullptr) {
            for (size_t i = 0; i < std::min<size_t>(4, type_opt->values.size()); ++i) {
                if (!type_opt->values[i].empty())
                    materials[i] = wxString::FromUTF8(type_opt->values[i]);
            }
        }
    }

    if (!got_colors)
        return;

    // Keep the assigned (loaded tool) colors already fetched from the device,
    // or fall back to the model colors themselves when no device data is present.
    const bool has_tool_colors = std::any_of(
        m_filament_tool_has_color.begin(), m_filament_tool_has_color.end(),
        [](bool has_color) { return has_color; });

    std::array<wxColour, 4> assigned_colors = has_tool_colors
        ? m_filament_loaded_tool_colors
        : model_colors;

    std::array<int, 4> assigned_tools = has_tool_colors
        ? nearest_unique_tool_assignment(model_colors, assigned_colors)
        : default_filament_preview_assigned_tools();

    if (has_tool_colors) {
        std::array<wxColour, 4> row_colors = assigned_colors;
        for (int i = 0; i < 4; ++i)
            row_colors[i] = assigned_colors[assigned_tools[i] - 1];
        assigned_colors = row_colors;
    }

    // Save for use as fallback when the printer is idle (no active file metadata)
    m_plater_synced_colors    = model_colors;
    m_plater_synced_materials = materials;
    m_has_plater_synced_colors = true;

    apply_filament_preview_rows(model_colors, materials,
                                default_filament_preview_weights(),
                                assigned_tools, assigned_colors);

    // Invalidate the fetch key so the next full device refresh re-fetches metadata
    m_filament_preview_fetch_key.clear();
}

void PrinterWebView::refresh_filament_preview_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    const wxString display_file_name = active_file_name_text(obj);
    const wxString metadata_file_path = active_file_metadata_path(obj);
    const bool has_file = !metadata_file_path.empty();

    if (obj == nullptr || !obj->is_online() || base.empty()) {
        m_filament_preview_fetch_key.clear();
        m_filament_preview_fetch_in_progress = false;
        m_filament_tool_has_color.fill(false);
        for (int i = 0; i < 4; ++i) {
            m_filament_loaded_tool_colors[i] = wxColour();
            m_filament_loaded_tool_materials[i] = wxString::FromUTF8("Empty");
        }
        apply_filament_preview_fallback();
        return;
    }

    if (!has_file && !m_has_plater_synced_colors)
        sync_model_colors_from_plater();

    const wxString key = from_u8(base) + "|" + (has_file ? metadata_file_path : display_file_name);
    if (m_filament_preview_fetch_in_progress || key == m_filament_preview_fetch_key)
        return;

    m_filament_preview_fetch_in_progress = true;
    m_filament_preview_fetch_key = key;

    const std::string metadata_url = has_file
        ? base + "/server/files/metadata?filename=" + url_encode_component(metadata_file_path)
        : std::string();
    const std::string db_url = base + "/server/database/item?namespace=coprint&key=filament_selections";

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, key, metadata_url, db_url]() {
        auto fetch_json_text = [](const std::string &url) {
            std::string body;
            if (url.empty())
                return body;
            Http::get(url)
                .timeout_connect(2)
                .timeout_max(4)
                .on_complete([&](std::string response, unsigned status) {
                    if (status == 200)
                        body = std::move(response);
                })
                .on_error([](std::string, std::string, unsigned) {})
                .perform_sync();
            return body;
        };

        const std::string metadata_body = fetch_json_text(metadata_url);
        const std::string db_body = fetch_json_text(db_url);

        wxGetApp().CallAfter([this, lifetime, key, metadata_body, db_body]() {
            if (lifetime.expired() || m_destroying)
                return;
            m_filament_preview_fetch_in_progress = false;
            if (key != m_filament_preview_fetch_key)
                return;

            auto meta   = parse_filament_metadata(parse_json_body(metadata_body));
            auto loaded = parse_filament_db(parse_json_body(db_body));

            for (int i = 0; i < 4; ++i) {
                m_filament_tool_has_color[i] = loaded.tool_has_color[i];
                if (loaded.tool_has_color[i]) {
                    m_filament_loaded_tool_colors[i] = loaded.assigned_colors[i];
                    m_filament_loaded_tool_materials[i] = loaded.materials[i].empty()
                        ? wxString::FromUTF8("Empty")
                        : loaded.materials[i];
                } else {
                    m_filament_loaded_tool_colors[i] = wxColour();
                    m_filament_loaded_tool_materials[i] = wxString::FromUTF8("Empty");
                }
            }

            auto assigned_tools  = default_filament_preview_assigned_tools();
            auto assigned_colors = loaded.assigned_colors;
            if (loaded.has_colors) {
                assigned_tools = nearest_unique_tool_assignment(meta.model_colors, loaded.assigned_colors);
                for (int i = 0; i < 4; ++i)
                    assigned_colors[i] = loaded.assigned_colors[assigned_tools[i] - 1];
            }

            // If metadata returned no model colors, keep the colors synced from
            // the Plater, but still apply the loaded-filament / Assigned Tools
            // information fetched from Moonraker DB above.
            if (!meta.colors_parsed && m_has_plater_synced_colors) {
                meta.model_colors = m_plater_synced_colors;
                meta.materials    = m_plater_synced_materials;
                if (loaded.has_colors) {
                    assigned_tools = nearest_unique_tool_assignment(meta.model_colors, m_filament_loaded_tool_colors);
                    for (int i = 0; i < 4; ++i)
                        assigned_colors[i] = m_filament_loaded_tool_colors[assigned_tools[i] - 1];
                }
                apply_filament_preview_rows(meta.model_colors, meta.materials, meta.weights, assigned_tools, assigned_colors);
                return;
            }

            if (meta.colors_parsed)
                m_has_plater_synced_colors = false;

            apply_filament_preview_rows(meta.model_colors, meta.materials, meta.weights, assigned_tools, assigned_colors);
        });
    }).detach();
}

void PrinterWebView::sync_loaded_tool_filaments(MachineObject *obj, std::function<void()> on_done)
{
    auto finish = [on_done = std::move(on_done)]() {
        if (on_done)
            on_done();
    };

    if (m_destroying || obj == nullptr || !obj->is_online()) {
        finish();
        return;
    }
    const std::string base = moonraker_base_url(obj);
    if (base.empty()) {
        finish();
        return;
    }

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, finish = std::move(finish)]() {
        std::string db_body;
        Http::get(base + "/server/database/item?namespace=coprint&key=filament_selections")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    db_body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        wxGetApp().CallAfter([this, lifetime, db_body, finish]() {
            if (!lifetime.expired() && !m_destroying && !db_body.empty()) {
                const auto loaded = parse_filament_db(parse_json_body(db_body));
                for (int i = 0; i < 4; ++i) {
                    m_filament_tool_has_color[i] = loaded.tool_has_color[i];
                    if (loaded.tool_has_color[i]) {
                        m_filament_loaded_tool_colors[i] = loaded.assigned_colors[i];
                        m_filament_loaded_tool_materials[i] = loaded.materials[i].empty()
                            ? wxString::FromUTF8("Empty")
                            : loaded.materials[i];
                    }
                }
            }
            finish();
        });
    }).detach();
}

bool PrinterWebView::get_loaded_tool_filament(int tool_0based, wxColour *color_out, wxString *material_out) const
{
    if (tool_0based < 0 || tool_0based >= 4 || !m_filament_tool_has_color[tool_0based])
        return false;
    const wxColour &cached = m_filament_loaded_tool_colors[tool_0based];
    if (!cached.IsOk())
        return false;
    if (color_out)
        *color_out = cached;
    if (material_out)
        *material_out = m_filament_loaded_tool_materials[tool_0based];
    return true;
}

bool PrinterWebView::show_filament_material_dialog(bool start_load_after_save, const wxPoint& anchor_screen_pos)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (start_load_after_save && (obj == nullptr || !obj->is_online() || obj->is_in_printing()))
        return false;

    const int ui_tool = m_selected_filament_tool + 1;
    FilamentMaterialDialog dialog(
        this,
        ui_tool,
        m_filament_loaded_tool_materials[m_selected_filament_tool],
        m_filament_loaded_tool_colors[m_selected_filament_tool]);
    position_dialog_near_anchor(dialog, this, anchor_screen_pos);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    const wxString material = dialog.material();
    const wxString color_hex = dialog.color_hex();
    m_filament_loaded_tool_materials[m_selected_filament_tool] = material;
    m_filament_loaded_tool_colors[m_selected_filament_tool] = colour_from_hex(into_u8(color_hex), m_filament_loaded_tool_colors[m_selected_filament_tool]);
    m_filament_tool_has_color[m_selected_filament_tool] = true;
    m_dashboard_state_store.update([&](DeviceDashboard::DeviceDashboardState &state) {
        auto &tool = state.filament.tools[m_selected_filament_tool];
        tool.color = m_filament_loaded_tool_colors[m_selected_filament_tool];
        tool.material = material;
    });
    apply_filament_tool_selection(m_selected_filament_tool);
    save_filament_selection_to_moonraker(ui_tool, material, color_hex);
    if (start_load_after_save)
        show_filament_load_wizard();
    return true;
}

void PrinterWebView::prompt_and_save_filament_selection_then_load()
{
    show_filament_load_wizard();
}

void PrinterWebView::save_filament_selection_to_moonraker(int ui_tool, const wxString &material, const wxString &color_hex)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    ui_tool = std::max(1, std::min(4, ui_tool));
    const std::string material_utf8 = into_u8(material);
    const std::string color_utf8 = into_u8(color_hex);

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, ui_tool, material_utf8, color_utf8]() {
        nlohmann::json value = nlohmann::json::object();
        std::string body;
        Http::get(base + "/server/database/item?namespace=coprint&key=filament_selections")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        if (!body.empty()) {
            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (!parsed.is_discarded()) {
                if (parsed.contains("result"))
                    parsed = parsed["result"];
                if (parsed.is_object() && parsed.contains("value") && parsed["value"].is_object())
                    value = parsed["value"];
            }
        }

        value.erase("toolhead_" + std::to_string(ui_tool - 1));
        value["toolhead_" + std::to_string(ui_tool)] = {
            { "type", material_utf8 },
            { "color_hex", color_utf8 }
        };

        nlohmann::json payload;
        payload["namespace"] = "coprint";
        payload["key"] = "filament_selections";
        payload["value"] = value;

        Http::post(base + "/server/database/item")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: filament selection saved status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: filament selection save failed status=" << status << " error=" << error;
            })
            .perform_sync();

        wxGetApp().CallAfter([this, lifetime]() {
            if (lifetime.expired() || m_destroying)
                return;
            m_filament_preview_fetch_key.clear();
            refresh_filament_preview_from_selected_machine();
        });
    }).detach();
}

void PrinterWebView::clear_filament_selection_from_moonraker(int ui_tool)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return;

    ui_tool = std::max(1, std::min(4, ui_tool));
    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, ui_tool]() {
        nlohmann::json value = nlohmann::json::object();
        std::string body;
        Http::get(base + "/server/database/item?namespace=coprint&key=filament_selections")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        if (!body.empty()) {
            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (!parsed.is_discarded()) {
                if (parsed.contains("result"))
                    parsed = parsed["result"];
                if (parsed.is_object() && parsed.contains("value") && parsed["value"].is_object())
                    value = parsed["value"];
            }
        }

        value.erase("toolhead_" + std::to_string(ui_tool));
        value.erase("toolhead_" + std::to_string(ui_tool - 1));

        nlohmann::json payload;
        payload["namespace"] = "coprint";
        payload["key"] = "filament_selections";
        payload["value"] = value;

        Http::post(base + "/server/database/item")
            .header("Content-Type", "application/json")
            .set_post_body(payload.dump())
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: filament selection cleared status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: filament selection clear failed status=" << status << " error=" << error;
            })
            .perform_sync();

        wxGetApp().CallAfter([this, lifetime]() {
            if (lifetime.expired() || m_destroying)
                return;
            m_filament_preview_fetch_key.clear();
            refresh_filament_preview_from_selected_machine();
        });
    }).detach();
}

void PrinterWebView::refresh_moonraker_status_from_selected_machine()
{
    if (m_destroying)
        return;
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    const std::string machine_id = obj != nullptr ? obj->get_dev_id() : std::string();

    // CoPrint talks to Moonraker over HTTP. Do not wait for Bambu is_online()
    // (push_status / m_push_count); that flag stays false for LAN printers
    // until JSON arrives, which never happens if we skip this fetch.
    if (obj == nullptr || base.empty()) {
        m_has_moonraker_status = false;
        m_has_moonraker_print_status = false;
        m_moonraker_print_job = DeviceDashboard::PrintJobState();
        m_moonraker_available_tool_count = 0;
        m_moonraker_status_fetch_in_progress = false;
        m_moonraker_status_machine_id.clear();
        return;
    }

    if (m_moonraker_status_machine_id != machine_id) {
        m_has_moonraker_status = false;
        m_has_moonraker_print_status = false;
        m_moonraker_print_job = DeviceDashboard::PrintJobState();
        m_moonraker_available_tool_count = 0;
        m_moonraker_status_fetch_in_progress = false;
        m_moonraker_status_machine_id = machine_id;
    }

    if (m_moonraker_status_fetch_in_progress)
        return;

    m_moonraker_status_fetch_in_progress = true;
    const std::string status_query_url = base +
        "/printer/objects/query?extruder=temperature,target"
        "&extruder1=temperature,target"
        "&extruder2=temperature,target"
        "&extruder3=temperature,target"
        "&heater_bed=temperature,target"
        "&toolhead=extruder"
        "&print_stats=filename,state,total_duration,print_duration,info"
        "&virtual_sdcard=progress";
    const std::string fan_query_url = base +
        "/printer/objects/query?fan=speed,power"
        "&fan_generic%20fan_t0=speed,rpm"
        "&fan_generic%20fan_t1=speed,rpm"
        "&fan_generic%20fan_t2=speed,rpm"
        "&fan_generic%20fan_t3=speed,rpm";

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, machine_id, base, status_query_url, fan_query_url]() {
        std::string body;
        std::string fan_body;
        std::string coprint_info_body;
        std::string metadata_body;
        Http::get(status_query_url)
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        if (!body.empty()) {
            auto parsed_status = nlohmann::json::parse(body, nullptr, false, true);
            if (!parsed_status.is_discarded()) {
                if (parsed_status.contains("result"))
                    parsed_status = parsed_status["result"];
                if (parsed_status.is_object() && parsed_status.contains("status") && parsed_status["status"].is_object()) {
                    const auto &status = parsed_status["status"];
                    if (status.contains("print_stats") && status["print_stats"].is_object()) {
                        const auto &print_stats = status["print_stats"];
                        if (print_stats.contains("filename") && print_stats["filename"].is_string()) {
                            const std::string raw_filename = print_stats["filename"].get<std::string>();
                            if (!raw_filename.empty()) {
                                const std::string metadata_url = base + "/server/files/metadata?filename=" + url_encode_path_preserving_slashes(raw_filename);
                                Http::get(metadata_url)
                                    .timeout_connect(2)
                                    .timeout_max(4)
                                    .on_complete([&](std::string response, unsigned status_code) {
                                        if (status_code == 200)
                                            metadata_body = std::move(response);
                                    })
                                    .on_error([](std::string, std::string, unsigned) {})
                                    .perform_sync();
                            }
                        }
                    }
                }
            }
        }

        Http::get(fan_query_url)
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    fan_body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        Http::get(base + "/machine/coprint/info")
            .timeout_connect(2)
            .timeout_max(5)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    coprint_info_body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        // Also use objects/list when /machine/coprint/info is missing/slow — Quadro has extruder1+.
        std::string objects_list_body;
        Http::get(base + "/printer/objects/list")
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string response, unsigned status) {
                if (status == 200)
                    objects_list_body = std::move(response);
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();

        // Resolve Moonraker hostname so placeholder names like "Unknown Printer"
        // can be upgraded to Co Print product names (e.g. chromahead -> ChromaSet).
        std::string hostname_hint;
        auto capture_hostname = [&](const std::string &path) {
            if (!hostname_hint.empty())
                return;
            std::string response_body;
            Http::get(base + path)
                .timeout_connect(2)
                .timeout_max(4)
                .on_complete([&](std::string response, unsigned status) {
                    if (status == 200)
                        response_body = std::move(response);
                })
                .on_error([](std::string, std::string, unsigned) {})
                .perform_sync();
            if (response_body.empty())
                return;
            auto parsed_host = nlohmann::json::parse(response_body, nullptr, false, true);
            if (parsed_host.is_discarded())
                return;
            if (parsed_host.contains("result"))
                parsed_host = parsed_host["result"];
            if (!parsed_host.is_object())
                return;
            if (parsed_host.contains("machine_name") && parsed_host["machine_name"].is_string())
                hostname_hint = parsed_host["machine_name"].get<std::string>();
            else if (parsed_host.contains("hostname") && parsed_host["hostname"].is_string())
                hostname_hint = parsed_host["hostname"].get<std::string>();
            else if (parsed_host.contains("system_info") && parsed_host["system_info"].is_object() &&
                     parsed_host["system_info"].contains("hostname") &&
                     parsed_host["system_info"]["hostname"].is_string())
                hostname_hint = parsed_host["system_info"]["hostname"].get<std::string>();
        };
        capture_hostname("/server/info");
        capture_hostname("/printer/info");
        capture_hostname("/machine/system_info");

        wxGetApp().CallAfter([this, lifetime, machine_id, body, fan_body, coprint_info_body, objects_list_body, metadata_body, hostname_hint, base]() {
            if (lifetime.expired() || m_destroying)
                return;
            if (m_moonraker_status_machine_id == machine_id)
                m_moonraker_status_fetch_in_progress = false;
            auto *dev_manager = wxGetApp().getDeviceManager();
            MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
            if (obj == nullptr || obj->get_dev_id() != machine_id)
                return;

            auto upgrade_placeholder_identity = [&](std::string detected_name, std::string detected_type, int moonraker_tool_count) {
                if (detected_type.empty() && !coprint_info_body.empty()) {
                    auto coprint_info = nlohmann::json::parse(coprint_info_body, nullptr, false, true);
                    if (!coprint_info.is_discarded())
                        coprint_identity_from_info_json(coprint_info, detected_name, detected_type);
                }

                if (detected_type.empty() && !objects_list_body.empty()) {
                    auto objects_json = nlohmann::json::parse(objects_list_body, nullptr, false, true);
                    if (!objects_json.is_discarded()) {
                        if (objects_json.contains("result"))
                            objects_json = objects_json["result"];
                        if (objects_json.is_object() && objects_json.contains("objects") && objects_json["objects"].is_array()) {
                            bool has_extruder = false;
                            bool has_extra = false;
                            for (const auto &object_name : objects_json["objects"]) {
                                if (!object_name.is_string())
                                    continue;
                                const std::string name = object_name.get<std::string>();
                                if (name == "extruder")
                                    has_extruder = true;
                                else if (name == "extruder1" || name == "extruder2" || name == "extruder3")
                                    has_extra = true;
                            }
                            if (has_extra)
                                detected_type = "Co_Print_Quadro";
                            else if (has_extruder)
                                detected_type = "Co_Print_ChromaSet";
                        }
                    }
                }

                if (detected_type.empty() && !hostname_hint.empty()) {
                    std::string mapped_name;
                    apply_coprint_name_from_hostname(hostname_hint, mapped_name, detected_type);
                }

                if (detected_type.empty() && moonraker_tool_count > 0) {
                    const std::string cached_identity = to_lower_ascii(
                        obj->get_dev_name() + " " +
                        obj->printer_type + " " +
                        into_u8(obj->get_printer_type_display_str()) + " " +
                        hostname_hint);
                    const bool known_coprint =
                        cached_identity.find("coprint") != std::string::npos ||
                        cached_identity.find("co print") != std::string::npos ||
                        hostname_looks_like_quadro(cached_identity) ||
                        cached_identity.find("chroma") != std::string::npos ||
                        cached_identity.find("chromahead") != std::string::npos;
                    const bool placeholder_name = is_placeholder_printer_name(obj->get_dev_name());
                    if (known_coprint || placeholder_name) {
                        if (moonraker_tool_count >= 2)
                            detected_type = "Co_Print_Quadro";
                        else if (moonraker_tool_count == 1)
                            detected_type = "Co_Print_ChromaSet";
                    }
                }

                std::string display_name;
                if (!hostname_hint.empty() && !is_placeholder_printer_name(hostname_hint))
                    display_name = hostname_hint;
                else if (!detected_name.empty() && !is_placeholder_printer_name(detected_name))
                    display_name = detected_name;

                if (detected_type.empty() && display_name.empty())
                    return;

                const bool type_changed = !detected_type.empty() && obj->printer_type != detected_type;
                const bool name_should_update =
                    !display_name.empty() &&
                    display_name != obj->get_dev_name() &&
                    is_placeholder_printer_name(obj->get_dev_name());
                if (!type_changed && !name_should_update)
                    return;

                if (name_should_update)
                    obj->set_dev_name(display_name);
                if (type_changed)
                    obj->printer_type = detected_type;
                DeviceManager::update_local_machine(*obj);
                m_sidebar_printer_list_signature.clear();
                rebuild_sidebar_printer_list();
            };

            // Even if status JSON is missing, hostname alone can fix "Unknown Printer".
            if (body.empty()) {
                upgrade_placeholder_identity({}, {}, 0);
                return;
            }

            auto parsed = nlohmann::json::parse(body, nullptr, false, true);
            if (parsed.is_discarded()) {
                upgrade_placeholder_identity({}, {}, 0);
                return;
            }
            if (parsed.contains("result"))
                parsed = parsed["result"];
            if (!parsed.is_object() || !parsed.contains("status") || !parsed["status"].is_object()) {
                upgrade_placeholder_identity({}, {}, 0);
                return;
            }

            const auto &status = parsed["status"];
            bool got_any = false;
            int active_tool_index = -1;
            int moonraker_tool_count = 0;
            int coprint_info_tool_count = 0;
            std::string detected_coprint_name;
            std::string detected_coprint_type;
            if (!coprint_info_body.empty()) {
                auto coprint_info = nlohmann::json::parse(coprint_info_body, nullptr, false, true);
                if (!coprint_info.is_discarded()) {
                    coprint_info_tool_count = coprint_tool_count_from_info_json(coprint_info);
                    coprint_identity_from_info_json(coprint_info, detected_coprint_name, detected_coprint_type);
                }
            }
            for (int i = 0; i < 4; ++i) {
                const std::string object_name = i == 0 ? "extruder" : "extruder" + std::to_string(i);
                if (!status.contains(object_name) || !status[object_name].is_object())
                    continue;
                const auto &tool = status[object_name];
                const bool has_temperature = tool.contains("temperature") && tool["temperature"].is_number();
                const bool has_target = tool.contains("target") && tool["target"].is_number();
                if (!has_temperature && !has_target)
                    continue;

                moonraker_tool_count = std::max(moonraker_tool_count, i + 1);
                if (has_temperature) {
                    m_moonraker_nozzle_current[i] = tool["temperature"].get<double>();
                    got_any = true;
                }
                if (has_target) {
                    m_moonraker_nozzle_target[i] = tool["target"].get<double>();
                    got_any = true;
                }
            }

            upgrade_placeholder_identity(detected_coprint_name, detected_coprint_type, moonraker_tool_count);

            if (status.contains("toolhead") && status["toolhead"].is_object()) {
                const auto &toolhead = status["toolhead"];
                if (toolhead.contains("extruder") && toolhead["extruder"].is_string()) {
                    active_tool_index = active_tool_index_from_moonraker_extruder(toolhead["extruder"].get<std::string>());
                    got_any = got_any || active_tool_index >= 0;
                }
            }

            if (status.contains("heater_bed") && status["heater_bed"].is_object()) {
                const auto &bed = status["heater_bed"];
                if (bed.contains("temperature") && bed["temperature"].is_number()) {
                    m_moonraker_bed_current = bed["temperature"].get<double>();
                    got_any = true;
                }
                if (bed.contains("target") && bed["target"].is_number()) {
                    m_moonraker_bed_target = bed["target"].get<double>();
                    got_any = true;
                }
            }

            DeviceDashboard::PrintJobState moonraker_print_job;
            bool got_print_state = false;
            double print_duration = 0.0;
            int estimated_total_seconds = -1;
            if (status.contains("print_stats") && status["print_stats"].is_object()) {
                const auto &print_stats = status["print_stats"];
                wxString print_state;
                if (print_stats.contains("state") && print_stats["state"].is_string())
                    print_state = from_u8(print_stats["state"].get<std::string>()).Lower();

                moonraker_print_job.has_active_job = print_state == "printing" || print_state == "paused";
                got_print_state = !print_state.empty();
                if (moonraker_print_job.has_active_job) {
                    if (print_stats.contains("filename") && print_stats["filename"].is_string())
                        moonraker_print_job.file_name = clean_moonraker_print_filename(from_u8(print_stats["filename"].get<std::string>()));
                    if (print_stats.contains("print_duration") && print_stats["print_duration"].is_number())
                        print_duration = print_stats["print_duration"].get<double>();
                    if (print_stats.contains("info") && print_stats["info"].is_object()) {
                        const auto &info = print_stats["info"];
                        double value = 0.0;
                        if (moonraker_json_number_value(info, {"current_layer", "current_layer_num", "layer", "layer_num"}, value))
                            moonraker_print_job.current_layer = std::max(0, static_cast<int>(value));
                        if (moonraker_json_number_value(info, {"total_layer", "total_layers", "total_layer_num", "layer_count", "layers"}, value))
                            moonraker_print_job.total_layers = std::max(0, static_cast<int>(value));
                    }
                    moonraker_print_job.thumbnail_url = obj->slice_info != nullptr ? from_u8(obj->slice_info->thumbnail_url) : wxString();
                    got_any = true;
                }
            }

            if (moonraker_print_job.has_active_job) {
                if (status.contains("virtual_sdcard") && status["virtual_sdcard"].is_object()) {
                    const auto &virtual_sdcard = status["virtual_sdcard"];
                    if (virtual_sdcard.contains("progress") && virtual_sdcard["progress"].is_number())
                        moonraker_print_job.progress_percent = std::clamp(
                            static_cast<int>(std::round(virtual_sdcard["progress"].get<double>() * 100.0)), 0, 100);
                    else
                        moonraker_print_job.progress_percent = std::clamp(obj->mc_print_percent, 0, 100);
                } else {
                    moonraker_print_job.progress_percent = std::clamp(obj->mc_print_percent, 0, 100);
                }

                if (!metadata_body.empty()) {
                    auto metadata = nlohmann::json::parse(metadata_body, nullptr, false, true);
                    if (!metadata.is_discarded()) {
                        if (metadata.contains("result"))
                            metadata = metadata["result"];
                        if (metadata.is_object()) {
                            if (moonraker_print_job.thumbnail_url.IsEmpty())
                                moonraker_print_job.thumbnail_url = moonraker_thumbnail_url_from_metadata(metadata, base);

                            if (metadata.contains("estimated_time") && metadata["estimated_time"].is_number())
                                estimated_total_seconds = static_cast<int>(std::round(metadata["estimated_time"].get<double>()));
                            else if (metadata.contains("print_time") && metadata["print_time"].is_number())
                                estimated_total_seconds = static_cast<int>(std::round(metadata["print_time"].get<double>()));

                            if (moonraker_print_job.total_layers <= 0) {
                                if (metadata.contains("layer_count") && metadata["layer_count"].is_number_integer()) {
                                    moonraker_print_job.total_layers = metadata["layer_count"].get<int>();
                                } else if (metadata.contains("total_layer_count") && metadata["total_layer_count"].is_number_integer()) {
                                    moonraker_print_job.total_layers = metadata["total_layer_count"].get<int>();
                                } else if (metadata.contains("object_height") && metadata["object_height"].is_number() &&
                                           metadata.contains("layer_height") && metadata["layer_height"].is_number()) {
                                    const double object_height = metadata["object_height"].get<double>();
                                    const double layer_height = metadata["layer_height"].get<double>();
                                    const double first_layer_height = metadata.contains("first_layer_height") && metadata["first_layer_height"].is_number()
                                        ? metadata["first_layer_height"].get<double>()
                                        : layer_height;
                                    if (object_height > 0.0 && layer_height > 0.0)
                                        moonraker_print_job.total_layers = std::max(1, static_cast<int>(
                                            std::ceil(std::max(0.0, object_height - first_layer_height) / layer_height)) + 1);
                                }
                            }
                        }
                    }
                }

                moonraker_finalize_print_job(moonraker_print_job, obj);
                moonraker_print_job.elapsed_seconds = moonraker_compute_total_estimate_seconds(
                    moonraker_print_job.progress_percent, estimated_total_seconds, print_duration, obj);
                moonraker_print_job.remaining_seconds = moonraker_compute_remaining_seconds(
                    moonraker_print_job.progress_percent,
                    estimated_total_seconds,
                    print_duration,
                    moonraker_print_job.current_layer,
                    moonraker_print_job.total_layers,
                    obj->mc_left_time > 0 ? obj->mc_left_time : -1);

                if (moonraker_print_job.total_layers > 0 && moonraker_print_job.current_layer <= 0)
                    moonraker_print_job.current_layer = std::clamp(
                        static_cast<int>(std::ceil(moonraker_print_job.total_layers * std::clamp(moonraker_print_job.progress_percent, 0, 100) / 100.0)),
                        1,
                        moonraker_print_job.total_layers);
            }
            if (got_print_state) {
                m_has_moonraker_print_status = true;
                m_moonraker_print_job = moonraker_print_job;
            }

            nlohmann::json fan_status_storage;
            const nlohmann::json *fan_status = &status;
            if (!fan_body.empty()) {
                auto parsed_fan = nlohmann::json::parse(fan_body, nullptr, false, true);
                if (!parsed_fan.is_discarded()) {
                    if (parsed_fan.contains("result"))
                        parsed_fan = parsed_fan["result"];
                    if (parsed_fan.is_object() && parsed_fan.contains("status") && parsed_fan["status"].is_object()) {
                        fan_status_storage = parsed_fan["status"];
                        fan_status = &fan_status_storage;
                    }
                }
            }

            int standard_fan_percent = 0;
            bool has_standard_fan = false;
            if (fan_status->contains("fan") && (*fan_status)["fan"].is_object())
                has_standard_fan = moonraker_fan_percent_from_json((*fan_status)["fan"], standard_fan_percent);

            for (int i = 0; i < 4; ++i) {
                const std::string object_name = "fan_generic fan_t" + std::to_string(i);
                int fan_percent = 0;
                if (fan_status->contains(object_name) && (*fan_status)[object_name].is_object() &&
                    moonraker_fan_percent_from_json((*fan_status)[object_name], fan_percent)) {
                    m_moonraker_fan_percent[i] = fan_percent;
                    m_moonraker_fan_available[i] = true;
                    got_any = true;
                } else {
                    m_moonraker_fan_available[i] = false;
                }
            }

            // Single-tool Klipper printers expose [fan] as "fan", not fan_generic fan_t0.
            if (has_standard_fan) {
                const int tool_index = active_tool_index >= 0 ? active_tool_index : 0;
                if (!m_moonraker_fan_available[tool_index] || m_moonraker_fan_percent[tool_index] == 0) {
                    m_moonraker_fan_percent[tool_index] = standard_fan_percent;
                    m_moonraker_fan_available[tool_index] = true;
                    got_any = true;
                }
            }

            m_has_moonraker_status = got_any;
            if (got_any)
                obj->set_online_state(true);
            // Moonraker verisi geldi — sadece sıcaklık panelini güncelle
            // (refresh_layer_info_from_selected_machine() çağırmıyoruz;
            //  o fonksiyon içinde yeni bir Moonraker fetch başlatır → sonsuz döngü)
            if (got_any && m_dashboard_page != nullptr) {
                DeviceDashboard::DeviceDashboardState patched = m_dashboard_state_store.state();
                if (active_tool_index >= 0) {
                    m_selected_extruder_index = active_tool_index;
                    patched.movement.selected_tool = active_tool_index;
                }
                if (coprint_info_tool_count > 0) {
                    m_moonraker_available_tool_count = std::clamp(coprint_info_tool_count, 1, DeviceDashboard::MaxDashboardTools);
                    patched.movement.available_tool_count = m_moonraker_available_tool_count;
                } else if (moonraker_tool_count > 0) {
                    m_moonraker_available_tool_count = std::clamp(moonraker_tool_count, 1, DeviceDashboard::MaxDashboardTools);
                    patched.movement.available_tool_count = m_moonraker_available_tool_count;
                } else {
                    const int model_tool_count = coprint_tool_count_override(obj);
                    if (model_tool_count > 0)
                        patched.movement.available_tool_count = model_tool_count;
                }
                patched.movement.selected_tool = std::clamp(
                    patched.movement.selected_tool,
                    0,
                    std::max(0, patched.movement.available_tool_count - 1));
                patched.bed.temperature.available = true;
                patched.bed.temperature.current   = m_moonraker_bed_current;
                patched.bed.temperature.target    = m_moonraker_bed_target;
                for (int i = 0; i < DeviceDashboard::MaxDashboardTools; ++i) {
                    patched.tools[i].nozzle.available = true;
                    patched.tools[i].nozzle.current   = m_moonraker_nozzle_current[i];
                    patched.tools[i].nozzle.target    = m_moonraker_nozzle_target[i];
                    if (m_moonraker_fan_available[i]) {
                        patched.tools[i].fan.available = true;
                        patched.tools[i].fan.percent   = m_moonraker_fan_percent[i];
                    }
                    if (active_tool_index >= 0)
                        patched.tools[i].active = i == active_tool_index;
                }
                if (got_print_state)
                    patched.print_job = moonraker_print_job;
                if (active_tool_index >= 0 && active_tool_index < patched.movement.available_tool_count && m_dashboard_page->printer_status_panel() != nullptr)
                    m_dashboard_page->printer_status_panel()->set_active_tool(active_tool_index);
                m_dashboard_state_store.set_state(patched);
                m_dashboard_page->apply_state(patched);
            }
            if (m_dashboard_page != nullptr && m_dashboard_page->printer_status_panel() != nullptr)
                m_dashboard_page->printer_status_panel()->Refresh();
            if (got_print_state && m_dashboard_page != nullptr && m_dashboard_page->print_status_panel() != nullptr)
                m_dashboard_page->print_status_panel()->Refresh();
        });
    }).detach();
}

void PrinterWebView::ensure_camera_webview_created()
{
    if (m_destroying || !IsShownOnScreen())
        return;
    if (m_camera_webview_initialized || m_camera_webview_host == nullptr)
        return;
    m_camera_webview_initialized = true;

    wxWebView *const wv = ::WebView::CreateWebView(m_camera_webview_host, wxString{});
    if (wv == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "PrinterWebView: WebView::CreateWebView returned null; camera area disabled";
        auto *msg = new wxStaticText(
            m_camera_webview_host,
            wxID_ANY,
            _L("Camera preview could not start. Install or repair Microsoft WebView2 Runtime."));
        auto *vs = new wxBoxSizer(wxVERTICAL);
        vs->AddStretchSpacer(1);
        vs->Add(msg, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
        vs->AddStretchSpacer(1);
        m_camera_webview_host->SetSizer(vs);
        m_camera_webview_host->Layout();
        m_camera_stream_requested = false;
        if (m_dashboard_page != nullptr && m_dashboard_page->camera_panel() != nullptr)
            m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Failed);
        return;
    }

    m_camera_webview = wv;
    m_camera_webview->SetBackgroundColour(*wxBLACK);
    m_camera_webview->SetPage("<!doctype html><html><body style='margin:0;background:#000'></body></html>", "");
    std::weak_ptr<int> title_lifetime = m_lifetime_token;
    m_camera_webview->Bind(wxEVT_WEBVIEW_TITLE_CHANGED, [this, title_lifetime](wxWebViewEvent &event) {
        if (title_lifetime.expired() || m_destroying)
            return;
        handle_camera_webview_title(event.GetString());
    });

    auto *hs = new wxBoxSizer(wxHORIZONTAL);
    hs->Add(m_camera_webview, 1, wxEXPAND);
    m_camera_webview_host->SetSizer(hs);

#ifdef __WXMSW__
    m_camera_webview->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        e.Skip();
        if (m_camera_webview == nullptr)
            return;
        if (m_camera_webview->GetNativeBackend() == nullptr)
            return;
        wxSize sz = e.GetSize();
        if (sz.x <= 0 || sz.y <= 0)
            return;
        HWND hwnd = (HWND) m_camera_webview->GetHWND();
        if (hwnd == nullptr)
            return;
        const int d = this->FromDIP(12) * 2;
        HRGN hrgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
        if (hrgn == nullptr)
            return;
        if (!::SetWindowRgn(hwnd, hrgn, TRUE))
            ::DeleteObject(hrgn);
    });
#endif

    m_camera_webview_host->Layout();
    // Do not call refresh_layer_info_from_selected_machine() here: it can re-enter device/network
    // paths while WebView2 is still attaching and has been linked to startup crashes.
    std::weak_ptr<int> lifetime = m_lifetime_token;
    wxGetApp().CallAfter([this, lifetime]() {
        if (lifetime.expired() || m_destroying)
            return;
        if (m_camera_webview != nullptr)
            refresh_layer_info_from_selected_machine();
    });
}

void PrinterWebView::apply_filament_tool_selection(int tool_index)
{
    if (tool_index < 0 || tool_index > 3)
        tool_index = 0;
    m_selected_filament_tool = tool_index;

    m_dashboard_state_store.update([&](DeviceDashboard::DeviceDashboardState &state) {
        state.filament.selected_tool = tool_index;
    });
    if (m_dashboard_page != nullptr)
        m_dashboard_page->filament_panel()->apply_state(m_dashboard_state_store.state().filament);
}

void PrinterWebView::apply_printer_status_tool_selection(int tool_index)
{
    if (tool_index < 0 || tool_index > 3)
        tool_index = 0;

    m_selected_extruder_index = tool_index;

    if (m_dashboard_page != nullptr)
        m_dashboard_page->printer_status_panel()->set_active_tool(m_selected_extruder_index);

    refresh_layer_info_from_selected_machine();
    Layout();
}

bool PrinterWebView::send_toolhead_fan_speed_command(int tool_index, int fan_percent)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string base = moonraker_base_url(obj);
    if (obj == nullptr || !obj->is_online() || base.empty())
        return false;

    tool_index = std::max(0, std::min(3, tool_index));
    fan_percent = std::max(0, std::min(100, fan_percent));

    std::ostringstream speed;
    speed << std::fixed << std::setprecision(3) << (static_cast<double>(fan_percent) / 100.0);

    nlohmann::json payload;
    payload["script"] = "SET_FAN_SPEED FAN=fan_t" + std::to_string(tool_index) + " SPEED=" + speed.str();
    const std::string body = payload.dump();

    std::weak_ptr<int> lifetime = m_lifetime_token;
    std::thread([this, lifetime, base, body]() {
        Http::post(base + "/printer/gcode/script")
            .header("Content-Type", "application/json")
            .set_post_body(body)
            .timeout_connect(2)
            .timeout_max(4)
            .on_complete([](std::string, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: tool fan speed command status=" << status;
            })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "PrinterWebView: tool fan speed command failed status=" << status << " error=" << error;
            })
            .perform_sync();

        wxGetApp().CallAfter([this, lifetime]() {
            if (lifetime.expired() || m_destroying)
                return;
            refresh_moonraker_status_from_selected_machine();
        });
    }).detach();

    return true;
}

void PrinterWebView::show_toolhead_fan_dialog(int active_extruder_index)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online())
        return;

    active_extruder_index = std::max(0, std::min(3, active_extruder_index));
    const int current_percent = m_moonraker_fan_available[active_extruder_index] ? m_moonraker_fan_percent[active_extruder_index] : 0;

    const auto S = [this](int value) {
        return FromDIP(static_cast<int>(std::round(value * 1.3)));
    };
    const auto scale_font = [](wxWindow* window, double factor, int min_points = 0) {
        wxFont font = window->GetFont();
        int point_size = font.GetPointSize();
        if (point_size > 0) {
            point_size = static_cast<int>(std::round(point_size * factor));
            if (min_points > 0)
                point_size = std::max(min_points, point_size);
            font.SetPointSize(point_size);
            window->SetFont(font);
        }
    };

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#000000"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *dialog_shell = new StaticBox(&dlg, wxID_ANY);
    dialog_shell->SetCornerRadius(S(10));
    dialog_shell->SetBorderWidth(1);
    dialog_shell->SetBorderColorNormal(wxColour("#D9DBDB"));
    dialog_shell->SetBackgroundColorNormal(wxColour("#F7F7F5"));
    dialog_shell->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *shell_sz = new wxBoxSizer(wxVERTICAL);

    auto *title_bar = new wxPanel(dialog_shell, wxID_ANY);
    title_bar->SetBackgroundColour(wxColour("#FFFFFF"));
    auto *title_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *title_txt = new wxStaticText(title_bar, wxID_ANY, _L("Change Toolhead Fan"));
    title_txt->SetForegroundColour(wxColour("#232527"));
    {
        wxFont tf = title_txt->GetFont();
        if (tf.GetPointSize() > 1)
            tf.SetPointSize(static_cast<int>(std::round((tf.GetPointSize() + 1) * 1.3)));
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title_txt->SetFont(tf);
    }
    title_sz->Add(title_txt, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, S(14));
    auto *close_btn = new wxButton(title_bar, wxID_ANY, wxString::FromUTF8("\u00D7"), wxDefaultPosition, wxSize(S(34), S(34)), wxBORDER_NONE);
    close_btn->SetBackgroundColour(wxColour("#FFFFFF"));
    close_btn->SetForegroundColour(wxColour("#232527"));
    scale_font(close_btn, 1.3);
    title_sz->Add(close_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(4));
    title_bar->SetSizer(title_sz);
    title_bar->SetMinSize(wxSize(-1, S(38)));
    shell_sz->Add(title_bar, 0, wxEXPAND);

    auto *body = new wxPanel(dialog_shell);
    body->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *body_sz = new wxBoxSizer(wxHORIZONTAL);

    auto *left = new wxPanel(body, wxID_ANY);
    left->SetBackgroundColour(wxColour("#F7F7F5"));
    left->SetMinSize(wxSize(S(190), -1));
    auto *left_sz = new wxBoxSizer(wxVERTICAL);

    auto *active_card = new StaticBox(left, wxID_ANY);
    active_card->SetMinSize(wxSize(S(152), S(98)));
    active_card->SetCornerRadius(S(10));
    active_card->SetBorderWidth(1);
    active_card->SetBorderColorNormal(wxColour("#ECEDEC"));
    active_card->SetBackgroundColorNormal(wxColour("#FBFBFA"));
    active_card->SetBackgroundColour(wxColour("#FBFBFA"));
    auto *active_card_sz = new wxBoxSizer(wxVERTICAL);

    auto *tool_hdr_wrap = new wxPanel(active_card, wxID_ANY);
    tool_hdr_wrap->SetBackgroundColour(wxColour("#F2F3F1"));
    auto *thw_sz = new wxBoxSizer(wxVERTICAL);
    auto *tool_hdr = new wxStaticText(tool_hdr_wrap, wxID_ANY, wxString::Format("Tool %d", active_extruder_index + 1));
    tool_hdr->SetBackgroundColour(wxColour("#F2F3F1"));
    tool_hdr->SetForegroundColour(wxColour("#4C4E50"));
    {
        wxFont hf = tool_hdr->GetFont();
        hf.SetWeight(wxFONTWEIGHT_BOLD);
        tool_hdr->SetFont(hf);
    }
    scale_font(tool_hdr, 1.3);
    thw_sz->Add(tool_hdr, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(10));
    tool_hdr_wrap->SetSizer(thw_sz);
    active_card_sz->Add(tool_hdr_wrap, 0, wxEXPAND);

    auto *fan_row = new wxBoxSizer(wxHORIZONTAL);
    wxBitmap fan_bitmap = create_scaled_bitmap("cp_tool_fan", &dlg, 31);
    if (fan_bitmap.IsOk())
        fan_row->Add(new wxStaticBitmap(active_card, wxID_ANY, fan_bitmap), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(8));
    auto *big_cur = new wxStaticText(active_card, wxID_ANY, wxString::Format("%d%%", current_percent));
    {
        wxFont bf = big_cur->GetFont();
        bf.SetPointSize(std::max(23, static_cast<int>(std::round((bf.GetPointSize() + 8) * 1.3))));
        bf.SetWeight(wxFONTWEIGHT_BOLD);
        big_cur->SetFont(bf);
    }
    big_cur->SetForegroundColour(wxColour("#596068"));
    fan_row->Add(big_cur, 0, wxALIGN_CENTER_VERTICAL);
    active_card_sz->Add(fan_row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(14));
    active_card->SetSizer(active_card_sz);
    left_sz->Add(active_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, S(8));

    auto *input_wrap = new StaticBox(left, wxID_ANY);
    input_wrap->SetCornerRadius(S(8));
    input_wrap->SetBorderWidth(1);
    input_wrap->SetBorderColorNormal(wxColour("#E0E2E2"));
    input_wrap->SetBackgroundColorNormal(*wxWHITE);
    input_wrap->SetBackgroundColour(*wxWHITE);
    input_wrap->SetMinSize(wxSize(S(112), S(29)));
    auto *input_wrap_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *inp = new wxTextCtrl(input_wrap, wxID_ANY, wxString::Format("%d%%", current_percent), wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE);
    inp->SetBackgroundColour(*wxWHITE);
    inp->SetForegroundColour(wxColour("#4F555A"));
    scale_font(inp, 1.3);
    input_wrap_sz->Add(inp, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, S(10));
    input_wrap->SetSizer(input_wrap_sz);

    auto *set_btn = new StaticBox(left, wxID_ANY);
    set_btn->SetMinSize(wxSize(S(50), S(29)));
    set_btn->SetCornerRadius(S(7));
    set_btn->SetBorderWidth(0);
    set_btn->SetBackgroundColorNormal(wxColour("#4E4E4E"));
    set_btn->SetBackgroundColour(wxColour("#4E4E4E"));
    set_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *set_btn_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *set_label = new wxStaticText(set_btn, wxID_ANY, _L("Set"));
    set_label->SetForegroundColour(*wxWHITE);
    set_label->SetCursor(wxCursor(wxCURSOR_HAND));
    scale_font(set_label, 1.3);
    set_btn_sz->AddStretchSpacer(1);
    set_btn_sz->Add(set_label, 0, wxALIGN_CENTER_VERTICAL);
    set_btn_sz->AddStretchSpacer(1);
    set_btn->SetSizer(set_btn_sz);
    auto *inp_row = new wxBoxSizer(wxHORIZONTAL);
    inp_row->Add(input_wrap, 0, wxALIGN_CENTER_VERTICAL);
    inp_row->Add(set_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, S(10));
    left_sz->Add(inp_row, 0, wxLEFT | wxRIGHT | wxTOP, S(12));
    left_sz->AddStretchSpacer(1);
    left->SetSizer(left_sz);

    auto *right = new wxPanel(body, wxID_ANY);
    right->SetBackgroundColour(wxColour("#F7F7F5"));
    right->SetMinSize(wxSize(S(190), -1));
    auto *right_sz = new wxBoxSizer(wxVERTICAL);

    constexpr int k_select_tool_base_id = wxID_HIGHEST + 340;
    for (int i = 0; i < 4; ++i) {
        if (i == active_extruder_index)
            continue;
        const int fan = m_moonraker_fan_available[i] ? m_moonraker_fan_percent[i] : 0;
        auto *row = new StaticBox(right, wxID_ANY);
        row->SetMinSize(wxSize(S(190), S(31)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        row->SetCornerRadius(S(8));
        row->SetBorderWidth(1);
        row->SetBorderColorNormal(wxColour("#ECEDEC"));
        row->SetBackgroundColorNormal(wxColour("#FBFBFA"));
        row->SetBackgroundColour(wxColour("#FBFBFA"));
        auto *rs = new wxBoxSizer(wxHORIZONTAL);

        auto *pill = new wxPanel(row, wxID_ANY);
        pill->SetCursor(wxCursor(wxCURSOR_HAND));
        pill->SetBackgroundColour(wxColour("#F2F3F1"));
        auto *ps = new wxBoxSizer(wxVERTICAL);
        auto *pn = new wxStaticText(pill, wxID_ANY, wxString::Format("Tool %d", i + 1));
        pn->SetCursor(wxCursor(wxCURSOR_HAND));
        pn->SetBackgroundColour(wxColour("#F2F3F1"));
        pn->SetForegroundColour(wxColour("#4C4E50"));
        {
            wxFont pf = pn->GetFont();
            pf.SetWeight(wxFONTWEIGHT_BOLD);
            pn->SetFont(pf);
        }
        scale_font(pn, 1.3);
        ps->Add(pn, 0, wxALL, S(8));
        pill->SetSizer(ps);
        rs->Add(pill, 0, wxALIGN_CENTER_VERTICAL);
        rs->AddSpacer(S(10));

        wxBitmap sm = create_scaled_bitmap("cp_tool_fan", &dlg, 21);
        wxStaticBitmap *sm_icon = nullptr;
        if (sm.IsOk())
            sm_icon = new wxStaticBitmap(row, wxID_ANY, sm);
        if (sm_icon != nullptr) {
            sm_icon->SetCursor(wxCursor(wxCURSOR_HAND));
            rs->Add(sm_icon, 0, wxALIGN_CENTER_VERTICAL);
        }
        auto *tx = new wxStaticText(row, wxID_ANY, wxString::Format("%d%%", fan));
        tx->SetCursor(wxCursor(wxCURSOR_HAND));
        tx->SetForegroundColour(wxColour("#596068"));
        scale_font(tx, 1.3);
        rs->Add(tx, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, S(6));
        row->SetSizer(rs);
        right_sz->Add(row, 0, wxEXPAND | wxTOP, S(8));
        const int tool_index = i;
        auto select_tool = [&dlg, tool_index, k_select_tool_base_id](wxMouseEvent &evt) {
            evt.StopPropagation();
            dlg.EndModal(k_select_tool_base_id + tool_index);
        };
        row->Bind(wxEVT_LEFT_DOWN, select_tool);
        pill->Bind(wxEVT_LEFT_DOWN, select_tool);
        pn->Bind(wxEVT_LEFT_DOWN, select_tool);
        if (sm_icon != nullptr)
            sm_icon->Bind(wxEVT_LEFT_DOWN, select_tool);
        tx->Bind(wxEVT_LEFT_DOWN, select_tool);
    }
    right_sz->AddStretchSpacer(1);
    right->SetSizer(right_sz);

    body_sz->Add(left, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, S(14));
    body_sz->AddSpacer(S(14));
    body_sz->Add(right, 0, wxEXPAND | wxRIGHT | wxTOP | wxBOTTOM, S(14));
    body->SetSizer(body_sz);
    shell_sz->Add(body, 1, wxEXPAND);

    dialog_shell->SetSizer(shell_sz);
    root->Add(dialog_shell, 1, wxEXPAND | wxALL, FromDIP(1));
    dlg.SetSizer(root);

    const auto apply_fan = [&]() {
        wxString raw = inp->GetValue();
        raw.Trim(true);
        raw.Trim(false);
        raw.Replace("%", wxEmptyString, true);
        raw.Trim(true);
        raw.Trim(false);
        long v = 0;
        if (!raw.ToLong(&v)) {
            wxMessageBox(_L("Please enter a valid fan speed."), _L("Change Toolhead Fan"), wxOK | wxICON_WARNING, &dlg);
            return;
        }
        v = std::max(0L, std::min(100L, v));
        if (!send_toolhead_fan_speed_command(active_extruder_index, static_cast<int>(v))) {
            wxMessageBox(_L("Fan speed command could not be sent."), _L("Change Toolhead Fan"), wxOK | wxICON_WARNING, &dlg);
            return;
        }
        dlg.EndModal(wxID_OK);
    };

    close_btn->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    set_btn->Bind(wxEVT_LEFT_DOWN, [&apply_fan](wxMouseEvent &) { apply_fan(); });
    set_label->Bind(wxEVT_LEFT_DOWN, [&apply_fan](wxMouseEvent &) { apply_fan(); });
    inp->Bind(wxEVT_TEXT_ENTER, [&apply_fan](wxCommandEvent &) { apply_fan(); });
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent &e) {
        if (dlg.IsModal())
            dlg.EndModal(wxID_CANCEL);
        else
            e.Skip();
    });

    dlg.Fit();
    dlg.SetMinSize(dlg.GetSize());
    {
        const wxSize size = dlg.GetSize();
        wxBitmap shape_bmp(size.GetWidth(), size.GetHeight());
        wxMemoryDC dc(shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), S(10));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            dlg.SetShape(region);
    }
    dlg.CentreOnParent();
    const int modal_result = dlg.ShowModal();
    if (modal_result >= k_select_tool_base_id && modal_result < k_select_tool_base_id + 4) {
        const int selected_tool = modal_result - k_select_tool_base_id;
        wxGetApp().CallAfter([this, token = std::weak_ptr<int>(m_lifetime_token), selected_tool]() {
            if (token.expired() || m_destroying)
                return;
            show_toolhead_fan_dialog(selected_tool);
        });
    }
}

void PrinterWebView::show_toolhead_temperature_dialog(int active_extruder_index)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->GetExtderSystem() == nullptr)
        return;

    active_extruder_index = std::max(0, std::min(3, active_extruder_index));

    long min_t = 0;
    long max_t = 300;
    if (obj->nozzle_temp_range.size() >= 2) {
        min_t = obj->nozzle_temp_range[0];
        max_t = obj->nozzle_temp_range[1];
    }

    const auto S = [this](int value) {
        return FromDIP(static_cast<int>(std::round(value * 1.3)));
    };
    const auto scale_font = [](wxWindow* window, double factor, int min_points = 0) {
        wxFont font = window->GetFont();
        int point_size = font.GetPointSize();
        if (point_size > 0) {
            point_size = static_cast<int>(std::round(point_size * factor));
            if (min_points > 0)
                point_size = std::max(min_points, point_size);
            font.SetPointSize(point_size);
            window->SetFont(font);
        }
    };

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#000000"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *dialog_shell = new StaticBox(&dlg, wxID_ANY);
    dialog_shell->SetCornerRadius(S(10));
    dialog_shell->SetBorderWidth(1);
    dialog_shell->SetBorderColorNormal(wxColour("#D9DBDB"));
    dialog_shell->SetBackgroundColorNormal(wxColour("#F7F7F5"));
    dialog_shell->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *shell_sz = new wxBoxSizer(wxVERTICAL);

    auto *title_bar = new wxPanel(dialog_shell, wxID_ANY);
    title_bar->SetBackgroundColour(wxColour("#FFFFFF"));
    auto *title_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *title_txt = new wxStaticText(title_bar, wxID_ANY, _L("Change Toolhead Temperature"));
    title_txt->SetForegroundColour(wxColour("#232527"));
    {
        wxFont tf = title_txt->GetFont();
        if (tf.GetPointSize() > 1)
            tf.SetPointSize(static_cast<int>(std::round((tf.GetPointSize() + 1) * 1.3)));
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title_txt->SetFont(tf);
    }
    title_sz->Add(title_txt, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, S(14));
    auto *close_btn = new wxButton(title_bar, wxID_ANY, wxString::FromUTF8("\u00D7"), wxDefaultPosition, wxSize(S(34), S(34)), wxBORDER_NONE);
    close_btn->SetBackgroundColour(wxColour("#FFFFFF"));
    close_btn->SetForegroundColour(wxColour("#232527"));
    scale_font(close_btn, 1.3);
    title_sz->Add(close_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(4));
    title_bar->SetSizer(title_sz);
    title_bar->SetMinSize(wxSize(-1, S(38)));
    shell_sz->Add(title_bar, 0, wxEXPAND);

    auto *body = new wxPanel(dialog_shell);
    body->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *body_sz = new wxBoxSizer(wxHORIZONTAL);

    auto *left = new wxPanel(body, wxID_ANY);
    left->SetBackgroundColour(wxColour("#F7F7F5"));
    left->SetMinSize(wxSize(S(190), -1));
    auto *left_sz = new wxBoxSizer(wxVERTICAL);

    auto *active_card = new StaticBox(left, wxID_ANY);
    active_card->SetMinSize(wxSize(S(152), S(98)));
    active_card->SetCornerRadius(S(10));
    active_card->SetBorderWidth(1);
    active_card->SetBorderColorNormal(wxColour("#ECEDEC"));
    active_card->SetBackgroundColorNormal(wxColour("#FBFBFA"));
    active_card->SetBackgroundColour(wxColour("#FBFBFA"));
    auto *active_card_sz = new wxBoxSizer(wxVERTICAL);

    auto *tool_hdr_wrap = new wxPanel(active_card, wxID_ANY);
    tool_hdr_wrap->SetBackgroundColour(wxColour("#F2F3F1"));
    auto *thw_sz = new wxBoxSizer(wxVERTICAL);
    auto *tool_hdr = new wxStaticText(tool_hdr_wrap, wxID_ANY, wxString::Format("Tool %d", active_extruder_index + 1));
    tool_hdr->SetBackgroundColour(wxColour("#F2F3F1"));
    tool_hdr->SetForegroundColour(wxColour("#4C4E50"));
    {
        wxFont hf = tool_hdr->GetFont();
        hf.SetWeight(wxFONTWEIGHT_BOLD);
        tool_hdr->SetFont(hf);
    }
    scale_font(tool_hdr, 1.3);
    thw_sz->Add(tool_hdr, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(10));
    tool_hdr_wrap->SetSizer(thw_sz);
    active_card_sz->Add(tool_hdr_wrap, 0, wxEXPAND);

    const float cur_f = obj->GetExtderSystem()->GetNozzleTempCurrent(active_extruder_index);
    const float tgt_f = obj->GetExtderSystem()->GetNozzleTempTarget(active_extruder_index);

    auto *temp_row = new wxBoxSizer(wxHORIZONTAL);
    wxBitmap therm = create_scaled_bitmap("tool_temperature_popup", &dlg, 31);
    if (therm.IsOk())
        temp_row->Add(new wxStaticBitmap(active_card, wxID_ANY, therm), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(8));
    auto *big_cur = new wxStaticText(active_card, wxID_ANY, wxString::Format("%.0f", static_cast<double>(cur_f)));
    {
        wxFont bf = big_cur->GetFont();
        bf.SetPointSize(std::max(23, static_cast<int>(std::round((bf.GetPointSize() + 8) * 1.3))));
        bf.SetWeight(wxFONTWEIGHT_BOLD);
        big_cur->SetFont(bf);
    }
    big_cur->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_cur, 0, wxALIGN_CENTER_VERTICAL);
    auto *slash_txt = new wxStaticText(active_card, wxID_ANY, "/");
    scale_font(slash_txt, 1.3);
    temp_row->Add(slash_txt, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, S(2));
    auto *big_tgt = new wxStaticText(active_card, wxID_ANY,
                                     wxString::Format("%.0f %s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C")));
    {
        wxFont sf = big_tgt->GetFont();
        if (sf.GetPointSize() > 1)
            sf.SetPointSize(static_cast<int>(std::round((sf.GetPointSize() + 1) * 1.3)));
        big_tgt->SetFont(sf);
    }
    big_tgt->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_tgt, 0, wxALIGN_CENTER_VERTICAL);
    active_card_sz->Add(temp_row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(14));
    active_card->SetSizer(active_card_sz);
    left_sz->Add(active_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, S(8));

    wxString init_val = wxString::Format("%.0f%s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C"));
    auto *input_wrap = new StaticBox(left, wxID_ANY);
    input_wrap->SetCornerRadius(S(8));
    input_wrap->SetBorderWidth(1);
    input_wrap->SetBorderColorNormal(wxColour("#E0E2E2"));
    input_wrap->SetBackgroundColorNormal(*wxWHITE);
    input_wrap->SetBackgroundColour(*wxWHITE);
    input_wrap->SetMinSize(wxSize(S(112), S(29)));
    auto *input_wrap_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *inp = new wxTextCtrl(input_wrap, wxID_ANY, init_val, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE);
    inp->SetBackgroundColour(*wxWHITE);
    inp->SetForegroundColour(wxColour("#4F555A"));
    scale_font(inp, 1.3);
    input_wrap_sz->Add(inp, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, S(10));
    input_wrap->SetSizer(input_wrap_sz);

    auto *set_btn = new StaticBox(left, wxID_ANY);
    set_btn->SetMinSize(wxSize(S(50), S(29)));
    set_btn->SetCornerRadius(S(7));
    set_btn->SetBorderWidth(0);
    set_btn->SetBackgroundColorNormal(wxColour("#4E4E4E"));
    set_btn->SetBackgroundColour(wxColour("#4E4E4E"));
    set_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *set_btn_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *set_label = new wxStaticText(set_btn, wxID_ANY, _L("Set"));
    set_label->SetForegroundColour(*wxWHITE);
    set_label->SetCursor(wxCursor(wxCURSOR_HAND));
    scale_font(set_label, 1.3);
    set_btn_sz->AddStretchSpacer(1);
    set_btn_sz->Add(set_label, 0, wxALIGN_CENTER_VERTICAL);
    set_btn_sz->AddStretchSpacer(1);
    set_btn->SetSizer(set_btn_sz);
    auto *inp_row = new wxBoxSizer(wxHORIZONTAL);
    inp_row->Add(input_wrap, 0, wxALIGN_CENTER_VERTICAL);
    inp_row->Add(set_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, S(10));
    left_sz->Add(inp_row, 0, wxLEFT | wxRIGHT | wxTOP, S(12));
    left_sz->AddStretchSpacer(1);
    left->SetSizer(left_sz);

    auto *right = new wxPanel(body, wxID_ANY);
    right->SetBackgroundColour(wxColour("#F7F7F5"));
    right->SetMinSize(wxSize(S(190), -1));
    auto *right_sz = new wxBoxSizer(wxVERTICAL);

    constexpr int k_select_tool_base_id = wxID_HIGHEST + 300;
    for (int i = 0; i < 4; ++i) {
        if (i == active_extruder_index)
            continue;
        const float oc = obj->GetExtderSystem()->GetNozzleTempCurrent(i);
        const float ot = obj->GetExtderSystem()->GetNozzleTempTarget(i);
        auto *row = new StaticBox(right, wxID_ANY);
        row->SetMinSize(wxSize(S(190), S(31)));
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        row->SetCornerRadius(S(8));
        row->SetBorderWidth(1);
        row->SetBorderColorNormal(wxColour("#ECEDEC"));
        row->SetBackgroundColorNormal(wxColour("#FBFBFA"));
        row->SetBackgroundColour(wxColour("#FBFBFA"));
        auto *rs = new wxBoxSizer(wxHORIZONTAL);

        auto *pill = new wxPanel(row, wxID_ANY);
        pill->SetCursor(wxCursor(wxCURSOR_HAND));
        pill->SetBackgroundColour(wxColour("#F2F3F1"));
        auto *ps = new wxBoxSizer(wxVERTICAL);
        auto *pn = new wxStaticText(pill, wxID_ANY, wxString::Format("Tool %d", i + 1));
        pn->SetCursor(wxCursor(wxCURSOR_HAND));
        pn->SetBackgroundColour(wxColour("#F2F3F1"));
        pn->SetForegroundColour(wxColour("#4C4E50"));
        {
            wxFont pf = pn->GetFont();
            pf.SetWeight(wxFONTWEIGHT_BOLD);
            pn->SetFont(pf);
        }
        scale_font(pn, 1.3);
        ps->Add(pn, 0, wxALL, S(8));
        pill->SetSizer(ps);
        rs->Add(pill, 0, wxALIGN_CENTER_VERTICAL);
        rs->AddSpacer(S(10));
        wxBitmap sm = create_scaled_bitmap("tool_temperature_popup", &dlg, 21);
        wxStaticBitmap *sm_icon = nullptr;
        if (sm.IsOk())
            sm_icon = new wxStaticBitmap(row, wxID_ANY, sm);
        if (sm_icon != nullptr) {
            sm_icon->SetCursor(wxCursor(wxCURSOR_HAND));
            rs->Add(sm_icon, 0, wxALIGN_CENTER_VERTICAL);
        }
        auto *tx = new wxStaticText(row, wxID_ANY,
                                    wxString::Format("%.0f/%.0f %s", static_cast<double>(oc), static_cast<double>(ot),
                                                     wxString::FromUTF8("\xC2\xB0""C")));
        tx->SetCursor(wxCursor(wxCURSOR_HAND));
        tx->SetForegroundColour(wxColour("#596068"));
        scale_font(tx, 1.3);
        rs->Add(tx, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, S(6));
        row->SetSizer(rs);
        right_sz->Add(row, 0, wxEXPAND | wxTOP, S(8));
        const int tool_index = i;
        auto select_tool = [&dlg, tool_index, k_select_tool_base_id](wxMouseEvent &evt) {
            evt.StopPropagation();
            dlg.EndModal(k_select_tool_base_id + tool_index);
        };
        row->Bind(wxEVT_LEFT_DOWN, select_tool);
        pill->Bind(wxEVT_LEFT_DOWN, select_tool);
        pn->Bind(wxEVT_LEFT_DOWN, select_tool);
        if (sm_icon != nullptr)
            sm_icon->Bind(wxEVT_LEFT_DOWN, select_tool);
        tx->Bind(wxEVT_LEFT_DOWN, select_tool);
    }
    right_sz->AddStretchSpacer(1);
    right->SetSizer(right_sz);

    body_sz->Add(left, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, S(14));
    body_sz->AddSpacer(S(14));
    body_sz->Add(right, 0, wxEXPAND | wxRIGHT | wxTOP | wxBOTTOM, S(14));
    body->SetSizer(body_sz);
    shell_sz->Add(body, 1, wxEXPAND);

    dialog_shell->SetSizer(shell_sz);
    root->Add(dialog_shell, 1, wxEXPAND | wxALL, FromDIP(1));
    dlg.SetSizer(root);

    const auto apply_temp = [&]() {
        wxString raw = inp->GetValue();
        raw.Trim(true);
        raw.Trim(false);
        raw.Replace(wxString::FromUTF8("\xC2\xB0""C"), wxEmptyString, true);
        raw.Replace("C", wxEmptyString, true);
        raw.Trim(true);
        raw.Trim(false);
        long v = 0;
        if (!raw.ToLong(&v)) {
            wxMessageBox(_L("Please enter a valid temperature."), _L("Change Toolhead Temperature"), wxOK | wxICON_WARNING, &dlg);
            return;
        }
        v = std::max(min_t, std::min(max_t, v));
        obj->command_set_nozzle_new(active_extruder_index, static_cast<int>(v));
        dlg.EndModal(wxID_OK);
    };

    close_btn->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    set_btn->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    set_label->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    inp->Bind(wxEVT_TEXT_ENTER, [&apply_temp](wxCommandEvent &) { apply_temp(); });
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent &e) {
        if (dlg.IsModal())
            dlg.EndModal(wxID_CANCEL);
        else
            e.Skip();
    });

    dlg.Fit();
    dlg.SetMinSize(dlg.GetSize());
    {
        const wxSize size = dlg.GetSize();
        wxBitmap shape_bmp(size.GetWidth(), size.GetHeight());
        wxMemoryDC dc(shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), S(10));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            dlg.SetShape(region);
    }
    dlg.CentreOnParent();
    const int modal_result = dlg.ShowModal();
    if (modal_result >= k_select_tool_base_id && modal_result < k_select_tool_base_id + 4) {
        const int selected_tool = modal_result - k_select_tool_base_id;
        wxGetApp().CallAfter([this, token = std::weak_ptr<int>(m_lifetime_token), selected_tool]() {
            if (token.expired() || m_destroying)
                return;
            show_toolhead_temperature_dialog(selected_tool);
        });
    }
}

void PrinterWebView::show_bed_temperature_dialog()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->GetBed() == nullptr)
        return;

    long min_t = 0;
    long max_t = obj->get_bed_temperature_limit();
    if (obj->bed_temp_range.size() >= 2) {
        min_t = obj->bed_temp_range[0];
        max_t = obj->bed_temp_range[1];
    }

    const float cur_f = obj->GetBed()->GetBedTemp();
    const float tgt_f = obj->GetBed()->GetBedTempTarget();

    const auto S = [this](int value) {
        return FromDIP(static_cast<int>(std::round(value * 1.3)));
    };
    const auto scale_font = [](wxWindow* window, double factor, int min_points = 0) {
        wxFont font = window->GetFont();
        int point_size = font.GetPointSize();
        if (point_size > 0) {
            point_size = static_cast<int>(std::round(point_size * factor));
            if (min_points > 0)
                point_size = std::max(min_points, point_size);
            font.SetPointSize(point_size);
            window->SetFont(font);
        }
    };

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#000000"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *dialog_shell = new StaticBox(&dlg, wxID_ANY);
    dialog_shell->SetCornerRadius(S(10));
    dialog_shell->SetBorderWidth(1);
    dialog_shell->SetBorderColorNormal(wxColour("#D9DBDB"));
    dialog_shell->SetBackgroundColorNormal(wxColour("#F7F7F5"));
    dialog_shell->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *shell_sz = new wxBoxSizer(wxVERTICAL);

    auto *title_bar = new wxPanel(dialog_shell, wxID_ANY);
    title_bar->SetBackgroundColour(wxColour("#FFFFFF"));
    auto *title_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *title_txt = new wxStaticText(title_bar, wxID_ANY, _L("Change Build Plate Temperature"));
    title_txt->SetForegroundColour(wxColour("#232527"));
    {
        wxFont tf = title_txt->GetFont();
        if (tf.GetPointSize() > 1)
            tf.SetPointSize(static_cast<int>(std::round((tf.GetPointSize() + 1) * 1.3)));
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title_txt->SetFont(tf);
    }
    title_sz->Add(title_txt, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, S(14));
    auto *close_btn = new wxButton(title_bar, wxID_ANY, wxString::FromUTF8("\u00D7"), wxDefaultPosition, wxSize(S(34), S(34)), wxBORDER_NONE);
    close_btn->SetBackgroundColour(wxColour("#FFFFFF"));
    close_btn->SetForegroundColour(wxColour("#232527"));
    scale_font(close_btn, 1.3);
    title_sz->Add(close_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(4));
    title_bar->SetSizer(title_sz);
    title_bar->SetMinSize(wxSize(-1, S(38)));
    shell_sz->Add(title_bar, 0, wxEXPAND);

    auto *body = new wxPanel(dialog_shell);
    body->SetBackgroundColour(wxColour("#F7F7F5"));
    auto *body_sz = new wxBoxSizer(wxVERTICAL);

    auto *active_card = new StaticBox(body, wxID_ANY);
    active_card->SetMinSize(wxSize(S(220), S(98)));
    active_card->SetCornerRadius(S(10));
    active_card->SetBorderWidth(1);
    active_card->SetBorderColorNormal(wxColour("#ECEDEC"));
    active_card->SetBackgroundColorNormal(wxColour("#FBFBFA"));
    active_card->SetBackgroundColour(wxColour("#FBFBFA"));
    auto *active_card_sz = new wxBoxSizer(wxVERTICAL);

    auto *bed_hdr_wrap = new wxPanel(active_card, wxID_ANY);
    bed_hdr_wrap->SetBackgroundColour(wxColour("#F2F3F1"));
    auto *bhw_sz = new wxBoxSizer(wxVERTICAL);
    auto *bed_hdr = new wxStaticText(bed_hdr_wrap, wxID_ANY, _L("Build Plate"));
    bed_hdr->SetBackgroundColour(wxColour("#F2F3F1"));
    bed_hdr->SetForegroundColour(wxColour("#4C4E50"));
    {
        wxFont hf = bed_hdr->GetFont();
        hf.SetWeight(wxFONTWEIGHT_BOLD);
        bed_hdr->SetFont(hf);
    }
    scale_font(bed_hdr, 1.3);
    bhw_sz->Add(bed_hdr, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(10));
    bed_hdr_wrap->SetSizer(bhw_sz);
    active_card_sz->Add(bed_hdr_wrap, 0, wxEXPAND);

    auto *temp_row = new wxBoxSizer(wxHORIZONTAL);
    wxBitmap therm = safe_scaled_bitmap(&dlg, "cp_bed_heating", 31);
    if (therm.IsOk())
        temp_row->Add(new wxStaticBitmap(active_card, wxID_ANY, therm), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, S(8));
    auto *big_cur = new wxStaticText(active_card, wxID_ANY, wxString::Format("%.0f", static_cast<double>(cur_f)));
    {
        wxFont bf = big_cur->GetFont();
        bf.SetPointSize(std::max(23, static_cast<int>(std::round((bf.GetPointSize() + 8) * 1.3))));
        bf.SetWeight(wxFONTWEIGHT_BOLD);
        big_cur->SetFont(bf);
    }
    big_cur->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_cur, 0, wxALIGN_CENTER_VERTICAL);
    auto *slash_txt = new wxStaticText(active_card, wxID_ANY, "/");
    scale_font(slash_txt, 1.3);
    temp_row->Add(slash_txt, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, S(2));
    auto *big_tgt = new wxStaticText(active_card, wxID_ANY,
                                     wxString::Format("%.0f %s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C")));
    {
        wxFont sf = big_tgt->GetFont();
        if (sf.GetPointSize() > 1)
            sf.SetPointSize(static_cast<int>(std::round((sf.GetPointSize() + 1) * 1.3)));
        big_tgt->SetFont(sf);
    }
    big_tgt->SetForegroundColour(wxColour("#596068"));
    temp_row->Add(big_tgt, 0, wxALIGN_CENTER_VERTICAL);
    active_card_sz->Add(temp_row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, S(14));
    active_card->SetSizer(active_card_sz);

    wxString init_val = wxString::Format("%.0f%s", static_cast<double>(tgt_f), wxString::FromUTF8("\xC2\xB0""C"));
    auto *input_wrap = new StaticBox(body, wxID_ANY);
    input_wrap->SetCornerRadius(S(8));
    input_wrap->SetBorderWidth(1);
    input_wrap->SetBorderColorNormal(wxColour("#E0E2E2"));
    input_wrap->SetBackgroundColorNormal(*wxWHITE);
    input_wrap->SetBackgroundColour(*wxWHITE);
    input_wrap->SetMinSize(wxSize(S(112), S(29)));
    auto *input_wrap_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *inp = new wxTextCtrl(input_wrap, wxID_ANY, init_val, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE);
    inp->SetBackgroundColour(*wxWHITE);
    inp->SetForegroundColour(wxColour("#4F555A"));
    scale_font(inp, 1.3);
    input_wrap_sz->Add(inp, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, S(10));
    input_wrap->SetSizer(input_wrap_sz);

    auto *set_btn = new StaticBox(body, wxID_ANY);
    set_btn->SetMinSize(wxSize(S(50), S(29)));
    set_btn->SetCornerRadius(S(7));
    set_btn->SetBorderWidth(0);
    set_btn->SetBackgroundColorNormal(wxColour("#4E4E4E"));
    set_btn->SetBackgroundColour(wxColour("#4E4E4E"));
    set_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    auto *set_btn_sz = new wxBoxSizer(wxHORIZONTAL);
    auto *set_label = new wxStaticText(set_btn, wxID_ANY, _L("Set"));
    set_label->SetForegroundColour(*wxWHITE);
    set_label->SetCursor(wxCursor(wxCURSOR_HAND));
    scale_font(set_label, 1.3);
    set_btn_sz->AddStretchSpacer(1);
    set_btn_sz->Add(set_label, 0, wxALIGN_CENTER_VERTICAL);
    set_btn_sz->AddStretchSpacer(1);
    set_btn->SetSizer(set_btn_sz);

    auto *inp_row = new wxBoxSizer(wxHORIZONTAL);
    inp_row->Add(input_wrap, 0, wxALIGN_CENTER_VERTICAL);
    inp_row->Add(set_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, S(10));

    body_sz->Add(active_card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, S(14));
    body_sz->Add(inp_row, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, S(14));
    body->SetSizer(body_sz);
    shell_sz->Add(body, 1, wxEXPAND);

    dialog_shell->SetSizer(shell_sz);
    root->Add(dialog_shell, 1, wxEXPAND | wxALL, FromDIP(1));
    dlg.SetSizer(root);

    const auto apply_temp = [&]() {
        wxString raw = inp->GetValue();
        raw.Trim(true);
        raw.Trim(false);
        raw.Replace(wxString::FromUTF8("\xC2\xB0""C"), wxEmptyString, true);
        raw.Replace("C", wxEmptyString, true);
        raw.Trim(true);
        raw.Trim(false);
        long v = 0;
        if (!raw.ToLong(&v)) {
            wxMessageBox(_L("Please enter a valid temperature."), _L("Change Build Plate Temperature"), wxOK | wxICON_WARNING, &dlg);
            return;
        }
        v = std::max(min_t, std::min(max_t, v));
        obj->command_set_bed(static_cast<int>(v));
        dlg.EndModal(wxID_OK);
    };

    close_btn->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    set_btn->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    set_label->Bind(wxEVT_LEFT_DOWN, [&apply_temp](wxMouseEvent &) { apply_temp(); });
    inp->Bind(wxEVT_TEXT_ENTER, [&apply_temp](wxCommandEvent &) { apply_temp(); });
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent &e) {
        if (dlg.IsModal())
            dlg.EndModal(wxID_CANCEL);
        else
            e.Skip();
    });

    dlg.Fit();
    dlg.SetMinSize(dlg.GetSize());
    {
        const wxSize size = dlg.GetSize();
        wxBitmap shape_bmp(size.GetWidth(), size.GetHeight());
        wxMemoryDC dc(shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), S(10));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            dlg.SetShape(region);
    }
    dlg.CentreOnParent();
    dlg.ShowModal();
}

void PrinterWebView::apply_nozzle_target_temperature(int extruder_index, int temperature)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->GetExtderSystem() == nullptr)
        return;

    extruder_index = std::max(0, std::min(3, extruder_index));
    long min_t = 0;
    long max_t = 300;
    if (obj->nozzle_temp_range.size() >= 2) {
        min_t = obj->nozzle_temp_range[0];
        max_t = obj->nozzle_temp_range[1];
    }
    const long value = std::max(min_t, std::min(max_t, static_cast<long>(temperature)));
    obj->command_set_nozzle_new(extruder_index, static_cast<int>(value));
}

void PrinterWebView::apply_bed_target_temperature(int temperature)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->GetBed() == nullptr)
        return;

    const long min_t = 0;
    const long max_t = obj->get_bed_temperature_limit();
    const long value = std::max(min_t, std::min(max_t, static_cast<long>(temperature)));
    obj->command_set_bed(static_cast<int>(value));
}

void PrinterWebView::prompt_ps_target_temperature(bool is_bed, int extruder_index)
{
    if (!is_bed) {
        show_toolhead_temperature_dialog(extruder_index);
        return;
    }

    show_bed_temperature_dialog();
}

void PrinterWebView::show_filament_load_wizard()
{
    show_filament_busy_dialog(true);
}

namespace {

// Native dual-ring style spinner — avoids wxWebView's white startup flash.
class FilamentBusySpinner : public wxPanel
{
public:
    FilamentBusySpinner(wxWindow *parent, const wxSize &size, const wxColour &bg)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_timer(this)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(bg);
        Bind(wxEVT_PAINT, &FilamentBusySpinner::on_paint, this);
        Bind(wxEVT_TIMER, &FilamentBusySpinner::on_timer, this);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
        m_timer.Start(16);
    }

    ~FilamentBusySpinner() override
    {
        if (m_timer.IsRunning())
            m_timer.Stop();
    }

private:
    void on_timer(wxTimerEvent &)
    {
        m_angle_deg = std::fmod(m_angle_deg + 6.0, 360.0);
        Refresh(false);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxSize sz = GetClientSize();
        if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
            return;

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        const double side = std::min(sz.GetWidth(), sz.GetHeight());
        const double cx = sz.GetWidth() * 0.5;
        const double cy = sz.GetHeight() * 0.5;
        // Match filament_busy_spinner.svg: r=32, stroke=9 on 100x100 viewBox.
        const double radius = side * 0.32;
        const double stroke = std::max(2.0, side * 0.09);

        wxPen pen(wxColour("#25cc7c"), int(std::lround(stroke)));
        pen.SetCap(wxCAP_ROUND);
        pen.SetJoin(wxJOIN_ROUND);
        gc->SetPen(gc->CreatePen(pen));

        wxGraphicsPath path = gc->CreatePath();
        // Half-circle arc (dasharray ~50% of circumference), matching Dual Ring SVG.
        constexpr double kPi = 3.14159265358979323846;
        const double start_rad = m_angle_deg * kPi / 180.0;
        const double end_rad   = start_rad + kPi;
        path.AddArc(cx, cy, radius, start_rad, end_rad, true);
        gc->StrokePath(path);
    }

    wxTimer m_timer;
    double  m_angle_deg{0.0};
};

} // namespace

void PrinterWebView::show_filament_busy_dialog(bool is_load)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online() || obj->is_in_printing())
        return;

    const int tool_index = std::clamp(m_selected_filament_tool, 0, 3);
    const std::string slot = std::to_string(tool_index);
    BOOST_LOG_TRIVIAL(info) << "PrinterWebView: send filament "
                            << (is_load ? "load" : "unload") << " command slot=" << slot;
    obj->command_ams_change_filament(is_load, "0", slot);
    if (!is_load)
        clear_filament_selection_from_moonraker(tool_index + 1);

    wxDialog dlg(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    dlg.SetBackgroundColour(wxColour("#000000"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *shell = new StaticBox(&dlg, wxID_ANY);
    shell->SetCornerRadius(FromDIP(10));
    shell->SetBorderWidth(1);
    shell->SetBorderColorNormal(wxColour("#C7C7C7"));
    shell->SetBackgroundColorNormal(*wxWHITE);
    shell->SetBackgroundColour(*wxWHITE);
    shell->SetMinSize(wxSize(FromDIP(360), -1));

    auto *shell_sizer = new wxBoxSizer(wxVERTICAL);
    auto *title_row = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new wxStaticText(shell, wxID_ANY,
        wxString::Format(is_load ? "Tool %d loading..." : "Tool %d unloading...", tool_index + 1));
    title->SetForegroundColour(wxColour("#232527"));
    {
        wxFont f = title->GetFont();
        f.SetPointSize(std::max(12, f.GetPointSize() + 2));
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(f);
    }
    auto *close = new wxStaticText(shell, wxID_ANY, wxString::FromUTF8("\xC3\x97"));
    close->SetForegroundColour(wxColour("#232527"));
    close->SetCursor(wxCursor(wxCURSOR_HAND));
    title_row->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, FromDIP(22));
    title_row->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxTOP, FromDIP(18));
    shell_sizer->Add(title_row, 0, wxEXPAND);

    auto *accent = new wxPanel(shell, wxID_ANY);
    accent->SetMinSize(wxSize(FromDIP(42), FromDIP(4)));
    accent->SetMaxSize(wxSize(FromDIP(42), FromDIP(4)));
    accent->SetBackgroundColour(wxColour("#10B79A"));
    shell_sizer->Add(accent, 0, wxLEFT | wxTOP, FromDIP(22));

    auto *message = new wxStaticText(shell, wxID_ANY,
        wxString::Format(is_load ? "Tool %d filament loading is in progress." :
                                   "Tool %d filament unloading is in progress.",
                         tool_index + 1));
    message->SetForegroundColour(wxColour("#767C84"));
    shell_sizer->Add(message, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(22));

    const int spinner_px = FromDIP(72);
    auto *spinner = new FilamentBusySpinner(shell, wxSize(spinner_px, spinner_px), *wxWHITE);
    shell_sizer->Add(spinner, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(22));

    shell->SetSizer(shell_sizer);
    root->Add(shell, 1, wxEXPAND);
    dlg.SetSizer(root);
    dlg.Fit();
    dlg.SetMinSize(dlg.GetSize());

    close->Bind(wxEVT_LEFT_DOWN, [&dlg](wxMouseEvent &) { dlg.EndModal(wxID_OK); });
    dlg.Bind(wxEVT_CLOSE_WINDOW, [&dlg](wxCloseEvent &e) {
        if (dlg.IsModal())
            dlg.EndModal(wxID_CANCEL);
        else
            e.Skip();
    });

    dlg.CentreOnParent();
    dlg.ShowModal();
}

void PrinterWebView::ensure_storage_page_created()
{
    if (m_storage_page != nullptr || m_storage_placeholder == nullptr)
        return;

    wxSizer *const sizer = m_storage_placeholder->GetContainingSizer();
    wxWindow *const parent = m_storage_placeholder->GetParent();
    if (sizer == nullptr || parent == nullptr)
        return;

    m_storage_page = new CloudTaskManagerPage(parent, CloudTaskManagerPage::MediaPresentation::TimelapseOnly);
    m_storage_page->Hide();

    if (!sizer->Replace(m_storage_placeholder, m_storage_page, false)) {
        BOOST_LOG_TRIVIAL(error) << "PrinterWebView: ensure_storage_page_created Replace failed";
        delete m_storage_page;
        m_storage_page = nullptr;
        return;
    }

    m_storage_placeholder->Destroy();
    m_storage_placeholder = nullptr;
    parent->Layout();
}

void PrinterWebView::select_tab(PrinterWebViewTab tab)
{
    m_selected_tab = tab;

    if (tab == PrinterWebViewTab::Storage || tab == PrinterWebViewTab::PrintModels)
        ensure_storage_page_created();

    if (m_storage_page != nullptr) {
        m_storage_page->set_media_presentation(
            tab == PrinterWebViewTab::PrintModels
                ? CloudTaskManagerPage::MediaPresentation::ModelOnly
                : CloudTaskManagerPage::MediaPresentation::TimelapseOnly);
    }

    if (m_status_page != nullptr)
        m_status_page->Show(tab == PrinterWebViewTab::Status);
    if (m_storage_page != nullptr)
        m_storage_page->Show(tab == PrinterWebViewTab::Storage || tab == PrinterWebViewTab::PrintModels);
    else if (m_storage_placeholder != nullptr)
        m_storage_placeholder->Show(tab == PrinterWebViewTab::Storage || tab == PrinterWebViewTab::PrintModels);
    if (m_update_page != nullptr)
        m_update_page->Show(tab == PrinterWebViewTab::Update);
    if (m_assistant_page != nullptr)
        m_assistant_page->Show(tab == PrinterWebViewTab::Assistant);

    if ((tab == PrinterWebViewTab::Storage || tab == PrinterWebViewTab::PrintModels) && m_storage_page != nullptr) {
        m_storage_page->refresh_user_device();
        m_storage_page->update_page();
    }

    update_sidebar_selection();
    Layout();
    Refresh();
}

void PrinterWebView::update_sidebar_selection()
{
    for (auto &item : m_sidebar_items) {
        const bool selected = item.tab == m_selected_tab;
        if (item.panel != nullptr)
            item.panel->SetBackgroundColour(selected ? wxColour(243, 248, 241) : *wxWHITE);
        if (item.active_strip != nullptr)
            item.active_strip->SetBackgroundColour(selected ? wxColour(47, 181, 90) : *wxWHITE);
        if (item.label != nullptr)
            item.label->SetForegroundColour(wxColour("#232527"));
        if (item.chevron != nullptr)
            item.chevron->SetForegroundColour(selected ? wxColour("#232527") : wxColour("#767C84"));
    }
}

wxPanel *PrinterWebView::create_placeholder_page(wxWindow *parent, const wxString &title, const wxString &description)
{
    auto *page = new wxPanel(parent, wxID_ANY);
    page->SetBackgroundColour(wxColour("#EEEEEF"));

    auto *page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->AddStretchSpacer(1);

    auto *card = new StaticBox(page, wxID_ANY);
    card->SetCornerRadius(0);
    card->SetBorderWidth(0);
    card->SetBackgroundColorNormal(*wxWHITE);
    card->SetBackgroundColour(*wxWHITE);
    card->SetMinSize(wxSize(FromDIP(520), FromDIP(220)));

    auto *card_sizer = new wxBoxSizer(wxVERTICAL);
    card_sizer->AddStretchSpacer(1);

    auto *title_label = new wxStaticText(card, wxID_ANY, title);
    title_label->SetForegroundColour(wxColour("#232527"));
    wxFont title_font = title_label->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 4);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_label->SetFont(title_font);
    card_sizer->Add(title_label, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));

    auto *description_label = new wxStaticText(card, wxID_ANY, description);
    description_label->SetForegroundColour(wxColour("#767C84"));
    card_sizer->Add(description_label, 0, wxALIGN_CENTER_HORIZONTAL);

    card_sizer->AddStretchSpacer(1);
    card->SetSizer(card_sizer);

    page_sizer->Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
    page_sizer->AddStretchSpacer(1);
    page->SetSizer(page_sizer);
    return page;
}

wxPanel *PrinterWebView::create_update_page(wxWindow *parent)
{
    auto *page = new wxPanel(parent, wxID_ANY);
    page->SetBackgroundColour(wxColour("#EEEEEF"));

    auto *page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->AddSpacer(FromDIP(40));

    auto *card = new StaticBox(page, wxID_ANY);
    card->SetCornerRadius(0);
    card->SetBorderWidth(0);
    card->SetBackgroundColorNormal(*wxWHITE);
    card->SetBackgroundColour(*wxWHITE);
    card->SetMinSize(wxSize(FromDIP(728), FromDIP(481)));

    auto *card_sizer = new wxBoxSizer(wxVERTICAL);

    auto *header = new wxPanel(card, wxID_ANY);
    header->SetBackgroundColour(*wxWHITE);
    header->SetMinSize(wxSize(-1, FromDIP(75)));
    auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_update_header_title = new wxStaticText(header, wxID_ANY, "P1P(Bosta)");
    wxFont header_font = m_update_header_title->GetFont();
    header_font.SetPointSize(header_font.GetPointSize() + 4);
    header_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_update_header_title->SetFont(header_font);
    m_update_header_title->SetForegroundColour(wxColour("#232527"));
    header_sizer->Add(m_update_header_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(29));
    m_update_connection_badge = new wxStaticText(header, wxID_ANY, "");
    m_update_connection_badge->SetForegroundColour(wxColour("#35CE82"));
    header_sizer->Add(m_update_connection_badge, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
    header_sizer->AddStretchSpacer(1);
    header->SetSizer(header_sizer);
    card_sizer->Add(header, 0, wxEXPAND);

    auto *body = new wxPanel(card, wxID_ANY);
    body->SetBackgroundColour(*wxWHITE);
    auto *body_sizer = new wxBoxSizer(wxVERTICAL);

    auto *top_row = new wxBoxSizer(wxHORIZONTAL);
    top_row->AddSpacer(FromDIP(31));

    m_update_printer_bitmap = new wxStaticBitmap(body, wxID_ANY, create_quadro_printer_thumbnail(this, 202));
    m_update_printer_bitmap->SetMinSize(wxSize(FromDIP(241), FromDIP(182)));
    top_row->Add(m_update_printer_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(31));

    auto *info_box = new StaticBox(body, wxID_ANY);
    info_box->SetCornerRadius(FromDIP(6));
    info_box->SetBorderWidth(1);
    info_box->SetBorderColorNormal(wxColour("#C7C7C7"));
    info_box->SetBackgroundColorNormal(*wxWHITE);
    info_box->SetBackgroundColour(*wxWHITE);
    info_box->SetMinSize(wxSize(FromDIP(351), FromDIP(156)));

    auto *info_col = new wxBoxSizer(wxVERTICAL);
    info_col->AddSpacer(FromDIP(21));

    auto make_info_row = [info_box, this](const wxString &label_text, wxStaticText **value_out) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        auto *label = new wxStaticText(info_box, wxID_ANY, label_text);
        wxFont label_font = label->GetFont();
        label_font.SetWeight(wxFONTWEIGHT_BOLD);
        label->SetFont(label_font);
        label->SetForegroundColour(wxColour("#767C84"));
        row->Add(label, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(23));

        auto *value = new wxStaticText(info_box, wxID_ANY, "-");
        value->SetForegroundColour(wxColour("#232527"));
        row->AddStretchSpacer(1);
        row->Add(value, 0, wxALIGN_CENTER_VERTICAL);
        *value_out = value;
        return row;
    };

    info_col->Add(make_info_row("Model", &m_update_model_value), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    info_col->Add(make_info_row("Serial", &m_update_serial_value), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    info_col->Add(make_info_row("Version", &m_update_version_value), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    info_box->SetSizer(info_col);
    top_row->Add(info_box, 0, wxALIGN_CENTER_VERTICAL);
    top_row->AddStretchSpacer(1);
    body_sizer->Add(top_row, 0, wxEXPAND | wxBOTTOM, FromDIP(18));

    auto *status_box = new StaticBox(body, wxID_ANY);
    status_box->SetCornerRadius(FromDIP(6));
    status_box->SetBorderWidth(1);
    status_box->SetBorderColorNormal(wxColour("#C7C7C7"));
    status_box->SetBackgroundColorNormal(*wxWHITE);
    status_box->SetBackgroundColour(*wxWHITE);
    status_box->SetMinSize(wxSize(FromDIP(663), FromDIP(91)));
    auto *status_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto *refresh_mark = new wxStaticBitmap(status_box, wxID_ANY, create_scaled_bitmap("coprint_sync_icon_exact", this, 57));
    refresh_mark->SetMinSize(wxSize(FromDIP(57), FromDIP(57)));
    status_sizer->Add(refresh_mark, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(21));

    auto *status_col = new wxBoxSizer(wxVERTICAL);
    m_update_status_value = new wxStaticText(status_box, wxID_ANY, "Waiting for update information");
    m_update_status_value->SetForegroundColour(wxColour(35, 206, 130));
    status_col->Add(m_update_status_value, 0, wxBOTTOM, FromDIP(13));

    auto *progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_update_progress_gauge = new ProgressBar(status_box, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(468), FromDIP(21)), false);
    m_update_progress_gauge->SetMinSize(wxSize(FromDIP(468), FromDIP(21)));
    m_update_progress_gauge->SetMaxSize(wxSize(FromDIP(468), FromDIP(21)));
    m_update_progress_gauge->SetRadius(FromDIP(10));
    m_update_progress_gauge->SetPadding(FromDIP(3));
    m_update_progress_gauge->SetProgressForedColour(wxColour(132, 162, 188));
    m_update_progress_gauge->SetProgressBackgroundColour(wxColour("#35CE82"));
    m_update_progress_gauge->SetBackgroundColour(*wxWHITE);
    m_update_progress_gauge->SetValue(0);
    m_update_progress_gauge->Hide();
    progress_row->Add(m_update_progress_gauge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    m_update_percent_value = new wxStaticText(status_box, wxID_ANY, "0%");
    m_update_percent_value->SetForegroundColour(wxColour(35, 206, 130));
    m_update_percent_value->Hide();
    progress_row->Add(m_update_percent_value, 0, wxALIGN_CENTER_VERTICAL);
    status_col->Add(progress_row, 0, wxEXPAND);
    status_sizer->Add(status_col, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(26));
    status_sizer->AddStretchSpacer(1);
    status_box->SetSizer(status_sizer);
    body_sizer->Add(status_box, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    auto *actions = new wxPanel(body, wxID_ANY);
    actions->SetBackgroundColour(*wxWHITE);
    auto *actions_sizer = new wxBoxSizer(wxHORIZONTAL);
    actions_sizer->AddSpacer(FromDIP(16));

    auto *update_button = new Button(actions, "Update Firmware");
    m_update_firmware_button = update_button;
    update_button->SetMinSize(wxSize(FromDIP(189), FromDIP(44)));
    update_button->SetCornerRadius(FromDIP(8));
    update_button->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour("#23C77A"), StateColor::Normal)));
    update_button->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour("#23C77A"), StateColor::Normal)));
    update_button->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal)));
    update_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj == nullptr || !obj->is_connected()) {
            wxMessageBox("Connect to a printer before updating firmware.", "Update Firmware",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (m_update_sim_active || obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress)
            return;

        wxMessageDialog confirm(this,
            "Start firmware update? Progress will be shown while the update runs.",
            "Update Firmware",
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;

        // UI progress preview (Moonraker printers do not use Bambu OTA push here).
        m_update_sim_active = true;
        m_update_sim_percent = 0;
        obj->upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
        obj->upgrade_progress = "0";
        obj->upgrade_status = "UPGRADING";
        obj->upgrade_err_code = UpgradeNoError;
        if (m_update_firmware_button != nullptr)
            m_update_firmware_button->Disable();

        if (m_update_progress_timer == nullptr) {
            m_update_progress_timer = new wxTimer(this);
            Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
                if (!m_update_sim_active || m_update_progress_timer == nullptr)
                    return;

                m_update_sim_percent = std::min(100, m_update_sim_percent + 4);
                auto *dm = wxGetApp().getDeviceManager();
                MachineObject *machine = dm ? dm->get_selected_machine() : nullptr;
                if (machine != nullptr) {
                    machine->upgrade_progress = std::to_string(m_update_sim_percent);
                    machine->upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
                }

                if (m_update_sim_percent >= 100) {
                    m_update_progress_timer->Stop();
                    m_update_sim_active = false;
                    if (machine != nullptr) {
                        machine->upgrade_display_state = DevFirmwareUpgradingState::UpgradingFinished;
                        machine->upgrade_progress = "100";
                        machine->upgrade_status = "UPGRADE_SUCCESS";
                        machine->upgrade_err_code = UpgradeNoError;
                    }
                }
                refresh_update_page_from_selected_machine();
            }, m_update_progress_timer->GetId());
        }
        m_update_progress_timer->Start(250);
        refresh_update_page_from_selected_machine();
    });
    actions_sizer->Add(update_button, 0, wxRIGHT, FromDIP(18));

    auto *export_log_button = new Button(actions, "Export Log");
    export_log_button->SetMinSize(wxSize(FromDIP(189), FromDIP(44)));
    export_log_button->SetCornerRadius(FromDIP(8));
    export_log_button->SetBackgroundColor(StateColor(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal)));
    export_log_button->SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour("#C7C7C7"), StateColor::Normal)));
    export_log_button->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#232527"), StateColor::Normal)));
    export_log_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        const std::string base = moonraker_base_url(obj);
        if (base.empty()) {
            wxMessageBox("No printer address is available for log export.", "Export Log", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxFileDialog save_dialog(
            this,
            "Export Log",
            wxEmptyString,
            "klippy.log",
            "Log files (*.log)|*.log|All files (*.*)|*.*",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );

        if (save_dialog.ShowModal() != wxID_OK)
            return;

        const std::string url = base + "/server/files/logs/klippy.log";
        const wxString save_path = save_dialog.GetPath();
        const std::weak_ptr<int> lifetime = m_lifetime_token;

        std::thread([this, lifetime, url, save_path]() {
            std::string body;
            std::string error;
            unsigned status = 0;

            try {
                Http::get(url)
                    .timeout_connect(5)
                    .timeout_max(30)
                    .on_complete([&](std::string response, unsigned http_status) {
                        body = std::move(response);
                        status = http_status;
                    })
                    .on_error([&](std::string response, std::string err, unsigned http_status) {
                        body = std::move(response);
                        error = std::move(err);
                        status = http_status;
                    })
                    .perform_sync();
            } catch (const std::exception &e) {
                error = e.what();
            }

            wxGetApp().CallAfter([this, lifetime, save_path, url, body = std::move(body), error = std::move(error), status]() {
                if (lifetime.expired())
                    return;

                if (!error.empty() || status == 0 || status >= 400) {
                    BOOST_LOG_TRIVIAL(error) << "PrinterWebView: failed to download printer log from " << url
                                             << ", status=" << status << ", error=" << error;
                    wxString reason;
                    if (!error.empty())
                        reason = wxString::FromUTF8(error);
                    else if (status != 0)
                        reason = wxString::Format("HTTP %u", status);
                    else
                        reason = "No response from printer";

                    wxMessageBox(
                        wxString::Format("Failed to download the printer log file.\n\nURL: %s\nReason: %s",
                                         wxString::FromUTF8(url),
                                         reason),
                        "Export Log",
                        wxOK | wxICON_ERROR,
                        this);
                    return;
                }

                wxFile file(save_path, wxFile::write);
                if (!file.IsOpened() || file.Write(body.data(), body.size()) != body.size()) {
                    BOOST_LOG_TRIVIAL(error) << "PrinterWebView: failed to save printer log to " << save_path.ToUTF8().data();
                    wxMessageBox("Failed to save the printer log file.", "Export Log", wxOK | wxICON_ERROR, this);
                    return;
                }

                BOOST_LOG_TRIVIAL(info) << "PrinterWebView: exported printer log from " << url
                                        << " to " << save_path.ToUTF8().data();
                wxMessageBox("Printer log exported successfully.", "Export Log", wxOK | wxICON_INFORMATION, this);
            });
        }).detach();
    });
    actions_sizer->Add(export_log_button, 0, wxRIGHT, FromDIP(29));

    m_update_release_note_link = new wxStaticText(actions, wxID_ANY, "Release notes");
    m_update_release_note_link->SetForegroundColour(wxColour(94, 156, 255));
    actions_sizer->Add(m_update_release_note_link, 0, wxALIGN_CENTER_VERTICAL);
    actions_sizer->AddStretchSpacer(1);
    actions->SetSizer(actions_sizer);
    body_sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    body->SetSizer(body_sizer);

    card_sizer->Add(body, 1, wxEXPAND);
    card->SetSizer(card_sizer);

    page_sizer->Add(card, 0, wxLEFT | wxRIGHT, FromDIP(31));
    page_sizer->AddStretchSpacer(1);
    page->SetSizer(page_sizer);
    return page;
}

void PrinterWebView::clear_preview_thumbnail()
{
    if (m_preview_thumbnail == nullptr)
        return;

    m_preview_thumbnail_url.clear();
    m_thumbnail_image = wxImage();
    if (m_dashboard_page != nullptr)
        m_dashboard_page->print_status_panel()->reset_thumbnail_placeholder();
    else
        m_preview_thumbnail->SetBitmap(wxNullBitmap);
    Layout();
}

void PrinterWebView::on_thumbnail_webrequest_state(wxWebRequestEvent &evt)
{
    if (!m_thumbnail_web_request.IsOk())
        return;

    switch (evt.GetState()) {
    case wxWebRequest::State_Completed: {
        m_thumbnail_image = *evt.GetResponse().GetStream();
        if (!m_dashboard_state_store.state().print_job.has_active_job) {
            clear_preview_thumbnail();
        } else if (m_preview_thumbnail != nullptr && m_thumbnail_image.IsOk()) {
            const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(120), FromDIP(120));
            wxImage resized = scale_preview_thumbnail(m_thumbnail_image, target.GetWidth(), target.GetHeight());
            m_preview_thumbnail->SetBitmap(wxBitmap(resized));
            Layout();
        } else {
            clear_preview_thumbnail();
        }
        break;
    }
    case wxWebRequest::State_Failed:
    case wxWebRequest::State_Cancelled:
    case wxWebRequest::State_Unauthorized:
        clear_preview_thumbnail();
        break;
    case wxWebRequest::State_Active:
    case wxWebRequest::State_Idle:
        break;
    default:
        break;
    }
}

void PrinterWebView::update_preview_thumbnail(const MachineObject *obj, bool has_active_job)
{
    if (!has_active_job || obj == nullptr) {
        if (m_thumbnail_web_request.IsOk())
            m_thumbnail_web_request.Cancel();
        clear_preview_thumbnail();
        return;
    }

    wxString next_source = m_dashboard_state_store.state().print_job.thumbnail_url;
    if (next_source.IsEmpty() && obj->slice_info != nullptr)
        next_source = from_u8(obj->slice_info->thumbnail_url);
    if (next_source.IsEmpty() && obj->slice_info != nullptr && !obj->slice_info->thumbnail_dir.empty() && !obj->slice_info->thumbnail_name.empty()) {
        wxFileName thumbnail_file(from_u8(obj->slice_info->thumbnail_dir), from_u8(obj->slice_info->thumbnail_name));
        next_source = thumbnail_file.GetFullPath();
    }

    if (next_source.IsEmpty()) {
        if (m_thumbnail_web_request.IsOk())
            m_thumbnail_web_request.Cancel();
        clear_preview_thumbnail();
        return;
    }

    if (next_source == m_preview_thumbnail_url)
        return;

    if (m_thumbnail_web_request.IsOk())
        m_thumbnail_web_request.Cancel();

    if (!is_http_url(next_source)) {
        const wxString local_path = normalize_local_file_url(next_source);
        wxImage local_image;
        if (wxFileName::FileExists(local_path) && local_image.LoadFile(local_path, wxBITMAP_TYPE_ANY) && local_image.IsOk()) {
            m_preview_thumbnail_url = next_source;
            m_thumbnail_image = local_image;
            const wxSize target = preview_thumbnail_target_size(m_preview_thumbnail, FromDIP(120), FromDIP(120));
            m_preview_thumbnail->SetBitmap(wxBitmap(scale_preview_thumbnail(local_image, target.GetWidth(), target.GetHeight())));
            Layout();
            return;
        }
        clear_preview_thumbnail();
        return;
    }

    m_preview_thumbnail_url = next_source;
    m_thumbnail_web_request = wxWebSession::GetDefault().CreateRequest(this, m_preview_thumbnail_url);
    if (!m_thumbnail_web_request.IsOk()) {
        clear_preview_thumbnail();
        return;
    }

    m_thumbnail_web_request.Start();
}


void PrinterWebView::handle_dashboard_command(const DeviceDashboard::DeviceCommand &command)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    switch (command.kind) {
    case DeviceDashboard::DeviceCommandKind::SelectTool:
        if (command.tool_index < 0 || command.tool_index >= m_dashboard_state_store.state().movement.available_tool_count)
            return;
        if (!send_tool_select_command(command.tool_index))
            apply_printer_status_tool_selection(command.tool_index);
        break;
    case DeviceDashboard::DeviceCommandKind::SelectFilamentTool:
        apply_filament_tool_selection(command.tool_index);
        break;
    case DeviceDashboard::DeviceCommandKind::AssignModelSlotToTool:
        set_filament_assigned_tool(command.model_slot, command.tool_index + 1, true);
        break;
    case DeviceDashboard::DeviceCommandKind::LoadFilament:
        apply_filament_tool_selection(command.tool_index);
        prompt_and_save_filament_selection_then_load();
        break;
    case DeviceDashboard::DeviceCommandKind::UnloadFilament:
        apply_filament_tool_selection(command.tool_index);
        show_filament_busy_dialog(false);
        break;
    case DeviceDashboard::DeviceCommandKind::ConfigureFilament:
        apply_filament_tool_selection(command.tool_index);
        show_filament_material_dialog(false, wxPoint(command.screen_x, command.screen_y));
        break;
    case DeviceDashboard::DeviceCommandKind::SetMotionDistance:
        m_axis_move_step = command.value > 0.0 ? command.value : 1.0;
        break;
    case DeviceDashboard::DeviceCommandKind::PausePrint:
        if (obj == nullptr || !obj->is_online())
            return;
        if (!send_print_control_command(false))
            obj->command_task_pause();
        break;
    case DeviceDashboard::DeviceCommandKind::ResumePrint:
        if (obj == nullptr || !obj->is_online())
            return;
        obj->command_task_resume();
        break;
    case DeviceDashboard::DeviceCommandKind::StopPrint:
        if (obj == nullptr || !obj->is_online())
            return;
        {
            wxMessageDialog confirm(this,
                _L("This will permanently stop the current print. Do you want to continue?"),
                _L("Stop print"),
                wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
            if (confirm.ShowModal() != wxID_YES)
                return;
        }
        if (!send_print_control_command(true))
            obj->command_task_abort();
        break;
    case DeviceDashboard::DeviceCommandKind::Home:
        if (obj == nullptr || !obj->is_online())
            return;
        obj->command_go_home();
        break;
    case DeviceDashboard::DeviceCommandKind::SetPrintSpeed: {
        if (obj == nullptr || !obj->is_online())
            return;
        const int percent = static_cast<int>(std::round(command.value));
        DevPrintingSpeedLevel level = SPEED_LEVEL_NORMAL;
        if (percent <= 50)
            level = SPEED_LEVEL_SILENCE;
        else if (percent <= 100)
            level = SPEED_LEVEL_NORMAL;
        else if (percent <= 125)
            level = SPEED_LEVEL_RAPID;
        else
            level = SPEED_LEVEL_RAMPAGE;
        obj->command_set_printing_speed(level);
        break;
    }
    case DeviceDashboard::DeviceCommandKind::MoveAxis: {
        if (obj == nullptr || !obj->is_online())
            return;
        std::string axis;
        int speed = 3000;
        switch (command.axis) {
        case DeviceDashboard::Axis::X: axis = "X"; break;
        case DeviceDashboard::Axis::Y: axis = "Y"; break;
        case DeviceDashboard::Axis::Z:
            axis = "Z";
            speed = 900;
            break;
        }
        if (!axis.empty())
            obj->command_axis_control(axis, 1.0, command.value, speed);
        break;
    }
    default:
        break;
    }
}

void PrinterWebView::refresh_dashboard_panels(MachineObject *obj)
{
    auto dashboard_state = DeviceDashboard::DashboardStateAdapter::from_machine(obj);
    if (m_moonraker_available_tool_count > 0)
        dashboard_state.movement.available_tool_count = m_moonraker_available_tool_count;
    else if (obj == nullptr || !obj->is_online()) {
        const int model_tool_count = coprint_tool_count_override(obj);
        if (model_tool_count > 0)
            dashboard_state.movement.available_tool_count = model_tool_count;
    }
    dashboard_state.filament = m_dashboard_state_store.state().filament;
    dashboard_state.filament.selected_tool    = std::clamp(m_selected_filament_tool, 0, 3);
    dashboard_state.filament.can_load_unload  = obj != nullptr && obj->is_online() && !obj->is_in_printing();
    dashboard_state.filament.is_loading       = false;
    dashboard_state.filament.loading_tool     = -1;
    dashboard_state.movement.selected_tool    = m_selected_extruder_index;
    dashboard_state.movement.selected_tool    = std::clamp(
        dashboard_state.movement.selected_tool,
        0,
        std::max(0, dashboard_state.movement.available_tool_count - 1));
    dashboard_state.movement.selected_distance_mm = m_axis_move_step;

    if (obj != nullptr && obj->is_online()) {
        if (auto* extruders = obj->GetExtderSystem()) {
            dashboard_state.filament.is_loading = extruders->IsBusyLoading();
            const int loading_tool = extruders->GetLoadingExtderId();
            dashboard_state.filament.loading_tool =
                dashboard_state.filament.is_loading && loading_tool >= 0 && loading_tool < DeviceDashboard::MaxDashboardTools
                    ? loading_tool
                    : -1;
        }

        for (int i = 0; i < DeviceDashboard::MaxDashboardTools; ++i) {
            if (m_filament_tool_has_color[i]) {
                dashboard_state.filament.tools[i].color = m_filament_loaded_tool_colors[i];
                dashboard_state.filament.tools[i].material = is_empty_filament_material(m_filament_loaded_tool_materials[i])
                    ? wxString::FromUTF8("Empty")
                    : m_filament_loaded_tool_materials[i];
            } else {
                dashboard_state.filament.tools[i].color = wxColour();
                dashboard_state.filament.tools[i].material = wxString::FromUTF8("Empty");
            }
        }
    }

    // Moonraker WebSocket verileri MachineObject içinde değil, PrinterWebView'ın
    // kendi m_moonraker_* alanlarında tutuluyor — DashboardStateAdapter çıktısının üzerine yaz.
    if (m_has_moonraker_status) {
        dashboard_state.bed.temperature.available = true;
        dashboard_state.bed.temperature.current   = m_moonraker_bed_current;
        dashboard_state.bed.temperature.target    = m_moonraker_bed_target;
        for (int i = 0; i < DeviceDashboard::MaxDashboardTools; ++i) {
            dashboard_state.tools[i].nozzle.available = true;
            dashboard_state.tools[i].nozzle.current   = m_moonraker_nozzle_current[i];
            dashboard_state.tools[i].nozzle.target    = m_moonraker_nozzle_target[i];
            if (m_moonraker_fan_available[i]) {
                dashboard_state.tools[i].fan.available = true;
                dashboard_state.tools[i].fan.percent   = m_moonraker_fan_percent[i];
            }
            dashboard_state.tools[i].active           = i == m_selected_extruder_index;
        }
        dashboard_state.movement.selected_tool = std::clamp(
            m_selected_extruder_index,
            0,
            std::max(0, dashboard_state.movement.available_tool_count - 1));
    }
    if (m_has_moonraker_print_status) {
        if (m_moonraker_print_job.has_active_job) {
            DeviceDashboard::PrintJobState merged = m_moonraker_print_job;
            const DeviceDashboard::PrintJobState &adapter_job = dashboard_state.print_job;
            if (merged.remaining_seconds <= 0 && adapter_job.remaining_seconds > 0)
                merged.remaining_seconds = adapter_job.remaining_seconds;
            if (merged.elapsed_seconds <= 0 && adapter_job.elapsed_seconds > 0)
                merged.elapsed_seconds = adapter_job.elapsed_seconds;
            dashboard_state.print_job = merged;
        } else {
            dashboard_state.print_job = DeviceDashboard::PrintJobState();
        }
    }

    m_dashboard_state_store.set_state(dashboard_state);
    const auto &state = m_dashboard_state_store.state();
    if (m_dashboard_page != nullptr)
        m_dashboard_page->apply_state(m_dashboard_state_store.state());
}

void PrinterWebView::refresh_connected_printer_header(MachineObject *obj)
{
    if (m_connected_printer_panel == nullptr ||
        m_connected_printer_status_label == nullptr ||
        m_connected_printer_logout_label == nullptr)
        return;

    const bool has_connected = m_has_active_printer_connection && obj != nullptr && obj->is_online();
    bool changed = false;
    wxFont font = m_connected_printer_status_label->GetFont();
    if (has_connected) {
        changed |= set_text_if_changed(m_connected_printer_status_label, from_u8(obj->get_dev_name()));
        m_connected_printer_status_label->SetForegroundColour(wxColour(235, 235, 235));
        font.SetWeight(wxFONTWEIGHT_BOLD);
    } else {
        changed |= set_text_if_changed(m_connected_printer_status_label,
            wxString::FromUTF8("Ba\xC4\x9Fl\xC4\xB1 yaz\xC4\xB1""c\xC4\xB1 yok"));
        m_connected_printer_status_label->SetForegroundColour(wxColour(150, 156, 166));
        font.SetWeight(wxFONTWEIGHT_NORMAL);
    }
    m_connected_printer_status_label->SetFont(font);
    changed |= m_connected_printer_logout_label->IsShown() != has_connected;
    m_connected_printer_logout_label->Show(has_connected);
    if (changed && m_connected_printer_panel->GetParent() != nullptr) {
        m_connected_printer_panel->GetParent()->Layout();
        m_connected_printer_panel->GetParent()->Refresh();
    }
}

void PrinterWebView::refresh_printer_info_labels(MachineObject *obj)
{
    set_text_if_changed(m_printer_name_value,
        obj != nullptr ? from_u8(obj->get_dev_name()) : "N/A");
    set_text_if_changed(m_printer_model_value,
        obj != nullptr ? obj->get_printer_type_display_str() : "N/A");

    if (m_printer_serial_value != nullptr) {
        wxString serial = obj != nullptr ? from_u8(obj->get_dev_id()) : "N/A";
        set_text_if_changed(m_printer_serial_value, serial.MakeUpper());
    }

    if (m_printer_firmware_value != nullptr) {
        wxString version = "N/A";
        if (obj != nullptr) {
            auto ota_it = obj->module_vers.find("ota");
            version = ota_it != obj->module_vers.end()
                ? ota_it->second.sw_ver
                : from_u8(obj->get_ota_version());
            if (version.empty())
                version = "N/A";
        }
        set_text_if_changed(m_printer_firmware_value, version);
    }

    if (m_printer_photo_bitmap != nullptr && obj != nullptr) {
        const wxBitmap bmp = create_dark_printer_thumbnail(this, obj->get_printer_thumbnail_img_str(), 82);
        if (bmp.IsOk())
            m_printer_photo_bitmap->SetBitmap(bmp);
    }
}

void PrinterWebView::start_camera_stream()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string dev_id = obj != nullptr ? obj->get_dev_id() : "";
    if (dev_id != m_camera_machine_id)
        m_camera_machine_id = dev_id;
    m_camera_stream_url.clear();
    m_camera_stream_requested = true;
    if (m_dashboard_page != nullptr && m_dashboard_page->camera_panel() != nullptr)
        m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Initializing);
    refresh_camera_stream(obj);
}

void PrinterWebView::stop_camera_stream()
{
    m_camera_stream_requested = false;
    m_camera_stream_url.clear();
    if (m_camera_webview != nullptr)
        m_camera_webview->SetPage("<!doctype html><html><body style='margin:0;background:#000'></body></html>", "");
    if (m_dashboard_page != nullptr && m_dashboard_page->camera_panel() != nullptr)
        m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Idle);
}

void PrinterWebView::handle_camera_webview_title(const wxString &title)
{
    if (!m_camera_stream_requested || m_dashboard_page == nullptr || m_dashboard_page->camera_panel() == nullptr)
        return;
    if (title == "coprint-camera-ok") {
        m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Live);
        return;
    }
    if (title != "coprint-camera-fail")
        return;
    m_camera_stream_requested = false;
    if (m_camera_webview != nullptr)
        m_camera_webview->SetPage("<!doctype html><html><body style='margin:0;background:#000'></body></html>", "");
    m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Failed);
}

void PrinterWebView::refresh_camera_stream(MachineObject *obj)
{
    const std::string next_machine_id = obj != nullptr ? obj->get_dev_id() : "";
    const std::vector<wxString> camera_urls = configured_camera_stream_urls(obj);
    const wxString next_url = camera_urls.empty() ? wxString() : camera_urls.front();

    const bool machine_changed = next_machine_id != m_camera_machine_id;
    if (machine_changed) {
        m_camera_stream_requested = false;
        if (m_camera_webview != nullptr)
            m_camera_webview->SetPage("<!doctype html><html><body style='margin:0;background:#000'></body></html>", "");
        m_camera_machine_id = next_machine_id;
        m_camera_stream_url.clear();
        if (m_dashboard_page != nullptr && m_dashboard_page->camera_panel() != nullptr)
            m_dashboard_page->camera_panel()->set_load_state(DeviceDashboard::CameraLoadState::Idle);
        return;
    }

    if (!m_camera_stream_requested)
        return;

    DeviceDashboard::CameraPanel *cam =
        m_dashboard_page != nullptr ? m_dashboard_page->camera_panel() : nullptr;
    const bool stream_changed = next_url != m_camera_stream_url;
    m_camera_stream_url = next_url;

    if (next_url.IsEmpty()) {
        m_camera_stream_requested = false;
        if (m_camera_webview != nullptr)
            m_camera_webview->SetPage("<!doctype html><html><body style='margin:0;background:#000'></body></html>", "");
        if (cam != nullptr)
            cam->set_load_state(DeviceDashboard::CameraLoadState::Failed);
        return;
    }

    if (cam != nullptr && cam->load_state() != DeviceDashboard::CameraLoadState::Live)
        cam->set_load_state(DeviceDashboard::CameraLoadState::Initializing);

    if (m_camera_webview_host != nullptr)
        ensure_camera_webview_created();

    if (m_camera_webview == nullptr) {
        m_camera_stream_requested = false;
        if (cam != nullptr)
            cam->set_load_state(DeviceDashboard::CameraLoadState::Failed);
        return;
    }

    if (stream_changed)
        m_camera_webview->SetPage(camera_stream_page(camera_urls), m_camera_stream_url.BeforeLast('/'));
}

void PrinterWebView::toggle_camera_timelapse()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (obj == nullptr || !obj->is_online()) {
        wxMessageBox(_L("Timelapse is unavailable."), _L("Timelapse"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    const bool enable = !obj->is_timelapse();
    if (enable) {
        wxString error_message;
        if (!obj->canEnableTimelapse(error_message)) {
            wxMessageBox(error_message, _L("Timelapse"), wxOK | wxICON_INFORMATION, this);
            return;
        }
    }

    obj->command_ipcam_timelapse(enable);
}

void PrinterWebView::refresh_layer_info_from_selected_machine()
{
    if (m_destroying)
        return;
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    const std::string machine_id = obj != nullptr ? obj->get_dev_id() : std::string();
    const bool machine_changed = machine_id != m_last_refresh_machine_id;
    m_last_refresh_machine_id = machine_id;
    if (machine_changed) {
        m_has_moonraker_status = false;
        m_has_moonraker_print_status = false;
        m_moonraker_print_job = DeviceDashboard::PrintJobState();
        m_moonraker_available_tool_count = 0;
        m_moonraker_status_fetch_in_progress = false;
        m_moonraker_status_machine_id = machine_id;
        m_filament_tool_has_color = {};
        m_filament_loaded_tool_colors = {};
        m_filament_loaded_tool_materials = {};
        m_filament_preview_fetch_in_progress = false;
        m_filament_preview_fetch_key.clear();
        m_camera_machine_id.clear();
        m_camera_stream_url.clear();
    }
    const bool periodic_heavy_refresh = (++m_refresh_tick_counter % 5) == 0;
    const bool has_active_job = m_dashboard_state_store.state().print_job.has_active_job;

    update_sidebar_connect_attempt_state();
    refresh_moonraker_status_from_selected_machine();
    refresh_dashboard_panels(obj);
    if (machine_changed || periodic_heavy_refresh || has_active_job)
        update_preview_thumbnail(obj, has_active_job);
    if (machine_changed || periodic_heavy_refresh || has_active_job)
        refresh_filament_preview_from_selected_machine();
    if (machine_changed || periodic_heavy_refresh) {
        refresh_connected_printer_header(obj);
        refresh_printer_info_labels(obj);
        refresh_camera_stream(obj);
    }
    if (m_selected_tab == PrinterWebViewTab::Update && (machine_changed || periodic_heavy_refresh))
        refresh_update_page_from_selected_machine();
}

void PrinterWebView::refresh_update_page_from_selected_machine()
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    auto *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
    if (m_update_page == nullptr)
        return;

    if (obj == nullptr) {
        if (m_update_header_title != nullptr) m_update_header_title->SetLabelText("No printer selected");
        if (m_update_connection_badge != nullptr) m_update_connection_badge->SetLabelText("");
        if (m_update_model_value != nullptr) m_update_model_value->SetLabelText("-");
        if (m_update_serial_value != nullptr) m_update_serial_value->SetLabelText("-");
        if (m_update_version_value != nullptr) m_update_version_value->SetLabelText("-");
        if (m_update_status_value != nullptr) m_update_status_value->SetLabelText("Waiting for printer connection");
        if (m_update_percent_value != nullptr) {
            m_update_percent_value->SetLabelText("0%");
            m_update_percent_value->Hide();
        }
        if (m_update_progress_gauge != nullptr) {
            m_update_progress_gauge->SetValue(0);
            m_update_progress_gauge->Hide();
        }
        if (m_update_firmware_button != nullptr)
            m_update_firmware_button->Disable();
        m_update_page->Layout();
        return;
    }

    if (m_update_header_title != nullptr) {
        const wxString header_title = wxString::Format("%s (%s)", from_u8(obj->get_dev_name()), obj->is_connected() ? "Ready" : "Offline");
        m_update_header_title->SetLabelText(header_title);
    }
    if (m_update_connection_badge != nullptr) {
        m_update_connection_badge->SetLabelText(obj->is_connected() ? "  Online  " : "  Offline  ");
        m_update_connection_badge->SetForegroundColour(obj->is_connected() ? wxColour("#35CE82") : wxColour("#AAB2BD"));
    }

    if (m_update_model_value != nullptr)
        m_update_model_value->SetLabelText(obj->get_printer_type_display_str());

    if (m_update_serial_value != nullptr) {
        wxString serial = from_u8(obj->get_dev_id());
        m_update_serial_value->SetLabelText(serial.MakeUpper());
    }

    wxString current_version = "-";
    wxString next_version;
    auto ota_it = obj->module_vers.find("ota");
    if (ota_it != obj->module_vers.end())
        current_version = ota_it->second.sw_ver;

    if (!obj->ota_new_version_number.empty())
        next_version = from_u8(obj->ota_new_version_number);
    else if (ota_it != obj->module_vers.end())
        next_version = ota_it->second.sw_new_ver;

    if (m_update_version_value != nullptr) {
        if (!next_version.empty() && next_version != current_version && current_version != "-")
            m_update_version_value->SetLabelText(wxString::Format("%s -> %s", current_version, next_version));
        else if (current_version != "-")
            m_update_version_value->SetLabelText(wxString::Format("%s (Latest)", current_version));
        else
            m_update_version_value->SetLabelText("-");
    }

    int progress = 0;
    wxString status = obj->is_connected() ? "Firmware is up to date" : "Printer offline";
    wxColour status_colour(35, 206, 130);

    if (!obj->is_connected()) {
        progress = 0;
        status_colour = wxColour(170, 176, 184);
    } else if (m_update_sim_active || obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress) {
        progress = m_update_sim_active ? m_update_sim_percent
                                       : std::max(0, std::min(100, obj->get_upgrade_percent()));
        status = "Updating";
    } else if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingFinished) {
        progress = std::max(0, std::min(100, obj->get_upgrade_percent()));
        if (obj->upgrade_status == "UPGRADE_FAIL" || obj->upgrade_err_code != UpgradeNoError) {
            status = "Update failed";
            status_colour = wxColour(231, 111, 81);
        } else {
            status = "Firmware is up to date";
            if (progress <= 0)
                progress = 100;
        }
    } else if (!next_version.empty() && next_version != current_version && current_version != "-") {
        progress = 100;
        status = "Update available";
    } else if (current_version != "-") {
        progress = 100;
        status = "Firmware is up to date";
    }

    if (m_update_status_value != nullptr) {
        m_update_status_value->SetLabelText(status);
        m_update_status_value->SetForegroundColour(status_colour);
    }

    // Progress bar only while an update is actively running (after Update Firmware).
    const bool show_progress =
        m_update_sim_active ||
        obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress;
    if (m_update_percent_value != nullptr) {
        m_update_percent_value->SetLabelText(wxString::Format("%d%%", progress));
        m_update_percent_value->SetForegroundColour(status_colour);
        m_update_percent_value->Show(show_progress);
    }
    if (m_update_progress_gauge != nullptr) {
        m_update_progress_gauge->SetValue(show_progress ? progress : 0);
        m_update_progress_gauge->Show(show_progress);
    }

    if (m_update_firmware_button != nullptr) {
        const bool can_start_update =
            obj->is_connected() &&
            !m_update_sim_active &&
            obj->upgrade_display_state != DevFirmwareUpgradingState::UpgradingInProgress;
        m_update_firmware_button->Enable(can_start_update);
    }

    if (m_update_printer_bitmap != nullptr) {
        try {
            const wxBitmap bmp = create_quadro_printer_thumbnail(this, 150);
            if (bmp.IsOk())
                m_update_printer_bitmap->SetBitmap(bmp);
        } catch (...) {
        }
    }

    m_update_page->Layout();
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());
    refresh_layer_info_from_selected_machine();

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_browser == nullptr || m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    SendAPIKey();
}

} // GUI
} // Slic3r
