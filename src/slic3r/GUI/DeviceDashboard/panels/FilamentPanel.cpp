#include "FilamentPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../FilamentTrackPaint.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "libslic3r/Utils.hpp"
#ifdef __APPLE__
#include "../../../Utils/MacDarkMode.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

int d(wxWindow* win, int v) { return DeviceUiStyle::dip(win, v); }
int s(int v) { return DeviceUiStyle::scaled(v); }

constexpr int kHoverDarken = 18;

wxColour darken(const wxColour& colour, int amount)
{
    auto ch = [amount](int v) { return std::max(0, v - amount); };
    return wxColour(ch(colour.Red()), ch(colour.Green()), ch(colour.Blue()), colour.Alpha());
}

wxBitmap load_png_size(wxWindow* parent, const std::string& name, int dip_w, int dip_h)
{
    wxImage image;
    const wxString path = wxString::FromUTF8(Slic3r::var(name + ".png").c_str());
    const bool loaded = wxFileName::FileExists(path) && image.LoadFile(path, wxBITMAP_TYPE_PNG) && image.IsOk()
        && image.GetWidth() > 0 && image.GetHeight() > 0;
    if (!loaded)
        return wxBitmap();

    double scale = 1.0;
#ifdef __APPLE__
    scale = std::max(1.0, mac_max_scaling_factor());
#elif defined(__WXMSW__)
    scale = std::max(1.0, parent->GetDPIScaleFactor());
#endif

    const int logical_w = std::max(1, parent->FromDIP(dip_w));
    const int logical_h = std::max(1, parent->FromDIP(dip_h));
#ifdef __APPLE__
    const int dst_w = std::max(1, static_cast<int>(std::lround(logical_w * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::lround(logical_h * scale)));
#else
    const int dst_w = logical_w;
    const int dst_h = logical_h;
#endif
    image.Rescale(dst_w, dst_h, wxIMAGE_QUALITY_HIGH);

#ifdef __APPLE__
    return wxBitmap(image, -1, scale);
#else
    wxBitmap bmp(image);
#ifdef __WXMSW__
    bmp.SetScaleFactor(scale);
#endif
    return bmp;
#endif
}

wxBitmap tint_edit_icon(wxWindow* win, int dip, const wxColour& fg)
{
    double scale = 1.0;
#ifdef __APPLE__
    scale = std::max(1.0, mac_max_scaling_factor());
#elif defined(__WXMSW__)
    scale = std::max(1.0, win->GetDPIScaleFactor());
#endif

    auto make_tinted = [&](int raster_dip) -> wxImage {
        wxBitmap bitmap = create_scaled_bitmap("ams_editable", win, raster_dip);
        if (!bitmap.IsOk())
            return wxImage();
        wxImage image = bitmap.ConvertToImage();
        if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
            return wxImage();
        unsigned char* data = image.GetData();
        if (data == nullptr)
            return wxImage();
        const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
        const int pixels = image.GetWidth() * image.GetHeight();
        for (int i = 0; i < pixels; ++i) {
            if (alpha != nullptr && alpha[i] == 0)
                continue;
            const int offset = i * 3;
            data[offset + 0] = fg.Red();
            data[offset + 1] = fg.Green();
            data[offset + 2] = fg.Blue();
        }
        return image;
    };

    wxImage image = make_tinted(dip);
    if (!image.IsOk())
        return wxBitmap();

    // ConvertToImage can drop Retina backing. Never upscale that 1x raster (looks pixelated);
    // rasterize the SVG again at a larger DIP so 12 DIP still has 2x pixels.
    const int want_h = std::max(1, static_cast<int>(std::lround(win->FromDIP(dip) * scale)));
    if (image.GetHeight() + 1 < want_h) {
        const int hi_dip = std::max(dip + 1, static_cast<int>(std::lround(dip * scale)));
        wxImage hi = make_tinted(hi_dip);
        if (hi.IsOk())
            image = std::move(hi);
    }

    const double out_scale = (image.GetHeight() + 1 >= want_h) ? scale : 1.0;
#ifdef __APPLE__
    return wxBitmap(image, -1, out_scale);
#else
    wxBitmap tinted(image);
    if (out_scale > 1.01)
        tinted.SetScaleFactor(out_scale);
    return tinted;
#endif
}

} // namespace

bool is_valid_loaded_filament_colour(const wxColour& colour)
{
    return colour.IsOk();
}

bool is_empty_filament_material(const wxString& material)
{
    return material.IsEmpty()
        || material.CmpNoCase(wxString::FromUTF8("Empty")) == 0
        || material.CmpNoCase(wxString::FromUTF8("N/A")) == 0;
}

class FilamentToolMapView : public wxPanel
{
public:
    using ToolHandler = std::function<void(int)>;

    explicit FilamentToolMapView(wxWindow* parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(wxSize(d(this, 620), FromDIP(311)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(DeviceUiStyle::card_background());
        Bind(wxEVT_PAINT, &FilamentToolMapView::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &FilamentToolMapView::on_left_down, this);
        Bind(wxEVT_MOTION, &FilamentToolMapView::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            SetCursor(wxCursor(wxCURSOR_ARROW));
            if (m_hovered_tool != -1) {
                m_hovered_tool = -1;
                Refresh();
            }
            event.Skip();
        });

        m_tool_colors = {
            wxColour(58, 139, 222),
            wxColour(80, 84, 92),
            wxColour(110, 162, 82),
            wxColour(238, 151, 42)
        };
        m_tool_materials = {
            wxString::FromUTF8("PLA"),
            wxString::FromUTF8("Empty"),
            wxString::FromUTF8("PLA"),
            wxString::FromUTF8("PLA")
        };
        reload_icons();
    }

    void set_tool_handler(ToolHandler handler) { m_tool_handler = std::move(handler); }
    void set_configure_handler(ToolHandler handler) { m_configure_handler = std::move(handler); }
    wxPoint last_configure_anchor_screen() const { return m_last_configure_anchor; }

    void apply_state(const FilamentState& state)
    {
        bool changed = false;
        for (int i = 0; i < MaxDashboardTools; ++i) {
            const ToolState& loaded = state.tools[i];

            const bool has_loaded = is_valid_loaded_filament_colour(loaded.color);
            if (m_tool_loaded[i] != has_loaded) {
                m_tool_loaded[i] = has_loaded;
                changed = true;
            }

            wxColour tool_color = has_loaded
                ? readable_filament_track_colour(loaded.color, m_tool_colors[i])
                : wxColour(80, 84, 92);
            if (m_tool_colors[i] != tool_color) {
                m_tool_colors[i] = tool_color;
                changed = true;
            }

            wxString material = has_loaded && !is_empty_filament_material(loaded.material)
                ? loaded.material
                : wxString::FromUTF8("Empty");
            if (m_tool_materials[i] != material) {
                m_tool_materials[i] = material;
                changed = true;
            }
        }
        if (m_selected_tool != state.selected_tool) {
            m_selected_tool = state.selected_tool;
            changed = true;
        }
        if (changed)
            Refresh();
    }

private:
    bool is_tool_empty(int tool) const
    {
        return !m_tool_loaded[tool];
    }

    wxString tool_material_label(int tool) const
    {
        return is_tool_empty(tool) ? wxString::FromUTF8("Empty") : m_tool_materials[tool];
    }

    int hit_test_tool(const wxPoint& pos) const
    {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (m_card_hit_rects[i].Contains(pos))
                return i;
        }
        return -1;
    }

    int hit_test_track(const wxPoint& pos) const
    {
        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (m_track_hit_rects[i].Contains(pos))
                return i;
        }
        return -1;
    }

    void on_left_down(wxMouseEvent& event)
    {
        const wxPoint pos = event.GetPosition();
        const int track_tool = hit_test_track(pos);
        if (track_tool >= 0) {
            if (m_configure_handler) {
                m_last_configure_anchor = ClientToScreen(m_track_hit_rects[track_tool].GetPosition() +
                    wxPoint(m_track_hit_rects[track_tool].GetWidth() / 2, m_track_hit_rects[track_tool].GetHeight() / 2));
                m_configure_handler(track_tool);
            }
            return;
        }

        const int tool = hit_test_tool(pos);
        if (tool >= 0) {
            if (m_tool_handler)
                m_tool_handler(tool);
            return;
        }
        event.Skip();
    }

    void set_hovered_tool(int tool)
    {
        if (m_hovered_tool == tool)
            return;
        m_hovered_tool = tool;
        Refresh();
    }

    void on_motion(wxMouseEvent& event)
    {
        const wxPoint pos = event.GetPosition();
        const int track = hit_test_track(pos);
        const int tool = track >= 0 ? -1 : hit_test_tool(pos);
        SetCursor((track >= 0 || tool >= 0) ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
        set_hovered_tool(tool);
        event.Skip();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        const wxSize size = GetClientSize();
        const wxSize spool = filament_spool_bounds_size(this);
        const int track_w = spool.GetWidth();
        const int track_h = spool.GetHeight();
        const int row_gap = FromDIP(24);
        const int canvas_top = FromDIP(20);
        const int body_w = FromDIP(193);
        const int body_h = FromDIP(271);
        const int tracks_h = track_h + row_gap + track_h;
        const int content_h = std::max(tracks_h, body_h);
        const int canvas_w = d(this, 610);
        const int canvas_h = canvas_top + content_h + canvas_top;
        const int ox = std::max(0, (size.x - canvas_w) / 2);
        const int oy = std::max(0, (size.y - canvas_h) / 2);

        const int card_w = d(this, 66);
        const int card_h = d(this, 86);
        const int card_track_gap = d(this, 30);
        const int track_body_gap = 0;
        const int assembly_w = 2 * (card_w + card_track_gap + track_w) + body_w;
        const int layout_start = std::max(0, (canvas_w - assembly_w) / 2);

        const int card_left_x = ox + layout_start;
        const int track_left_x = card_left_x + card_w + card_track_gap;
        const int body_x = track_left_x + track_w + track_body_gap;
        const int track_right_x = body_x + body_w + track_body_gap;
        const int card_right_x = track_right_x + track_w + card_track_gap;

        const int track0_y = oy + canvas_top + (content_h - tracks_h) / 2;
        const int track1_y = track0_y + track_h + row_gap;
        const int card0_y = track0_y + (track_h - card_h) / 2;
        const int card1_y = track1_y + (track_h - card_h) / 2;
        const int body_y = oy + canvas_top + (content_h - body_h) / 2;

        if (m_center_bmp.IsOk())
            gc->DrawBitmap(m_center_bmp, body_x, body_y, body_w, body_h);

        const wxRect card0(card_left_x, card0_y, card_w, card_h);
        const wxRect track0(track_left_x, track0_y, track_w, track_h);
        const wxRect card1(card_left_x, card1_y, card_w, card_h);
        const wxRect track1(track_left_x, track1_y, track_w, track_h);

        const wxRect track2(track_right_x, track0_y, track_w, track_h);
        const wxRect card2(card_right_x, card0_y, card_w, card_h);
        const wxRect track3(track_right_x, track1_y, track_w, track_h);
        const wxRect card3(card_right_x, card1_y, card_w, card_h);

        const std::array<wxRect, MaxDashboardTools> cards{{card0, card1, card2, card3}};
        const std::array<wxRect, MaxDashboardTools> tracks{{track0, track1, track2, track3}};

        for (int i = 0; i < MaxDashboardTools; ++i) {
            if (is_tool_empty(i))
                draw_empty_track(gc.get(), tracks[i]);
            else
                draw_filled_track(gc.get(), tracks[i], m_tool_colors[i]);
            draw_tool_card(gc.get(), cards[i], i);
            m_card_hit_rects[i] = cards[i];
            m_track_hit_rects[i] = tracks[i];
        }
    }

    wxString tool_short_label(int tool_index) const
    {
        return wxString::Format("T%d", tool_index + 1);
    }

    void draw_tool_card(wxGraphicsContext* gc, const wxRect& rect, int tool)
    {
        const bool selected = tool == m_selected_tool;
        const bool hovered = tool == m_hovered_tool;
        wxColour bg = selected ? wxColour(232, 233, 234) : DeviceUiStyle::card_background();
        if (hovered)
            bg = darken(bg, kHoverDarken);
        const wxColour fg = DeviceUiStyle::text_primary();
        const wxColour sub = DeviceUiStyle::text_primary();

        gc->SetBrush(wxBrush(bg));
        gc->SetPen(wxPen(selected ? DeviceUiStyle::accent() : DeviceUiStyle::card_border(), d(this, 1)));
        gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, d(this, 10));

        draw_text(gc, tool_short_label(tool), wxRect(rect.x, rect.y + d(this, 14), rect.width, d(this, 32)), fg, 16, wxFONTWEIGHT_SEMIBOLD);
        draw_text(gc, tool_material_label(tool), wxRect(rect.x, rect.y + d(this, 48), rect.width, d(this, 22)), sub, 10, wxFONTWEIGHT_NORMAL);
    }

    void reload_icons()
    {
        m_edit_icon_on_dark = tint_edit_icon(this, kFilamentEditIconDip, *wxWHITE);
        m_edit_icon_on_light = tint_edit_icon(this, kFilamentEditIconDip, DeviceUiStyle::text_primary());
        m_add_icon_bmp  = create_scaled_bitmap("add_filament", this, s(18));
        m_center_bmp = load_png_size(this, "filament-center", 193, 271);
    }

    void draw_filled_track(wxGraphicsContext* gc, const wxRect& track, const wxColour& color)
    {
        const wxBitmap& icon = filament_track_fill_is_dark(color) ? m_edit_icon_on_dark : m_edit_icon_on_light;
        draw_filament_spool(gc, track, 0, color, true, FilamentTrackCenter::EditIcon, icon, this);
    }

    void draw_empty_track(wxGraphicsContext* gc, const wxRect& track)
    {
        const wxBitmap empty;
        draw_filament_spool(gc, track, 0, wxColour(), false, FilamentTrackCenter::PlusSign, empty, this);
    }

    void draw_text(wxGraphicsContext* gc, const wxString& text, const wxRect& rect, const wxColour& colour, int point_size, wxFontWeight weight)
    {
        wxFont font(wxFontInfo(std::max(1, point_size))
            .Family(wxFONTFAMILY_SWISS)
            .FaceName(wxString::FromUTF8("Bahnschrift"))
            .Weight(weight));
        gc->SetFont(font, colour);
        double tw = 0.0;
        double th = 0.0;
        gc->GetTextExtent(text, &tw, &th);
        gc->DrawText(text, rect.x + std::max(0.0, (rect.width - tw) / 2.0), rect.y + std::max(0.0, (rect.height - th) / 2.0));
    }

    std::array<wxColour, MaxDashboardTools> m_tool_colors;
    std::array<wxString, MaxDashboardTools> m_tool_materials;
    std::array<bool, MaxDashboardTools> m_tool_loaded{};
    std::array<wxRect, MaxDashboardTools> m_card_hit_rects;
    std::array<wxRect, MaxDashboardTools> m_track_hit_rects;
    wxBitmap m_edit_icon_on_dark;
    wxBitmap m_edit_icon_on_light;
    wxBitmap m_add_icon_bmp;
    wxBitmap m_center_bmp;
    int m_selected_tool{0};
    int m_hovered_tool{-1};
    wxPoint m_last_configure_anchor{wxDefaultPosition};
    ToolHandler m_tool_handler;
    ToolHandler m_configure_handler;
};

void style_action_button(Button* button)
{
    if (button == nullptr)
        return;
    const wxSize size(d(button, 80), d(button, 30));
    button->SetMinSize(size);
    button->SetMaxSize(size);
    button->SetSize(size);
    button->SetCornerRadius(d(button, 8));
    button->SetBorderWidth(0);
    StateColor bg(
        std::pair(wxColour(232, 232, 232), (int) StateColor::Disabled),
        std::pair(wxColour(224, 224, 224), (int) StateColor::Pressed),
        std::pair(wxColour(238, 238, 238), (int) StateColor::Hovered),
        std::pair(wxColour(245, 245, 245), (int) StateColor::Normal));
    bg.setTakeFocusedAsHovered(false);
    StateColor fg(
        std::pair(DeviceUiStyle::text_muted(), (int) StateColor::Disabled),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Pressed),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Hovered),
        std::pair(DeviceUiStyle::text_primary(), (int) StateColor::Normal));
    fg.setTakeFocusedAsHovered(false);
    button->SetBackgroundColor(bg);
    button->SetTextColor(fg);
    button->SetCanFocus(false);
}

FilamentPanel::FilamentPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Filament"));
    wxWindow* host = m_frame->content_parent();
    host->SetBackgroundColour(DeviceUiStyle::card_background());

    m_tool_map_view = new FilamentToolMapView(host);
    m_tool_map_view->set_tool_handler([this](int tool_index) {
        select_manage_tool(tool_index);
    });
    m_tool_map_view->set_configure_handler([this](int tool_index) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::ConfigureFilament;
        command.tool_index = tool_index;
        const wxPoint anchor = m_tool_map_view->last_configure_anchor_screen();
        command.screen_x = anchor.x;
        command.screen_y = anchor.y;
        dispatch(command);
    });

    m_load_button = new Button(host, wxString::FromUTF8("Load"));
    style_action_button(m_load_button);
    m_load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::LoadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    m_unload_button = new Button(host, wxString::FromUTF8("Unload"));
    style_action_button(m_unload_button);
    m_unload_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        DeviceCommand command;
        command.kind = DeviceCommandKind::UnloadFilament;
        command.tool_index = m_selected_tool_index;
        dispatch(command);
    });

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    actions->Add(m_load_button, 0, wxALIGN_CENTER_VERTICAL);
    actions->AddSpacer(d(this, 8));
    actions->Add(m_unload_button, 0, wxALIGN_CENTER_VERTICAL);

    auto* content = new wxBoxSizer(wxVERTICAL);
    content->Add(m_tool_map_view, 1, wxEXPAND);
    content->AddSpacer(d(this, 10));
    content->Add(actions, 0, wxALIGN_LEFT);
    m_frame->set_content(content);

    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);
}

void FilamentPanel::apply_state(const FilamentState& state)
{
    if (m_tool_map_view != nullptr)
        m_tool_map_view->apply_state(state);

    set_selected_tool(state.selected_tool);
    const bool selected_tool_loading = state.is_loading && state.loading_tool == m_selected_tool_index;
    const bool controls_enabled = state.can_load_unload && !state.is_loading;
    if (m_load_button != nullptr) {
        m_load_button->SetLabel(selected_tool_loading ? wxString::FromUTF8("Loading...") : wxString::FromUTF8("Load"));
        m_load_button->Enable(controls_enabled);
    }
    if (m_unload_button != nullptr)
        m_unload_button->Enable(controls_enabled);
}

void FilamentPanel::set_command_handler(CommandHandler handler)
{
    m_command_handler = std::move(handler);
}

void FilamentPanel::dispatch(DeviceCommand command) const
{
    if (m_command_handler)
        m_command_handler(command);
}

void FilamentPanel::select_manage_tool(int tool_index)
{
    DeviceCommand command;
    command.kind = DeviceCommandKind::SelectFilamentTool;
    command.tool_index = tool_index;
    dispatch(command);
    set_selected_tool(tool_index);
}

void FilamentPanel::set_selected_tool(int tool_index)
{
    if (tool_index < 0 || tool_index >= MaxDashboardTools)
        tool_index = 0;
    m_selected_tool_index = tool_index;
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
