// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/closing_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QEvent>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTimer>

namespace FritchiiUi {

namespace {

constexpr double TwoPi = 6.28318530717958647692;
constexpr qint64 FadeInMs = 170;
constexpr qint64 FadeOutMs = 220;

void DrawSparkleStar(QPainter& p, QPointF center, qreal radius, const QColor& color, qreal alpha) {
    if (alpha <= 0.02 || radius <= 0.5) {
        return;
    }

    // Soft glowing halo
    QRadialGradient glow(center, radius * 2.4);
    QColor c_inner = color;
    c_inner.setAlphaF(std::clamp(alpha * 0.65, 0.0, 1.0));
    QColor c_outer = color;
    c_outer.setAlphaF(0.0);
    glow.setColorAt(0.0, c_inner);
    glow.setColorAt(1.0, c_outer);
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(center, radius * 2.4, radius * 2.4);

    // Crisp 4-point star glitter shape
    const qreal inner = radius * 0.28;
    QPainterPath star;
    star.moveTo(center.x(), center.y() - radius);
    star.quadTo(center.x() + inner * 0.5, center.y() - inner * 0.5, center.x() + radius, center.y());
    star.quadTo(center.x() + inner * 0.5, center.y() + inner * 0.5, center.x(), center.y() + radius);
    star.quadTo(center.x() - inner * 0.5, center.y() + inner * 0.5, center.x() - radius, center.y());
    star.quadTo(center.x() - inner * 0.5, center.y() - inner * 0.5, center.x(), center.y() - radius);
    star.closeSubpath();

    QColor core = color.lighter(125);
    core.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    p.setBrush(core);
    p.drawPath(star);
}

} // namespace

ClosingOverlay::ClosingOverlay(QWidget* host_window, const QString& subtitle)
    : QWidget(host_window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                               Qt::NoDropShadowWindowHint),
      host(host_window), subtitle_text(subtitle) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);

    // Deterministic, balanced starlight/glitter palette (lavender, violet, cyan, gold, starlight)
    const std::array<QColor, 5> palette{{
        QColor(0xd8, 0xb4, 0xfe), // lavender
        QColor(0xa8, 0x55, 0xf7), // bright violet
        QColor(0x67, 0xe8, 0xf9), // cyan glint
        QColor(0xfd, 0xe6, 0x8a), // warm gold glint
        QColor(0xff, 0xff, 0xff), // starlight white
    }};

    constexpr int NumSparkles = 34;
    sparkles.reserve(NumSparkles);
    for (int i = 0; i < NumSparkles; ++i) {
        const double frac = static_cast<double>(i) / static_cast<double>(NumSparkles);
        Sparkle s;
        s.angle = frac * TwoPi + (i % 3) * 0.19;
        s.base_radius = 46.0 + (i % 7) * 18.0;
        s.radial_speed = 18.0 + (i % 5) * 11.0;
        s.angular_speed = ((i % 2 == 0) ? 1.0 : -1.0) * (0.35 + (i % 4) * 0.14);
        s.size = 3.2 + (i % 5) * 1.45;
        s.phase = i * 0.87;
        s.twinkle_speed = 5.0 + (i % 6) * 1.3;
        s.color = palette[static_cast<std::size_t>(i) % palette.size()];
        sparkles.push_back(s);
    }

    if (host) {
        host->installEventFilter(this);
    }
    SyncGeometry();
    clock.start();

    anim_timer = new QTimer(this);
    anim_timer->setInterval(16);
    connect(anim_timer, &QTimer::timeout, this, &ClosingOverlay::Tick);
    anim_timer->start();

    show();
    raise();
}

ClosingOverlay::~ClosingOverlay() {
    if (host) {
        host->removeEventFilter(this);
    }
}

void ClosingOverlay::SyncGeometry() {
    if (!host) {
        return;
    }
    const QPoint top_left = host->mapToGlobal(QPoint(0, 0));
    setGeometry(QRect(top_left, host->size()));
}

bool ClosingOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == host &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        SyncGeometry();
    }
    return QWidget::eventFilter(watched, event);
}

void ClosingOverlay::FinishAndFadeOut() {
    if (fade_out_start_ms >= 0) {
        return;
    }
    fade_out_start_ms = clock.elapsed();
}

void ClosingOverlay::DismissImmediate() {
    if (anim_timer) {
        anim_timer->stop();
    }
    hide();
    deleteLater();
}

void ClosingOverlay::Tick() {
    if (fade_out_start_ms >= 0 && (clock.elapsed() - fade_out_start_ms) >= FadeOutMs) {
        DismissImmediate();
        return;
    }
    update();
}

void ClosingOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qint64 now_ms = clock.elapsed();
    const double t = static_cast<double>(now_ms) / 1000.0;

    // Smooth ease-in and ease-out master opacity
    double master_alpha = std::clamp(static_cast<double>(now_ms) / static_cast<double>(FadeInMs),
                                     0.0, 1.0);
    master_alpha = 1.0 - std::pow(1.0 - master_alpha, 3.0);
    if (fade_out_start_ms >= 0) {
        const double out_prog = std::clamp(
            static_cast<double>(now_ms - fade_out_start_ms) / static_cast<double>(FadeOutMs), 0.0,
            1.0);
        master_alpha *= (1.0 - out_prog * out_prog);
    }
    if (master_alpha <= 0.01) {
        return;
    }

    const qreal w = width();
    const qreal h = height();
    const QPointF center(w * 0.5, h * 0.5);

    // 1. Translucent dark glass scrim
    QColor scrim(0x0b, 0x0b, 0x11);
    scrim.setAlphaF(0.78 * master_alpha);
    p.fillRect(rect(), scrim);

    // 2. Soft ambient violet aura behind the center card
    QRadialGradient aura(center, 220.0);
    QColor aura_c1(0x8b, 0x5c, 0xf6);
    aura_c1.setAlphaF(0.24 * master_alpha);
    QColor aura_c2(0x38, 0xbd, 0xf8);
    aura_c2.setAlphaF(0.08 * master_alpha);
    QColor aura_c3(0x0b, 0x0b, 0x11, 0);
    aura.setColorAt(0.0, aura_c1);
    aura.setColorAt(0.5, aura_c2);
    aura.setColorAt(1.0, aura_c3);
    p.setPen(Qt::NoPen);
    p.setBrush(aura);
    p.drawEllipse(center, 220.0, 220.0);

    // 3. Swirling twinkling glitter particles around the center
    for (const auto& s : sparkles) {
        const double cycle = std::fmod(t * 0.85 + s.phase * 0.13, 1.0);
        const double dist = s.base_radius + cycle * s.radial_speed * 2.2;
        const double ang = s.angle + t * s.angular_speed;
        const QPointF pt(center.x() + std::cos(ang) * dist * 1.45,
                         center.y() + std::sin(ang) * dist * 0.85 - cycle * 18.0);
        const double twinkle = 0.35 + 0.65 * (0.5 + 0.5 * std::sin(t * s.twinkle_speed + s.phase));
        const double life_env = std::sin(cycle * 3.14159265358979);
        const qreal alpha = static_cast<qreal>(master_alpha * twinkle * life_env);
        DrawSparkleStar(p, pt, static_cast<qreal>(s.size * (0.75 + 0.35 * twinkle)), s.color, alpha);
    }

    // 4. Frosted dark console pill card in the center
    const QRectF card(center.x() - 165.0, center.y() - 56.0, 330.0, 112.0);
    QColor card_bg(0x16, 0x16, 0x20);
    card_bg.setAlphaF(0.92 * master_alpha);
    QColor card_edge(0xa7, 0x8b, 0xfa);
    card_edge.setAlphaF(0.45 * master_alpha);
    p.setPen(QPen(card_edge, 1.3));
    p.setBrush(card_bg);
    p.drawRoundedRect(card, 18.0, 18.0);

    // 5. Sleek glowing dual-arc spinner on the left of the card
    const QPointF spin_c(card.left() + 52.0, card.center().y());
    const QRectF spin_box(spin_c.x() - 19.0, spin_c.y() - 19.0, 38.0, 38.0);
    QColor track(255, 255, 255);
    track.setAlphaF(0.10 * master_alpha);
    p.setPen(QPen(track, 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(spin_box);

    const int start_deg = static_cast<int>(std::fmod(t * 320.0, 360.0));
    QColor arc1(0xa8, 0x55, 0xf7);
    arc1.setAlphaF(0.95 * master_alpha);
    p.setPen(QPen(arc1, 3.2, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(spin_box, -start_deg * 16, -125 * 16);

    QColor arc2(0x67, 0xe8, 0xf9);
    arc2.setAlphaF(0.85 * master_alpha);
    p.setPen(QPen(arc2, 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(spin_box.adjusted(4.5, 4.5, -4.5, -4.5), (start_deg + 140) * 16, 95 * 16);

    // Tiny central sparkle glint inside the spinner
    DrawSparkleStar(p, spin_c, 4.5, QColor(0xfd, 0xe6, 0x8a),
                    static_cast<qreal>(master_alpha * (0.65 + 0.35 * std::sin(t * 8.0))));

    // 6. Clean console text
    QFont title_font(QStringLiteral("Inter"));
    title_font.setPixelSize(16);
    title_font.setWeight(QFont::DemiBold);
    p.setFont(title_font);
    QColor txt_main(0xf2, 0xf2, 0xf8);
    txt_main.setAlphaF(master_alpha);
    p.setPen(txt_main);
    p.drawText(QRectF(card.left() + 88.0, card.top() + 28.0, card.width() - 104.0, 26.0),
               Qt::AlignLeft | Qt::AlignVCenter, tr("Closing game..."));

    QFont sub_font(QStringLiteral("Inter"));
    sub_font.setPixelSize(11);
    p.setFont(sub_font);
    QColor txt_sub(0xb0, 0xb0, 0xc4);
    txt_sub.setAlphaF(0.88 * master_alpha);
    p.setPen(txt_sub);
    const QString sub = subtitle_text.isEmpty() ? tr("Returning to Genesis Home") : subtitle_text;
    p.drawText(QRectF(card.left() + 88.0, card.top() + 56.0, card.width() - 104.0, 22.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(sub, Qt::ElideRight,
                                          static_cast<int>(card.width() - 108.0)));
}

} // namespace FritchiiUi
