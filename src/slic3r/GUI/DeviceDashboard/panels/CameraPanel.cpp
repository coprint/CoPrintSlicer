#include "CameraPanel.hpp"

#include "../DeviceCardFrame.hpp"
#include "../DeviceUiStyle.hpp"
#include "../../wxExtensions.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

namespace {

class CameraPlayButton final : public wxPanel
{
public:
    using ClickHandler = std::function<void()>;

    explicit CameraPlayButton(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(92), FromDIP(30)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        m_stop_icon = load_stop_icon();
        Bind(wxEVT_PAINT, &CameraPlayButton::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_click_handler)
                m_click_handler();
        });
    }

    void set_click_handler(ClickHandler handler) { m_click_handler = std::move(handler); }

    void set_playing(bool playing)
    {
        if (m_playing == playing)
            return;
        m_playing = playing;
        Refresh();
    }

private:
    wxBitmap load_stop_icon()
    {
        const char* names[] = { "print_control_stop", "media_stop" };
        for (const char* name : names) {
            wxBitmap bitmap = create_scaled_bitmap(name, this, 10);
            if (!bitmap.IsOk())
                continue;
            wxImage image = bitmap.ConvertToImage();
            if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
                continue;
            unsigned char* data = image.GetData();
            if (data == nullptr)
                continue;
            const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
            const int pixels = image.GetWidth() * image.GetHeight();
            for (int i = 0; i < pixels; ++i) {
                if (alpha != nullptr && alpha[i] == 0)
                    continue;
                const int offset = i * 3;
                data[offset + 0] = 255;
                data[offset + 1] = 255;
                data[offset + 2] = 255;
            }
            const double scale = bitmap.GetScaleFactor();
#ifdef __APPLE__
            return wxBitmap(image, -1, scale > 0.01 ? scale : 1.0);
#else
            wxBitmap tinted(image);
            if (scale > 0.01)
                tinted.SetScaleFactor(scale);
            return tinted;
#endif
        }
        return wxBitmap();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC raw_dc(this);
        raw_dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : DeviceUiStyle::page_background()));
        raw_dc.Clear();

        const wxSize size = GetClientSize();
        wxGCDC dc(raw_dc);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_playing ? DeviceUiStyle::danger() : DeviceUiStyle::accent()));
        dc.DrawRoundedRectangle(wxRect(0, 0, size.GetWidth(), size.GetHeight()), FromDIP(8));

        const wxString label = wxString::FromUTF8(m_playing ? "Stop" : "Play");
        wxCoord text_w = 0;
        wxCoord text_h = 0;
        dc.SetTextForeground(*wxWHITE);
        dc.GetTextExtent(label, &text_w, &text_h);

        const int gap = FromDIP(6);
        const int icon_w = FromDIP(9);
        const int icon_h = FromDIP(10);
        const int total_w = icon_w + gap + text_w;
        int x = (size.GetWidth() - total_w) / 2;
        const int icon_y = (size.GetHeight() - icon_h) / 2;

        if (m_playing && m_stop_icon.IsOk()) {
            const wxSize icon_size = m_stop_icon.GetScaledSize();
            const int draw_x = x + (icon_w - icon_size.GetWidth()) / 2;
            const int draw_y = (size.GetHeight() - icon_size.GetHeight()) / 2;
            dc.DrawBitmap(m_stop_icon, draw_x, draw_y, true);
        } else {
            wxGraphicsContext* gc = dc.GetGraphicsContext();
            if (gc != nullptr) {
                wxGraphicsPath shape = gc->CreatePath();
                if (m_playing) {
                    const double r = FromDIP(1);
                    shape.AddRoundedRectangle(x + 0.5, icon_y + 0.5, icon_w, icon_h, r);
                } else {
                    shape.MoveToPoint(x + 0.5, icon_y + 0.5);
                    shape.AddLineToPoint(x + 0.5, icon_y + icon_h - 0.5);
                    shape.AddLineToPoint(x + icon_w + 0.5, icon_y + icon_h / 2.0);
                    shape.CloseSubpath();
                }
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(*wxWHITE));
                gc->FillPath(shape);
            }
        }
        x += icon_w + gap;
        dc.DrawText(label, x, (size.GetHeight() - text_h) / 2);
    }

    ClickHandler m_click_handler;
    wxBitmap m_stop_icon;
    bool m_playing{false};
};

wxColour camera_header_background()
{
    return DeviceUiStyle::card_header_background();
}

wxColour camera_idle_background()
{
    return wxColour(18, 20, 24);
}

wxBitmap create_header_icon_bitmap(const std::string& bitmap_name, wxWindow* win, int dip_size)
{
    wxBitmap bitmap = create_scaled_bitmap(bitmap_name, win, dip_size);
    if (!bitmap.IsOk())
        return bitmap;

    wxImage image = bitmap.ConvertToImage();
    if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return bitmap;

    unsigned char* data = image.GetData();
    if (data == nullptr)
        return bitmap;

    const wxColour fg = DeviceUiStyle::text_primary();
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

    // Recolouring goes through wxImage, which drops the backing scale.
    // Rebuild with the original factor so Retina icons stay 18/16 DIP
    // instead of using the 2x pixel size as the widget size.
    const double scale = bitmap.GetScaleFactor();
#ifdef __APPLE__
    return wxBitmap(image, -1, scale > 0.01 ? scale : 1.0);
#else
    wxBitmap tinted(image);
    if (scale > 0.01)
        tinted.SetScaleFactor(scale);
    return tinted;
#endif
}

void pin_header_icon(wxStaticBitmap* icon, int dip_size)
{
    if (icon == nullptr)
        return;
    wxSize size = ScalableBitmap::GetBmpSize(icon->GetBitmap());
    if (size.x <= 0 || size.y <= 0) {
        const int side = icon->FromDIP(dip_size);
        size = wxSize(side, side);
    }
    icon->SetMinSize(size);
    icon->SetMaxSize(size);
}

wxImage try_load_image_file(const boost::filesystem::path& path, wxBitmapType type)
{
    const wxString wpath = wxString::FromUTF8(path.string().c_str());
    if (!wxFileName::FileExists(wpath))
        return wxImage();

    wxImage image;
    // Try explicit type first, then auto-detect. Some wx/libpng builds reject AI PNGs.
    if ((!image.LoadFile(wpath, type) || !image.IsOk()) &&
        (!image.LoadFile(wpath, wxBITMAP_TYPE_ANY) || !image.IsOk())) {
        return wxImage();
    }
    if (!image.IsOk() || image.GetWidth() < 8 || image.GetHeight() < 8)
        return wxImage();
    return image;
}

wxImage load_camera_idle_source_image(wxWindow* /*win*/)
{
    const boost::filesystem::path images_dir =
        (boost::filesystem::path(resources_dir()) / "images").make_preferred();

    const boost::filesystem::path candidates[] = {
        images_dir / "camera_idle_placeholder.jpg",
        images_dir / "camera_idle_placeholder.png",
    };
    const wxBitmapType types[] = {wxBITMAP_TYPE_JPEG, wxBITMAP_TYPE_PNG};

    for (size_t i = 0; i < 2; ++i) {
        wxImage image = try_load_image_file(candidates[i], types[i]);
        if (image.IsOk()) {
            BOOST_LOG_TRIVIAL(warning) << "CameraPanel: loaded idle placeholder "
                                       << candidates[i].string() << " "
                                       << image.GetWidth() << "x" << image.GetHeight();
            return image;
        }
        BOOST_LOG_TRIVIAL(warning) << "CameraPanel: could not load " << candidates[i].string();
    }

    // Do NOT fall back to create_scaled_bitmap(): on miss it returns a valid empty bitmap.
    BOOST_LOG_TRIVIAL(error) << "CameraPanel: failed to load camera_idle_placeholder from "
                             << images_dir.string();
    return wxImage();
}

class CameraIdlePlaceholder final : public wxPanel
{
public:
    explicit CameraIdlePlaceholder(wxWindow* parent, wxImage image)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_image(std::move(image))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(camera_idle_background());
        Bind(wxEVT_PAINT, &CameraIdlePlaceholder::on_paint, this);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
            evt.Skip();
            Refresh(false);
        });
    }

    void set_unavailable_label(wxStaticText* label) { m_unavailable = label; }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(camera_idle_background()));
        dc.Clear();
        if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
            return;

        if (m_image.IsOk()) {
            const double sx = static_cast<double>(size.GetWidth()) / m_image.GetWidth();
            const double sy = static_cast<double>(size.GetHeight()) / m_image.GetHeight();
            const double scale = std::max(sx, sy);
            const int scaled_w = std::max(1, static_cast<int>(std::lround(m_image.GetWidth() * scale)));
            const int scaled_h = std::max(1, static_cast<int>(std::lround(m_image.GetHeight() * scale)));
            wxImage scaled = m_image.Scale(scaled_w, scaled_h, wxIMAGE_QUALITY_BILINEAR);
            if (scaled.IsOk()) {
                const int x = (size.GetWidth() - scaled_w) / 2;
                const int y = (size.GetHeight() - scaled_h) / 2;
                dc.DrawBitmap(wxBitmap(scaled), x, y, false);
            }
        }

        if (m_unavailable != nullptr && m_unavailable->IsShown()) {
            // Label is a child window; nothing to draw here.
        }
    }

    wxImage m_image;
    wxStaticText* m_unavailable{nullptr};
};

} // namespace

wxPanel* CameraPanel::webview_host() const
{
    return m_stream_host;
}

CameraPanel::CameraPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(DeviceUiStyle::page_background());
    SetMinSize(wxSize(FromDIP(460), FromDIP(320)));

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Live Camera"), -1, -1, 0, 0, 0);
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::card_background());
    auto* content_sizer = new wxBoxSizer(wxVERTICAL);

    auto* header_actions = new wxPanel(m_frame, wxID_ANY);
    header_actions->SetBackgroundColour(camera_header_background());
    auto* header_actions_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_timelapse_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_header_icon_bitmap("camera_timelapse_white", header_actions, 18));
    pin_header_icon(m_timelapse_btn, 18);
    m_timelapse_btn->SetBackgroundColour(camera_header_background());
    m_timelapse_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_timelapse_btn->SetToolTip(wxString::FromUTF8("Timelapse"));
    m_timelapse_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_timelapse_handler) m_timelapse_handler();
    });
    header_actions_sizer->Add(m_timelapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

    m_refresh_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_header_icon_bitmap("camera_refresh_white", header_actions, 16));
    pin_header_icon(m_refresh_btn, 16);
    m_refresh_btn->SetBackgroundColour(camera_header_background());
    m_refresh_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_refresh_btn->SetToolTip(wxString::FromUTF8("Refresh"));
    m_refresh_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_refresh_handler) m_refresh_handler();
    });
    header_actions_sizer->Add(m_refresh_btn, 0, wxALIGN_CENTER_VERTICAL);
    header_actions->SetSizer(header_actions_sizer);
    m_frame->set_header_action(header_actions);

    m_viewport = new wxPanel(m_frame->content_parent(), wxID_ANY);
    m_viewport->SetBackgroundColour(camera_idle_background());
    const int preview_min_w = FromDIP(420);
    const int preview_min_h = std::max(1, static_cast<int>(std::lround(preview_min_w * 9.0 / 16.0)));
    m_viewport->SetMinSize(wxSize(preview_min_w, preview_min_h));
    // Absolute stacking: idle placeholder and stream host share the same rect.
    m_viewport->SetSizer(nullptr);

    m_idle_source_image = load_camera_idle_source_image(this);
    auto* idle = new CameraIdlePlaceholder(m_viewport, m_idle_source_image);
    m_idle_placeholder = idle;
    m_empty_state = new wxStaticText(m_idle_placeholder, wxID_ANY,
        wxString::FromUTF8("Camera unavailable"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    m_empty_state->SetForegroundColour(DeviceUiStyle::text_muted());
    m_empty_state->Hide();
    idle->set_unavailable_label(m_empty_state);
    {
        auto* idle_overlay = new wxBoxSizer(wxVERTICAL);
        idle_overlay->AddStretchSpacer(1);
        idle_overlay->Add(m_empty_state, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(16));
        m_idle_placeholder->SetSizer(idle_overlay);
    }

    m_stream_host = new wxPanel(m_viewport, wxID_ANY);
    m_stream_host->SetBackgroundColour(*wxBLACK);
    m_stream_host->SetMinSize(wxSize(preview_min_w, preview_min_h));
    m_stream_host->Hide();

    m_viewport->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        layout_viewport_layers();
    });

    auto* control_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* play_btn = new CameraPlayButton(m_frame->content_parent());
    play_btn->set_click_handler([this]() {
        if (m_play_handler)
            m_play_handler();
    });
    m_play_btn = play_btn;
    m_status_label = new wxStaticText(m_frame->content_parent(), wxID_ANY, wxEmptyString);
    m_status_label->SetForegroundColour(DeviceUiStyle::text_muted());
    m_status_label->SetBackgroundColour(DeviceUiStyle::card_background());
    m_status_label->Hide();
    control_sizer->Add(m_play_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
    control_sizer->AddStretchSpacer(1);
    control_sizer->Add(m_status_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

    content_sizer->Add(m_viewport, 1, wxEXPAND);
    content_sizer->Add(control_sizer, 0, wxEXPAND);
    m_frame->set_content(content_sizer);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);

    update_from_load_state();
    CallAfter([this]() { layout_viewport_layers(); });
}

void CameraPanel::layout_viewport_layers()
{
    if (m_viewport == nullptr)
        return;
    const wxSize size = m_viewport->GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;
    if (m_idle_placeholder != nullptr) {
        m_idle_placeholder->SetSize(0, 0, size.GetWidth(), size.GetHeight());
        if (m_idle_placeholder->IsShown()) {
            m_idle_placeholder->Raise();
            m_idle_placeholder->Refresh(false);
        }
    }
    if (m_stream_host != nullptr) {
        m_stream_host->SetSize(0, 0, size.GetWidth(), size.GetHeight());
        if (m_stream_host->IsShown())
            m_stream_host->Raise();
    }
}

int CameraPanel::chrome_height() const
{
    const int fallback = FromDIP(98);
    if (m_frame == nullptr || m_frame->GetSizer() == nullptr || m_viewport == nullptr)
        return fallback;

    const int viewport_h = std::max(1, m_viewport->GetMinSize().GetHeight());
    const int frame_h = m_frame->GetSizer()->CalcMin().GetHeight();
    if (frame_h > viewport_h)
        return frame_h - viewport_h;
    return fallback;
}

void CameraPanel::fit_preview(int max_width, int max_height)
{
    if (m_fitting_preview || m_viewport == nullptr || max_width <= 0 || max_height <= 0)
        return;

    const int min_w = FromDIP(240);
    const int min_h = FromDIP(135);
    const int chrome = chrome_height();
    int preview_w = std::max(min_w, max_width);
    if (m_frame != nullptr && m_frame->content_parent() != nullptr) {
        const int content_w = m_frame->content_parent()->GetClientSize().GetWidth();
        if (content_w > 0)
            preview_w = std::max(min_w, content_w);
    }

    const int max_preview_h = std::max(min_h, max_height - chrome);
    int preview_h = static_cast<int>(std::lround(preview_w * 9.0 / 16.0));
    if (preview_h > max_preview_h)
        preview_h = max_preview_h;
    preview_h = std::max(min_h, preview_h);

    const int panel_h = preview_h + chrome;
    const wxSize next(preview_w, preview_h);
    if (m_fitted_preview == next && GetMaxSize().GetHeight() == panel_h)
        return;

    m_fitting_preview = true;
    m_fitted_preview = next;
    m_viewport->SetMinSize(wxSize(min_w, preview_h));
    m_viewport->SetMaxSize(wxDefaultSize);
    if (m_stream_host != nullptr)
        m_stream_host->SetMinSize(wxSize(min_w, min_h));
    SetMinSize(wxSize(FromDIP(460), panel_h));
    SetMaxSize(wxSize(-1, panel_h));
    Layout();
    layout_viewport_layers();
    m_fitting_preview = false;
}

void CameraPanel::update_from_load_state()
{
    const bool show_stream = m_load_state == CameraLoadState::Live;
    if (m_stream_host != nullptr)
        m_stream_host->Show(show_stream);
    if (m_idle_placeholder != nullptr) {
        m_idle_placeholder->Show(!show_stream);
        if (!show_stream)
            m_idle_placeholder->Raise();
    }
    if (m_empty_state != nullptr)
        m_empty_state->Hide();
    if (auto* play_btn = static_cast<CameraPlayButton*>(m_play_btn))
        play_btn->set_playing(show_stream);
    if (m_status_label != nullptr) {
        if (m_load_state == CameraLoadState::Initializing) {
            m_status_label->SetLabelText(wxString::FromUTF8("Initializing"));
            m_status_label->SetForegroundColour(DeviceUiStyle::text_muted());
            m_status_label->Show();
        } else if (m_load_state == CameraLoadState::Failed) {
            m_status_label->SetLabelText(wxString::FromUTF8("Camera could not be opened"));
            m_status_label->SetForegroundColour(DeviceUiStyle::danger());
            m_status_label->Show();
        } else {
            m_status_label->Hide();
        }
    }
    layout_viewport_layers();
    Layout();
    if (m_viewport != nullptr)
        m_viewport->Refresh(false);
}

void CameraPanel::apply_state(const CameraState&)
{
}

void CameraPanel::set_refresh_handler(RefreshHandler handler)
{
    m_refresh_handler = std::move(handler);
}

void CameraPanel::set_play_handler(PlayHandler handler)
{
    m_play_handler = std::move(handler);
}

void CameraPanel::set_timelapse_handler(TimelapseHandler handler)
{
    m_timelapse_handler = std::move(handler);
}

void CameraPanel::set_load_state(CameraLoadState state)
{
    if (m_load_state == state)
        return;
    m_load_state = state;
    update_from_load_state();
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
