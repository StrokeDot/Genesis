// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <QWidget>

class QTimer;

namespace FritchiiUi {

/// Sleek console-style animated closing overlay with a luminous ring and twinkling
/// starlight/glitter particles that plays while a game shuts down and fades out smoothly
/// into Genesis Home.
class ClosingOverlay : public QWidget {
    Q_OBJECT

public:
    explicit ClosingOverlay(QWidget* host_window, const QString& subtitle = {});
    ~ClosingOverlay() override;

    /// Smoothly fades out over ~200 ms and self-deletes once the game finishes closing.
    void FinishAndFadeOut();

    /// Immediately closes and deletes without fade (used when closing the whole app).
    void DismissImmediate();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Sparkle {
        double angle = 0.0;      // radians around center
        double base_radius = 0.0;
        double radial_speed = 0.0;
        double angular_speed = 0.0;
        double size = 4.0;
        double phase = 0.0;
        double twinkle_speed = 5.0;
        QColor color;
    };

    void SyncGeometry();
    void Tick();

    QWidget* host = nullptr;
    QString subtitle_text;
    QTimer* anim_timer = nullptr;
    QElapsedTimer clock;
    qint64 fade_out_start_ms = -1;
    std::vector<Sparkle> sparkles;
};

} // namespace FritchiiUi
