#ifndef slic3r_GUI_DeviceDashboard_StatusPresetPopups_hpp_
#define slic3r_GUI_DeviceDashboard_StatusPresetPopups_hpp_

#include "../Widgets/PopupWindow.hpp"

#include <array>
#include <functional>

namespace Slic3r {
namespace GUI {
namespace DeviceDashboard {

class PresetStepSlider;

class PrintSpeedPopup : public PopupWindow
{
public:
    using ChangeHandler = std::function<void(int percent)>;

    explicit PrintSpeedPopup(wxWindow *parent);

    void set_percent(int percent);
    void set_change_handler(ChangeHandler handler);
    void popup_at(wxWindow *anchor);

protected:
    void OnDismiss() override;

private:
    PresetStepSlider *m_slider{nullptr};
    ChangeHandler     m_change_handler;
};

class FanSpeedPopup : public PopupWindow
{
public:
    using ChangeHandler = std::function<void(int tool_index, int percent)>;

    explicit FanSpeedPopup(wxWindow *parent);

    void set_percents(const std::array<int, 4> &percents);
    void set_change_handler(ChangeHandler handler);
    void popup_at(wxWindow *anchor);

protected:
    void OnDismiss() override;

private:
    std::array<PresetStepSlider *, 4> m_sliders{nullptr, nullptr, nullptr, nullptr};
    ChangeHandler                     m_change_handler;
};

} // namespace DeviceDashboard
} // namespace GUI
} // namespace Slic3r

#endif
