#pragma once

#include <functional>

#include <wx/event.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>

class Button;
class wxStaticText;
class wxSizeEvent;
#ifdef __WXMSW__
class wxFrame;
class wxMoveEvent;
#endif

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceBusySpinner : public wxPanel
{
public:
    DeviceBusySpinner(wxWindow *parent, const wxSize &size, const wxColour &bg);
    ~DeviceBusySpinner() override;

    void Play();
    void Stop();

private:
    void on_timer(wxTimerEvent &);
    void on_paint(wxPaintEvent &);

    wxTimer m_timer;
    wxColour m_arc_colour;
    double m_angle_deg{0.0};
};

class PrinterOfflineOverlay : public wxPanel
{
public:
    enum class Kind {
        Hidden,
        Dim,
        Connecting,
        Failed
    };

    explicit PrinterOfflineOverlay(wxWindow *parent);
    ~PrinterOfflineOverlay() override;

    void set_kind(Kind kind, const wxString &title = wxEmptyString, const wxString &hint = wxEmptyString);
    void set_visible(bool visible, const wxString &printer_name = wxEmptyString);
    void set_retry_handler(std::function<void()> handler);
    void set_ok_handler(std::function<void()> handler);
    void layout_over_parent();
    Kind kind() const { return m_kind; }
    bool is_overlay_visible() const;

private:
    void on_parent_size(wxSizeEvent &event);
    void apply_kind();
    void wrap_failed_labels(const wxString &title, const wxString &hint);
    wxPanel *active_card() const;
#ifdef __WXMSW__
    void on_owner_move(wxMoveEvent &event);
    void ensure_msw_chrome();
    void destroy_msw_chrome();
    void layout_msw_chrome();
    void apply_msw_scrim_alpha();
#endif

    Kind m_kind{Kind::Hidden};
    bool m_in_layout{false};
    wxPanel *m_connecting_card{nullptr};
    DeviceBusySpinner *m_spinner{nullptr};
    wxStaticText *m_connecting_label{nullptr};
    wxPanel *m_failed_card{nullptr};
    wxStaticText *m_title{nullptr};
    wxStaticText *m_hint{nullptr};
    Button *m_ok{nullptr};
#ifdef __WXMSW__
    wxFrame *m_scrim_frame{nullptr};
    wxFrame *m_card_host{nullptr};
    wxWindow *m_owner_tlw{nullptr};
    int m_scrim_alpha_applied{-1};
#endif
    std::function<void()> m_retry_handler;
    std::function<void()> m_ok_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r
