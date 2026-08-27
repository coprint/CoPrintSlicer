#ifndef slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_
#define slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_

#include "../Widgets/StaticBox.hpp"

#include <wx/string.h>

class wxBoxSizer;
class wxPanel;
class wxSizer;
class wxStaticText;
class wxWindow;

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class DeviceCardFrame : public StaticBox
{
public:
    explicit DeviceCardFrame(wxWindow* parent, const wxString& title = wxString(),
                            int pad_horizontal = -1, int pad_vertical = -1,
                            int content_pad_horizontal = -1, int content_pad_top = -1,
                            int content_pad_bottom = -1);

    wxWindow* content_parent() const;
    void set_title(const wxString& title);
    void set_content(wxWindow* content);
    void set_content(wxSizer* content);
    void set_header_action(wxWindow* action);

private:
    void layout_bottom_corner_masks();

    wxStaticText* m_title{nullptr};
    wxPanel* m_header_panel{nullptr};
    wxPanel* m_content_parent{nullptr};
    wxPanel* m_bottom_left_mask{nullptr};
    wxPanel* m_bottom_right_mask{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
    wxBoxSizer* m_header_row{nullptr};
    wxWindow* m_header_action{nullptr};
    int m_corner_radius{0};
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DeviceDashboard_DeviceCardFrame_hpp_
