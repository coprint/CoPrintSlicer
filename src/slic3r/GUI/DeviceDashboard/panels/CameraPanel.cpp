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

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/filename.h>
#include <wx/frame.h>
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
        SetMinSize(wxSize(FromDIP(86), FromDIP(30)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &CameraPlayButton::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_click_handler)
                m_click_handler();
        });
    }

    void set_click_handler(ClickHandler handler) { m_click_handler = std::move(handler); }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC raw_dc(this);
        raw_dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : DeviceUiStyle::page_background()));
        raw_dc.Clear();

        const wxSize size = GetClientSize();
        wxGCDC dc(raw_dc);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(DeviceUiStyle::accent()));
        dc.DrawRoundedRectangle(wxRect(0, 0, size.GetWidth(), size.GetHeight()), FromDIP(8));

        const wxString label = wxString::FromUTF8("Play");
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
        wxGraphicsContext* gc = dc.GetGraphicsContext();
        if (gc != nullptr) {
            wxGraphicsPath play_shape = gc->CreatePath();
            play_shape.MoveToPoint(x + 0.5, icon_y + 0.5);
            play_shape.AddLineToPoint(x + 0.5, icon_y + icon_h - 0.5);
            play_shape.AddLineToPoint(x + icon_w + 0.5, icon_y + icon_h / 2.0);
            play_shape.CloseSubpath();
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(*wxWHITE));
            gc->FillPath(play_shape);
        }
        x += icon_w + gap;

        dc.DrawText(label, x, (size.GetHeight() - text_h) / 2);
    }

    ClickHandler m_click_handler;
};

wxColour camera_header_background()
{
    return DeviceUiStyle::page_background();
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

    wxImage flattened(image.GetWidth(), image.GetHeight(), false);
    unsigned char* dst = flattened.GetData();
    const unsigned char* src = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    if (dst == nullptr || src == nullptr)
        return bitmap;

    const wxColour bg = camera_header_background();
    const int bg_r = bg.Red();
    const int bg_g = bg.Green();
    const int bg_b = bg.Blue();
    const int pixels = image.GetWidth() * image.GetHeight();
    for (int i = 0; i < pixels; ++i) {
        const int a = alpha != nullptr ? alpha[i] : 255;
        const int inv = 255 - a;
        const int offset = i * 3;
        dst[offset + 0] = static_cast<unsigned char>((src[offset + 0] * a + bg_r * inv) / 255);
        dst[offset + 1] = static_cast<unsigned char>((src[offset + 1] * a + bg_g * inv) / 255);
        dst[offset + 2] = static_cast<unsigned char>((src[offset + 2] * a + bg_b * inv) / 255);
    }

    return wxBitmap(flattened);
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
    SetMinSize(wxSize(FromDIP(460), FromDIP(555)));

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_frame = new DeviceCardFrame(this, wxString::FromUTF8("Live Camera"));
    m_frame->content_parent()->SetBackgroundColour(DeviceUiStyle::page_background());
    auto* content_sizer = new wxBoxSizer(wxVERTICAL);

    auto* header_actions = new wxPanel(m_frame, wxID_ANY);
    header_actions->SetBackgroundColour(camera_header_background());
    auto* header_actions_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_timelapse_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_header_icon_bitmap("camera_timelapse_white", header_actions, 18));
    m_timelapse_btn->SetBackgroundColour(camera_header_background());
    m_timelapse_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_timelapse_btn->SetToolTip(wxString::FromUTF8("Timelapse"));
    m_timelapse_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_timelapse_handler) m_timelapse_handler();
    });
    header_actions_sizer->Add(m_timelapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

    m_fullscreen_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_header_icon_bitmap("camera_fullscreen_white", header_actions, 18));
    m_fullscreen_btn->SetBackgroundColour(camera_header_background());
    m_fullscreen_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    m_fullscreen_btn->SetToolTip(wxString::FromUTF8("Fullscreen"));
    m_fullscreen_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        toggle_fullscreen();
        if (m_fullscreen_handler) m_fullscreen_handler();
    });
    header_actions_sizer->Add(m_fullscreen_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

    m_refresh_btn = new wxStaticBitmap(header_actions, wxID_ANY,
        create_header_icon_bitmap("camera_refresh_white", header_actions, 16));
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
    m_viewport->SetMinSize(wxSize(FromDIP(420), FromDIP(390)));
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
    m_stream_host->SetMinSize(wxSize(FromDIP(420), FromDIP(360)));
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
    control_sizer->Add(m_play_btn, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));

    content_sizer->Add(m_viewport, 1, wxEXPAND);
    content_sizer->Add(control_sizer, 0, wxEXPAND);
    m_frame->set_content(content_sizer);
    root->Add(m_frame, 1, wxEXPAND);
    SetSizer(root);

    update_idle_visibility(false);
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

void CameraPanel::update_idle_visibility(bool stream_available)
{
    m_stream_available = stream_available;
    const bool show_stream = m_stream_started && stream_available;
    if (m_stream_host != nullptr)
        m_stream_host->Show(show_stream);
    if (m_idle_placeholder != nullptr) {
        m_idle_placeholder->Show(!show_stream);
        if (!show_stream)
            m_idle_placeholder->Raise();
    }
    if (m_empty_state != nullptr)
        m_empty_state->Show(m_stream_started && !stream_available);
    layout_viewport_layers();
    if (m_viewport != nullptr)
        m_viewport->Refresh(false);
}

void CameraPanel::apply_state(const CameraState& state)
{
    const bool available = state.available && !state.stream_url.IsEmpty();
    update_idle_visibility(available);
}

void CameraPanel::set_refresh_handler(RefreshHandler handler)
{
    m_refresh_handler = std::move(handler);
}

void CameraPanel::set_play_handler(PlayHandler handler)
{
    m_play_handler = std::move(handler);
}

void CameraPanel::set_fullscreen_handler(FullscreenHandler handler)
{
    m_fullscreen_handler = std::move(handler);
}

void CameraPanel::toggle_fullscreen()
{
    if (m_fullscreen_frame != nullptr) {
        // Already fullscreen -- treat a second click the same as Esc/close box.
        m_fullscreen_frame->Close();
        return;
    }
    if (m_stream_host == nullptr || m_viewport == nullptr)
        return;

    auto* frame = new wxFrame(nullptr, wxID_ANY, wxString::FromUTF8("Live Camera"));
    frame->SetBackgroundColour(*wxBLACK);
    m_fullscreen_frame = frame;

    m_stream_host->Reparent(frame);
    m_stream_host->Show(true);
    m_stream_host->Lower();

    // Plain wxButton, not a bare styled label: guaranteed visible against any
    // background regardless of font glyph support, no custom paint needed.
    auto* close_btn = new wxButton(frame, wxID_ANY, wxString::FromUTF8("X"),
        wxDefaultPosition, wxSize(FromDIP(36), FromDIP(36)));
    close_btn->SetBackgroundColour(wxColour(60, 60, 60));
    close_btn->SetForegroundColour(*wxWHITE);
    close_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    close_btn->SetToolTip(wxString::FromUTF8("Close (Esc)"));
    {
        wxFont f = close_btn->GetFont();
        f.SetPointSize(f.GetPointSize() + 2);
        f.SetWeight(wxFONTWEIGHT_BOLD);
        close_btn->SetFont(f);
    }

    // Returns the stream host to the Device tab and tears down the fullscreen
    // window. Guarded by the m_fullscreen_frame nullptr check so it's safe to
    // call more than once (close button click + the frame's own close event).
    auto close_fullscreen = [this]() {
        if (m_fullscreen_frame == nullptr)
            return;
        wxFrame* frame_to_close = m_fullscreen_frame;
        m_fullscreen_frame = nullptr;
        if (m_stream_host != nullptr && m_viewport != nullptr) {
            m_stream_host->Reparent(m_viewport);
            layout_viewport_layers();
            update_idle_visibility(m_stream_available);
        }
        frame_to_close->Destroy();
    };

    close_btn->Bind(wxEVT_BUTTON, [close_fullscreen](wxCommandEvent&) { close_fullscreen(); });
    frame->Bind(wxEVT_CHAR_HOOK, [close_fullscreen](wxKeyEvent& evt) {
        if (evt.GetKeyCode() == WXK_ESCAPE)
            close_fullscreen();
        else
            evt.Skip();
    });
    frame->Bind(wxEVT_CLOSE_WINDOW, [close_fullscreen](wxCloseEvent&) { close_fullscreen(); });

    auto reposition = [this, frame, close_btn]() {
        const wxSize client = frame->GetClientSize();
        if (client.GetWidth() <= 0 || client.GetHeight() <= 0)
            return;
        if (m_stream_host != nullptr)
            m_stream_host->SetSize(0, 0, client.GetWidth(), client.GetHeight());
        close_btn->SetPosition(wxPoint(client.GetWidth() - close_btn->GetSize().GetWidth() - FromDIP(24), FromDIP(20)));
        close_btn->Raise();
    };
    frame->Bind(wxEVT_SIZE, [reposition](wxSizeEvent& evt) {
        evt.Skip();
        reposition();
    });

    frame->Maximize(true);
    frame->Show(true);
    frame->SendSizeEvent();
    reposition();
    // Maximize can settle a frame later than the same-turn SendSizeEvent on
    // some window managers; correct the button position once more next idle.
    CallAfter([reposition]() { reposition(); });
    frame->SetFocus();
}

void CameraPanel::set_timelapse_handler(TimelapseHandler handler)
{
    m_timelapse_handler = std::move(handler);
}

void CameraPanel::set_stream_started(bool started)
{
    if (m_stream_started == started)
        return;
    m_stream_started = started;
    update_idle_visibility(m_stream_available);
}

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
