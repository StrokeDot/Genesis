// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <functional>

#include <QWidget>

#include "qt_common/config/uisettings.h"

class MainWindow;

namespace Core {
struct PerfStatsResults;
}
namespace VideoCore {
class ShaderNotify;
}

namespace FritchiiUi {

/// Minimal, non-invasive performance overlay with two user-selectable layouts:
///  - SimpleFps: tiny corner pill showing ONLY the live FPS number.
///  - DipStudy: compact card showing FPS, frametime, recent lowest dip, dip count,
///    and a color-coded frametime/FPS sparkline highlighting every frame dip.
/// Drag to reposition; double-click to toggle between Simple FPS and Dip Study;
/// right-click for a compact layout/hide menu.
class PerfOverlay : public QWidget {
    Q_OBJECT
public:
    PerfOverlay(MainWindow* main_window, std::function<void()> on_hidden);
    ~PerfOverlay() override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void OnStats(const Core::PerfStatsResults& results);
    void FollowWindow();
    void ApplyLayout();
    void SetLayout(UISettings::PerfOverlayLayout next_layout);

    MainWindow* main_window;
    std::function<void()> on_hidden;
    std::deque<double> fps_samples; ///< NaN marks a loading/freeze gap
    double fps = 0.0;
    double frametime_ms = 0.0;
    UISettings::PerfOverlayLayout layout = UISettings::PerfOverlayLayout::SimpleFps;
    QPoint offset{20, 20};
    QPoint drag_start;
};

} // namespace FritchiiUi
