// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/perf_overlay.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QAction>
#include <QFont>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "common/settings.h"
#include "core/perf_stats.h"
#include "yuzu/main_window.h"

namespace FritchiiUi {
namespace {

constexpr size_t MaxSamples = 120; // ~60s of samples at 500ms tick
constexpr double GapFps = 8.0;     // Below this is a loading screen / menu pause gap

// Simple FPS pill: whisper-small, number only
constexpr int SimpleWidth = 60;
constexpr int SimpleHeight = 32;

// Dip Study card: compact, focused on seeing and studying dips
constexpr int StudyWidth = 248;
constexpr int StudyHeight = 128;

const QColor PanelBg{0x12, 0x12, 0x17, 210};
const QColor PanelEdge{255, 255, 255, 28};
const QColor TextMain{0xf2, 0xf2, 0xf6};
const QColor TextMuted{0x9a, 0x9a, 0xaa};
const QColor ColorSmooth{0x6e, 0xe7, 0xb7}; // Emerald-mint when smooth
const QColor ColorMildDip{0xfb, 0xbf, 0x24}; // Amber on mild dip (<92% target)
const QColor ColorHeavyDip{0xf8, 0x71, 0x71}; // Coral-red on heavy dip (<75% target)
const QColor GuideLine{255, 255, 255, 42};

QFont InterFont(int pixel_size, QFont::Weight weight) {
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(pixel_size);
    font.setWeight(weight);
    return font;
}

QColor ColorForFps(double sample, double target) {
    if (sample >= target * 0.92) {
        return ColorSmooth;
    }
    if (sample >= target * 0.75) {
        return ColorMildDip;
    }
    return ColorHeavyDip;
}

} // namespace

PerfOverlay::PerfOverlay(MainWindow* main_window_, std::function<void()> on_hidden_)
    : QWidget(main_window_, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                                Qt::NoDropShadowWindowHint),
      main_window{main_window_}, on_hidden{std::move(on_hidden_)} {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    ApplyLayout();
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag to move. Double-click to switch Simple FPS / Dip Study. Right-click for menu."));
    FollowWindow();
    connect(main_window, &MainWindow::positionChanged, this, &PerfOverlay::FollowWindow);
    connect(main_window, &MainWindow::statsUpdated, this,
            [this](const Core::PerfStatsResults& results, const VideoCore::ShaderNotify&) {
                OnStats(results);
            });
}

PerfOverlay::~PerfOverlay() = default;

void PerfOverlay::FollowWindow() {
    move(main_window->pos() + offset);
}

void PerfOverlay::ApplyLayout() {
    const auto configured = static_cast<UISettings::PerfOverlayLayout>(
        UISettings::values.perf_overlay_layout.GetValue());
    layout = (configured == UISettings::PerfOverlayLayout::DipStudy)
                 ? UISettings::PerfOverlayLayout::DipStudy
                 : UISettings::PerfOverlayLayout::SimpleFps;
    if (layout == UISettings::PerfOverlayLayout::SimpleFps) {
        setFixedSize(SimpleWidth, SimpleHeight);
    } else {
        setFixedSize(StudyWidth, StudyHeight);
    }
    update();
}

void PerfOverlay::SetLayout(UISettings::PerfOverlayLayout next_layout) {
    UISettings::values.perf_overlay_layout.SetValue(static_cast<u8>(next_layout));
    ApplyLayout();
}

void PerfOverlay::OnStats(const Core::PerfStatsResults& results) {
    const auto configured = static_cast<UISettings::PerfOverlayLayout>(
        UISettings::values.perf_overlay_layout.GetValue());
    if (configured != layout) {
        ApplyLayout();
    }
    if (!std::isnan(results.average_game_fps)) {
        fps = results.average_game_fps;
        fps_samples.push_back(fps > GapFps ? fps : std::numeric_limits<double>::quiet_NaN());
        while (fps_samples.size() > MaxSamples) {
            fps_samples.pop_front();
        }
    }
    if (!std::isnan(results.frametime)) {
        frametime_ms = results.frametime * 1000.0;
    }
    update();
}

void PerfOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double target = Settings::values.oot_30fps.GetValue() ? 30.0 : 60.0;

    if (layout == UISettings::PerfOverlayLayout::SimpleFps) {
        // Ultra-clean pill showing ONLY the FPS number.
        painter.setPen(QPen(PanelEdge, 1.0));
        painter.setBrush(PanelBg);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10.0, 10.0);

        painter.setFont(InterFont(17, QFont::Bold));
        painter.setPen(fps > GapFps ? ColorForFps(fps, target) : TextMain);
        painter.drawText(rect(), Qt::AlignCenter, QString::number(std::lround(fps)));
        return;
    }

    // Dip Study layout: compact telemetry strip + color-coded dip graph
    painter.setPen(QPen(PanelEdge, 1.0));
    painter.setBrush(PanelBg);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12.0, 12.0);

    constexpr int pad = 12;

    // Compute recent dip statistics across the visible window
    double lowest_fps = std::numeric_limits<double>::infinity();
    size_t lowest_idx = 0;
    int dip_count = 0;
    for (size_t i = 0; i < fps_samples.size(); ++i) {
        const double s = fps_samples[i];
        if (std::isnan(s)) {
            continue;
        }
        if (s < lowest_fps) {
            lowest_fps = s;
            lowest_idx = i;
        }
        if (s < target * 0.92) {
            ++dip_count;
        }
    }

    // Top row: live FPS + frametime (left) | Low + Dips count (right)
    const QString fps_str = QString::number(std::lround(fps));
    const QFont big_font = InterFont(24, QFont::Bold);
    const QFont small_font = InterFont(11, QFont::Medium);
    const QFont tiny_font = InterFont(10, QFont::Normal);

    painter.setFont(big_font);
    painter.setPen(fps > GapFps ? ColorForFps(fps, target) : TextMain);
    painter.drawText(QRect(pad, 6, 54, 28), Qt::AlignLeft | Qt::AlignVCenter, fps_str);

    const int fps_w = QFontMetrics(big_font).horizontalAdvance(fps_str);
    painter.setFont(small_font);
    painter.setPen(TextMuted);
    painter.drawText(QRect(pad + fps_w + 5, 10, 80, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("FPS  %1ms").arg(frametime_ms, 0, 'f', 1));

    // Right side: Lowest dip & dip counter
    const bool has_low = !std::isinf(lowest_fps);
    painter.setFont(small_font);
    painter.setPen(has_low ? ColorForFps(lowest_fps, target) : TextMuted);
    const QString low_str =
        has_low ? tr("Low %1").arg(std::lround(lowest_fps)) : tr("Low --");
    painter.drawText(QRect(StudyWidth - pad - 95, 7, 95, 16), Qt::AlignRight | Qt::AlignVCenter,
                     low_str);

    painter.setFont(tiny_font);
    painter.setPen(dip_count > 0 ? ColorMildDip : TextMuted);
    painter.drawText(QRect(StudyWidth - pad - 95, 22, 95, 14), Qt::AlignRight | Qt::AlignVCenter,
                     dip_count == 1 ? tr("1 dip") : tr("%1 dips").arg(dip_count));

    // Dip study graph area
    const QRectF graph(pad, 42.0, StudyWidth - 2.0 * pad, StudyHeight - 54.0);
    painter.setPen(QPen(QColor(255, 255, 255, 18), 1.0));
    painter.setBrush(QColor(0x0b, 0x0b, 0x0f, 165));
    painter.drawRoundedRect(graph, 6.0, 6.0);

    double top_fps = target * 1.15;
    for (const double s : fps_samples) {
        if (!std::isnan(s)) {
            top_fps = std::max(top_fps, s * 1.08);
        }
    }
    const auto y_of = [&](double v) {
        const double clamped = std::clamp(v, 0.0, top_fps);
        return graph.bottom() - 3.0 - (clamped / top_fps) * (graph.height() - 6.0);
    };

    // Target FPS dashed reference line
    const qreal target_y = y_of(target);
    painter.setPen(QPen(GuideLine, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(graph.left() + 2.0, target_y), QPointF(graph.right() - 2.0, target_y));

    // Dip threshold subtle guide (92% of target)
    const qreal dip_y = y_of(target * 0.92);
    painter.setPen(QPen(QColor(0xfb, 0xbf, 0x24, 48), 1.0, Qt::DotLine));
    painter.drawLine(QPointF(graph.left() + 2.0, dip_y), QPointF(graph.right() - 2.0, dip_y));

    // Tiny target label
    painter.setFont(InterFont(9, QFont::Medium));
    painter.setPen(QColor(0x9a, 0x9a, 0xaa, 180));
    painter.drawText(QRectF(graph.right() - 26.0, target_y - 12.0, 22.0, 12.0),
                     Qt::AlignRight | Qt::AlignVCenter, QString::number(static_cast<int>(target)));

    // Draw color-coded segments so every dip pops out visually in amber / coral!
    if (fps_samples.size() >= 2) {
        const double usable_w = graph.width() - 6.0;
        const double step = usable_w / static_cast<double>(MaxSamples - 1);
        const double x0 = graph.right() - 3.0 - step * static_cast<double>(fps_samples.size() - 1);

        QPointF lowest_pt;
        bool has_lowest_pt = false;

        for (size_t i = 1; i < fps_samples.size(); ++i) {
            const double prev = fps_samples[i - 1];
            const double curr = fps_samples[i];
            if (std::isnan(prev) || std::isnan(curr)) {
                continue;
            }
            const QPointF p1(x0 + step * static_cast<double>(i - 1), y_of(prev));
            const QPointF p2(x0 + step * static_cast<double>(i), y_of(curr));
            const double seg_min = std::min(prev, curr);
            const QColor seg_col = ColorForFps(seg_min, target);

            // Subtle translucent fill under dip segments so dips are unmistakable
            if (seg_min < target * 0.92) {
                QColor dip_fill = seg_col;
                dip_fill.setAlpha(44);
                QPainterPath band;
                band.moveTo(p1);
                band.lineTo(p2);
                band.lineTo(p2.x(), graph.bottom() - 2.0);
                band.lineTo(p1.x(), graph.bottom() - 2.0);
                band.closeSubpath();
                painter.setPen(Qt::NoPen);
                painter.setBrush(dip_fill);
                painter.drawPath(band);
            }

            painter.setPen(QPen(seg_col, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(p1, p2);
        }

        if (has_low && lowest_fps < target * 0.95 && lowest_idx < fps_samples.size()) {
            lowest_pt = QPointF(x0 + step * static_cast<double>(lowest_idx), y_of(lowest_fps));
            has_lowest_pt = true;
        }
        if (has_lowest_pt) {
            const QColor marker_col = ColorForFps(lowest_fps, target);
            painter.setPen(QPen(QColor(0x12, 0x12, 0x17), 1.5));
            painter.setBrush(marker_col);
            painter.drawEllipse(lowest_pt, 3.8, 3.8);
        }
    }
}

void PerfOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        QMenu menu(this);
        auto* simple_act = menu.addAction(tr("Simple FPS (Number Only)"));
        simple_act->setCheckable(true);
        simple_act->setChecked(layout == UISettings::PerfOverlayLayout::SimpleFps);
        connect(simple_act, &QAction::triggered, this,
                [this] { SetLayout(UISettings::PerfOverlayLayout::SimpleFps); });

        auto* study_act = menu.addAction(tr("Dip Study (Lows & Dip Graph)"));
        study_act->setCheckable(true);
        study_act->setChecked(layout == UISettings::PerfOverlayLayout::DipStudy);
        connect(study_act, &QAction::triggered, this,
                [this] { SetLayout(UISettings::PerfOverlayLayout::DipStudy); });

        menu.addSeparator();
        auto* hide_act = menu.addAction(tr("Hide FPS Overlay"));
        connect(hide_act, &QAction::triggered, this, [this] {
            hide();
            if (on_hidden) {
                on_hidden();
            }
        });
        menu.exec(event->globalPosition().toPoint());
        return;
    }
    if (event->button() == Qt::LeftButton) {
        drag_start = event->pos();
    }
}

void PerfOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const auto next = (layout == UISettings::PerfOverlayLayout::SimpleFps)
                              ? UISettings::PerfOverlayLayout::DipStudy
                              : UISettings::PerfOverlayLayout::SimpleFps;
        SetLayout(next);
    }
}

void PerfOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        const QPoint position = event->globalPosition().toPoint() - drag_start;
        offset = position - main_window->pos();
        move(position);
    }
}

} // namespace FritchiiUi
