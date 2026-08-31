#include "MultiTaskManagerPage.hpp"
#include "I18N.hpp"
#include "DeviceDashboard/PrinterOfflineOverlay.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "StartPrint/StartPrintDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/RadioBox.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include <wx/listimpl.cpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#ifdef __APPLE__
#include "../Utils/MacDarkMode.hpp"
#endif
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/mstream.h>
#include <wx/timer.h>
#include <wx/time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {
constexpr int CLOUD_HISTORY_ITEM_HEIGHT = 96;
constexpr size_t MOONRAKER_MODEL_FILE_LIMIT = 30;
constexpr int kModelCardWDip = 265;
constexpr int kModelCardHDip = 265;
constexpr int kModelGridGapDip = 18;
constexpr int kModelScrollBarWDip = 10;
constexpr int kModelScrollThumbWDip = 4;
const wxColour kModelScrollThumb(0x7A, 0x80, 0x88);

wxBitmap load_model_card_png(wxWindow* host, const char* filename, int dip)
{
    wxImage img;
    if (!img.LoadFile(wxString::FromUTF8(Slic3r::var(filename)), wxBITMAP_TYPE_PNG) || !img.IsOk())
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

// GDI on Windows ignores brush alpha, so wxColour(0,0,0,135) paints solid black.
void fill_rect_alpha(wxDC &dc, const wxRect &rect, const wxColour &colour, double radius = 0)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
#ifdef __WXMSW__
    auto draw = [&](wxDC &target) {
        target.SetPen(*wxTRANSPARENT_PEN);
        target.SetBrush(wxBrush(colour));
        if (radius > 0)
            target.DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
        else
            target.DrawRectangle(rect);
    };
    if (auto *mem = dynamic_cast<wxMemoryDC *>(&dc)) {
        wxGCDC gcdc(*mem);
        draw(gcdc);
        return;
    }
    if (auto *win = dynamic_cast<wxWindowDC *>(&dc)) {
        wxGCDC gcdc(*win);
        draw(gcdc);
        return;
    }
    const int a = colour.Alpha();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(colour.Red() * a / 255, colour.Green() * a / 255,
        colour.Blue() * a / 255)));
#else
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(colour));
#endif
    if (radius > 0)
        dc.DrawRoundedRectangle(rect, radius);
    else
        dc.DrawRectangle(rect);
}

class ModelGridOverlayScroll : public wxWindow
{
public:
    explicit ModelGridOverlayScroll(wxWindow* parent)
        : wxWindow()
        , m_idle_timer(this)
        , m_fade_timer(this)
    {
#ifdef __WXOSX__
        SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
#else
        SetBackgroundStyle(wxBG_STYLE_PAINT);
#endif
        Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        SetCanFocus(false);
        Hide();
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &ModelGridOverlayScroll::on_paint, this);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_LEFT_DOWN, &ModelGridOverlayScroll::on_down, this);
        Bind(wxEVT_LEFT_UP, &ModelGridOverlayScroll::on_up, this);
        Bind(wxEVT_MOTION, &ModelGridOverlayScroll::on_move, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
            m_hover = true;
            reveal();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            m_hover = false;
            if (!m_dragging)
                arm_idle();
        });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& evt) {
            if (m_scrolled != nullptr)
                m_scrolled->GetEventHandler()->ProcessEvent(evt);
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_dragging = false;
            arm_idle();
        });
        m_idle_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { start_fade(); });
        m_fade_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { step_fade(); });
    }

    void attach(wxScrolledWindow* scrolled) { m_scrolled = scrolled; }
    void set_on_scroll(std::function<void()> cb) { m_on_scroll = std::move(cb); }

    void sync(bool do_reveal)
    {
        if (m_scrolled == nullptr || !m_scrolled->IsShown()) {
            m_fade_timer.Stop();
            m_idle_timer.Stop();
            Hide();
            return;
        }
        pin_to_viewport();
        update_thumb();
        if (do_reveal)
            reveal();
        else if (m_need)
            Refresh();
        else
            Hide();
    }

    void reveal()
    {
        if (m_scrolled == nullptr || !m_scrolled->IsShown()) {
            Hide();
            return;
        }
        pin_to_viewport();
        update_thumb();
        if (!m_need) {
            m_fade_timer.Stop();
            m_idle_timer.Stop();
            m_alpha = 0;
            Hide();
            return;
        }
        m_alpha = 255;
        m_fade_timer.Stop();
        Show();
        Raise();
        Refresh();
        if (!m_hover && !m_dragging)
            arm_idle();
        else
            m_idle_timer.Stop();
    }

private:
    void pin_to_viewport()
    {
        if (m_scrolled == nullptr)
            return;
        const wxRect r = m_scrolled->GetRect();
        const int bar_w = FromDIP(kModelScrollBarWDip);
        SetSize(r.x + r.width - bar_w, r.y, bar_w, r.height);
        Raise();
    }

    void update_thumb()
    {
        m_need = false;
        m_thumb_h = 0;
        m_thumb_y = 0;
        if (m_scrolled == nullptr)
            return;
        int y = 0;
        int x = 0;
        m_scrolled->GetViewStart(&x, &y);
        int ux = 0;
        int uy = 0;
        m_scrolled->GetScrollPixelsPerUnit(&ux, &uy);
        const int content_h = m_scrolled->GetVirtualSize().GetHeight();
        const int view_h = m_scrolled->GetClientSize().GetHeight();
        const int track_h = GetClientSize().GetHeight();
        m_need = content_h > view_h && view_h > 0 && uy > 0 && track_h > 0;
        if (!m_need)
            return;
        m_thumb_h = std::max(FromDIP(24), track_h * view_h / content_h);
        const int max_scroll = std::max(1, content_h - view_h);
        m_thumb_y = std::max(0, (track_h - m_thumb_h) * (y * uy) / max_scroll);
    }

    void arm_idle()
    {
        m_idle_timer.StartOnce(1100);
    }

    void start_fade()
    {
        if (m_hover || m_dragging)
            return;
        m_fade_timer.Start(32);
    }

    void step_fade()
    {
        if (m_hover || m_dragging) {
            m_fade_timer.Stop();
            m_alpha = 255;
            Refresh();
            return;
        }
        m_alpha = std::max(0, m_alpha - 28);
        Refresh();
        if (m_alpha <= 0) {
            m_fade_timer.Stop();
            Hide();
        }
    }

    void on_paint(wxPaintEvent&)
    {
#ifdef __WXOSX__
        wxPaintDC raw_dc(this);
        wxGCDC dc(raw_dc);
#else
        wxAutoBufferedPaintDC raw_dc(this);
        wxGCDC dc(raw_dc);
        const wxColour bg = m_scrolled != nullptr ? m_scrolled->GetBackgroundColour() : GetBackgroundColour();
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
#endif
        if (m_thumb_h <= 0 || m_alpha <= 0)
            return;
        const int bar_w = FromDIP(kModelScrollThumbWDip);
        const int bar_x = (GetClientSize().GetWidth() - bar_w) / 2;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(kModelScrollThumb.Red(), kModelScrollThumb.Green(), kModelScrollThumb.Blue(), m_alpha)));
        dc.DrawRoundedRectangle(bar_x, m_thumb_y, bar_w, m_thumb_h, bar_w / 2.0);
    }

    void scroll_to_thumb_y(int thumb_y)
    {
        if (m_scrolled == nullptr || m_thumb_h <= 0)
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
        pin_to_viewport();
        update_thumb();
        Refresh();
        if (m_on_scroll)
            m_on_scroll();
    }

    void on_down(wxMouseEvent& evt)
    {
        if (m_thumb_h <= 0)
            return;
        reveal();
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
        arm_idle();
    }

    void on_move(wxMouseEvent& evt)
    {
        if (!m_dragging || !evt.Dragging())
            return;
        scroll_to_thumb_y(evt.GetY() - m_drag_off);
    }

    wxScrolledWindow* m_scrolled{nullptr};
    std::function<void()> m_on_scroll;
    wxTimer           m_idle_timer;
    wxTimer           m_fade_timer;
    int               m_thumb_y{0};
    int               m_thumb_h{0};
    int               m_drag_off{0};
    int               m_alpha{0};
    bool              m_dragging{false};
    bool              m_hover{false};
    bool              m_need{false};
};

std::atomic<unsigned> s_moonraker_model_probe_gen{ 0 };
std::atomic<bool> s_moonraker_model_probe_stop{ false };

bool moonraker_probe_should_stop()
{
    return s_moonraker_model_probe_stop.load(std::memory_order_acquire);
}

bool moonraker_probe_can_log()
{
    return !moonraker_probe_should_stop() && static_cast<bool>(boost::log::core::get());
}

std::string moonraker_base_url(MachineObject* obj)
{
    if (!obj)
        return {};

    std::string host = obj->get_dev_ip();
    if (host.empty())
        host = obj->get_dev_id();
    if (host.empty())
        return {};

    auto trim = [](std::string &value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
    };
    trim(host);

    std::string port = "7125";
    if (host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0) {
        std::string parsed_port;
        host = Http::get_host_from_url(host, &parsed_port);
        if (!parsed_port.empty())
            port = parsed_port;
    } else {
        const size_t slash_pos = host.find('/');
        if (slash_pos != std::string::npos)
            host = host.substr(0, slash_pos);

        // Preserve host:port when present (e.g. 192.168.1.150:7125).
        if (std::count(host.begin(), host.end(), ':') == 1) {
            const size_t port_pos = host.rfind(':');
            if (port_pos != std::string::npos && port_pos + 1 < host.size()) {
                port = host.substr(port_pos + 1);
                host = host.substr(0, port_pos);
            }
        }
    }

    trim(host);
    if (host.empty())
        return {};

    return "http://" + host + ":" + port;
}

std::string moonraker_api_key(MachineObject* obj)
{
    if (!obj)
        return {};
    std::string key = obj->get_access_code();
    if (key.empty())
        key = obj->get_user_access_code();
    return key;
}

struct MoonrakerModelProbeResult
{
    bool        ok{ false };
    unsigned    status_code{ 0 };
    size_t      file_count{ 0 };
    std::string error_message;
    std::vector<MoonrakerModelFileView> files;
};

struct MoonrakerModelDeleteResult
{
    bool        ok{ false };
    unsigned    status_code{ 0 };
    std::string error_message;
    std::string path;
};

wxString format_moonraker_file_size(std::uint64_t bytes)
{
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return wxString::Format("%.1f MB", mb);
}

wxString format_moonraker_modified_time(double modified)
{
    if (modified <= 0.0)
        return "-";

    wxDateTime date;
    date.Set(static_cast<time_t>(modified));
    if (!date.IsValid())
        return "-";

    return date.Format("%Y-%m-%d %H:%M");
}

wxString format_moonraker_duration(double seconds)
{
    if (seconds <= 0.0)
        return "-";

    const int total_minutes = std::max(1, static_cast<int>(std::round(seconds / 60.0)));
    const int hours = total_minutes / 60;
    const int minutes = total_minutes % 60;
    if (hours > 0)
        return wxString::Format("%dh%02dm", hours, minutes);
    return wxString::Format("%dm", minutes);
}

wxString format_moonraker_filament_weight(double grams)
{
    if (grams <= 0.0)
        return "-";
    wxString text = wxString::Format("%.2f", grams);
    while (text.length() > 1 && text.Contains('.') && (text.Last() == '0' || text.Last() == '.'))
        text.RemoveLast();
    return text + "g";
}

wxString basename_from_moonraker_path(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return wxString::FromUTF8(name.c_str());
}

wxString moonraker_model_card_name(const std::string& path)
{
    return wxString::Format("moonraker_model_card_%zu", std::hash<std::string>{}(path));
}

std::string moonraker_url_encode_path(std::string path)
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

std::string moonraker_thumbnail_path_from_metadata(const nlohmann::json& metadata)
{
    if (!metadata.is_object() || !metadata.contains("thumbnails") || !metadata["thumbnails"].is_array())
        return {};

    const nlohmann::json* best_thumbnail = nullptr;
    int best_score = -1;
    for (const auto& thumbnail : metadata["thumbnails"]) {
        if (!thumbnail.is_object())
            continue;

        bool has_path = false;
        for (const char* key : { "relative_path", "path", "filename" }) {
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
        return {};

    std::string relative_path;
    for (const char* key : { "relative_path", "path", "filename" }) {
        if (best_thumbnail->contains(key) && (*best_thumbnail)[key].is_string()) {
            relative_path = (*best_thumbnail)[key].get<std::string>();
            if (!relative_path.empty())
                break;
        }
    }

    for (char& ch : relative_path) {
        if (ch == '\\')
            ch = '/';
    }
    while (!relative_path.empty() && relative_path.front() == '/')
        relative_path.erase(relative_path.begin());
    if (relative_path.rfind("gcodes/", 0) == 0)
        relative_path.erase(0, 7);

    return relative_path;
}

wxImage load_moonraker_thumbnail_image(const std::string& url, const std::string& api_key = {})
{
    std::string body;
    unsigned status_code = 0;
    std::string error_message;

    auto http = Http::get(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(5)
        .timeout_max(12)
        .size_limit(1024 * 1024)
        .on_complete([&](std::string response, unsigned status) {
            status_code = status;
            if (status == 200)
                body = std::move(response);
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            status_code = status;
            error_message = std::move(error);
        })
        .perform_sync();

    if (body.empty()) {
        if (moonraker_probe_can_log())
            BOOST_LOG_TRIVIAL(info) << "Moonraker thumbnail download: url=" << url
                                    << " status=" << status_code
                                    << " error=" << error_message;
        return wxImage();
    }

    wxMemoryInputStream stream(body.data(), body.size());
    wxImage image(stream, wxBITMAP_TYPE_ANY);
    if (moonraker_probe_can_log())
        BOOST_LOG_TRIVIAL(info) << "Moonraker thumbnail download: url=" << url
                                << " status=" << status_code
                                << " ok=" << image.IsOk()
                                << " bytes=" << body.size();
    return image.IsOk() ? image : wxImage();
}

std::string trim_moonraker_token(std::string s)
{
    auto is_wrapper = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '[' || c == ']' || c == '\\';
    };
    while (!s.empty() && is_wrapper(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_wrapper(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

wxColour parse_moonraker_filament_colour(const std::string& hex)
{
    const std::string v = trim_moonraker_token(hex);
    wxColour colour(wxString::FromUTF8(v.c_str()));
    return colour.IsOk() ? colour : wxColour(120, 120, 120);
}

std::vector<std::string> split_moonraker_meta_list(const std::string& raw)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        const std::string token = trim_moonraker_token(cur);
        if (!token.empty())
            out.push_back(token);
        cur.clear();
    };
    for (char ch : raw) {
        if (ch == ',' || ch == ';' || ch == '\n')
            flush();
        else
            cur.push_back(ch);
    }
    flush();
    return out;
}

void collect_moonraker_string_list(const nlohmann::json& node, std::vector<std::string>& out)
{
    auto push_token = [&](std::string token) {
        token = trim_moonraker_token(std::move(token));
        if (!token.empty())
            out.push_back(std::move(token));
    };
    if (node.is_array()) {
        for (const auto& item : node) {
            if (item.is_string())
                push_token(item.get<std::string>());
        }
        return;
    }
    if (!node.is_string())
        return;
    const std::string raw = node.get<std::string>();
    auto parsed = nlohmann::json::parse(raw, nullptr, false, true);
    if (!parsed.is_discarded() && (parsed.is_array() || parsed.is_string())) {
        collect_moonraker_string_list(parsed, out);
        return;
    }
    for (const auto& part : split_moonraker_meta_list(raw))
        push_token(part);
}

void collect_moonraker_number_list(const nlohmann::json& node, std::vector<double>& out)
{
    auto push_number = [&](double value) { out.push_back(value); };
    if (node.is_number()) {
        push_number(node.get<double>());
        return;
    }
    if (node.is_array()) {
        for (const auto& item : node) {
            if (item.is_number())
                push_number(item.get<double>());
            else if (item.is_string()) {
                try {
                    push_number(std::stod(item.get<std::string>()));
                } catch (...) {
                }
            }
        }
        return;
    }
    if (!node.is_string())
        return;
    auto parsed = nlohmann::json::parse(node.get<std::string>(), nullptr, false, true);
    if (!parsed.is_discarded() && (parsed.is_array() || parsed.is_number() || parsed.is_string())) {
        collect_moonraker_number_list(parsed, out);
        return;
    }
    for (const auto& part : split_moonraker_meta_list(node.get<std::string>())) {
        try {
            push_number(std::stod(part));
        } catch (...) {
        }
    }
}

nlohmann::json fetch_moonraker_file_metadata_json(const std::string& base_url,
                                                  const std::string& api_key,
                                                  const std::string& path)
{
    nlohmann::json parsed;
    if (base_url.empty() || path.empty())
        return parsed;

    const std::string metadata_url = base_url + "/server/files/metadata?filename=" + moonraker_url_encode_path(path);
    std::string body;
    auto http = Http::get(metadata_url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(8)
        .timeout_max(20)
        .on_complete([&](std::string response, unsigned status) {
            if (status == 200)
                body = std::move(response);
        })
        .on_error([&](std::string, std::string, unsigned) {})
        .perform_sync();

    if (body.empty())
        return parsed;

    parsed = nlohmann::json::parse(body, nullptr, false, true);
    if (parsed.is_discarded())
        return {};
    if (parsed.contains("result"))
        parsed = parsed["result"];
    return parsed.is_object() ? parsed : nlohmann::json{};
}

void apply_moonraker_metadata_to_request(const nlohmann::json& parsed,
                                         const std::string& base_url,
                                         const std::string& api_key,
                                         PrinterStoragePrintRequest& request)
{
    if (!parsed.is_object())
        return;

    if (parsed.contains("estimated_time") && parsed["estimated_time"].is_number())
        request.time_text = format_moonraker_duration(parsed["estimated_time"].get<double>());
    else if (parsed.contains("print_time") && parsed["print_time"].is_number())
        request.time_text = format_moonraker_duration(parsed["print_time"].get<double>());

    std::vector<std::string> types;
    std::vector<std::string> colors;
    std::vector<double> used_g;
    std::vector<double> used_mm;

    if (parsed.contains("filament_weight_total")) {
        if (parsed["filament_weight_total"].is_number()) {
            request.weight_text = format_moonraker_filament_weight(parsed["filament_weight_total"].get<double>());
        } else {
            std::vector<double> total_parts;
            collect_moonraker_number_list(parsed["filament_weight_total"], total_parts);
            if (total_parts.size() > 1)
                used_g = total_parts;
            double total = 0.0;
            for (double w : total_parts)
                total += w;
            if (total > 0.0)
                request.weight_text = format_moonraker_filament_weight(total);
        }
    }

    if (parsed.contains("filament_type"))
        collect_moonraker_string_list(parsed["filament_type"], types);
    if (types.empty() && parsed.contains("filament_types"))
        collect_moonraker_string_list(parsed["filament_types"], types);
    if (parsed.contains("filament_colors"))
        collect_moonraker_string_list(parsed["filament_colors"], colors);
    else if (parsed.contains("filament_colour"))
        collect_moonraker_string_list(parsed["filament_colour"], colors);

    for (const char* key : { "filament_used_g", "filament_used_weight", "filament_weights" }) {
        if (!parsed.contains(key) || !used_g.empty())
            continue;
        collect_moonraker_number_list(parsed[key], used_g);
    }
    // filament_total / filament_weight_total are Moonraker sums, not per-slot lists.
    for (const char* key : { "filament_used_mm", "filament_used_m" }) {
        if (!parsed.contains(key) || !used_mm.empty())
            continue;
        collect_moonraker_number_list(parsed[key], used_mm);
        if (used_mm.size() == 1)
            used_mm.clear();
    }

    std::vector<int> referenced_raw;
    if (parsed.contains("referenced_tools")) {
        const auto& tools_node = parsed["referenced_tools"];
        auto push_raw = [&](int value) { referenced_raw.push_back(value); };
        if (tools_node.is_array()) {
            for (const auto& item : tools_node) {
                if (item.is_number_integer())
                    push_raw(item.get<int>());
            }
        } else {
            std::vector<double> tool_nums;
            collect_moonraker_number_list(tools_node, tool_nums);
            for (double value : tool_nums)
                push_raw(static_cast<int>(std::lround(value)));
        }
    }
    std::vector<int> referenced_tools;
    bool zero_based_tools = false;
    for (int value : referenced_raw) {
        if (value == 0)
            zero_based_tools = true;
    }
    for (int value : referenced_raw) {
        const int idx = zero_based_tools ? value : value - 1;
        if (idx >= 0 && idx < 4)
            referenced_tools.push_back(idx);
    }

    const size_t n = std::min<size_t>(4, std::max({ types.size(), colors.size(), used_g.size(), used_mm.size() }));
    std::vector<char> keep(n, 0);
    bool filtered = false;
    if (!used_g.empty()) {
        // A single value means only extruder 0 was used (CoPrint omits trailing zeros).
        filtered = true;
        for (size_t i = 0; i < n; ++i)
            keep[i] = i < used_g.size() && used_g[i] > 0.01;
    } else if (!referenced_tools.empty()) {
        filtered = true;
        for (int idx : referenced_tools) {
            if (idx >= 0 && static_cast<size_t>(idx) < n)
                keep[static_cast<size_t>(idx)] = 1;
        }
    } else if (used_mm.size() > 1) {
        filtered = true;
        for (size_t i = 0; i < n; ++i)
            keep[i] = i < used_mm.size() && used_mm[i] > 1.0;
    }
    if (!filtered) {
        if (n <= 1) {
            if (n == 1)
                keep[0] = 1;
        } else {
            bool same_color = colors.size() <= 1;
            if (!same_color) {
                same_color = true;
                for (size_t i = 1; i < colors.size(); ++i) {
                    if (parse_moonraker_filament_colour(colors[i]) != parse_moonraker_filament_colour(colors[0])) {
                        same_color = false;
                        break;
                    }
                }
            }
            if (same_color)
                keep[0] = 1;
            else {
                for (size_t i = 0; i < n; ++i)
                    keep[i] = 1;
            }
        }
    }

    bool any_kept = false;
    for (char flag : keep) {
        if (flag)
            any_kept = true;
    }
    if (!any_kept && n > 0)
        keep[0] = 1;

    request.filaments.clear();
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i])
            continue;
        PrinterStorageFilament slot;
        slot.type = i < types.size() && !types[i].empty() ? types[i] : "PLA";
        slot.color = i < colors.size() ? parse_moonraker_filament_colour(colors[i]) : wxColour(120, 120, 120);
        request.filaments.push_back(std::move(slot));
    }

    const std::string thumbnail_path = moonraker_thumbnail_path_from_metadata(parsed);
    if (!thumbnail_path.empty()) {
        const std::string thumbnail_url = base_url + "/server/files/gcodes/" + moonraker_url_encode_path(thumbnail_path);
        wxImage thumb = load_moonraker_thumbnail_image(thumbnail_url, api_key);
        if (thumb.IsOk())
            request.thumbnail = std::move(thumb);
    }
}

void probe_moonraker_model_metadata(const std::string& base_url,
                                    const std::string& api_key,
                                    MoonrakerPrinterAgent* moonraker_agent,
                                    std::vector<MoonrakerModelFileView>& files,
                                    const std::function<void(MoonrakerModelFileView)>& on_file_meta)
{
    constexpr size_t max_metadata_probe_count = MOONRAKER_MODEL_FILE_LIMIT;
    const size_t probe_count = std::min(max_metadata_probe_count, files.size());
    size_t thumbnail_count = 0;

    for (size_t i = 0; i < probe_count; ++i) {
        if (moonraker_probe_should_stop())
            return;

        auto& file = files[i];
        std::string body;
        unsigned status_code = 0;
        std::string error_message;
        nlohmann::json parsed;

        bool got_metadata = false;
        if (moonraker_agent != nullptr && moonraker_agent->is_websocket_connected()) {
            nlohmann::json metadata;
            std::string ws_error;
            if (moonraker_agent->fetch_gcode_metadata(file.path, metadata, ws_error, 12000)) {
                parsed = std::move(metadata);
                got_metadata = true;
                status_code = 200;
            } else {
                error_message = ws_error;
            }
        }

        if (!got_metadata) {
            const std::string metadata_url = base_url + "/server/files/metadata?filename=" + moonraker_url_encode_path(file.path);
            auto http = Http::get(metadata_url);
            if (!api_key.empty())
                http.header("X-Api-Key", api_key);
            http.timeout_connect(8)
                .timeout_max(15)
                .on_complete([&](std::string response, unsigned status) {
                    status_code = status;
                    if (status == 200)
                        body = std::move(response);
                })
                .on_error([&](std::string, std::string error, unsigned status) {
                    status_code = status;
                    error_message = std::move(error);
                })
                .perform_sync();

            if (!body.empty()) {
                parsed = nlohmann::json::parse(body, nullptr, false, true);
                if (!parsed.is_discarded()) {
                    if (parsed.contains("result"))
                        parsed = parsed["result"];
                    got_metadata = parsed.is_object();
                }
            }
        }

        std::string thumbnail_path;
        if (got_metadata && parsed.is_object()) {
            thumbnail_path = moonraker_thumbnail_path_from_metadata(parsed);
            if (parsed.contains("estimated_time") && parsed["estimated_time"].is_number())
                file.estimated_time_seconds = parsed["estimated_time"].get<double>();
            else if (parsed.contains("print_time") && parsed["print_time"].is_number())
                file.estimated_time_seconds = parsed["print_time"].get<double>();
            if (parsed.contains("filament_weight_total") && parsed["filament_weight_total"].is_number())
                file.filament_weight_grams = parsed["filament_weight_total"].get<double>();
        }
        if (!thumbnail_path.empty())
            ++thumbnail_count;
        if (!thumbnail_path.empty()) {
            file.thumbnail_url = base_url + "/server/files/gcodes/" + moonraker_url_encode_path(thumbnail_path);
        }
        if (on_file_meta)
            on_file_meta(file);

        if (moonraker_probe_can_log())
            BOOST_LOG_TRIVIAL(info) << "Moonraker model metadata probe: file=" << file.path
                                    << " status=" << status_code
                                    << " thumbnail=" << file.thumbnail_url
                                    << " estimated_time=" << file.estimated_time_seconds
                                    << " filament_weight=" << file.filament_weight_grams
                                    << " error=" << error_message;
    }

    if (moonraker_probe_can_log())
        BOOST_LOG_TRIVIAL(info) << "Moonraker model metadata probe summary: checked=" << probe_count
                                << " thumbnails=" << thumbnail_count;
}

MoonrakerModelDeleteResult delete_moonraker_model_file_sync(const std::string& base_url,
                                                            const std::string& api_key,
                                                            const std::string& path)
{
    MoonrakerModelDeleteResult result;
    result.path = path;

    if (base_url.empty() || path.empty()) {
        result.error_message = "Missing printer URL or file path";
        return result;
    }

    const std::string url = base_url + "/server/files/gcodes/" + moonraker_url_encode_path(path);
    std::string body;

    auto http = Http::del(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(5)
        .timeout_max(15)
        .on_complete([&](std::string response, unsigned status) {
            body = std::move(response);
            result.status_code = status;
            result.ok = status >= 200 && status < 300;
        })
        .on_error([&](std::string response, std::string error, unsigned status) {
            body = std::move(response);
            result.error_message = std::move(error);
            result.status_code = status;
            result.ok = false;
        })
        .perform_sync();

    BOOST_LOG_TRIVIAL(info) << "Moonraker model delete: file=" << path
                            << " status=" << result.status_code
                            << " ok=" << result.ok
                            << " error=" << result.error_message
                            << " body=" << body;

    return result;
}

bool confirm_moonraker_model_action(wxWindow* parent,
                                    const wxString& display_name,
                                    const wxString& title,
                                    const wxString& body_text,
                                    const wxString& confirm_label)
{
    const wxColour dialog_bg(*wxWHITE);
    const wxColour card_bg(*wxWHITE);
    const wxColour border("#C7C7C7");
    const wxColour accent("#00A886");
    const wxColour muted_text("#767C84");
    const wxColour main_text("#232527");

    wxDialog dlg(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFRAME_SHAPED);
    dlg.SetBackgroundColour(dialog_bg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* card = new wxPanel(&dlg, wxID_ANY);
    card->SetBackgroundStyle(wxBG_STYLE_PAINT);
    card->SetMinSize(wxSize(dlg.FromDIP(360), dlg.FromDIP(220)));

    auto button_rects = [card]() {
        const wxSize size = card->GetClientSize();
        const int button_w = card->FromDIP(104);
        const int button_h = card->FromDIP(34);
        const int gap = card->FromDIP(12);
        const int right = card->FromDIP(24);
        const int bottom = card->FromDIP(24);
        const int y = size.y - bottom - button_h;
        const int delete_x = size.x - right - button_w;
        const int cancel_x = delete_x - gap - button_w;
        return std::make_pair(wxRect(cancel_x, y, button_w, button_h), wxRect(delete_x, y, button_w, button_h));
    };

    auto draw_text_ellipsis = [](wxDC& dc, wxString text, int x, int y, int max_width) {
        wxCoord text_w = 0;
        wxCoord text_h = 0;
        dc.GetTextExtent(text, &text_w, &text_h);
        if (text_w <= max_width) {
            dc.DrawText(text, x, y);
            return;
        }

        wxString trimmed = text;
        const wxString suffix("...");
        while (!trimmed.empty()) {
            trimmed.RemoveLast();
            wxString candidate = trimmed + suffix;
            dc.GetTextExtent(candidate, &text_w, &text_h);
            if (text_w <= max_width) {
                dc.DrawText(candidate, x, y);
                return;
            }
        }
        dc.DrawText(suffix, x, y);
    };

    card->Bind(wxEVT_PAINT, [card, dialog_bg, card_bg, border, accent, muted_text, main_text, display_name, title, body_text, confirm_label, button_rects, draw_text_ellipsis](wxPaintEvent&) {
        wxAutoBufferedPaintDC raw_dc(card);
        wxGCDC dc(raw_dc);
        const wxSize size = card->GetClientSize();
        const int pad = card->FromDIP(24);
        dc.SetBackground(wxBrush(dialog_bg));
        dc.Clear();
        dc.SetPen(wxPen(border, card->FromDIP(1)));
        dc.SetBrush(wxBrush(card_bg));
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, card->FromDIP(12));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(accent));
        dc.DrawRoundedRectangle(card->FromDIP(18), card->FromDIP(18), card->FromDIP(42), card->FromDIP(4), card->FromDIP(2));

        wxFont title_font = card->GetFont();
        title_font.SetPointSize(title_font.GetPointSize() + 3);
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(title_font);
        dc.SetTextForeground(main_text);
        dc.DrawText(title, pad, card->FromDIP(42));

        wxFont body_font = card->GetFont();
        body_font.SetPointSize(body_font.GetPointSize() + 1);
        dc.SetFont(body_font);
        dc.SetTextForeground(muted_text);
        draw_text_ellipsis(dc, body_text, pad, card->FromDIP(86), size.x - pad * 2);

        wxFont filename_font = body_font;
        filename_font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(filename_font);
        dc.SetTextForeground(main_text);
        draw_text_ellipsis(dc, display_name, pad, card->FromDIP(124), size.x - pad * 2);

        const auto rects = button_rects();
        const wxRect cancel_rect = rects.first;
        const wxRect delete_rect = rects.second;

        auto draw_button = [&dc, card](const wxRect& rect, const wxString& label, const wxColour& bg, const wxColour& border_colour, const wxColour& text_colour) {
            dc.SetPen(wxPen(border_colour, card->FromDIP(1)));
            dc.SetBrush(wxBrush(bg));
            dc.DrawRoundedRectangle(rect.x, rect.y, rect.width - 1, rect.height - 1, card->FromDIP(12));

            dc.SetFont(Label::Body_14);
            dc.SetTextForeground(text_colour);
            wxCoord text_w = 0;
            wxCoord text_h = 0;
            dc.GetTextExtent(label, &text_w, &text_h);
            dc.DrawText(label, rect.x + (rect.width - text_w) / 2, rect.y + (rect.height - text_h) / 2);
        };

        draw_button(cancel_rect, _L("Cancel"), *wxWHITE, wxColour("#C7C7C7"), main_text);
        draw_button(delete_rect, confirm_label, accent, accent, wxColour("#FFFFFF"));
    });

    card->Bind(wxEVT_MOTION, [card, button_rects](wxMouseEvent& evt) {
        const auto rects = button_rects();
        if (rects.first.Contains(evt.GetPosition()) || rects.second.Contains(evt.GetPosition()))
            card->SetCursor(wxCursor(wxCURSOR_HAND));
        else
            card->SetCursor(wxCursor(wxCURSOR_ARROW));
    });

    card->Bind(wxEVT_LEFT_UP, [&dlg, button_rects](wxMouseEvent& evt) {
        const auto rects = button_rects();
        if (rects.first.Contains(evt.GetPosition())) {
            dlg.EndModal(wxID_CANCEL);
            return;
        }
        if (rects.second.Contains(evt.GetPosition())) {
            dlg.EndModal(wxID_OK);
            return;
        }
    });

    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& evt) {
        if (evt.GetKeyCode() == WXK_ESCAPE) {
            dlg.EndModal(wxID_CANCEL);
            return;
        }
        if (evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_NUMPAD_ENTER) {
            dlg.EndModal(wxID_OK);
            return;
        }
        evt.Skip();
    });

    root->Add(card, 0, wxEXPAND);
    dlg.SetSizerAndFit(root);
    {
        const wxSize size = dlg.GetSize();
        wxBitmap shape_bmp(size.GetWidth(), size.GetHeight());
        wxMemoryDC dc(shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), dlg.FromDIP(12));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            dlg.SetShape(region);
    }
    dlg.CentreOnParent();

    return dlg.ShowModal() == wxID_OK;
}

bool confirm_delete_moonraker_model(wxWindow* parent, const wxString& display_name)
{
    return confirm_moonraker_model_action(parent,
                                         display_name,
                                         _L("Delete model?"),
                                         _L("This model will be removed from the printer storage."),
                                         _L("Delete"));
}

class MoonrakerModelFileCard : public wxPanel
{
public:
    MoonrakerModelFileCard(wxWindow* parent,
                           const MoonrakerModelFileView& file,
                           std::function<void(MoonrakerModelFileCard*, const MoonrakerModelFileView&)> on_delete_click = {},
                           std::function<void(MoonrakerModelFileCard*, const MoonrakerModelFileView&)> on_print_click = {})
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_file(file)
        , m_on_delete_click(std::move(on_delete_click))
        , m_on_print_click(std::move(on_print_click))
    {
        SetName(moonraker_model_card_name(m_file.path));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_timer_icon = load_model_card_png(this, "cp_file_timer.png", 15);
        m_filament_icon = load_model_card_png(this, "cp_file_filament.png", 15);
        const wxSize card_size(FromDIP(kModelCardWDip), FromDIP(kModelCardHDip));
        SetMinSize(card_size);
        SetInitialSize(card_size);
        Bind(wxEVT_PAINT, &MoonrakerModelFileCard::on_paint, this);
        Bind(wxEVT_WEBREQUEST_STATE, &MoonrakerModelFileCard::on_thumbnail_request, this);
        Bind(wxEVT_LEFT_UP, &MoonrakerModelFileCard::on_left_up, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& evt) {
            m_hover = true;
            SetCursor(wxCURSOR_HAND);
            Refresh();
            evt.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            m_hover = false;
            SetCursor(wxCURSOR_ARROW);
            Refresh();
            evt.Skip();
        });
    }

    void apply_metadata(const MoonrakerModelFileView& file)
    {
        m_file.estimated_time_seconds = file.estimated_time_seconds;
        m_file.filament_weight_grams = file.filament_weight_grams;
        m_file.thumbnail_url = file.thumbnail_url;
        Refresh();
    }

    void apply_thumbnail(const wxImage& image)
    {
        if (!image.IsOk())
            return;
        m_thumbnail_image = image.Copy();
        Refresh();
    }

    void request_thumbnail()
    {
        if (m_retiring || m_thumbnail_image.IsOk() || m_thumbnail_request.IsOk() || m_file.thumbnail_url.empty())
            return;
        m_thumbnail_request = wxWebSession::GetDefault().CreateRequest(this, wxString::FromUTF8(m_file.thumbnail_url));
        if (m_thumbnail_request.IsOk())
            m_thumbnail_request.Start();
    }

    bool has_thumbnail() const { return m_thumbnail_image.IsOk(); }

    bool is_retiring() const { return m_retiring; }

    // Keep this window alive until NSURLSession finishes. Destroying a wxWebRequest
    // handler on macOS while State_Active crashes in SetState on a background queue.
    void retire()
    {
        if (m_retiring)
            return;
        m_retiring = true;
        Hide();
        Disable();
        if (m_thumbnail_request.IsOk() && m_thumbnail_request.GetState() == wxWebRequest::State_Active) {
            m_thumbnail_request.Cancel();
            return;
        }
        Destroy();
    }

    std::string file_path() const { return m_file.path; }

    void set_on_thumbnail_loaded(std::function<void(const std::string&, const wxImage&)> cb)
    {
        m_on_thumbnail_loaded = std::move(cb);
    }

    void set_card_size(const wxSize& card_size)
    {
        SetMinSize(card_size);
        SetInitialSize(card_size);
    }

    wxImage thumbnail() const { return m_thumbnail_image; }

    ~MoonrakerModelFileCard() override
    {
        SetEvtHandlerEnabled(false);
        if (m_thumbnail_request.IsOk())
            m_thumbnail_request.Cancel();
    }

private:
    wxRect action_rect() const
    {
        const wxSize size = GetSize();
        const int details_h = FromDIP(76);
        const int preview_h = std::max(FromDIP(120), size.y - details_h);
        const int action_h = FromDIP(38);
        return wxRect(FromDIP(2), preview_h - action_h + FromDIP(1), size.x - FromDIP(4), action_h);
    }

    wxRect delete_action_rect() const
    {
        wxRect rect = action_rect();
        rect.width /= 2;
        return rect;
    }

    wxRect print_action_rect() const
    {
        wxRect rect = action_rect();
        const int left_width = rect.width / 2;
        rect.x += left_width;
        rect.width -= left_width;
        return rect;
    }

    void draw_ellipsis(wxDC& dc, wxString text, int x, int y, int max_width)
    {
        wxCoord width = 0;
        wxCoord height = 0;
        dc.GetTextExtent(text, &width, &height);
        if (width <= max_width) {
            dc.DrawText(text, x, y);
            return;
        }

        const wxString ellipsis = "...";
        wxCoord ellipsis_width = 0;
        dc.GetTextExtent(ellipsis, &ellipsis_width, nullptr);
        while (!text.empty()) {
            text.RemoveLast();
            dc.GetTextExtent(text, &width, nullptr);
            if (width + ellipsis_width <= max_width)
                break;
        }
        dc.DrawText(text + ellipsis, x, y);
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetSize();
        const int radius = FromDIP(8);
        const int pad = FromDIP(12);
        const int details_h = FromDIP(76);
        const int preview_h = std::max(FromDIP(120), size.y - details_h);
        const wxColour parent_bg("#EEEEEF");
        const wxColour border_colour = m_hover ? wxColour("#00B894") : wxColour("#C7C7C7");
        const wxColour card_bg(*wxWHITE);
        const wxColour preview_bg(*wxWHITE);
        const wxColour details_bg(*wxWHITE);
        const wxColour title_colour("#232527");
        const wxColour meta_colour("#767C84");

        dc.SetBackground(wxBrush(parent_bg));
        dc.Clear();

        dc.SetPen(wxPen(border_colour, FromDIP(1)));
        dc.SetBrush(wxBrush(card_bg));
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, radius);

        wxRect preview(FromDIP(2), FromDIP(2), size.x - FromDIP(4), preview_h - FromDIP(1));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(preview_bg));
        dc.DrawRoundedRectangle(preview.x, preview.y, preview.width, preview.height + radius, radius - FromDIP(1));
        dc.DrawRectangle(preview.x, preview.y + preview.height - radius, preview.width, radius);

        wxRect details(FromDIP(1), preview_h, size.x - FromDIP(2), size.y - preview_h - FromDIP(1));
        dc.SetBrush(wxBrush(details_bg));
        dc.DrawRoundedRectangle(details.x, details.y - FromDIP(1), details.width, details.height + FromDIP(1), radius - FromDIP(1));
        dc.DrawRectangle(details.x, details.y - FromDIP(1), details.width, radius);

        if (m_thumbnail_image.IsOk()) {
            wxImage thumb = m_thumbnail_image.Copy();
            wxRect image_area = preview;
            image_area.Deflate(FromDIP(14), FromDIP(14));
            const double scale =
#ifdef __APPLE__
                std::max(1.0, mac_max_scaling_factor());
#else
                std::max(1.0, GetContentScaleFactor());
#endif
            const double fit = std::min(
                static_cast<double>(image_area.width) / std::max(1, thumb.GetWidth()),
                static_cast<double>(image_area.height) / std::max(1, thumb.GetHeight()));
            const int thumb_w = std::max(1, static_cast<int>(std::lround(thumb.GetWidth() * fit)));
            const int thumb_h = std::max(1, static_cast<int>(std::lround(thumb.GetHeight() * fit)));
            thumb.Rescale(std::max(1, static_cast<int>(std::lround(thumb_w * scale))),
                          std::max(1, static_cast<int>(std::lround(thumb_h * scale))),
                          wxIMAGE_QUALITY_HIGH);
#ifdef __APPLE__
            dc.DrawBitmap(wxBitmap(std::move(thumb), -1, scale),
                          image_area.x + (image_area.width - thumb_w) / 2,
                          image_area.y + (image_area.height - thumb_h) / 2, true);
#else
            dc.DrawBitmap(wxBitmap(thumb),
                          image_area.x + (image_area.width - thumb_w) / 2,
                          image_area.y + (image_area.height - thumb_h) / 2, true);
#endif
        } else {
            dc.SetFont(Label::Head_24);
            dc.SetTextForeground(wxColour("#AEB7C2"));
            wxCoord glyph_w = 0;
            wxCoord glyph_h = 0;
            dc.GetTextExtent("G", &glyph_w, &glyph_h);
            dc.DrawText("G", preview.x + (preview.width - glyph_w) / 2, preview.y + (preview.height - glyph_h) / 2);
        }

        if (m_hover) {
            wxRect actions = action_rect();
            fill_rect_alpha(dc, actions, wxColour(0, 0, 0, 135));

            const int divider_x = actions.x + actions.width / 2;
            dc.SetPen(wxPen(wxColour("#59616B"), FromDIP(1)));
            dc.DrawLine(divider_x, actions.y, divider_x, actions.y + actions.height);

            dc.SetFont(Label::Body_14);
            dc.SetTextForeground(*wxWHITE);
            const wxString delete_text = _L("Delete");
            const wxString print_text = _L("Print");
            wxCoord delete_w = 0;
            wxCoord delete_h = 0;
            wxCoord print_w = 0;
            wxCoord print_h = 0;
            dc.GetTextExtent(delete_text, &delete_w, &delete_h);
            dc.GetTextExtent(print_text, &print_w, &print_h);
            dc.DrawText(delete_text,
                        actions.x + actions.width / 4 - delete_w / 2,
                        actions.y + (actions.height - delete_h) / 2);
            dc.DrawText(print_text,
                        actions.x + actions.width * 3 / 4 - print_w / 2,
                        actions.y + (actions.height - print_h) / 2);
        }

        dc.SetFont(Label::Head_13);
        dc.SetTextForeground(title_colour);
        draw_ellipsis(dc, basename_from_moonraker_path(m_file.path), pad, details.y + FromDIP(13), size.x - pad * 2);

        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(meta_colour);
        const wxString duration_text = format_moonraker_duration(m_file.estimated_time_seconds);
        const wxString weight_text = format_moonraker_filament_weight(m_file.filament_weight_grams);
        const int meta_y = size.y - FromDIP(28);
        const int icon_size = FromDIP(15);
        const int icon_gap = FromDIP(5);
        wxCoord duration_h = 0;
        dc.GetTextExtent(duration_text, nullptr, &duration_h);
        const int icon_y = meta_y + (duration_h - icon_size) / 2;
        if (m_timer_icon.IsOk())
            dc.DrawBitmap(m_timer_icon, pad, icon_y, true);
        dc.DrawText(duration_text, pad + icon_size + icon_gap, meta_y);

        wxCoord weight_w = 0;
        dc.GetTextExtent(weight_text, &weight_w, nullptr);
        const int filament_x = size.x - pad - weight_w - icon_size - icon_gap;
        if (m_filament_icon.IsOk())
            dc.DrawBitmap(m_filament_icon, filament_x, icon_y, true);
        dc.DrawText(weight_text, filament_x + icon_size + icon_gap, meta_y);

        dc.SetPen(wxPen(border_colour, FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, radius);
    }

    void on_thumbnail_request(wxWebRequestEvent& evt)
    {
        if (m_retiring) {
            if (evt.GetState() != wxWebRequest::State_Active && evt.GetState() != wxWebRequest::State_Idle)
                CallAfter([this] { Destroy(); });
            return;
        }
        if (evt.GetState() == wxWebRequest::State_Completed && evt.GetResponse().GetStream() != nullptr) {
            wxImage image;
            if (image.LoadFile(*evt.GetResponse().GetStream(), wxBITMAP_TYPE_ANY)) {
                m_thumbnail_image = image;
                if (m_on_thumbnail_loaded)
                    m_on_thumbnail_loaded(m_file.path, m_thumbnail_image);
                Refresh();
            }
        }
    }

    void on_left_up(wxMouseEvent& evt)
    {
        if (m_hover) {
            if (delete_action_rect().Contains(evt.GetPosition()) && m_on_delete_click) {
                m_on_delete_click(this, m_file);
                return;
            }
            if (print_action_rect().Contains(evt.GetPosition()) && m_on_print_click) {
                m_on_print_click(this, m_file);
                return;
            }
        }

        evt.Skip();
    }

    bool                   m_hover{ false };
    bool                   m_retiring{ false };
    MoonrakerModelFileView m_file;
    wxBitmap               m_timer_icon;
    wxBitmap               m_filament_icon;
    wxImage                m_thumbnail_image;
    wxWebRequest           m_thumbnail_request;
    std::function<void(MoonrakerModelFileCard*, const MoonrakerModelFileView&)> m_on_delete_click;
    std::function<void(MoonrakerModelFileCard*, const MoonrakerModelFileView&)> m_on_print_click;
    std::function<void(const std::string&, const wxImage&)> m_on_thumbnail_loaded;
};

void probe_moonraker_model_files(std::function<void(MoonrakerModelProbeResult)> on_result = {},
                                 std::function<void(MoonrakerModelFileView)> on_file_meta = {})
{
    Slic3r::DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
    const std::string machine_id = obj ? obj->get_dev_id() : std::string();
    const std::string base_url = moonraker_base_url(obj);
    const std::string api_key = moonraker_api_key(obj);
    if (base_url.empty()) {
        if (moonraker_probe_can_log())
            BOOST_LOG_TRIVIAL(warning) << "Moonraker model probe: no selected printer/base URL";
        MoonrakerModelProbeResult empty;
        empty.error_message = "No printer selected or printer IP is missing";
        if (on_result)
            on_result(std::move(empty));
        return;
    }

    auto* network_agent = wxGetApp().getAgent();
    std::shared_ptr<IPrinterAgent> printer_agent = network_agent ? network_agent->get_printer_agent() : nullptr;

    // Bump generation so only the newest probe updates the UI when Print Models
    // is opened/refreshed while a previous request is still in flight.
    const unsigned gen = ++s_moonraker_model_probe_gen;
    std::thread([machine_id, base_url, api_key, printer_agent, gen, on_result = std::move(on_result),
                 on_file_meta = std::move(on_file_meta)]() {
        if (moonraker_probe_should_stop())
            return;

        auto* moonraker_agent = dynamic_cast<MoonrakerPrinterAgent*>(printer_agent.get());
        const std::string url = base_url + "/server/files/list?root=gcodes";
        MoonrakerModelProbeResult result;

        auto parse_files_array = [&](const nlohmann::json& arr) {
            result.ok = true;
            result.status_code = 200;
            result.file_count = arr.size();
            result.files.reserve(result.file_count);
            for (const auto& item : arr) {
                if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                    continue;

                MoonrakerModelFileView file;
                file.path = item["path"].get<std::string>();
                if (item.contains("size") && item["size"].is_number_unsigned())
                    file.size = item["size"].get<std::uint64_t>();
                else if (item.contains("size") && item["size"].is_number_integer())
                    file.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, item["size"].get<std::int64_t>()));
                if (item.contains("modified") && item["modified"].is_number())
                    file.modified = item["modified"].get<double>();
                result.files.push_back(std::move(file));
            }
            std::sort(result.files.begin(), result.files.end(), [](const auto& a, const auto& b) {
                return a.modified > b.modified;
            });
            if (result.files.size() > MOONRAKER_MODEL_FILE_LIMIT)
                result.files.resize(MOONRAKER_MODEL_FILE_LIMIT);
            result.file_count = result.files.size();
        };

        auto run_via_websocket = [&]() -> bool {
            if (moonraker_agent == nullptr || !moonraker_agent->is_websocket_connected())
                return false;
            nlohmann::json files_json;
            std::string ws_error;
            if (!moonraker_agent->list_gcode_files(files_json, ws_error, 20000)) {
                result.error_message = ws_error.empty() ? "WebSocket files.list failed" : ws_error;
                if (moonraker_probe_can_log())
                    BOOST_LOG_TRIVIAL(warning) << "Moonraker model probe via websocket failed: " << result.error_message;
                return false;
            }
            parse_files_array(files_json);
            if (moonraker_probe_can_log())
                BOOST_LOG_TRIVIAL(info) << "Moonraker model probe via websocket: machine=" << machine_id
                                        << " files=" << result.file_count;
            return true;
        };

        auto run_via_http = [&]() {
            result = MoonrakerModelProbeResult{};
            std::string body;
            try {
                auto http = Http::get(url);
                if (!api_key.empty())
                    http.header("X-Api-Key", api_key);
                // Connect timeout must be generous: when Moonraker already has a WS
                // session open, new TCP accepts can stall for many seconds.
                http.timeout_connect(15)
                    .timeout_max(30)
                    .on_complete([&](std::string response, unsigned status) {
                        result.status_code = status;
                        if (status == 200) {
                            result.ok = true;
                            body = std::move(response);
                        } else {
                            result.error_message = "Unexpected HTTP status";
                        }
                    })
                    .on_error([&](std::string, std::string error, unsigned status) {
                        result.status_code = status;
                        result.error_message = std::move(error);
                        result.ok = false;
                    })
                    .perform_sync();

                if (!body.empty()) {
                    auto parsed = nlohmann::json::parse(body, nullptr, false, true);
                    if (!parsed.is_discarded() && parsed.contains("result") && parsed["result"].is_array()) {
                        parse_files_array(parsed["result"]);
                    } else {
                        result.ok = false;
                        if (result.error_message.empty())
                            result.error_message = "Invalid Moonraker files/list response";
                    }
                } else if (result.ok) {
                    result.ok = false;
                    result.error_message = "Empty Moonraker files/list response";
                }

                if (moonraker_probe_can_log())
                    BOOST_LOG_TRIVIAL(info) << "Moonraker model probe via http: machine=" << machine_id
                                            << " url=" << url
                                            << " status=" << result.status_code
                                            << " files=" << result.file_count
                                            << " error=" << result.error_message;
            } catch (const std::exception& ex) {
                result.ok = false;
                result.error_message = ex.what();
                if (moonraker_probe_can_log())
                    BOOST_LOG_TRIVIAL(error) << "Moonraker model probe failed: " << ex.what();
            } catch (...) {
                result.ok = false;
                result.error_message = "unknown exception";
                if (moonraker_probe_can_log())
                    BOOST_LOG_TRIVIAL(error) << "Moonraker model probe failed with unknown exception";
            }
        };

        try {
            if (moonraker_probe_should_stop())
                return;
            if (!run_via_websocket()) {
                run_via_http();
                if (!moonraker_probe_should_stop() && !result.ok && gen == s_moonraker_model_probe_gen.load()) {
                    if (moonraker_probe_can_log())
                        BOOST_LOG_TRIVIAL(warning) << "Moonraker model probe: retrying after HTTP failure"
                                                   << " error=" << result.error_message;
                    std::this_thread::sleep_for(std::chrono::milliseconds(700));
                    if (!moonraker_probe_should_stop() && gen == s_moonraker_model_probe_gen.load()) {
                        if (!run_via_websocket())
                            run_via_http();
                    }
                }
            }

            if (!moonraker_probe_should_stop() && gen == s_moonraker_model_probe_gen.load() && result.ok && on_result)
                on_result(result);

            if (!moonraker_probe_should_stop() && result.ok && !result.files.empty())
                probe_moonraker_model_metadata(base_url, api_key, moonraker_agent, result.files,
                    [&](MoonrakerModelFileView file) {
                        if (!moonraker_probe_should_stop() && gen == s_moonraker_model_probe_gen.load() && on_file_meta)
                            on_file_meta(std::move(file));
                    });
        } catch (const std::exception& ex) {
            result.ok = false;
            result.error_message = ex.what();
            if (moonraker_probe_can_log())
                BOOST_LOG_TRIVIAL(error) << "Moonraker model probe failed: " << ex.what();
        } catch (...) {
            result.ok = false;
            result.error_message = "unknown exception";
            if (moonraker_probe_can_log())
                BOOST_LOG_TRIVIAL(error) << "Moonraker model probe failed with unknown exception";
        }

        if (!moonraker_probe_should_stop() && gen == s_moonraker_model_probe_gen.load() && on_result && !result.ok)
            on_result(std::move(result));
    }).detach();
}

wxString history_status_text(TaskState state)
{
    switch (state) {
    case TS_PENDING:
    case TS_SENDING:
    case TS_SEND_COMPLETED:
    case TS_PRINTING:
        return _L("Printing");
    case TS_PRINT_SUCCESS:
        return _L("Completed");
    case TS_SEND_CANCELED:
    case TS_REMOVED:
        return _L("Canceled");
    case TS_SEND_FAILED:
    case TS_PRINT_FAILED:
        return _L("Stopped");
    default:
        return _L("Unknown");
    }
}

bool parse_cloud_time(const std::string& value, std::tm& out)
{
    if (value.empty())
        return false;
    out = {};
    std::istringstream iss(value);
    iss >> std::get_time(&out, "%Y-%m-%dT%H:%M:%SZ");
    return !iss.fail();
}

wxString history_duration_text(const std::string& start_time, const std::string& end_time)
{
    std::tm start_tm {};
    std::tm end_tm {};
    if (!parse_cloud_time(start_time, start_tm) || !parse_cloud_time(end_time, end_tm))
        return _L("Duration: N/A");

    const std::time_t start = std::mktime(&start_tm);
    const std::time_t end = std::mktime(&end_tm);
    if (start == static_cast<std::time_t>(-1) || end == static_cast<std::time_t>(-1) || end < start)
        return _L("Duration: N/A");

    const int total_minutes = static_cast<int>(std::difftime(end, start) / 60.0);
    const int hours = total_minutes / 60;
    const int minutes = total_minutes % 60;
    if (hours > 0)
        return wxString::Format(_L("Duration: %dh %dm"), hours, minutes);
    return wxString::Format(_L("Duration: %dm"), minutes);
}

class TimelapsePreviewCard : public wxPanel
{
public:
    TimelapsePreviewCard(wxWindow* parent, const wxString& name)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , m_name(name)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(260), FromDIP(148)));
        SetMaxSize(wxSize(FromDIP(360), FromDIP(205)));
        Bind(wxEVT_PAINT, &TimelapsePreviewCard::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
            m_selected = !m_selected;
            Refresh();
            evt.Skip();
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& evt) {
            m_hover = true;
            SetCursor(wxCURSOR_HAND);
            Refresh();
            evt.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            m_hover = false;
            SetCursor(wxCURSOR_ARROW);
            Refresh();
            evt.Skip();
        });
    }

    void set_selected(bool selected)
    {
        m_selected = selected;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize size = GetSize();
        const int radius = FromDIP(8);

        dc.SetPen(wxPen(m_hover || m_selected ? wxColour("#35AD27") : wxColour("#3A3F47"), FromDIP(1)));
        dc.SetBrush(wxBrush(wxColour("#252A31")));
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);

        wxRect image_rect(FromDIP(2), FromDIP(2), size.x - FromDIP(4), size.y - FromDIP(4));
        dc.SetClippingRegion(image_rect);
        dc.GradientFillLinear(image_rect, wxColour("#303741"), wxColour("#111318"), wxSOUTH);

        dc.SetPen(wxPen(wxColour("#515B68"), FromDIP(2)));
        for (int i = 0; i < 5; ++i) {
            const int y = image_rect.y + FromDIP(24 + i * 22);
            dc.DrawLine(image_rect.x + FromDIP(10), y, image_rect.GetRight() - FromDIP(10), y + FromDIP(10));
        }

        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(wxColour("#DDE3EA"));
        dc.DrawText(m_name, image_rect.x + FromDIP(12), image_rect.y + FromDIP(12));
        dc.DestroyClippingRegion();

        if (m_selected) {
            const int overlay_h = FromDIP(40);
            wxRect overlay(FromDIP(2), size.y - overlay_h - FromDIP(2), size.x - FromDIP(4), overlay_h);
            fill_rect_alpha(dc, overlay, wxColour(0, 0, 0, 170), FromDIP(6));

            dc.SetFont(Label::Head_13);
            dc.SetTextForeground(*wxWHITE);
            dc.DrawText(_L("Delete"), overlay.x + overlay.width / 4 - FromDIP(20), overlay.y + FromDIP(12));
            dc.DrawText(_L("Download"), overlay.x + overlay.width * 3 / 4 - FromDIP(32), overlay.y + FromDIP(12));
        }
    }

    bool     m_selected{ false };
    bool     m_hover{ false };
    wxString m_name;
};
} // namespace

void stop_moonraker_model_file_probes()
{
    s_moonraker_model_probe_stop.store(true, std::memory_order_release);
    ++s_moonraker_model_probe_gen;
}

MultiTaskItem::MultiTaskItem(wxWindow* parent, MachineObject* obj, int type)
    : DeviceItem(parent, obj),
    m_task_type(type)
{
    SetBackgroundColour(m_task_type == 1 ? wxColour("#EEEEEF") : *wxWHITE);
    const int item_height = m_task_type == 1 ? CLOUD_HISTORY_ITEM_HEIGHT : DEVICE_ITEM_MAX_HEIGHT;
    SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(item_height)));
    SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(item_height)));

    Bind(wxEVT_PAINT, &MultiTaskItem::paintEvent, this);
    Bind(wxEVT_WEBREQUEST_STATE, &MultiTaskItem::on_thumbnail_request, this);
    Bind(wxEVT_ENTER_WINDOW, &MultiTaskItem::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &MultiTaskItem::OnLeaveWindow, this);
    Bind(wxEVT_LEFT_DOWN, &MultiTaskItem::OnLeftDown, this);
    Bind(wxEVT_MOTION, &MultiTaskItem::OnMove, this);
    Bind(EVT_MULTI_DEVICE_SELECTED, &MultiTaskItem::OnSelectedDevice, this);

    m_bitmap_check_disable = ScalableBitmap(this, "check_off_disabled", 18);
    m_bitmap_check_off = ScalableBitmap(this, "check_off_focused", 18);
    m_bitmap_check_on = ScalableBitmap(this, "check_on", 18);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* item_sizer = new wxBoxSizer(wxHORIZONTAL);


    auto m_btn_bg_enable = StateColor(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );

    m_button_resume = new Button(this, _L("Resume"));
    m_button_resume->SetBackgroundColor(m_btn_bg_enable);
    m_button_resume->SetBorderColor(m_btn_bg_enable);
    m_button_resume->SetFont(Label::Body_12);
    m_button_resume->SetTextColor(StateColor::darkModeColorFor("#FFFFFE"));
    m_button_resume->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));
    m_button_resume->SetCornerRadius(6);


    StateColor clean_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled), std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered), std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Enabled),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));
    StateColor clean_bd(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    StateColor clean_text(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetBackgroundColor(clean_bg);
    m_button_cancel->SetBorderColor(clean_bd);
    m_button_cancel->SetTextColor(clean_text);
    m_button_cancel->SetFont(Label::Body_12);
    m_button_cancel->SetCornerRadius(6);
    m_button_cancel->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));

    m_button_pause = new Button(this, _L("Pause"));
    m_button_pause->SetBackgroundColor(clean_bg);
    m_button_pause->SetBorderColor(clean_bd);
    m_button_pause->SetTextColor(clean_text);
    m_button_pause->SetFont(Label::Body_12);
    m_button_pause->SetCornerRadius(6);
    m_button_pause->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));

    m_button_stop = new Button(this, _L("Stop"));
    m_button_stop->SetBackgroundColor(clean_bg);
    m_button_stop->SetBorderColor(clean_bd);
    m_button_stop->SetTextColor(clean_text);
    m_button_stop->SetFont(Label::Body_12);
    m_button_stop->SetCornerRadius(6);
    m_button_stop->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));


    item_sizer->Add(0, 0, 1, wxEXPAND, 0);
    item_sizer->Add(m_button_cancel, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_resume, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_pause, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_stop, 0, wxALIGN_CENTER, 0);

    m_button_cancel->Hide();
    m_button_pause->Hide();
    m_button_resume->Hide();
    m_button_stop->Hide();

    main_sizer->Add(item_sizer, 1, wxEXPAND, 0);
    SetSizer(main_sizer);
    Layout();

    m_button_cancel->Bind(wxEVT_BUTTON, [this](auto& e) {
        onCancel();
    });

    m_button_pause->Bind(wxEVT_BUTTON, [this](auto& e) {
        onPause();
    });

    m_button_resume->Bind(wxEVT_BUTTON, [this](auto& e) {
        onResume();
    });

    m_button_stop->Bind(wxEVT_BUTTON, [this](auto& e) {
        onStop();
    });

    wxGetApp().UpdateDarkUIWin(this);
}

MultiTaskItem::~MultiTaskItem()
{
    if (m_thumbnail_request.IsOk())
        m_thumbnail_request.Cancel();
}

void MultiTaskItem::set_history_info(TaskStateInfo& info, const wxString& date_text, const wxString& duration_text, const wxString& status_text)
{
    m_history_date = date_text.IsEmpty() ? _L("Date: N/A") : wxString::Format(_L("Date: %s"), date_text);
    m_history_duration = duration_text;
    m_history_status = status_text;
    m_thumbnail_url = wxString::FromUTF8(info.thumbnail_url);

    if (m_thumbnail_request.IsOk())
        m_thumbnail_request.Cancel();
    m_thumbnail_image = wxImage();

    if (!m_thumbnail_url.IsEmpty()) {
        m_thumbnail_request = wxWebSession::GetDefault().CreateRequest(this, m_thumbnail_url);
        if (m_thumbnail_request.IsOk())
            m_thumbnail_request.Start();
    }
    Refresh();
}

void MultiTaskItem::on_thumbnail_request(wxWebRequestEvent& evt)
{
    if (evt.GetState() == wxWebRequest::State_Completed && evt.GetResponse().GetStream() != nullptr) {
        wxImage image;
        if (image.LoadFile(*evt.GetResponse().GetStream(), wxBITMAP_TYPE_ANY))
            m_thumbnail_image = image;
        Refresh();
    }
}

void MultiTaskItem::update_info()
{
    if (m_task_type == 1) {
        m_button_cancel->Hide();
        m_button_stop->Hide();
        m_button_pause->Hide();
        m_button_resume->Hide();
        Layout();
        return;
    }

    //local
    if (m_task_type == 0) {
        m_button_stop->Hide();
        m_button_pause->Hide();
        m_button_resume->Hide();
        if (state_local_task  == 0 || state_local_task == 1) {
            m_button_cancel->Show();
            Layout();
        }
        else {
            m_button_cancel->Hide();
            Layout();
        }
    }
    //cloud
    else if (m_task_type == 1 && get_obj() && (m_job_id == get_obj()->profile_id_)) {
        m_button_cancel->Hide();

        if (obj_ && obj_->is_in_printing() && state_cloud_task == 0 ) {
            if (obj_->can_abort()) {
                m_button_stop->Show();
            }
            else {
                m_button_stop->Hide();
            }

            if (obj_->can_pause()) {
                m_button_pause->Show();
            }
            else {
                m_button_pause->Hide();
            }

            if (obj_->can_resume()) {
                m_button_resume->Show();
            }
            else {
                m_button_resume->Hide();
            }

            Layout();
        }
        else {
            m_button_stop->Hide();
            m_button_pause->Hide();
            m_button_resume->Hide();
            Layout();
        }
    }
    else {
        m_button_cancel->Hide();
        m_button_stop->Hide();
        m_button_pause->Hide();
        m_button_resume->Hide();
        Layout();
    }
}

void MultiTaskItem::onPause()
{
    if (get_obj() && !get_obj()->can_resume()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: pause current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_pause();
        m_button_pause->Hide();
        m_button_resume->Show();
        Layout();
    }
}

void MultiTaskItem::onResume()
{
    if (get_obj() && get_obj()->can_resume()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: resume current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_resume();
        m_button_pause->Show();
        m_button_resume->Hide();
        Layout();
    }
}

void MultiTaskItem::onStop()
{
    if (get_obj()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: abort current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_abort();
        m_button_pause->Hide();
        m_button_resume->Hide();
        m_button_stop->Hide();
        state_cloud_task = 2;
        Layout();
        Refresh();
    }
}


void MultiTaskItem::onCancel()
{
    if (task_obj) {
        task_obj->cancel();
        if (!task_obj->get_job_id().empty()) {
            get_obj()->command_task_cancel(task_obj->get_job_id());
        }
    }
}

void MultiTaskItem::OnEnterWindow(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh();
}

void MultiTaskItem::OnLeaveWindow(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh();
}

void MultiTaskItem::OnSelectedDevice(wxCommandEvent& evt)
{
    auto dev_id = evt.GetString();
    auto state = evt.GetInt();
    if (state == 0) {
        state_selected = 1;
    }
    else if (state == 1) {
        state_selected = 0;
    }
    Refresh();
}

void MultiTaskItem::OnLeftDown(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {

        if (m_task_type == 0 && state_local_task <= 1) {
            post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
        }
        else if (m_task_type == 1 && state_cloud_task == 0) {
            post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
        }
    }
}

void MultiTaskItem::OnMove(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {
        SetCursor(wxCURSOR_HAND);
    }
    else {
        SetCursor(wxCURSOR_ARROW);
    }
}

void MultiTaskItem::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void MultiTaskItem::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void MultiTaskItem::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    if (m_task_type == 1) {
        const wxColour bg("#EEEEEF");
        const wxColour card_bg(m_hover ? "#F7F7F7" : "#FFFFFF");
        const wxColour border(m_hover ? "#35AD27" : "#C7C7C7");
        const wxColour text("#232527");
        const wxColour muted("#767C84");
        const wxColour accent("#35AD27");
        const int radius = FromDIP(8);
        const wxRect card_rect(FromDIP(10), FromDIP(6), size.x - FromDIP(20), size.y - FromDIP(12));

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg));
        dc.DrawRectangle(0, 0, size.x, size.y);
        dc.SetPen(wxPen(border));
        dc.SetBrush(wxBrush(card_bg));
        dc.DrawRoundedRectangle(card_rect.x, card_rect.y, card_rect.width, card_rect.height, radius);

        const wxRect thumb_rect(card_rect.x + FromDIP(12), card_rect.y + FromDIP(12), FromDIP(72), FromDIP(60));
        dc.SetPen(wxPen(wxColour("#C7C7C7")));
        dc.SetBrush(wxBrush(wxColour("#F5F5F5")));
        dc.DrawRoundedRectangle(thumb_rect.x, thumb_rect.y, thumb_rect.width, thumb_rect.height, FromDIP(6));
        if (m_thumbnail_image.IsOk()) {
            wxImage thumb = m_thumbnail_image.Copy();
            thumb.Rescale(thumb_rect.width, thumb_rect.height, wxIMAGE_QUALITY_HIGH);
            dc.DrawBitmap(wxBitmap(thumb), thumb_rect.x, thumb_rect.y, true);
        } else {
            dc.SetTextForeground(muted);
            dc.SetFont(Label::Body_12);
            dc.DrawText(_L("Thumbnail"), thumb_rect.x + FromDIP(8), thumb_rect.y + FromDIP(22));
        }

        const int text_left = thumb_rect.GetRight() + FromDIP(16);
        const int status_width = FromDIP(120);
        const int text_width = card_rect.GetRight() - text_left - status_width - FromDIP(20);

        dc.SetTextForeground(text);
        dc.SetFont(Label::Head_14);
        DrawTextWithEllipsis(dc, m_project_name.IsEmpty() ? _L("Unknown model") : m_project_name, text_width, text_left, card_rect.y + FromDIP(14));

        dc.SetTextForeground(muted);
        dc.SetFont(Label::Body_12);
        DrawTextWithEllipsis(dc, m_dev_name.IsEmpty() ? _L("Unknown printer") : m_dev_name, text_width, text_left, card_rect.y + FromDIP(38));
        DrawTextWithEllipsis(dc, m_history_duration, FromDIP(170), text_left, card_rect.y + FromDIP(60));
        DrawTextWithEllipsis(dc, m_history_date, FromDIP(230), text_left + FromDIP(180), card_rect.y + FromDIP(60));

        const wxColour status_bg = m_history_status == _L("Completed") ? wxColour("#E8F6EC") :
                                   m_history_status == _L("Printing") ? wxColour("#E8F2FA") :
                                   m_history_status == _L("Canceled") ? wxColour("#F7F0E8") :
                                                                        wxColour("#F8E8E8");
        const wxColour status_fg = m_history_status == _L("Completed") ? accent :
                                   m_history_status == _L("Printing") ? wxColour("#7CB7FF") :
                                   m_history_status == _L("Canceled") ? wxColour("#F0B15B") :
                                                                        wxColour("#FF7474");
        const wxRect status_rect(card_rect.GetRight() - status_width - FromDIP(14), card_rect.y + FromDIP(28), status_width, FromDIP(32));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(status_bg));
        dc.DrawRoundedRectangle(status_rect.x, status_rect.y, status_rect.width, status_rect.height, FromDIP(16));
        dc.SetTextForeground(status_fg);
        dc.SetFont(Label::Body_12);
        const wxSize status_text = dc.GetTextExtent(m_history_status);
        dc.DrawText(m_history_status,
                    status_rect.x + (status_rect.width - status_text.x) / 2,
                    status_rect.y + (status_rect.height - status_text.y) / 2);
        return;
    }

    dc.SetPen(wxPen(*wxBLACK));

    int left = FromDIP(TASK_LEFT_PADDING_LEFT);


    //checkbox
    if (m_task_type == 0) {
        if (state_local_task >= 2) {
            dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
        }
        else {
            if (state_selected == 0) {
                dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
            else if (state_selected == 1) {
                dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
        }
    }
    else if(m_task_type == 1){
        if (state_cloud_task != 0) {
            dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
        }
        else {
            if (state_selected == 0) {
                dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
            else if (state_selected == 1) {
                dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
        }
    }


    left += FromDIP(TASK_LEFT_PRINTABLE);

    //project name
    DrawTextWithEllipsis(dc, m_project_name, FromDIP(TASK_LEFT_PRO_NAME), left);
    left += FromDIP(TASK_LEFT_PRO_NAME);

    //dev name
    DrawTextWithEllipsis(dc, m_dev_name, FromDIP(TASK_LEFT_DEV_NAME), left);
    left += FromDIP(TASK_LEFT_DEV_NAME);

    //local task state
    if (m_task_type == 0) {
        DrawTextWithEllipsis(dc, get_local_state_task(), FromDIP(TASK_LEFT_PRO_STATE), left);
    }
    else {
        DrawTextWithEllipsis(dc, get_cloud_state_task(), FromDIP(TASK_LEFT_PRO_STATE), left);
    }

    left += FromDIP(TASK_LEFT_PRO_STATE);

    //cloud task info
    if (m_task_type == 1) {
        if (get_obj()) {
            if (state_cloud_task == 0 && m_job_id == get_obj()->profile_id_) {
                dc.SetFont(Label::Body_13);
                if (state_device == 0) {
                    dc.SetTextForeground(*wxBLACK);
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device == 1) {
                    dc.SetTextForeground(wxColour(0, 150, 136));
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device == 2)
                {
                    dc.SetTextForeground(wxColour(208, 27, 27));
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device > 2 && state_device < 7) {
                    dc.SetFont(Label::Body_12);
                    dc.SetTextForeground(wxColour(0, 150, 136));
                    if (obj_->get_curr_stage() == _L("Printing") && obj_->subtask_) {
                        //wxString layer_info = wxString::Format(_L("Layer: %d/%d"), obj_->curr_layer, obj_->total_layers);
                        wxString progress_info = wxString::Format("%d", obj_->subtask_->task_progress);
                        wxString left_time = wxString::Format("%s", get_left_time(obj_->mc_left_time));

                        DrawTextWithEllipsis(dc, progress_info + "%  |  " + left_time, FromDIP(TASK_LEFT_PRO_INFO), left, FromDIP(10));

                        dc.SetPen(wxPen(wxColour(233, 233, 233)));
                        dc.SetBrush(wxBrush(wxColour(233, 233, 233)));
                        dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO), FromDIP(10), 2);

                        dc.SetPen(wxPen(wxColour(0, 150, 136)));
                        dc.SetBrush(wxBrush(wxColour(0, 150, 136)));
                        dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO) * (static_cast<float>(obj_->subtask_->task_progress) / 100.0f), FromDIP(10), 2);
                    }
                    else {
                        DrawTextWithEllipsis(dc, obj_->get_curr_stage(), FromDIP(TASK_LEFT_PRO_INFO), left);
                    }
                }
                else {
                    dc.SetTextForeground(*wxBLACK);
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(TASK_LEFT_PRO_INFO), left);
                }
            }
        }
    }
    else {
        if (state_local_task == 1) {
            wxString progress_info = wxString::Format("%d", m_sending_percent);
            DrawTextWithEllipsis(dc, progress_info + "% " , FromDIP(TASK_LEFT_PRO_INFO), left, FromDIP(10));

            dc.SetPen(wxPen(wxColour(233, 233, 233)));
            dc.SetBrush(wxBrush(wxColour(233, 233, 233)));
            dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO), FromDIP(10), 2);

            dc.SetPen(wxPen(wxColour(0, 150, 136)));
            dc.SetBrush(wxBrush(wxColour(0, 150, 136)));
            dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO) * (static_cast<float>(m_sending_percent) / 100.0f), FromDIP(10), 2);
        }
        /*else {
            if () {

            }
            if (m_button_cancel->IsShown()) {
                m_button_cancel->Hide();
                Layout();
            }
        }*/
    }
    left += FromDIP(TASK_LEFT_PRO_INFO);

    //send time
    dc.SetFont(Label::Body_13);
    dc.SetTextForeground(*wxBLACK);

    if (!boost::algorithm::contains(m_send_time, "1970")) {
        DrawTextWithEllipsis(dc, m_send_time, FromDIP(TASK_LEFT_SEND_TIME), left);
    }

    left += FromDIP(TASK_LEFT_SEND_TIME);

    if (m_hover) {
        dc.SetPen(wxPen(wxColour(0, 150, 136)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, 3);
    }
}

void MultiTaskItem::DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top) {
    wxSize size = GetSize();
    wxFont font = dc.GetFont();

    wxSize textSize = dc.GetTextExtent(text);

    int textWidth = textSize.GetWidth();

    if (textWidth > maxWidth) {
        wxString truncatedText = text;
        int ellipsisWidth = dc.GetTextExtent("...").GetWidth();
        int numChars = text.length();

        for (int i = numChars - 1; i >= 0; --i) {
            truncatedText = text.substr(0, i) + "...";
            int truncatedWidth = dc.GetTextExtent(truncatedText).GetWidth();

            if (truncatedWidth <= maxWidth - ellipsisWidth) {
                break;
            }
        }

        if (top == 0) {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2 - top);
        }

    }
    else {
        if (top == 0) {
            dc.DrawText(text, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(text, left, (size.y - textSize.y) / 2 - top);
        }
    }
}

void MultiTaskItem::post_event(wxCommandEvent&& event)
{
    event.SetEventObject(this);
    event.SetString(m_dev_id);
    event.SetInt(state_selected);
    wxPostEvent(this, event);
}

void MultiTaskItem::DoSetSize(int x, int y, int width, int height, int sizeFlags /*= wxSIZE_AUTO*/)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}

wxString MultiTaskItem::get_left_time(int mc_left_time)
{
    // update gcode progress
    std::string left_time;
    wxString    left_time_text = _L("N/A");

    try {
        left_time = get_bbl_monitor_time_dhm(mc_left_time);
    }
    catch (...) {
        ;
    }

    if (!left_time.empty()) left_time_text = wxString::Format("-%s", left_time);
    return left_time_text;
}


LocalTaskManagerPage::LocalTaskManagerPage(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(wxColour(0xEEEEEE));
    m_main_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_main_panel->SetBackgroundColour(*wxWHITE);
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    StateColor head_bg(
        std::pair<wxColour, int>(TABLE_HEAD_PRESSED_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(TABLE_HEAR_NORMAL_COLOUR, StateColor::Normal)
    );

    m_table_head_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_table_head_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetBackgroundColour(TABLE_HEAR_NORMAL_COLOUR);
    m_table_head_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_select_checkbox = new CheckBox(m_table_head_panel, wxID_ANY);
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_table_head_sizer->Add(m_select_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_select_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        if (m_select_checkbox->GetValue()) {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {

                if (it->second->state_local_task <= 1) {
                    it->second->selected();
                }
            }
        }
        else {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
                it->second->unselected();
            }
        }
        Refresh(false);
        e.Skip();
        });


    m_task_name = new Button(m_table_head_panel, _L("Task Name"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_task_name->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_task_name->SetFont(TABLE_HEAD_FONT);
    m_task_name->SetCornerRadius(0);
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetCenter(false);
    m_table_head_sizer->Add(m_task_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_printer_name = new Button(m_table_head_panel, _L("Device Name"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_printer_name->SetBackgroundColor(head_bg);
    m_printer_name->SetFont(TABLE_HEAD_FONT);
    m_printer_name->SetCornerRadius(0);
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetCenter(false);
    m_printer_name->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_printer_name->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_printer_name->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_name_big = !device_name_big;
        this->m_sort.set_role(SortItem::SortRule::SR_DEV_NAME, device_name_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_status = new Button(m_table_head_panel, _L("Task Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_status->SetBackgroundColor(head_bg);
    m_status->SetFont(TABLE_HEAD_FONT);
    m_status->SetCornerRadius(0);
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetCenter(false);
    m_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_state_big = !device_state_big;
        this->m_sort.set_role(SortItem::SortRule::SR_LOCAL_TASK_STATE, device_state_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_info = new Button(m_table_head_panel, _L("Info"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_info->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_info->SetFont(TABLE_HEAD_FONT);
    m_info->SetCornerRadius(0);
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetCenter(false);
    m_table_head_sizer->Add(m_info, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_send_time = new Button(m_table_head_panel, _L("Sent Time"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_send_time->SetBackgroundColor(head_bg);
    m_send_time->SetFont(TABLE_HEAD_FONT);
    m_send_time->SetCornerRadius(0);
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetCenter(false);
    m_send_time->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_send_time->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_send_time->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_send_time = !device_send_time;
        this->m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_send_time, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_action = new Button(m_table_head_panel, _L("Actions"), "", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_action->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_action->SetFont(TABLE_HEAD_FONT);
    m_action->SetCornerRadius(0);
    /* m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
     m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));*/
    m_action->SetCenter(false);
    m_table_head_sizer->Add(m_action, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_table_head_panel->SetSizer(m_table_head_sizer);
    m_table_head_panel->Layout();

    m_tip_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tip_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetLabel(_L("There are no tasks to be sent!"));
    m_tip_text->SetForegroundColour(wxColour(50, 58, 61));
    m_tip_text->SetFont(::Label::Head_24);
    m_tip_text->Wrap(-1);

    m_task_list = new wxScrolledWindow(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_task_list->SetBackgroundColour(*wxWHITE);
    m_task_list->SetScrollRate(0, 5);
    m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_list->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), 10 * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    m_sizer_task_list = new wxBoxSizer(wxVERTICAL);
    m_task_list->SetSizer(m_sizer_task_list);
    m_task_list->Layout();

    m_main_sizer->AddSpacer(FromDIP(50));
    m_main_sizer->Add(m_table_head_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->Add(m_tip_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_task_list, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->AddSpacer(FromDIP(5));

    // ctrl panel
    StateColor ctrl_bg(
        std::pair<wxColour, int>(CTRL_BUTTON_PRESSEN_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(CTRL_BUTTON_NORMAL_COLOUR, StateColor::Normal)
    );

    m_ctrl_btn_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_ctrl_btn_panel->SetBackgroundColour(*wxWHITE);
    m_ctrl_btn_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_ctrl_btn_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_stop_all = new Button(m_ctrl_btn_panel, _L("Stop"));
    btn_stop_all->SetBackgroundColor(ctrl_bg);
    btn_stop_all->SetCornerRadius(FromDIP(5));
    m_sel_text = new wxStaticText(m_ctrl_btn_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);

    m_btn_sizer->Add(m_sel_text, 0, wxLEFT, FromDIP(15));;
    m_btn_sizer->Add(btn_stop_all, 0, wxLEFT, FromDIP(10));
    m_ctrl_btn_panel->SetSizer(m_btn_sizer);
    m_ctrl_btn_panel->Layout();

    m_main_sizer->AddSpacer(FromDIP(10));
    m_main_sizer->Add(m_ctrl_btn_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    btn_stop_all->Bind(wxEVT_BUTTON, &LocalTaskManagerPage::cancel_all, this);
    m_main_panel->SetSizer(m_main_sizer);
    m_main_panel->Layout();

    page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->Add(m_main_panel, 1, wxALL | wxEXPAND, FromDIP(10)); // ORCA match margin with other tabs

    wxGetApp().UpdateDarkUIWin(this);

    SetSizer(page_sizer);
    Layout();
    Fit();
}

void LocalTaskManagerPage::update_page()
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        it->second->update_info();
    }
}

void LocalTaskManagerPage::refresh_user_device(bool clear)
{
    m_sizer_task_list->Clear(false);

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        m_ctrl_btn_panel->Show(false);
        return;
    }

    if(clear)return;

    std::vector<std::string> subscribe_list;
    std::vector<MultiTaskItem*> task_temps;

    auto all_machine = dev->get_my_cloud_machine_list();
    auto user_machine = std::map<std::string, MachineObject*>();

    //selected machine
    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        auto dev_id = wxGetApp().app_config->get("multi_devices", std::to_string(i));

        if (all_machine.count(dev_id) > 0) {
            user_machine[dev_id] = all_machine[dev_id];
        }
    }

    auto task_manager = wxGetApp().getTaskManager();
    if (task_manager) {
        auto m_task_obj_list = task_manager->get_local_task_list();

        for (auto it = m_task_obj_list.rbegin(); it != m_task_obj_list.rend(); ++it) {

            TaskStateInfo* task_state_info = it->second;

            if(!task_state_info) continue;

            MultiTaskItem* mtitem = new MultiTaskItem(m_task_list, nullptr, 0);
            mtitem->task_obj = task_state_info;
            mtitem->m_project_name = wxString::FromUTF8(task_state_info->get_task_name());
            mtitem->m_dev_name = wxString::FromUTF8(task_state_info->get_device_name());
            mtitem->m_dev_id = task_state_info->params().dev_id;
            mtitem->m_send_time = task_state_info->get_sent_time();
            mtitem->state_local_task = task_state_info->state();

            task_state_info->set_state_changed_fn([this, mtitem](TaskState state, int percent) {
                mtitem->state_local_task = state;
                if (state == TaskState::TS_SEND_COMPLETED) {

                    mtitem->m_send_time = mtitem->task_obj->get_sent_time();
                    wxCommandEvent event(EVT_MULTI_REFRESH);
                    event.SetEventObject(mtitem);
                    wxPostEvent(mtitem, event);
                }
                mtitem->m_sending_percent = percent;
            });

            if (m_task_items.find(it->first) != m_task_items.end()) {
                MultiTaskItem* item = m_task_items[it->first];
                if (item->state_selected == 1 && mtitem->state_local_task < 2)
                    mtitem->state_selected = item->state_selected;
                item->Destroy();
            }

            m_task_items[it->first] = mtitem;
            task_temps.push_back(mtitem);
        }

        if (m_sort.rule != SortItem::SortRule::SR_None && m_sort.rule != SortItem::SortRule::SR_SEND_TIME) {
            std::sort(task_temps.begin(), task_temps.end(), m_sort.get_call_back());
        }

        for (const auto& item : task_temps)
            m_sizer_task_list->Add(item, 0, wxALL | wxEXPAND, 0);

        // maintenance
        auto it = m_task_items.begin();
        while (it != m_task_items.end()) {
            if (m_task_obj_list.find(it->first) != m_task_obj_list.end())
                ++it;
            else {
                it->second->Destroy();
                it = m_task_items.erase(it);
            }
        }

        dev->subscribe_device_list(subscribe_list);
        int num = m_task_items.size() > 10 ? 10 : m_task_items.size();
        m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), num * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
        m_task_list->Layout();
    }
    m_tip_text->Show(m_task_items.empty());
    m_ctrl_btn_panel->Show(!m_task_items.empty());
    Layout();
}

bool LocalTaskManagerPage::Show(bool show)
{
    if (show) {
        refresh_user_device();
    }
    else {
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            dev->subscribe_device_list(std::vector<std::string>());
        }
    }
    return wxPanel::Show(show);
}

void LocalTaskManagerPage::cancel_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_cancel->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_local_task < 2) {
            it->second->onCancel();
        }
    }
}

void LocalTaskManagerPage::msw_rescale()
{
    m_select_checkbox->Rescale();
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->Rescale();
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->Rescale();
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->Rescale();
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->Rescale();
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->Rescale();
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->Rescale();
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    btn_stop_all->Rescale();

    for (auto it = m_task_items.begin(); it != m_task_items.end(); ++it) {
        it->second->Refresh();
    }

    Fit();
    Layout();
    Refresh();
}

CloudTaskManagerPage::CloudTaskManagerPage(wxWindow* parent)
    : CloudTaskManagerPage(parent, MediaPresentation::Combined)
{
}

CloudTaskManagerPage::CloudTaskManagerPage(wxWindow* parent, MediaPresentation presentation)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_media_presentation(presentation)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    const wxColour cprint_page_bg("#EEEEEF");
    const wxColour cprint_panel_bg("#EEEEEF");
    const wxColour cprint_table_head("#F5F5F5");
    const wxColour cprint_table_head_pressed("#EEEEEE");
    const wxColour cprint_control_bg("#F5F5F5");
    const wxColour cprint_control_pressed("#E8E8E8");
    const wxColour cprint_text("#232527");
    const wxColour cprint_muted("#767C84");

    SetBackgroundColour(cprint_page_bg);
    m_sort.set_role(SortItem::SR_SEND_TIME, true);

    SetBackgroundColour(cprint_page_bg);
    m_main_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_main_panel->SetBackgroundColour(cprint_panel_bg);
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    StateColor head_bg(
        std::pair<wxColour, int>(cprint_table_head_pressed, StateColor::Pressed),
        std::pair<wxColour, int>(cprint_table_head, StateColor::Hovered),
        std::pair<wxColour, int>(cprint_table_head, StateColor::Normal)
    );

    StateColor ctrl_bg(
        std::pair<wxColour, int>(cprint_control_pressed, StateColor::Pressed),
        std::pair<wxColour, int>(cprint_control_bg, StateColor::Hovered),
        std::pair<wxColour, int>(cprint_control_bg, StateColor::Normal)
    );
    StateColor header_text = StateColor(
        std::pair<wxColour, int>(wxColour("#232527"), StateColor::Normal)
    );

    m_table_head_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_table_head_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetBackgroundColour(cprint_table_head);
    m_table_head_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_select_checkbox = new CheckBox(m_table_head_panel, wxID_ANY);
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    //m_table_head_sizer->AddSpacer(FromDIP(TASK_LEFT_PADDING_LEFT));
    m_table_head_sizer->Add(m_select_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_select_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        if (m_select_checkbox->GetValue()) {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {

                if (it->second->state_cloud_task == 0) {
                    it->second->selected();
                }
            }
        }
        else {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
                it->second->unselected();
            }
        }
        Refresh(false);
        e.Skip();
    });



    m_task_name = new Button(m_table_head_panel, _L("Task Name"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_task_name->SetBackgroundColor(head_bg);
    m_task_name->SetTextColor(header_text);
    m_task_name->SetFont(TABLE_HEAD_FONT);
    m_task_name->SetCornerRadius(0);
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetCenter(false);
    m_table_head_sizer->Add(m_task_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_printer_name = new Button(m_table_head_panel, _L("Device Name"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_printer_name->SetBackgroundColor(head_bg);
    m_printer_name->SetTextColor(header_text);
    m_printer_name->SetFont(TABLE_HEAD_FONT);
    m_printer_name->SetCornerRadius(0);
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetCenter(false);
    m_printer_name->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_printer_name->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_printer_name->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_name_big = !device_name_big;
        this->m_sort.set_role(SortItem::SortRule::SR_DEV_NAME, device_name_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_status = new Button(m_table_head_panel, _L("Task Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_status->SetBackgroundColor(head_bg);
    m_status->SetTextColor(header_text);
    m_status->SetFont(TABLE_HEAD_FONT);
    m_status->SetCornerRadius(0);
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetCenter(false);
    m_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_state_big = !device_state_big;
        this->m_sort.set_role(SortItem::SortRule::SR_CLOUD_TASK_STATE, device_state_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_info = new Button(m_table_head_panel, _L("Info"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_info->SetBackgroundColor(head_bg);
    m_info->SetTextColor(header_text);
    m_info->SetFont(TABLE_HEAD_FONT);
    m_info->SetCornerRadius(0);
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetCenter(false);
    m_table_head_sizer->Add(m_info, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_send_time = new Button(m_table_head_panel, _L("Sent Time"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_send_time->SetBackgroundColor(head_bg);
    m_send_time->SetTextColor(header_text);
    m_send_time->SetFont(TABLE_HEAD_FONT);
    m_send_time->SetCornerRadius(0);
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetCenter(false);
    m_send_time->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_send_time->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_send_time->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_send_time = !device_send_time;
        this->m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_send_time, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_action = new Button(m_table_head_panel, _L("Actions"), "", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_action->SetBackgroundColor(head_bg);
    m_action->SetTextColor(header_text);
    m_action->SetFont(TABLE_HEAD_FONT);
    m_action->SetCornerRadius(0);
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetCenter(false);
    m_table_head_sizer->Add(m_action, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_table_head_panel->SetSizer(m_table_head_sizer);
    m_table_head_panel->Layout();

    m_tip_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tip_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetLabel(_L("No historical tasks!"));
    m_table_head_panel->Hide();
    m_tip_text->SetForegroundColour(cprint_text);
    m_tip_text->SetFont(::Label::Head_24);
    m_tip_text->Wrap(-1);

    m_loading_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_loading_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_loading_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_loading_text->SetLabel(_L("Loading..."));
    m_loading_text->SetForegroundColour(cprint_text);
    m_loading_text->SetFont(::Label::Head_24);
    m_loading_text->Wrap(-1);
    m_loading_text->Show(false);

    m_model_status_text = new wxStaticText(m_main_panel, wxID_ANY, _L("Select refresh to load printer models."), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_model_status_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_model_status_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_model_status_text->SetForegroundColour(cprint_text);
    m_model_status_text->SetFont(::Label::Head_14);
    m_model_status_text->Wrap(-1);
    m_model_status_text->Show(false);

    m_model_file_grid = new wxScrolledWindow(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxVSCROLL);
    m_model_file_grid->SetBackgroundColour(cprint_panel_bg);
    m_model_file_grid->SetScrollRate(0, FromDIP(12));
    m_model_file_grid->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    m_model_file_grid_sizer = new wxFlexGridSizer(0, 1, FromDIP(kModelGridGapDip), FromDIP(kModelGridGapDip));
    m_model_file_grid->SetSizer(m_model_file_grid_sizer);
    auto* overlay = new ModelGridOverlayScroll(m_main_panel);
    overlay->attach(m_model_file_grid);
    overlay->set_on_scroll([this] { load_visible_model_thumbnails(); });
    m_model_grid_scroll = overlay;
    m_model_file_grid->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        relayout_model_file_grid();
        sync_model_grid_overlay(false);
        load_visible_model_thumbnails();
    });
    const auto on_grid_scroll = [this](wxScrollWinEvent& evt) {
        evt.Skip();
        sync_model_grid_overlay(true);
        load_visible_model_thumbnails();
    };
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_TOP, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_BOTTOM, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_LINEUP, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_LINEDOWN, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_PAGEUP, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_PAGEDOWN, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_THUMBTRACK, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, on_grid_scroll);
    m_model_file_grid->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& evt) {
        evt.Skip();
        CallAfter([this] {
            sync_model_grid_overlay(true);
            load_visible_model_thumbnails();
        });
    });
    m_model_file_grid->Bind(wxEVT_MOTION, [this](wxMouseEvent& evt) {
        evt.Skip();
        const int edge = FromDIP(kModelScrollBarWDip + 6);
        if (m_model_file_grid != nullptr && evt.GetX() >= m_model_file_grid->GetClientSize().GetWidth() - edge)
            sync_model_grid_overlay(true);
    });
    m_model_file_grid->Show(false);

    m_task_list = new wxScrolledWindow(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_task_list->SetBackgroundColour(cprint_panel_bg);
    m_task_list->SetScrollRate(0, 5);
    m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(CLOUD_HISTORY_ITEM_HEIGHT)));
    m_task_list->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), 10 * FromDIP(CLOUD_HISTORY_ITEM_HEIGHT)));

    m_sizer_task_list = new wxBoxSizer(wxVERTICAL);
    m_task_list->SetSizer(m_sizer_task_list);
    m_task_list->Layout();
    m_task_list->Fit();

    m_timelapse_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_timelapse_panel->SetBackgroundColour(cprint_panel_bg);
    m_timelapse_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(520)));
    wxBoxSizer* timelapse_sizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* timelapse_header = new wxPanel(m_timelapse_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    timelapse_header->SetBackgroundColour(cprint_panel_bg);
    wxBoxSizer* timelapse_header_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_timelapse_date_range = new wxStaticText(timelapse_header, wxID_ANY, _L("2026-07-13 - 2026-04-29"));
    m_timelapse_date_range->SetForegroundColour(cprint_text);
    m_timelapse_date_range->SetFont(Label::Head_14);
    timelapse_header_sizer->Add(m_timelapse_date_range, 0, wxALIGN_BOTTOM, 0);
    timelapse_header_sizer->AddStretchSpacer(1);
    timelapse_header->SetSizer(timelapse_header_sizer);

    m_timelapse_grid = new wxScrolledWindow(m_timelapse_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_timelapse_grid->SetBackgroundColour(cprint_panel_bg);
    m_timelapse_grid->SetScrollRate(0, FromDIP(12));
    wxFlexGridSizer* timelapse_grid_sizer = new wxFlexGridSizer(4, FromDIP(10), FromDIP(10));
    timelapse_grid_sizer->AddGrowableCol(0, 1);
    timelapse_grid_sizer->AddGrowableCol(1, 1);
    timelapse_grid_sizer->AddGrowableCol(2, 1);
    timelapse_grid_sizer->AddGrowableCol(3, 1);
    m_timelapse_grid->SetSizer(timelapse_grid_sizer);
    m_timelapse_grid->Layout();
    update_timelapse_filter_tabs();

    timelapse_sizer->Add(timelapse_header, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    timelapse_sizer->Add(m_timelapse_grid, 1, wxEXPAND, 0);
    m_timelapse_panel->SetSizer(timelapse_sizer);
    m_timelapse_panel->Layout();
    m_timelapse_panel->Hide();

    m_media_mode_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_media_mode_panel->SetBackgroundColour(cprint_panel_bg);
    wxBoxSizer* media_mode_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_timelapse_tab = new Button(m_media_mode_panel, _L("Timelapse"));
    m_timelapse_tab->SetMinSize(wxSize(FromDIP(120), FromDIP(36)));
    m_timelapse_tab->SetMaxSize(wxSize(FromDIP(120), FromDIP(36)));
    m_timelapse_tab->SetCornerRadius(FromDIP(8));
    m_timelapse_tab->SetFont(::Label::Body_14);
    m_timelapse_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        set_media_mode(true);
        evt.Skip();
    });

    m_model_tab = new Button(m_media_mode_panel, _L("Model"));
    m_model_tab->SetMinSize(wxSize(FromDIP(120), FromDIP(36)));
    m_model_tab->SetMaxSize(wxSize(FromDIP(120), FromDIP(36)));
    m_model_tab->SetCornerRadius(FromDIP(8));
    m_model_tab->SetFont(::Label::Body_14);
    m_model_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        set_media_mode(false);
        evt.Skip();
    });

    m_refresh_tab = new Button(m_media_mode_panel, wxEmptyString, "refresh", wxNO_BORDER, 18);
    m_refresh_tab->SetMinSize(wxSize(FromDIP(32), FromDIP(30)));
    m_refresh_tab->SetMaxSize(wxSize(FromDIP(32), FromDIP(30)));
    m_refresh_tab->SetCornerRadius(0);
    m_refresh_tab->SetBorderWidth(0);
    m_refresh_tab->SetBackgroundColor(StateColor());
    m_refresh_tab->SetBackgroundColour(cprint_panel_bg);
    m_refresh_tab->SetToolTip(_L("Refresh"));
    m_refresh_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        if (m_media_timelapse_mode) {
            if (m_timelapse_panel)
                m_timelapse_panel->Refresh();
            evt.Skip();
            return;
        }
        refresh_moonraker_model_status();
        evt.Skip();
    });

    media_mode_sizer->Add(m_timelapse_tab, 0, wxRIGHT, FromDIP(4));
    media_mode_sizer->Add(m_model_tab, 0, wxRIGHT, FromDIP(12));
    media_mode_sizer->Add(m_refresh_tab, 0, wxALIGN_TOP | wxTOP, FromDIP(3));
    media_mode_sizer->AddStretchSpacer(1);

    m_timelapse_top_actions = new wxPanel(m_media_mode_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_timelapse_top_actions->SetBackgroundColour(cprint_panel_bg);
    wxBoxSizer* timelapse_actions_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* timelapse_action_row = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* timelapse_filter_row = new wxBoxSizer(wxHORIZONTAL);

    m_timelapse_select_all = new Button(m_timelapse_top_actions, _L("Select all"));
    m_timelapse_select_all->SetMinSize(wxSize(FromDIP(96), FromDIP(30)));
    m_timelapse_select_all->SetMaxSize(wxSize(FromDIP(96), FromDIP(30)));
    m_timelapse_select_all->SetCornerRadius(FromDIP(15));
    m_timelapse_select_all->SetBackgroundColor(wxColour("#00B050"));
    m_timelapse_select_all->SetTextColor(StateColor::darkModeColorFor("#FFFFFF"));
    m_timelapse_select_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        select_all_timelapse_cards();
        evt.Skip();
    });

    m_timelapse_select = new Button(m_timelapse_top_actions, _L("Select"));
    m_timelapse_select->SetMinSize(wxSize(FromDIP(72), FromDIP(30)));
    m_timelapse_select->SetMaxSize(wxSize(FromDIP(72), FromDIP(30)));
    m_timelapse_select->SetCornerRadius(FromDIP(15));
    m_timelapse_select->SetBackgroundColor(wxColour("#00B050"));
    m_timelapse_select->SetTextColor(StateColor::darkModeColorFor("#FFFFFF"));

    m_timelapse_all_files = new Button(m_timelapse_top_actions, _L("All files"));
    m_timelapse_year = new Button(m_timelapse_top_actions, _L("Year"));
    m_timelapse_month = new Button(m_timelapse_top_actions, _L("Month"));
    for (Button* filter_btn : { m_timelapse_all_files, m_timelapse_year, m_timelapse_month }) {
        filter_btn->SetMinSize(wxSize(FromDIP(94), FromDIP(28)));
        filter_btn->SetMaxSize(wxSize(FromDIP(94), FromDIP(28)));
        filter_btn->SetCornerRadius(FromDIP(8));
        filter_btn->SetFont(Label::Body_13);
    }
    m_timelapse_all_files->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { set_timelapse_filter(0); evt.Skip(); });
    m_timelapse_year->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { set_timelapse_filter(1); evt.Skip(); });
    m_timelapse_month->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { set_timelapse_filter(2); evt.Skip(); });

    timelapse_action_row->AddStretchSpacer(1);
    timelapse_action_row->Add(m_timelapse_select_all, 0, wxRIGHT, FromDIP(12));
    timelapse_action_row->Add(m_timelapse_select, 0, 0, 0);
    timelapse_filter_row->Add(m_timelapse_all_files, 0, wxRIGHT, FromDIP(18));
    timelapse_filter_row->Add(m_timelapse_year, 0, wxRIGHT, FromDIP(18));
    timelapse_filter_row->Add(m_timelapse_month, 0, 0, 0);
    timelapse_actions_sizer->Add(timelapse_action_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    timelapse_actions_sizer->Add(timelapse_filter_row, 0, wxALIGN_RIGHT, 0);
    m_timelapse_top_actions->SetSizer(timelapse_actions_sizer);
    m_timelapse_top_actions->Layout();
    m_timelapse_top_actions->Hide();
    media_mode_sizer->Add(m_timelapse_top_actions, 0, wxALIGN_CENTER_VERTICAL, 0);

    if (m_media_presentation != MediaPresentation::Combined) {
        m_media_timelapse_mode = m_media_presentation == MediaPresentation::TimelapseOnly;
        m_timelapse_tab->Hide();
        m_model_tab->Hide();
    }

    m_media_mode_panel->SetSizer(media_mode_sizer);
    m_media_mode_panel->Layout();

    m_main_sizer->AddSpacer(FromDIP(24));
    m_main_sizer->Add(m_media_mode_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    m_main_sizer->AddSpacer(FromDIP(18));
    m_main_sizer->Add(m_model_status_text, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));
    m_main_sizer->Add(m_model_file_grid, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    m_main_sizer->Add(m_table_head_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->Add(m_tip_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_loading_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_task_list, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->Add(m_timelapse_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    if (m_media_presentation != MediaPresentation::ModelOnly)
        m_main_sizer->AddSpacer(FromDIP(5));

    // add flipping page
    m_flipping_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_flipping_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_flipping_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_flipping_panel->SetBackgroundColour(cprint_panel_bg);

    m_flipping_page_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);
    btn_last_page = new Button(m_flipping_panel, "", "go_last_plate", wxBORDER_NONE, FromDIP(20));
    btn_last_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetBackgroundColor(head_bg);
    btn_last_page->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        evt.Skip();
        if (m_current_page == 0)
            return;
        enable_buttons(false);
        start_timer();
        m_current_page--;
        if (m_current_page < 0)
            m_current_page = 0;
        refresh_user_device();
        update_page_number();
        /*m_sizer_task_list->Clear(false);
        m_loading_text->Show(true);
        Layout();*/
    });
    st_page_number = new wxStaticText(m_flipping_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    st_page_number->SetForegroundColour(cprint_muted);
    btn_next_page = new Button(m_flipping_panel, "", "go_next_plate", wxBORDER_NONE, FromDIP(20));
    btn_next_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetBackgroundColor(head_bg);
    btn_next_page->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        evt.Skip();
        if (m_current_page == m_total_page - 1)
            return;
        enable_buttons(false);
        start_timer();
        m_current_page++;
        if (m_current_page > m_total_page - 1)
            m_current_page = m_total_page - 1;
        refresh_user_device();
        update_page_number();
        /*m_sizer_task_list->Clear(false);
        m_loading_text->Show(true);
        Layout();*/
    });

    m_page_num_input = new ::TextInput(m_flipping_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(50), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    m_page_num_input->SetBackgroundColor(input_bg);
    m_page_num_input->GetTextCtrl()->SetValue("1");
    wxTextValidator validator(wxFILTER_DIGITS);
    m_page_num_input->GetTextCtrl()->SetValidator(validator);
    m_page_num_input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent& e) {
        page_num_enter_evt();
    });

    m_page_num_enter = new Button(m_flipping_panel, _("Go"));
    m_page_num_enter->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetBackgroundColor(ctrl_bg);
    m_page_num_enter->SetCornerRadius(FromDIP(5));
    m_page_num_enter->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        page_num_enter_evt();
    });

    m_flipping_page_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_flipping_page_sizer->Add(btn_last_page, 0, wxALIGN_CENTER, 0);
    m_flipping_page_sizer->Add(st_page_number, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(btn_next_page, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(m_page_num_input, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(20));
    m_flipping_page_sizer->Add(m_page_num_enter, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_page_sizer->Add(m_flipping_page_sizer, 0, wxALIGN_CENTER_HORIZONTAL, FromDIP(5));
    m_flipping_panel->SetSizer(m_page_sizer);
    m_flipping_panel->Layout();
    m_main_sizer->Add(m_flipping_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    m_ctrl_btn_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_ctrl_btn_panel->SetBackgroundColour(cprint_panel_bg);
    m_ctrl_btn_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_ctrl_btn_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_pause_all = new Button(m_ctrl_btn_panel, _L("Pause"));
    btn_pause_all->SetBackgroundColor(ctrl_bg);
    btn_pause_all->SetCornerRadius(FromDIP(5));
    btn_continue_all = new Button(m_ctrl_btn_panel, _L("Resume"));
    btn_continue_all->SetBackgroundColor(ctrl_bg);
    btn_continue_all->SetCornerRadius(FromDIP(5));
    btn_stop_all = new Button(m_ctrl_btn_panel, _L("Stop"));
    btn_stop_all->SetBackgroundColor(ctrl_bg);
    btn_stop_all->SetCornerRadius(FromDIP(5));
    m_sel_text = new wxStaticText(m_ctrl_btn_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_sel_text->SetForegroundColour(cprint_muted);

    btn_pause_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::pause_all, this);
    btn_continue_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::resume_all, this);
    btn_stop_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::stop_all, this);

    m_btn_sizer->Add(m_sel_text, 0, wxLEFT, FromDIP(15));
    m_btn_sizer->Add(btn_pause_all, 0, wxLEFT, FromDIP(10));
    m_btn_sizer->Add(btn_continue_all, 0, wxLEFT, FromDIP(10));
    m_btn_sizer->Add(btn_stop_all, 0, wxLEFT, FromDIP(10));
    m_ctrl_btn_panel->SetSizer(m_btn_sizer);
    m_ctrl_btn_panel->Layout();

    if (m_media_presentation != MediaPresentation::ModelOnly)
        m_main_sizer->AddSpacer(FromDIP(10));
    m_main_sizer->Add(m_ctrl_btn_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_panel->SetSizer(m_main_sizer);
    m_main_panel->Layout();

    page_sizer = new wxBoxSizer(wxVERTICAL);
    const int page_pad = FromDIP(10);
    if (m_media_presentation == MediaPresentation::ModelOnly)
        page_sizer->Add(m_main_panel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page_pad);
    else
        page_sizer->Add(m_main_panel, 1, wxALL | wxEXPAND, page_pad);
    Bind(wxEVT_TIMER, &CloudTaskManagerPage::on_timer, this);

    wxGetApp().UpdateDarkUIWin(this);

    SetSizer(page_sizer);
    m_offline_overlay = new DeviceDashboard::PrinterOfflineOverlay(this);
    update_media_mode_tabs();
    Layout();
    Fit();
}

CloudTaskManagerPage::~CloudTaskManagerPage()
{
    m_model_status_lifetime.reset();
    if (m_flipping_timer)
        m_flipping_timer->Stop();
    delete m_flipping_timer;
}

void CloudTaskManagerPage::set_media_mode(bool timelapse)
{
    if (m_media_presentation != MediaPresentation::Combined)
        timelapse = m_media_presentation == MediaPresentation::TimelapseOnly;

    if (m_media_timelapse_mode == timelapse)
        return;

    m_media_timelapse_mode = timelapse;
    update_media_mode_tabs();

    if (!m_media_timelapse_mode && m_allow_moonraker_fetch)
        refresh_moonraker_model_status();
}

void CloudTaskManagerPage::set_media_presentation(MediaPresentation presentation)
{
    m_media_presentation = presentation;

    if (m_timelapse_tab)
        m_timelapse_tab->Show(m_media_presentation == MediaPresentation::Combined);
    if (m_model_tab)
        m_model_tab->Show(m_media_presentation == MediaPresentation::Combined);

    if (m_media_presentation == MediaPresentation::TimelapseOnly)
        set_media_mode(true);
    else if (m_media_presentation == MediaPresentation::ModelOnly)
        set_media_mode(false);
    else
        update_media_mode_tabs();

    if (m_media_mode_panel)
        m_media_mode_panel->Layout();
    Layout();
}

void CloudTaskManagerPage::refresh_moonraker_model_status()
{
    if (!m_allow_moonraker_fetch)
        return;
    if (!m_model_status_text)
        return;

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
    m_last_model_probe_machine_id = obj ? obj->get_dev_id() : std::string();
    m_model_probe_in_flight = true;
    m_last_model_probe_ok = false;
    m_last_model_probe_started_ms = wxGetUTCTimeMillis().GetValue();

    m_model_status_text->SetLabel(_L("Loading printer models..."));
    m_model_status_text->Show(!m_media_timelapse_mode);
    if (m_model_file_grid)
        m_model_file_grid->Show(!m_media_timelapse_mode);
    Layout();

    std::weak_ptr<int> lifetime = m_model_status_lifetime;
    const std::string probed_machine_id = m_last_model_probe_machine_id;
    probe_moonraker_model_files(
        [this, lifetime, probed_machine_id](MoonrakerModelProbeResult result) {
            wxGetApp().CallAfter([this, lifetime, probed_machine_id, result = std::move(result)]() {
                if (lifetime.expired() || !m_model_status_text)
                    return;
                if (m_last_model_probe_machine_id == probed_machine_id)
                    m_model_probe_in_flight = false;

                if (result.ok) {
                    m_last_model_probe_ok = m_last_model_probe_machine_id == probed_machine_id;
                    m_model_status_text->Hide();
                    render_moonraker_model_files(result.files);
                } else if (result.status_code != 0) {
                    m_last_model_probe_ok = false;
                    m_model_status_text->SetLabel(wxString::Format(_L("Printer model request failed. HTTP %u"), result.status_code));
                    m_model_status_text->Show(!m_media_timelapse_mode);
                } else if (!result.error_message.empty()) {
                    m_last_model_probe_ok = false;
                    m_model_status_text->SetLabel(wxString::Format(_L("Printer models could not be loaded. (%s)"),
                                                                   wxString::FromUTF8(result.error_message)));
                    m_model_status_text->Show(!m_media_timelapse_mode);
                } else {
                    m_last_model_probe_ok = false;
                    m_model_status_text->SetLabel(_L("Printer models could not be loaded."));
                    m_model_status_text->Show(!m_media_timelapse_mode);
                }
                Layout();
            });
        },
        [this, lifetime](MoonrakerModelFileView file) {
            wxGetApp().CallAfter([this, lifetime, file = std::move(file)]() {
                if (lifetime.expired())
                    return;
                apply_model_file_metadata(file);
            });
        });
}

int CloudTaskManagerPage::model_grid_column_count() const
{
    if (m_model_file_grid == nullptr)
        return 1;
    const int gap = FromDIP(kModelGridGapDip);
    const int card_w = FromDIP(kModelCardWDip);
    const int width = m_model_file_grid->GetClientSize().GetWidth();
    if (width <= 0)
        return 1;
    return std::max(1, (width + gap) / (card_w + gap));
}

void CloudTaskManagerPage::sync_model_grid_overlay(bool reveal)
{
    if (auto* bar = static_cast<ModelGridOverlayScroll*>(m_model_grid_scroll))
        bar->sync(reveal);
}

void CloudTaskManagerPage::relayout_model_file_grid()
{
    if (m_model_file_grid == nullptr || m_model_file_grid_sizer == nullptr)
        return;
    const int cols = model_grid_column_count();
    if (m_model_file_grid_sizer->GetCols() == cols)
        return;
    m_model_file_grid_sizer->SetCols(cols);
    m_model_file_grid_sizer->Layout();
    m_model_file_grid->FitInside();
    sync_model_grid_overlay(false);
    load_visible_model_thumbnails();
}

void CloudTaskManagerPage::load_visible_model_thumbnails()
{
    if (m_model_file_grid == nullptr)
        return;

    int xu = 1;
    int yu = 1;
    m_model_file_grid->GetScrollPixelsPerUnit(&xu, &yu);
    int vx = 0;
    int vy = 0;
    m_model_file_grid->GetViewStart(&vx, &vy);
    const wxSize client = m_model_file_grid->GetClientSize();
    const int prefetch = FromDIP(kModelCardHDip);
    const wxRect visible(vx * std::max(1, xu), vy * std::max(1, yu),
                         std::max(1, client.GetWidth()),
                         std::max(1, client.GetHeight()) + prefetch);

    for (wxWindow* child : m_model_file_grid->GetChildren()) {
        auto* card = dynamic_cast<MoonrakerModelFileCard*>(child);
        if (card == nullptr || card->is_retiring())
            continue;
        const wxRect rect(card->GetPosition(), card->GetSize());
        if (!visible.Intersects(rect))
            continue;
        if (!card->has_thumbnail()) {
            const auto cached = m_model_thumbnail_cache.find(card->file_path());
            if (cached != m_model_thumbnail_cache.end())
                card->apply_thumbnail(cached->second);
            else
                card->request_thumbnail();
        }
    }
}

void CloudTaskManagerPage::apply_model_file_metadata(const MoonrakerModelFileView& file)
{
    if (m_model_file_grid == nullptr)
        return;
    auto* found = wxWindow::FindWindowByName(moonraker_model_card_name(file.path), m_model_file_grid);
    auto* card = dynamic_cast<MoonrakerModelFileCard*>(found);
    if (card == nullptr)
        return;
    card->apply_metadata(file);
    load_visible_model_thumbnails();
}

void CloudTaskManagerPage::render_moonraker_model_files(const std::vector<MoonrakerModelFileView>& files)
{
    if (!m_model_file_grid || !m_model_file_grid_sizer)
        return;

    m_model_file_grid->Freeze();

    std::map<std::string, MoonrakerModelFileCard*> existing;
    for (wxWindow* child : m_model_file_grid->GetChildren()) {
        auto* card = dynamic_cast<MoonrakerModelFileCard*>(child);
        if (card == nullptr || card->is_retiring())
            continue;
        existing[card->file_path()] = card;
    }
    m_model_file_grid_sizer->Clear(false);
    m_model_file_grid_sizer->SetCols(model_grid_column_count());

    for (const auto& file : files) {
        MoonrakerModelFileCard* card = nullptr;
        const auto existing_it = existing.find(file.path);
        if (existing_it != existing.end()) {
            card = existing_it->second;
            card->apply_metadata(file);
            existing.erase(existing_it);
        } else {
        auto* new_card = new MoonrakerModelFileCard(m_model_file_grid, file,
        [this](MoonrakerModelFileCard*, const MoonrakerModelFileView& clicked_file) {
            if (!m_model_status_text)
                return;

            Slic3r::DeviceManager* dev = wxGetApp().getDeviceManager();
            MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
            const std::string base_url = moonraker_base_url(obj);
            const std::string api_key = moonraker_api_key(obj);
            const wxString display_name = basename_from_moonraker_path(clicked_file.path);
            const std::string file_path = clicked_file.path;
            const wxString card_name = moonraker_model_card_name(file_path);
            std::weak_ptr<int> lifetime = m_model_status_lifetime;

            if (!confirm_delete_moonraker_model(this, display_name))
                return;

            m_model_status_text->SetLabel(wxString::Format(_L("Deleting: %s"), display_name));
            m_model_status_text->Show(!m_media_timelapse_mode);
            Layout();

            std::thread([this, lifetime, base_url, api_key, file_path, display_name, card_name]() {
                MoonrakerModelDeleteResult delete_result = delete_moonraker_model_file_sync(base_url, api_key, file_path);
                wxGetApp().CallAfter([this, lifetime, delete_result = std::move(delete_result), display_name, card_name]() {
                    if (lifetime.expired() || !m_model_status_text)
                        return;

                    if (delete_result.ok) {
                        if (m_model_file_grid != nullptr && m_model_file_grid_sizer != nullptr) {
                            if (auto* deleted_card = wxWindow::FindWindowByName(card_name, m_model_file_grid)) {
                                m_model_file_grid_sizer->Detach(deleted_card);
                                if (auto* model_card = dynamic_cast<MoonrakerModelFileCard*>(deleted_card))
                                    model_card->retire();
                                else
                                    deleted_card->Destroy();
                                m_model_file_grid_sizer->Layout();
                                m_model_file_grid->FitInside();
                                sync_model_grid_overlay(false);
                            }
                        }
                        m_model_status_text->SetLabel(wxString::Format(_L("Deleted: %s"), display_name));
                        m_model_status_text->Show(!m_media_timelapse_mode);
                        Layout();
                    } else {
                        const wxString reason = delete_result.status_code != 0
                            ? wxString::Format("HTTP %u", delete_result.status_code)
                            : wxString::FromUTF8(delete_result.error_message);
                        m_model_status_text->SetLabel(wxString::Format(_L("Delete failed: %s (%s)"), display_name, reason));
                        m_model_status_text->Show(!m_media_timelapse_mode);
                        Layout();
                    }
                });
            }).detach();
        },
        [this](MoonrakerModelFileCard* card, const MoonrakerModelFileView& clicked_file) {
            Slic3r::DeviceManager* dev = wxGetApp().getDeviceManager();
            MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
            const wxString display_name = basename_from_moonraker_path(clicked_file.path);
            const std::string base_url = moonraker_base_url(obj);
            const std::string api_key = moonraker_api_key(obj);

            PrinterStoragePrintRequest request;
            request.file_path = clicked_file.path;
            request.machine_id = obj ? obj->get_dev_id() : std::string();
            request.display_name = display_name;
            request.time_text = format_moonraker_duration(clicked_file.estimated_time_seconds);
            request.weight_text = format_moonraker_filament_weight(clicked_file.filament_weight_grams);
            if (obj != nullptr) {
                wxString name = wxString::FromUTF8(obj->get_dev_name());
                wxString ip = wxString::FromUTF8(obj->get_dev_ip());
                const int colon = ip.Find(':');
                if (colon != wxNOT_FOUND && ip.Mid(colon + 1).Find(':') == wxNOT_FOUND)
                    ip = ip.Left(colon);
                request.printer_label = (!ip.empty() && name != ip)
                    ? wxString::Format("%s (%s)", name, ip)
                    : (name.empty() ? ip : name);
            }
            if (card != nullptr)
                request.thumbnail = card->thumbnail();

            wxWindow* dialog_parent = wxGetApp().mainframe != nullptr
                ? static_cast<wxWindow*>(wxGetApp().mainframe)
                : static_cast<wxWindow*>(this);
            auto* fetch_dlg = new ProgressDialog(_L("Print"), _L("Fetching model information..."), 100,
                                                 dialog_parent, wxPD_APP_MODAL | wxPD_NO_PROGRESS);
            fetch_dlg->Update(0, _L("Fetching model information..."));

            std::weak_ptr<int> lifetime = m_model_status_lifetime;
            std::thread([this, lifetime, base_url, api_key, request = std::move(request), fetch_dlg]() mutable {
                const nlohmann::json parsed = fetch_moonraker_file_metadata_json(base_url, api_key, request.file_path);
                apply_moonraker_metadata_to_request(parsed, base_url, api_key, request);
                wxGetApp().CallAfter([this, lifetime, request = std::move(request), fetch_dlg]() mutable {
                    if (fetch_dlg != nullptr)
                        fetch_dlg->Destroy();
                    if (lifetime.expired())
                        return;

                    wxWindow* start_parent = wxGetApp().mainframe != nullptr
                        ? static_cast<wxWindow*>(wxGetApp().mainframe)
                        : static_cast<wxWindow*>(this);
                    StartPrintDialog dlg(start_parent);
                    dlg.prepare_from_storage(std::move(request));
                    dlg.ShowModal();
                });
            }).detach();
        });
            card = new_card;
            card->set_on_thumbnail_loaded([this](const std::string& path, const wxImage& image) {
                if (image.IsOk())
                    m_model_thumbnail_cache[path] = image.Copy();
            });
            const auto cached = m_model_thumbnail_cache.find(file.path);
            if (cached != m_model_thumbnail_cache.end())
                card->apply_thumbnail(cached->second);
        }
        m_model_file_grid_sizer->Add(card, 0, wxFIXED_MINSIZE, 0);
    }

    for (auto& leftover : existing)
        leftover.second->retire();

    m_model_file_grid_sizer->Layout();
    m_model_file_grid->FitInside();
    m_model_file_grid->Show(!m_media_timelapse_mode);
    if (m_model_grid_scroll)
        m_model_grid_scroll->Raise();
    sync_model_grid_overlay(false);
    m_model_file_grid->Thaw();
    Layout();
    CallAfter([this] { load_visible_model_thumbnails(); });
}

void CloudTaskManagerPage::update_media_mode_tabs()
{
    if (!m_timelapse_tab || !m_model_tab)
        return;

    const wxColour selected_bg("#F5F5F5");
    const wxColour selected_hover("#FFFFFF");
    const wxColour inactive_bg(*wxWHITE);
    const wxColour inactive_hover("#F7F7F7");
    const wxColour selected_text("#232527");
    const wxColour inactive_text("#767C84");

    StateColor active_bg(
        std::pair<wxColour, int>(selected_hover, StateColor::Pressed),
        std::pair<wxColour, int>(selected_hover, StateColor::Hovered),
        std::pair<wxColour, int>(selected_bg, StateColor::Normal)
    );
    StateColor normal_bg(
        std::pair<wxColour, int>(inactive_hover, StateColor::Pressed),
        std::pair<wxColour, int>(inactive_hover, StateColor::Hovered),
        std::pair<wxColour, int>(inactive_bg, StateColor::Normal)
    );

    m_timelapse_tab->SetBackgroundColor(m_media_timelapse_mode ? active_bg : normal_bg);
    m_timelapse_tab->SetTextColor(StateColor(std::pair<wxColour, int>(m_media_timelapse_mode ? selected_text : inactive_text, StateColor::Normal)));
    m_model_tab->SetBackgroundColor(m_media_timelapse_mode ? normal_bg : active_bg);
    m_model_tab->SetTextColor(StateColor(std::pair<wxColour, int>(m_media_timelapse_mode ? inactive_text : selected_text, StateColor::Normal)));

    if (!m_table_head_panel || !m_tip_text || !m_loading_text || !m_task_list || !m_flipping_panel || !m_ctrl_btn_panel) {
        m_timelapse_tab->Refresh();
        m_model_tab->Refresh();
        return;
    }

    const bool model_mode = !m_media_timelapse_mode;
    if (m_timelapse_panel)
        m_timelapse_panel->Show(m_media_timelapse_mode);
    if (m_timelapse_top_actions)
        m_timelapse_top_actions->Show(m_media_timelapse_mode);
    if (m_table_head_panel)
        m_table_head_panel->Show(false);
    if (m_tip_text)
        m_tip_text->Show(false);
    if (m_loading_text)
        m_loading_text->Show(model_mode && m_loading_text->IsShown());
    if (m_model_status_text && !model_mode)
        m_model_status_text->Hide();
    if (m_model_file_grid)
        m_model_file_grid->Show(model_mode);
    sync_model_grid_overlay(false);
    if (m_task_list)
        m_task_list->Show(false);
        m_flipping_panel->Show(false);
    if (m_ctrl_btn_panel)
        m_ctrl_btn_panel->Show(false);

    m_timelapse_tab->Refresh();
    m_model_tab->Refresh();
    Layout();
    Refresh();
}

void CloudTaskManagerPage::set_timelapse_filter(int filter)
{
    if (m_timelapse_filter == filter)
        return;

    m_timelapse_filter = filter;
    update_timelapse_filter_tabs();
}

void CloudTaskManagerPage::update_timelapse_filter_tabs()
{
    if (!m_timelapse_all_files || !m_timelapse_year || !m_timelapse_month)
        return;

    const wxColour active_bg("#F5F5F5");
    const wxColour active_hover("#EEEEEE");
    const wxColour inactive_bg(*wxWHITE);
    const wxColour inactive_hover("#F7F7F7");
    StateColor active(
        std::pair<wxColour, int>(active_hover, StateColor::Pressed),
        std::pair<wxColour, int>(active_hover, StateColor::Hovered),
        std::pair<wxColour, int>(active_bg, StateColor::Normal)
    );
    StateColor inactive(
        std::pair<wxColour, int>(inactive_hover, StateColor::Pressed),
        std::pair<wxColour, int>(inactive_hover, StateColor::Hovered),
        std::pair<wxColour, int>(inactive_bg, StateColor::Normal)
    );
    StateColor active_text = StateColor(std::pair<wxColour, int>(wxColour("#232527"), StateColor::Normal));
    StateColor inactive_text = StateColor(std::pair<wxColour, int>(wxColour("#767C84"), StateColor::Normal));

    auto apply = [&](Button* btn, bool selected) {
        btn->SetBackgroundColor(selected ? active : inactive);
        btn->SetTextColor(selected ? active_text : inactive_text);
        btn->Refresh();
    };

    apply(m_timelapse_all_files, m_timelapse_filter == 0);
    apply(m_timelapse_year, m_timelapse_filter == 1);
    apply(m_timelapse_month, m_timelapse_filter == 2);

    if (m_timelapse_date_range) {
        if (m_timelapse_filter == 1)
            m_timelapse_date_range->SetLabel(_L("2026"));
        else if (m_timelapse_filter == 2)
            m_timelapse_date_range->SetLabel(_L("July 2026"));
        else
            m_timelapse_date_range->SetLabel(_L("2026-07-13 - 2026-04-29"));
    }

    Layout();
    Refresh();
}

void CloudTaskManagerPage::select_all_timelapse_cards()
{
    if (!m_timelapse_grid)
        return;

    for (wxWindowList::compatibility_iterator node = m_timelapse_grid->GetChildren().GetFirst(); node; node = node->GetNext()) {
        wxWindow* child = node->GetData();
        if (auto* card = dynamic_cast<TimelapsePreviewCard*>(child))
            card->set_selected(true);
    }
}


void CloudTaskManagerPage::refresh_user_device(bool clear)
{
    m_sizer_task_list->Clear(false);

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        m_flipping_panel->Show(false);
        m_ctrl_btn_panel->Show(false);
        return;
    }

    if (clear) return;

    std::vector<MultiTaskItem*> task_temps;
    std::vector<std::string> subscribe_list;

    auto all_machine = dev->get_my_cloud_machine_list();
    auto user_machine = std::map<std::string, MachineObject*>();

    //selected machine
    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        auto dev_id = wxGetApp().app_config->get("multi_devices", std::to_string(i));

        if (all_machine.count(dev_id) > 0) {
            user_machine[dev_id] = all_machine[dev_id];
        }
    }

    auto task_manager = wxGetApp().getTaskManager();
    if (task_manager) {
        auto m_task_obj_list = task_manager->get_task_list(m_current_page, m_count_page_item, m_total_count);

        for (auto it = m_task_obj_list.begin(); it != m_task_obj_list.end(); it++) {

            TaskStateInfo task_state_info = it->second;
            MachineObject* machine_obj  = nullptr;

            if (user_machine.count(task_state_info.params().dev_id)) {
                machine_obj = user_machine[task_state_info.params().dev_id];
            }

            MultiTaskItem* mtitem = new MultiTaskItem(m_task_list, machine_obj, 1);
            //mtitem->task_obj = task_state_info;
            mtitem->m_job_id = task_state_info.get_job_id();
            mtitem->m_project_name = wxString::FromUTF8(task_state_info.get_task_name());
            mtitem->m_dev_name = wxString::FromUTF8(task_state_info.get_device_name());
            mtitem->m_dev_id = task_state_info.params().dev_id;

            mtitem->m_send_time = utc_time_to_date(task_state_info.start_time);
            const std::string date_source = !task_state_info.start_time.empty() ? task_state_info.start_time : task_state_info.end_time;
            mtitem->set_history_info(task_state_info,
                                     date_source.empty() ? wxString() : wxString::FromUTF8(utc_time_to_date(date_source).c_str()),
                                     history_duration_text(task_state_info.start_time, task_state_info.end_time),
                                     history_status_text(task_state_info.state()));

            if (task_state_info.state() == TS_PRINTING) {
                mtitem->state_cloud_task = 0;
            }
            else if (task_state_info.state() == TS_PRINT_SUCCESS) {
                mtitem->state_cloud_task = 1;
            }
            else if (task_state_info.state() == TS_PRINT_FAILED) {
                mtitem->state_cloud_task = 2;
            }

            if (m_task_items.find(it->first) != m_task_items.end()) {
                MultiTaskItem* item = m_task_items[it->first];
                if (item->state_selected == 1 && mtitem->state_cloud_task == 0)
                    mtitem->state_selected = item->state_selected;
                item->Destroy();
            }

            m_task_items[it->first] = mtitem;
            mtitem->update_info();
            task_temps.push_back(mtitem);

            auto find_it = std::find(subscribe_list.begin(), subscribe_list.end(), mtitem->m_dev_id);
            if (find_it == subscribe_list.end()) {
                subscribe_list.push_back(mtitem->m_dev_id);
            }
        }

        dev->subscribe_device_list(subscribe_list);

        if (m_sort.rule == SortItem::SortRule::SR_None) {
            this->device_send_time = true;
            m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        }
        std::sort(task_temps.begin(), task_temps.end(), m_sort.get_call_back());

        for (const auto& item : task_temps)
            m_sizer_task_list->Add(item, 0, wxALL | wxEXPAND, 0);

        // maintenance
        auto it = m_task_items.begin();
        while (it != m_task_items.end()) {
            if (m_task_obj_list.find(it->first) != m_task_obj_list.end()) {
                ++it;
            }
            else {
                it->second->Destroy();
                it = m_task_items.erase(it);
            }
        }
        m_sizer_task_list->Layout();
        int num = m_task_items.size() > 10 ? 10 : m_task_items.size();
        m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), num * FromDIP(CLOUD_HISTORY_ITEM_HEIGHT)));
        m_task_list->Layout();
    }

    update_page_number();

    const bool model_mode = !m_media_timelapse_mode;
    m_table_head_panel->Show(false);
    m_tip_text->Show(false);
    if (m_model_status_text && !model_mode)
        m_model_status_text->Hide();
    if (m_model_file_grid)
        m_model_file_grid->Show(model_mode);
    sync_model_grid_overlay(false);
    m_task_list->Show(false);
    m_timelapse_panel->Show(m_media_timelapse_mode);
    m_flipping_panel->Show(false);
    m_ctrl_btn_panel->Show(false);
    Layout();
}

std::string CloudTaskManagerPage::utc_time_to_date(std::string utc_time)
{
    /*std::tm timeInfo = {};
    std::istringstream iss(utc_time);
    iss >> std::get_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(std::mktime(&timeInfo));
    std::time_t localTime = std::chrono::system_clock::to_time_t(tp);
    std::tm* localTimeInfo = std::localtime(&localTime);

    std::stringstream ss;
    ss << std::put_time(localTimeInfo, "%Y-%m-%d %H:%M:%S");
    return ss.str();*/
    std::string send_time;


    std::tm timeInfo = {};
    std::istringstream iss(utc_time);
    iss >> std::get_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(std::mktime(&timeInfo));
    std::time_t utcTime = std::chrono::system_clock::to_time_t(tp);


    wxDateTime::TimeZone tz(wxDateTime::Local);
    long offset = tz.GetOffset();


    std::time_t localTime = utcTime + offset;

    std::tm* localTimeInfo = std::localtime(&localTime);
    std::stringstream ss;
    ss << std::put_time(localTimeInfo, "%Y-%m-%d %H:%M:%S");
    send_time =  ss.str();


    return send_time;
}


bool CloudTaskManagerPage::Show(bool show)
{
    if (show) {
        refresh_user_device();
        if (!m_media_timelapse_mode)
            ensure_media_models_for_selected_machine();
    }
    else {
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            dev->subscribe_device_list(std::vector<std::string>());
        }
    }

    return wxPanel::Show(show);
}

void CloudTaskManagerPage::reload_media_models()
{
    ensure_media_models_for_selected_machine();
}

void CloudTaskManagerPage::invalidate_media_cache_and_reload()
{
    m_model_thumbnail_cache.clear();
    m_last_model_probe_ok = false;
    m_last_model_probe_started_ms = 0;
    m_model_probe_in_flight = false;

    if (!IsShownOnScreen())
        return;

    if (m_media_timelapse_mode) {
        if (m_timelapse_panel != nullptr)
            m_timelapse_panel->Refresh();
        return;
    }

    refresh_moonraker_model_status();
}

void CloudTaskManagerPage::set_allow_moonraker_fetch(bool allow)
{
    const bool changed = m_allow_moonraker_fetch != allow;
    m_allow_moonraker_fetch = allow;
    if (!allow) {
        m_last_model_probe_ok = false;
        m_last_model_probe_started_ms = 0;
        return;
    }
    if (changed && IsShownOnScreen() && !m_media_timelapse_mode)
        ensure_media_models_for_selected_machine();
}

void CloudTaskManagerPage::set_offline_overlay_visible(bool visible, const wxString &printer_name)
{
    if (m_offline_overlay != nullptr)
        m_offline_overlay->set_visible(visible, printer_name);
}

void CloudTaskManagerPage::set_offline_retry_handler(std::function<void()> handler)
{
    if (m_offline_overlay != nullptr)
        m_offline_overlay->set_retry_handler(std::move(handler));
}

void CloudTaskManagerPage::ensure_media_models_for_selected_machine()
{
    if (!m_allow_moonraker_fetch || m_media_timelapse_mode)
        return;

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
    const std::string machine_id = obj ? obj->get_dev_id() : std::string();
    if (machine_id.empty())
        return;
    if (m_model_probe_in_flight)
        return;
    if (m_last_model_probe_ok && m_last_model_probe_machine_id == machine_id)
        return;

    const long long now_ms = wxGetUTCTimeMillis().GetValue();
    if (m_last_model_probe_started_ms > 0 && (now_ms - m_last_model_probe_started_ms) < 1500)
        return;

    refresh_moonraker_model_status();
}

void CloudTaskManagerPage::update_page()
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        it->second->sync_state();
        it->second->update_info();
    }
}

void CloudTaskManagerPage::update_page_number()
{
    double result = static_cast<double>(m_total_count) / m_count_page_item;
    m_total_page = std::ceil(result);

    wxString number = wxString(std::to_string(m_current_page + 1)) + " / " + wxString(std::to_string(m_total_page));
    st_page_number->SetLabel(number);
}

void CloudTaskManagerPage::start_timer()
{
    if (m_flipping_timer) {
        m_flipping_timer->Stop();
    }
    else {
        m_flipping_timer = new wxTimer();
    }

    m_flipping_timer->SetOwner(this);
    m_flipping_timer->Start(1000);
    wxPostEvent(this, wxTimerEvent(*m_flipping_timer));
}

void CloudTaskManagerPage::on_timer(wxTimerEvent& event)
{
    m_flipping_timer->Stop();
    enable_buttons(true);
    update_page_number();
}

void CloudTaskManagerPage::pause_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_pause->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onPause();
        }
    }
}

void CloudTaskManagerPage::resume_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_resume->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onResume();
        }
    }
}

void CloudTaskManagerPage::stop_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_stop->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onStop();
        }
    }
}

void CloudTaskManagerPage::enable_buttons(bool enable)
{
    btn_last_page->Enable(enable);
    btn_next_page->Enable(enable);
    btn_pause_all->Enable(enable);
    btn_continue_all->Enable(enable);
    btn_stop_all->Enable(enable);
}

void CloudTaskManagerPage::page_num_enter_evt()
{
    enable_buttons(false);
    start_timer();
    auto value = m_page_num_input->GetTextCtrl()->GetValue();
    long page_num = 0;
    if (value.ToLong(&page_num)) {
        if (page_num > m_total_page)
            m_current_page = m_total_page - 1;
        else if (page_num < 1)
            m_current_page = 0;
        else
            m_current_page = page_num - 1;
    }
    refresh_user_device();
    update_page_number();
    /*m_sizer_task_list->Clear(false);
    m_loading_text->Show(true);
    Layout();*/
}

void CloudTaskManagerPage::msw_rescale()
{
    btn_last_page->Rescale();
    btn_last_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->Rescale();
    btn_next_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_page_num_enter->Rescale();
    m_page_num_enter->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));

    m_select_checkbox->Rescale();
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->Rescale();
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->Rescale();
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->Rescale();
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->Rescale();
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->Rescale();
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->Rescale();
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    btn_pause_all->Rescale();
    btn_continue_all->Rescale();
    btn_stop_all->Rescale();

    for (auto it = m_task_items.begin(); it != m_task_items.end(); ++it) {
        it->second->Refresh();
    }

    Fit();
    Layout();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
