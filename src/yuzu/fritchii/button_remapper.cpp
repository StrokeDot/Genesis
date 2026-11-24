// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/button_remapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include <QComboBox>
#include <QDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "common/param_package.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/fritchii/controller_exit_watcher.h"

namespace FritchiiUi {

namespace {

constexpr std::size_t NumRemappablePlayers = 4;

struct SwitchButtonSpec {
    Settings::NativeButton::Values id;
    const char* short_tag;
    const char* switch_name;
    const char* default_action;
};

constexpr std::array<SwitchButtonSpec, 17> kSwitchButtons{{
    {Settings::NativeButton::A, "Sw A", "Switch A", "Confirm / Primary Action"},
    {Settings::NativeButton::B, "Sw B", "Switch B", "Back / Secondary Action"},
    {Settings::NativeButton::X, "Sw X", "Switch X", "Top Face Action"},
    {Settings::NativeButton::Y, "Sw Y", "Switch Y", "Left Face Action"},
    {Settings::NativeButton::L, "Sw L", "Switch L", "Left Bumper"},
    {Settings::NativeButton::R, "Sw R", "Switch R", "Right Bumper"},
    {Settings::NativeButton::ZL, "Sw ZL", "Switch ZL", "Left Trigger"},
    {Settings::NativeButton::ZR, "Sw ZR", "Switch ZR", "Right Trigger"},
    {Settings::NativeButton::Plus, "Sw +", "Switch Plus (+)", "Pause / Menu"},
    {Settings::NativeButton::Minus, "Sw -", "Switch Minus (-)", "Map / Select"},
    {Settings::NativeButton::LStick, "Sw LS", "Switch L-Stick Click", "Left Stick Press"},
    {Settings::NativeButton::RStick, "Sw RS", "Switch R-Stick Click", "Right Stick Press"},
    {Settings::NativeButton::DUp, "D-Up", "D-Pad Up", "Directional Up"},
    {Settings::NativeButton::DDown, "D-Down", "D-Pad Down", "Directional Down"},
    {Settings::NativeButton::DLeft, "D-Left", "D-Pad Left", "Directional Left"},
    {Settings::NativeButton::DRight, "D-Right", "D-Pad Right", "Directional Right"},
    {Settings::NativeButton::Screenshot, "Capture", "Switch Capture", "Screenshot"},
}};

QString GameActionLabel(u64 program_id, const QString& game_title, const SwitchButtonSpec& spec) {
    const QString lower = game_title.toLower();

    // Mario Kart 8 Deluxe (0100152000022000)
    if (program_id == 0x0100152000022000ULL || lower.contains(QStringLiteral("mario kart 8"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Accelerate");
        case Settings::NativeButton::B:
            return QObject::tr("Brake / Reverse");
        case Settings::NativeButton::X:
            return QObject::tr("Look Back");
        case Settings::NativeButton::Y:
            return QObject::tr("Accelerate (Alt)");
        case Settings::NativeButton::L:
        case Settings::NativeButton::ZL:
            return QObject::tr("Use / Hold Item");
        case Settings::NativeButton::R:
        case Settings::NativeButton::ZR:
            return QObject::tr("Hop / Drift");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause Menu");
        case Settings::NativeButton::Minus:
            return QObject::tr("Map / Info");
        default:
            break;
        }
    }

    // Super Smash Bros. Ultimate (01006A800016E000)
    if (program_id == 0x01006A800016E000ULL || lower.contains(QStringLiteral("smash bros"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Attack");
        case Settings::NativeButton::B:
            return QObject::tr("Special Move");
        case Settings::NativeButton::X:
        case Settings::NativeButton::Y:
            return QObject::tr("Jump");
        case Settings::NativeButton::L:
        case Settings::NativeButton::R:
            return QObject::tr("Grab");
        case Settings::NativeButton::ZL:
        case Settings::NativeButton::ZR:
            return QObject::tr("Shield");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause");
        case Settings::NativeButton::DUp:
        case Settings::NativeButton::DDown:
        case Settings::NativeButton::DLeft:
        case Settings::NativeButton::DRight:
            return QObject::tr("Taunt");
        default:
            break;
        }
    }

    // Kirby and the Forgotten Land
    if (program_id == 0x01004D300C5AE000ULL || lower.contains(QStringLiteral("kirby"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
        case Settings::NativeButton::X:
            return QObject::tr("Jump / Hover");
        case Settings::NativeButton::B:
        case Settings::NativeButton::Y:
            return QObject::tr("Inhale / Attack");
        case Settings::NativeButton::L:
        case Settings::NativeButton::R:
        case Settings::NativeButton::ZL:
        case Settings::NativeButton::ZR:
            return QObject::tr("Guard / Dodge");
        case Settings::NativeButton::Minus:
            return QObject::tr("Drop Ability");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause Menu");
        default:
            break;
        }
    }

    // Super Mario 3D World + Bowser's Fury
    if (lower.contains(QStringLiteral("3d world"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
        case Settings::NativeButton::B:
            return QObject::tr("Jump");
        case Settings::NativeButton::X:
        case Settings::NativeButton::Y:
            return QObject::tr("Dash / Attack");
        case Settings::NativeButton::ZL:
        case Settings::NativeButton::ZR:
            return QObject::tr("Crouch / Pound");
        case Settings::NativeButton::L:
        case Settings::NativeButton::R:
            return QObject::tr("Rotate Camera");
        case Settings::NativeButton::Minus:
            return QObject::tr("Use Item");
        default:
            break;
        }
    }

    // Mario Tennis Aces (0100BDE00862A000)
    if (program_id == 0x0100BDE00862A000ULL || lower.contains(QStringLiteral("mario tennis"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Topspin Shot");
        case Settings::NativeButton::B:
            return QObject::tr("Slice Shot");
        case Settings::NativeButton::Y:
            return QObject::tr("Flat Shot");
        case Settings::NativeButton::X:
            return QObject::tr("Lob / Drop");
        case Settings::NativeButton::R:
        case Settings::NativeButton::ZR:
            return QObject::tr("Zone Speed");
        case Settings::NativeButton::L:
        case Settings::NativeButton::ZL:
            return QObject::tr("Special Shot");
        default:
            break;
        }
    }

    // Mario Strikers: Battle League (010019401051C000)
    if (program_id == 0x010019401051C000ULL || lower.contains(QStringLiteral("strikers"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Shoot / Tackle");
        case Settings::NativeButton::B:
            return QObject::tr("Pass / Switch");
        case Settings::NativeButton::Y:
            return QObject::tr("Lob Pass");
        case Settings::NativeButton::X:
            return QObject::tr("Dodge / Team Tackle");
        case Settings::NativeButton::ZR:
            return QObject::tr("Sprint");
        case Settings::NativeButton::R:
            return QObject::tr("Use Item");
        case Settings::NativeButton::ZL:
            return QObject::tr("Free Pass");
        default:
            break;
        }
    }

    // Super Mario Party Jamboree (0100965017338000)
    if (program_id == 0x0100965017338000ULL || lower.contains(QStringLiteral("mario party"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Confirm / Roll");
        case Settings::NativeButton::B:
            return QObject::tr("Cancel / Back");
        case Settings::NativeButton::X:
            return QObject::tr("View Board Map");
        case Settings::NativeButton::Y:
            return QObject::tr("Item Bag");
        default:
            break;
        }
    }

    // TMNT: Splintered Fate (01005CF01E784000)
    if (program_id == 0x01005CF01E784000ULL || lower.contains(QStringLiteral("ninja turtles"))) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Dash / Dodge");
        case Settings::NativeButton::B:
            return QObject::tr("Attack");
        case Settings::NativeButton::X:
            return QObject::tr("Tool");
        case Settings::NativeButton::Y:
            return QObject::tr("Special Attack");
        default:
            break;
        }
    }

    return QString::fromLatin1(spec.default_action);
}

enum class XboxInputKind {
    Button,
    AxisPositive,
    Hat,
    Unbound,
};

enum XboxOptionIndex : std::size_t {
    XB_A = 0,
    XB_B = 1,
    XB_X = 2,
    XB_Y = 3,
    XB_LB = 4,
    XB_RB = 5,
    XB_LT = 6,
    XB_RT = 7,
    XB_VIEW = 8,
    XB_MENU = 9,
    XB_LS = 10,
    XB_RS = 11,
    XB_DUP = 12,
    XB_DDOWN = 13,
    XB_DLEFT = 14,
    XB_DRIGHT = 15,
    XB_GUIDE = 16,
    XB_UNBOUND = 17,
};

struct XboxOption {
    const char* key;
    const char* label;
    const char* short_name;
    XboxInputKind kind;
    int code;
    const char* direction;
};

constexpr std::array<XboxOption, 18> kXboxOptions{{
    {"btn_0", "Xbox A (Green - Bottom)", "A", XboxInputKind::Button, 0, ""},
    {"btn_1", "Xbox B (Red - Right)", "B", XboxInputKind::Button, 1, ""},
    {"btn_2", "Xbox X (Blue - Left)", "X", XboxInputKind::Button, 2, ""},
    {"btn_3", "Xbox Y (Yellow - Top)", "Y", XboxInputKind::Button, 3, ""},
    {"btn_4", "LB (Left Bumper)", "LB", XboxInputKind::Button, 4, ""},
    {"btn_5", "RB (Right Bumper)", "RB", XboxInputKind::Button, 5, ""},
    {"axis_2", "LT (Left Trigger)", "LT", XboxInputKind::AxisPositive, 2, ""},
    {"axis_5", "RT (Right Trigger)", "RT", XboxInputKind::AxisPositive, 5, ""},
    {"btn_6", "View (Small Center-Left)", "View", XboxInputKind::Button, 6, ""},
    {"btn_7", "Menu (Small Center-Right)", "Menu", XboxInputKind::Button, 7, ""},
    {"btn_8", "Left Stick Click (LS)", "LS", XboxInputKind::Button, 8, ""},
    {"btn_9", "Right Stick Click (RS)", "RS", XboxInputKind::Button, 9, ""},
    {"hat_up", "D-Pad Up", "D-Up", XboxInputKind::Hat, 0, "up"},
    {"hat_down", "D-Pad Down", "D-Down", XboxInputKind::Hat, 0, "down"},
    {"hat_left", "D-Pad Left", "D-Left", XboxInputKind::Hat, 0, "left"},
    {"hat_right", "D-Pad Right", "D-Right", XboxInputKind::Hat, 0, "right"},
    {"btn_10", "Xbox Guide (Center Logo)", "Guide", XboxInputKind::Button, 10, ""},
    {"unbound", "Unbound (None)", "None", XboxInputKind::Unbound, -1, ""},
}};

struct SdlDeviceIdentity {
    std::string guid;
    int port = 0;
    bool valid = false;
};

SdlDeviceIdentity ExtractSdlIdentity(const Settings::PlayerInput& player, std::size_t player_index) {
    for (const auto& binding : player.buttons) {
        if (binding.empty() || binding == "[empty]") {
            continue;
        }
        Common::ParamPackage param(binding);
        if (param.Get("engine", "") == "sdl") {
            SdlDeviceIdentity id;
            id.guid = param.Get("guid", "");
            id.port = param.Get("port", static_cast<int>(player_index));
            id.valid = !id.guid.empty();
            if (id.valid) {
                return id;
            }
        }
    }
    return {};
}

std::optional<std::size_t> MatchXboxOption(const std::string& binding) {
    if (binding.empty() || binding == "[empty]") {
        return XB_UNBOUND;
    }
    Common::ParamPackage param(binding);
    if (param.Get("engine", "") != "sdl") {
        return std::nullopt;
    }
    if (param.Has("button")) {
        const int btn = param.Get("button", -1);
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::Button && kXboxOptions[i].code == btn) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("axis")) {
        const int axis = param.Get("axis", -1);
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::AxisPositive &&
                kXboxOptions[i].code == axis) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("hat")) {
        const std::string dir = param.Get("direction", "");
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::Hat && dir == kXboxOptions[i].direction) {
                return i;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string BuildXboxBinding(const XboxOption& opt, const SdlDeviceIdentity& id,
                             std::size_t player_index) {
    if (opt.kind == XboxInputKind::Unbound) {
        return "[empty]";
    }
    Common::ParamPackage param;
    param.Set("engine", "sdl");
    param.Set("port", id.valid ? id.port : static_cast<int>(player_index));
    if (id.valid && !id.guid.empty()) {
        param.Set("guid", id.guid);
    }
    switch (opt.kind) {
    case XboxInputKind::Button:
        param.Set("button", opt.code);
        break;
    case XboxInputKind::AxisPositive:
        param.Set("axis", opt.code);
        param.Set("invert", "+");
        param.Set("threshold", "0.500000");
        break;
    case XboxInputKind::Hat:
        param.Set("hat", 0);
        param.Set("direction", opt.direction);
        break;
    case XboxInputKind::Unbound:
        return "[empty]";
    }
    return param.Serialize();
}

QString DescribeCustomBinding(const std::string& binding) {
    if (binding.empty() || binding == "[empty]") {
        return QObject::tr("Unbound (None)");
    }
    if (const auto idx = MatchXboxOption(binding); idx.has_value()) {
        return QString::fromLatin1(kXboxOptions[*idx].label);
    }
    Common::ParamPackage param(binding);
    const std::string engine = param.Get("engine", "");
    if (engine == "keyboard") {
        return QObject::tr("Keyboard (Key %1)").arg(param.Get("code", 0));
    }
    if (engine == "sdl") {
        if (param.Has("button")) {
            return QObject::tr("Controller Button %1").arg(param.Get("button", 0));
        }
        if (param.Has("axis")) {
            return QObject::tr("Controller Axis %1").arg(param.Get("axis", 0));
        }
    }
    return QObject::tr("Custom Binding");
}

std::string ConfigNameForGame(u64 program_id, const std::string& game_path) {
    if (program_id != 0) {
        return fmt::format("{:016X}", program_id);
    }
    return QFileInfo(QString::fromStdString(game_path)).fileName().toStdString();
}

bool ButtonsEqualGlobal(const Settings::PlayerInput& current, const Settings::PlayerInput& global) {
    for (const auto& spec : kSwitchButtons) {
        if (current.buttons[spec.id] != global.buttons[spec.id]) {
            return false;
        }
    }
    return true;
}

/// Visual interactive Xbox controller widget that shows:
///  - Every physical Xbox button in its real position (LT, LB, View, Guide, Menu, RB, RT,
///    Left Stick, D-Pad, Right Stick, and colored Y/X/B/A face diamond).
///  - Clear callout lines showing what each Xbox button CURRENTLY DOES inside this game
///    (and which Switch button it triggers).
///  - Live amber/coral glow highlighting the exact physical button(s) used for the
///    selected "Exit Game" controller shortcut so the user never has to guess where View/Menu
///    or Guide is!
///  - Clicking any callout or button on the diagram focuses that button's action row below.
class XboxControllerDiagram : public QWidget {
public:
    struct ActionInfo {
        QString action_text;
        QString switch_tag;
        std::optional<std::size_t> row_index;
    };

    explicit XboxControllerDiagram(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(780, 316);
        setFixedHeight(318);
        setCursor(Qt::PointingHandCursor);
    }

    void SetClickHandler(std::function<void(std::size_t row_index)> handler) {
        on_row_clicked = std::move(handler);
    }

    void UpdateState(const Settings::PlayerInput& player, u64 program_id, const QString& game_title,
                     UISettings::ControllerExitShortcut exit_shortcut_,
                     std::optional<std::size_t> selected_row_) {
        exit_shortcut = exit_shortcut_;
        selected_row = selected_row_;
        for (auto& info : xbox_actions) {
            info = {};
        }

        // Map each Xbox physical input to the Switch/in-game action(s) currently bound to it.
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            const auto& spec = kSwitchButtons[i];
            const auto opt = MatchXboxOption(player.buttons[spec.id]);
            if (!opt.has_value() || *opt >= XB_UNBOUND) {
                continue;
            }
            auto& slot = xbox_actions[*opt];
            const QString act = GameActionLabel(program_id, game_title, spec);
            const QString tag = QString::fromLatin1(spec.short_tag);
            if (slot.action_text.isEmpty()) {
                slot.action_text = act;
                slot.switch_tag = tag;
                slot.row_index = i;
            } else {
                if (!slot.action_text.contains(act)) {
                    slot.action_text += QStringLiteral(" / ") + act;
                }
                slot.switch_tag += QStringLiteral("+") + tag;
            }
        }
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        const QPointF pt = event->position();
        for (const auto& hit : hit_regions) {
            if (hit.rect.contains(pt) && hit.row_index.has_value() && on_row_clicked) {
                on_row_clicked(*hit.row_index);
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        hit_regions.clear();

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal w = width();
        const qreal h = height();
        const qreal cx = w * 0.5;
        const qreal cy = h * 0.54;

        // Card backdrop
        p.setPen(QPen(QColor(0x2b, 0x2b, 0x36), 1.0));
        p.setBrush(QColor(0x18, 0x18, 0x20));
        p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 12.0, 12.0);

        // Legend strip at top-left and top-right
        {
            QFont legend_font = font();
            legend_font.setPixelSize(11);
            legend_font.setWeight(QFont::DemiBold);
            p.setFont(legend_font);
            p.setPen(QColor(0x9a, 0x9a, 0xaa));
            p.drawText(QRectF(16, 10, 320, 18), Qt::AlignLeft | Qt::AlignVCenter,
                       tr("LIVE XBOX CONTROLLER MAP (click any label to edit)"));

            if (exit_shortcut != UISettings::ControllerExitShortcut::Off) {
                const QRectF pill(w - 228, 8, 212, 22);
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b), 1.2));
                p.setBrush(QColor(0x45, 0x28, 0x0b));
                p.drawRoundedRect(pill, 11.0, 11.0);
                p.setPen(QColor(0xfd, 0xe6, 0x8a));
                p.drawText(pill, Qt::AlignCenter, tr("Orange Glow = Exit Game"));
            }
        }

        const auto is_exit_btn = [&](XboxOptionIndex idx) {
            switch (exit_shortcut) {
            case UISettings::ControllerExitShortcut::ViewAndMenu:
                return idx == XB_VIEW || idx == XB_MENU;
            case UISettings::ControllerExitShortcut::GuideButton:
            case UISettings::ControllerExitShortcut::HoldGuideButton:
                return idx == XB_GUIDE;
            case UISettings::ControllerExitShortcut::LbRbViewMenu:
                return idx == XB_LB || idx == XB_RB || idx == XB_VIEW || idx == XB_MENU;
            case UISettings::ControllerExitShortcut::ClickBothSticks:
                return idx == XB_LS || idx == XB_RS;
            case UISettings::ControllerExitShortcut::Off:
                return false;
            }
            return false;
        };

        const auto is_selected_btn = [&](XboxOptionIndex idx) {
            return selected_row.has_value() && xbox_actions[idx].row_index.has_value() &&
                   *xbox_actions[idx].row_index == *selected_row;
        };

        // 1. Controller Body Silhouette (clean Xbox proportions centered around cx, cy)
        {
            QPainterPath body;
            body.moveTo(cx - 102, cy - 68);
            body.cubicTo(cx - 50, cy - 82, cx + 50, cy - 82, cx + 102, cy - 68);
            body.cubicTo(cx + 136, cy - 58, cx + 158, cy - 16, cx + 162, cy + 38);
            body.cubicTo(cx + 165, cy + 84, cx + 144, cy + 108, cx + 122, cy + 104);
            body.cubicTo(cx + 96, cy + 98, cx + 82, cy + 56, cx + 54, cy + 52);
            body.lineTo(cx - 54, cy + 52);
            body.cubicTo(cx - 82, cy + 56, cx - 96, cy + 98, cx - 122, cy + 104);
            body.cubicTo(cx - 144, cy + 108, cx - 165, cy + 84, cx - 162, cy + 38);
            body.cubicTo(cx - 158, cy - 16, cx - 136, cy - 58, cx - 102, cy - 68);
            body.closeSubpath();

            QLinearGradient grad(cx, cy - 84, cx, cy + 108);
            grad.setColorAt(0.0, QColor(0x2a, 0x2a, 0x35));
            grad.setColorAt(1.0, QColor(0x1c, 0x1c, 0x24));
            p.setPen(QPen(QColor(0x42, 0x42, 0x54), 1.8));
            p.setBrush(grad);
            p.drawPath(body);
        }

        // Helper for drawing glowing ring when exit-shortcut or selected
        const auto draw_highlight_rect = [&](const QRectF& r, qreal radius, bool exit_hl,
                                             bool sel_hl) {
            if (exit_hl) {
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b, 140), 5.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2.5, -2.5, 2.5, 2.5), radius + 2.5, radius + 2.5);
            } else if (sel_hl) {
                p.setPen(QPen(QColor(0xa8, 0x55, 0xf7, 170), 4.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2.0, -2.0, 2.0, 2.0), radius + 2.0, radius + 2.0);
            }
        };

        const auto draw_highlight_circle = [&](QPointF center, qreal r, bool exit_hl, bool sel_hl) {
            if (exit_hl) {
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b, 160), 5.0));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(center, r + 3.0, r + 3.0);
            } else if (sel_hl) {
                p.setPen(QPen(QColor(0xa8, 0x55, 0xf7, 180), 4.0));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(center, r + 2.5, r + 2.5);
            }
        };

        QFont btn_font = font();
        btn_font.setPixelSize(10);
        btn_font.setWeight(QFont::Bold);

        // 2. Triggers & Bumpers (LT, LB, RB, RT)
        const QRectF lt_rect(cx - 122, cy - 108, 48, 20);
        const QRectF lb_rect(cx - 124, cy - 83, 62, 18);
        const QRectF rb_rect(cx + 62, cy - 83, 62, 18);
        const QRectF rt_rect(cx + 74, cy - 108, 48, 20);

        const auto draw_shoulder = [&](const QRectF& rect, const QString& label,
                                       XboxOptionIndex idx) {
            const bool exit_hl = is_exit_btn(idx);
            const bool sel_hl = is_selected_btn(idx);
            draw_highlight_rect(rect, 7.0, exit_hl, sel_hl);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x52, 0x52, 0x66)),
                          1.4));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e) : QColor(0x2e, 0x2e, 0x3b));
            p.drawRoundedRect(rect, 7.0, 7.0);
            p.setFont(btn_font);
            p.setPen(QColor(0xec, 0xec, 0xf1));
            p.drawText(rect, Qt::AlignCenter, label);
            hit_regions.push_back({rect, xbox_actions[idx].row_index});
        };

        draw_shoulder(lt_rect, QStringLiteral("LT"), XB_LT);
        draw_shoulder(lb_rect, QStringLiteral("LB"), XB_LB);
        draw_shoulder(rb_rect, QStringLiteral("RB"), XB_RB);
        draw_shoulder(rt_rect, QStringLiteral("RT"), XB_RT);

        // 3. Center Buttons: Xbox Guide, View (two overlapping squares), Menu (3 lines)
        const QPointF guide_pt(cx, cy - 44);
        const QPointF view_pt(cx - 32, cy - 16);
        const QPointF menu_pt(cx + 32, cy - 16);

        // Xbox Guide Orb
        {
            const bool exit_hl = is_exit_btn(XB_GUIDE);
            draw_highlight_circle(guide_pt, 16.0, exit_hl, false);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24) : QColor(0x70, 0x70, 0x88), 1.8));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e) : QColor(0x23, 0x23, 0x2d));
            p.drawEllipse(guide_pt, 15.0, 15.0);
            // Draw iconic X arcs inside Guide button
            p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xd8, 0xd8, 0xe6), 2.2,
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(guide_pt.x() - 6, guide_pt.y() - 6),
                       QPointF(guide_pt.x() + 6, guide_pt.y() + 6));
            p.drawLine(QPointF(guide_pt.x() + 6, guide_pt.y() - 6),
                       QPointF(guide_pt.x() - 6, guide_pt.y() + 6));
        }

        // View button (left of center, two overlapping squares icon)
        {
            const bool exit_hl = is_exit_btn(XB_VIEW);
            const bool sel_hl = is_selected_btn(XB_VIEW);
            draw_highlight_circle(view_pt, 11.5, exit_hl, sel_hl);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x64, 0x64, 0x7a)),
                          1.4));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e) : QColor(0x28, 0x28, 0x34));
            p.drawEllipse(view_pt, 11.0, 11.0);
            // Two overlapping tiny rectangles (authentic Xbox View glyph)
            p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xdc, 0xdc, 0xe6), 1.3));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(view_pt.x() - 4.5, view_pt.y() - 4.0, 5.5, 5.0));
            p.drawRect(QRectF(view_pt.x() - 1.0, view_pt.y() - 1.0, 5.5, 5.0));
            hit_regions.push_back(
                {QRectF(view_pt.x() - 12, view_pt.y() - 12, 24, 24), xbox_actions[XB_VIEW].row_index});
        }

        // Menu button (right of center, 3 horizontal lines icon)
        {
            const bool exit_hl = is_exit_btn(XB_MENU);
            const bool sel_hl = is_selected_btn(XB_MENU);
            draw_highlight_circle(menu_pt, 11.5, exit_hl, sel_hl);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x64, 0x64, 0x7a)),
                          1.4));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e) : QColor(0x28, 0x28, 0x34));
            p.drawEllipse(menu_pt, 11.0, 11.0);
            // Three horizontal bars (authentic Xbox Menu glyph)
            p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xdc, 0xdc, 0xe6), 1.5,
                          Qt::SolidLine, Qt::RoundCap));
            for (int row_y : {-3, 0, 3}) {
                p.drawLine(QPointF(menu_pt.x() - 4.5, menu_pt.y() + row_y),
                           QPointF(menu_pt.x() + 4.5, menu_pt.y() + row_y));
            }
            hit_regions.push_back(
                {QRectF(menu_pt.x() - 12, menu_pt.y() - 12, 24, 24), xbox_actions[XB_MENU].row_index});
        }

        // Tiny labels right under View / Menu / Guide
        {
            QFont tiny = font();
            tiny.setPixelSize(9);
            tiny.setWeight(QFont::DemiBold);
            p.setFont(tiny);
            p.setPen(is_exit_btn(XB_VIEW) ? QColor(0xfd, 0xe6, 0x8a) : QColor(0x9a, 0x9a, 0xaa));
            p.drawText(QRectF(view_pt.x() - 18, view_pt.y() + 12, 36, 13), Qt::AlignCenter,
                       QStringLiteral("View"));
            p.setPen(is_exit_btn(XB_MENU) ? QColor(0xfd, 0xe6, 0x8a) : QColor(0x9a, 0x9a, 0xaa));
            p.drawText(QRectF(menu_pt.x() - 18, menu_pt.y() + 12, 36, 13), Qt::AlignCenter,
                       QStringLiteral("Menu"));
            p.setPen(is_exit_btn(XB_GUIDE) ? QColor(0xfd, 0xe6, 0x8a) : QColor(0x9a, 0x9a, 0xaa));
            p.drawText(QRectF(guide_pt.x() - 24, guide_pt.y() - 29, 48, 12), Qt::AlignCenter,
                       QStringLiteral("Guide"));
        }

        // 4. Left Stick (LS) & Right Stick (RS)
        const QPointF ls_pt(cx - 94, cy - 22);
        const QPointF rs_pt(cx + 52, cy + 20);
        const auto draw_stick = [&](QPointF center, const QString& label, XboxOptionIndex idx) {
            const bool exit_hl = is_exit_btn(idx);
            const bool sel_hl = is_selected_btn(idx);
            draw_highlight_circle(center, 22.0, exit_hl, sel_hl);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x4d, 0x4d, 0x62)),
                          2.0));
            p.setBrush(QColor(0x1f, 0x1f, 0x28));
            p.drawEllipse(center, 21.0, 21.0);
            p.setPen(QPen(QColor(0x3a, 0x3a, 0x4c), 1.4));
            p.setBrush(exit_hl ? QColor(0x52, 0x2f, 0x0e) : QColor(0x2c, 0x2c, 0x39));
            p.drawEllipse(center, 14.0, 14.0);
            p.setFont(btn_font);
            p.setPen(QColor(0xdd, 0xdd, 0xe8));
            p.drawText(QRectF(center.x() - 14, center.y() - 8, 28, 16), Qt::AlignCenter, label);
            hit_regions.push_back(
                {QRectF(center.x() - 21, center.y() - 21, 42, 42), xbox_actions[idx].row_index});
        };
        draw_stick(ls_pt, QStringLiteral("LS"), XB_LS);
        draw_stick(rs_pt, QStringLiteral("RS"), XB_RS);

        // 5. D-Pad
        const QPointF dpad_pt(cx - 52, cy + 22);
        {
            p.setPen(QPen(QColor(0x4a, 0x4a, 0x5d), 1.2));
            p.setBrush(QColor(0x23, 0x23, 0x2f));
            p.drawRoundedRect(QRectF(dpad_pt.x() - 7, dpad_pt.y() - 20, 14, 40), 3.0, 3.0);
            p.drawRoundedRect(QRectF(dpad_pt.x() - 20, dpad_pt.y() - 7, 40, 14), 3.0, 3.0);
            p.setPen(Qt::NoPen);
            p.drawRect(QRectF(dpad_pt.x() - 6, dpad_pt.y() - 6, 12, 12));
        }

        // 6. Xbox Face Buttons Diamond (Top=Y Yellow, Left=X Blue, Right=B Red, Bottom=A Green)
        const QPointF face_center(cx + 98, cy - 22);
        const QPointF y_pt(face_center.x(), face_center.y() - 24);
        const QPointF x_pt(face_center.x() - 24, face_center.y());
        const QPointF b_pt(face_center.x() + 24, face_center.y());
        const QPointF a_pt(face_center.x(), face_center.y() + 24);

        const auto draw_face_btn = [&](QPointF pt, const QString& letter, QColor ring_color,
                                       QColor fill_color, XboxOptionIndex idx) {
            const bool sel_hl = is_selected_btn(idx);
            draw_highlight_circle(pt, 13.5, false, sel_hl);
            p.setPen(QPen(sel_hl ? QColor(0xe9, 0xd5, 0xff) : ring_color, 2.0));
            p.setBrush(fill_color);
            p.drawEllipse(pt, 12.5, 12.5);
            QFont f = font();
            f.setPixelSize(12);
            f.setWeight(QFont::Bold);
            p.setFont(f);
            p.setPen(ring_color.lighter(135));
            p.drawText(QRectF(pt.x() - 12, pt.y() - 12, 24, 24), Qt::AlignCenter, letter);
            hit_regions.push_back(
                {QRectF(pt.x() - 13, pt.y() - 13, 26, 26), xbox_actions[idx].row_index});
        };

        draw_face_btn(y_pt, QStringLiteral("Y"), QColor(0xea, 0xb3, 0x08), QColor(0x3b, 0x31, 0x10),
                      XB_Y);
        draw_face_btn(x_pt, QStringLiteral("X"), QColor(0x3b, 0x82, 0xf6), QColor(0x13, 0x29, 0x4b),
                      XB_X);
        draw_face_btn(b_pt, QStringLiteral("B"), QColor(0xef, 0x44, 0x44), QColor(0x45, 0x16, 0x19),
                      XB_B);
        draw_face_btn(a_pt, QStringLiteral("A"), QColor(0x22, 0xc5, 0x5e), QColor(0x11, 0x36, 0x20),
                      XB_A);

        // 7. Crisp Callout Cards with Leader Lines on Left & Right Margins!
        //    Shows: [Xbox Badge] -> [In-Game Action] (Switch Button)
        const auto draw_callout = [&](qreal x, qreal y, qreal box_w, bool on_left, QPointF anchor,
                                      XboxOptionIndex idx, QColor badge_color,
                                      const QString& badge_text) {
            const QRectF box(x, y, box_w, 32.0);
            const auto& info = xbox_actions[idx];
            const bool exit_hl = is_exit_btn(idx);
            const bool sel_hl = is_selected_btn(idx);

            // Leader line from callout edge to physical button on controller
            const QPointF line_start(on_left ? box.right() : box.left(), box.center().y());
            p.setPen(QPen(exit_hl ? QColor(0xf5, 0x9e, 0x0b, 200)
                                  : (sel_hl ? QColor(0xa8, 0x55, 0xf7, 220)
                                            : QColor(0x4d, 0x4d, 0x64, 175)),
                          exit_hl || sel_hl ? 1.8 : 1.1));
            const qreal elbow_x = on_left ? (box.right() + 14.0) : (box.left() - 14.0);
            p.drawLine(line_start, QPointF(elbow_x, line_start.y()));
            p.drawLine(QPointF(elbow_x, line_start.y()), anchor);

            // Callout pill box
            p.setPen(QPen(exit_hl ? QColor(0xf5, 0x9e, 0x0b)
                                  : (sel_hl ? QColor(0xa8, 0x55, 0xf7) : QColor(0x2e, 0x2e, 0x3d)),
                          exit_hl || sel_hl ? 1.6 : 1.0));
            p.setBrush(sel_hl ? QColor(0x2a, 0x20, 0x42)
                              : (exit_hl ? QColor(0x2b, 0x1e, 0x10) : QColor(0x1f, 0x1f, 0x29)));
            p.drawRoundedRect(box, 7.0, 7.0);

            // Small colored Xbox badge inside callout
            const QRectF badge_rect(box.left() + 6.0, box.top() + 6.0, 36.0, 20.0);
            p.setPen(QPen(badge_color, 1.2));
            p.setBrush(badge_color.darker(320));
            p.drawRoundedRect(badge_rect, 5.0, 5.0);
            p.setFont(btn_font);
            p.setPen(badge_color.lighter(135));
            p.drawText(badge_rect, Qt::AlignCenter, badge_text);

            // Action text (line 1) + Switch tag (line 2)
            QFont act_font = font();
            act_font.setPixelSize(11);
            act_font.setWeight(QFont::DemiBold);
            p.setFont(act_font);
            p.setPen(info.action_text.isEmpty() ? QColor(0x70, 0x70, 0x82) : QColor(0xec, 0xec, 0xf1));
            const QRectF text_top(badge_rect.right() + 7.0, box.top() + 2.0,
                                  box.right() - badge_rect.right() - 12.0, 16.0);
            p.drawText(text_top, Qt::AlignLeft | Qt::AlignVCenter,
                       info.action_text.isEmpty() ? tr("Not bound") : info.action_text);

            QFont sub_font = font();
            sub_font.setPixelSize(9);
            p.setFont(sub_font);
            p.setPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0x9a, 0x9a, 0xaa));
            QString sub = info.switch_tag.isEmpty() ? QString() : info.switch_tag;
            if (exit_hl) {
                sub = sub.isEmpty() ? tr("EXIT GAME") : (sub + QStringLiteral(" | EXIT GAME"));
            }
            const QRectF text_bot(badge_rect.right() + 7.0, box.top() + 17.0,
                                  box.right() - badge_rect.right() - 12.0, 13.0);
            p.drawText(text_bot, Qt::AlignLeft | Qt::AlignVCenter, sub);

            hit_regions.push_back({box, info.row_index});
        };

        const qreal col_w = std::clamp((w - 410.0) * 0.5, 165.0, 220.0);
        const qreal left_x = 14.0;
        const qreal right_x = w - 14.0 - col_w;

        // Left column callouts: LT, LB, LS, View, D-Pad
        draw_callout(left_x, 38.0, col_w, true, QPointF(lt_rect.left(), lt_rect.center().y()),
                     XB_LT, QColor(0x94, 0xa3, 0xb8), QStringLiteral("LT"));
        draw_callout(left_x, 82.0, col_w, true, QPointF(lb_rect.left(), lb_rect.center().y()),
                     XB_LB, QColor(0x94, 0xa3, 0xb8), QStringLiteral("LB"));
        draw_callout(left_x, 128.0, col_w, true, QPointF(ls_pt.x() - 20, ls_pt.y()), XB_LS,
                     QColor(0xa7, 0x8b, 0xfa), QStringLiteral("LS"));
        draw_callout(left_x, 178.0, col_w, true, QPointF(view_pt.x() - 10, view_pt.y() + 4),
                     XB_VIEW, QColor(0xf5, 0x9e, 0x0b), QStringLiteral("View"));
        draw_callout(left_x, 230.0, col_w, true, QPointF(dpad_pt.x() - 20, dpad_pt.y()), XB_DUP,
                     QColor(0x94, 0xa3, 0xb8), QStringLiteral("D-Pad"));

        // Right column callouts: RT, RB, Y (yellow), X (blue), B (red), A (green), Menu
        draw_callout(right_x, 34.0, col_w, false, QPointF(rt_rect.right(), rt_rect.center().y()),
                     XB_RT, QColor(0x94, 0xa3, 0xb8), QStringLiteral("RT"));
        draw_callout(right_x, 71.0, col_w, false, QPointF(rb_rect.right(), rb_rect.center().y()),
                     XB_RB, QColor(0x94, 0xa3, 0xb8), QStringLiteral("RB"));
        draw_callout(right_x, 108.0, col_w, false, QPointF(y_pt.x() + 10, y_pt.y() - 2), XB_Y,
                     QColor(0xea, 0xb3, 0x08), QStringLiteral("Y"));
        draw_callout(right_x, 145.0, col_w, false, QPointF(x_pt.x() + 8, x_pt.y() + 8), XB_X,
                     QColor(0x3b, 0x82, 0xf6), QStringLiteral("X"));
        draw_callout(right_x, 182.0, col_w, false, QPointF(b_pt.x() + 12, b_pt.y() + 2), XB_B,
                     QColor(0xef, 0x44, 0x44), QStringLiteral("B"));
        draw_callout(right_x, 219.0, col_w, false, QPointF(a_pt.x() + 10, a_pt.y() + 4), XB_A,
                     QColor(0x22, 0xc5, 0x5e), QStringLiteral("A"));
        draw_callout(right_x, 256.0, col_w, false, QPointF(menu_pt.x() + 10, menu_pt.y() + 6),
                     XB_MENU, QColor(0xf5, 0x9e, 0x0b), QStringLiteral("Menu"));

        // Bottom-center callout banner directly explaining the active Exit-Game Shortcut!
        {
            const QRectF exit_banner(cx - 148, h - 30, 296, 22);
            if (exit_shortcut != UISettings::ControllerExitShortcut::Off) {
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b), 1.0));
                p.setBrush(QColor(0x3a, 0x22, 0x0a));
                p.drawRoundedRect(exit_banner, 6.0, 6.0);
                QFont ef = font();
                ef.setPixelSize(10);
                ef.setWeight(QFont::Bold);
                p.setFont(ef);
                p.setPen(QColor(0xfd, 0xe6, 0x8a));
                p.drawText(exit_banner, Qt::AlignCenter,
                           tr("EXIT GAME: %1").arg(ControllerExitWatcher::ShortcutLabel(exit_shortcut)));
            }
        }
    }

private:
    struct HitRegion {
        QRectF rect;
        std::optional<std::size_t> row_index;
    };

    std::array<ActionInfo, kXboxOptions.size()> xbox_actions{};
    UISettings::ControllerExitShortcut exit_shortcut =
        UISettings::ControllerExitShortcut::ViewAndMenu;
    std::optional<std::size_t> selected_row;
    std::vector<HitRegion> hit_regions;
    std::function<void(std::size_t)> on_row_clicked;
};

class ButtonRemapperDialog : public QDialog {
public:
    ButtonRemapperDialog(QWidget* parent, Core::System& system_,
                         InputCommon::InputSubsystem* input_subsystem_, u64 program_id_,
                         const QString& game_title_, const std::string& game_path_)
        : QDialog(parent), system(system_), input_subsystem(input_subsystem_),
          program_id(program_id_), game_title(game_title_),
          config_name(ConfigNameForGame(program_id_, game_path_)) {
        const QChar emdash(0x2014);
        setWindowTitle(tr("Controls & Button Layout %1 %2").arg(emdash).arg(game_title));
        setMinimumSize(860, 720);
        resize(920, 760);
        setStyleSheet(QStringLiteral(
            "QDialog { background: #141418; color: #ececf1; }"
            "QLabel { color: #ececf1; }"
            "QFrame#Card { background: #1b1b22; border: 1px solid #2b2b36; border-radius: 10px; }"
            "QFrame#CardSelected { background: #261e3a; border: 1.5px solid #a855f7; border-radius: 10px; }"
            "QFrame#ExitCard { background: #221a12; border: 1px solid #6b441a; border-radius: 10px; }"
            "QComboBox { background: #23232e; color: #ececf1; border: 1px solid #353545; "
            "border-radius: 6px; padding: 4px 9px; min-height: 22px; }"
            "QComboBox::drop-down { border: none; width: 20px; }"
            "QComboBox QAbstractItemView { background: #1e1e27; color: #ececf1; "
            "selection-background-color: #7c3aed; border: 1px solid #353545; }"
            "QPushButton { background: #262633; color: #ececf1; border: 1px solid #3a3a4a; "
            "border-radius: 7px; padding: 6px 12px; font-weight: 500; }"
            "QPushButton:hover { background: #313142; border-color: #55556e; }"
            "QPushButton#PrimaryButton { background: #7c3aed; border-color: #8b5cf6; "
            "color: #ffffff; font-weight: 600; padding: 8px 18px; }"
            "QPushButton#PrimaryButton:hover { background: #8b5cf6; }"
            "QPushButton#PresetButton { background: #1f1f2b; border: 1px solid #3c3456; "
            "color: #d8b4fe; padding: 6px 11px; }"
            "QPushButton#PresetButton:hover { background: #2d2544; border-color: #7c3aed; }"
            "QPushButton#PressActive { background: #5b21b6; border-color: #a78bfa; color: #ffffff; }"));

        LoadState();
        BuildUi();
        RefreshAll();

        poll_timer = new QTimer(this);
        poll_timer->setInterval(25);
        connect(poll_timer, &QTimer::timeout, this, &ButtonRemapperDialog::PollNextInput);

        timeout_timer = new QTimer(this);
        timeout_timer->setSingleShot(true);
        connect(timeout_timer, &QTimer::timeout, this, [this] { CancelCapture(); });
    }

    ~ButtonRemapperDialog() override {
        CancelCapture();
    }

private:
    void LoadState() {
        global_players = Settings::values.players.GetValue(true);

        const bool was_global = Settings::values.players.UsingGlobal();
        const auto saved_custom = Settings::values.players.GetValue(false);
        {
            QtConfig per_game_cfg(config_name, Config::ConfigType::PerGameConfig);
            working_players = Settings::values.players.GetValue(false);
        }
        Settings::values.players.GetValue(false) = saved_custom;
        Settings::values.players.SetGlobal(was_global);

        for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
            sdl_ids[p] = ExtractSdlIdentity(global_players[p], p);
            if (!sdl_ids[p].valid) {
                sdl_ids[p] = ExtractSdlIdentity(working_players[p], p);
            }
            is_custom[p] = !working_players[p].profile_name.empty();
        }
    }

    UISettings::ControllerExitShortcut SelectedExitShortcut() const {
        if (!exit_shortcut_combo) {
            return static_cast<UISettings::ControllerExitShortcut>(
                UISettings::values.controller_exit_shortcut.GetValue());
        }
        return static_cast<UISettings::ControllerExitShortcut>(
            exit_shortcut_combo->currentData().toInt());
    }

    void BuildUi() {
        const QChar arrows(0x2194);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(20, 16, 20, 16);
        root->setSpacing(10);

        // Top Header: Game Title + Player Picker + Layout Status
        auto* header_row = new QHBoxLayout;
        auto* title_col = new QVBoxLayout;
        title_col->setSpacing(2);
        auto* heading = new QLabel(game_title, this);
        heading->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #ececf1;"));
        title_col->addWidget(heading);

        status_label = new QLabel(this);
        status_label->setStyleSheet(QStringLiteral("font-size: 12px; color: #9a9aa8;"));
        title_col->addWidget(status_label);
        header_row->addLayout(title_col, 1);

        player_combo = new QComboBox(this);
        for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
            QString label = tr("Player %1").arg(p + 1);
            if (!global_players[p].profile_name.empty()) {
                label += QStringLiteral(" (%1)")
                             .arg(QString::fromStdString(global_players[p].profile_name));
            }
            player_combo->addItem(label, static_cast<int>(p));
        }
        connect(player_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
                    CancelCapture();
                    current_player = static_cast<std::size_t>(std::clamp(idx, 0, 3));
                    selected_row.reset();
                    RefreshAll();
                });
        header_row->addWidget(player_combo);
        root->addLayout(header_row);

        // Visual Interactive Xbox Controller Diagram!
        diagram = new XboxControllerDiagram(this);
        diagram->SetClickHandler([this](std::size_t row_idx) { SelectRow(row_idx); });
        root->addWidget(diagram);

        // Console Exit-Game Shortcut Bar (prominently right under the diagram, updating its glow live!)
        auto* exit_card = new QFrame(this);
        exit_card->setObjectName(QStringLiteral("ExitCard"));
        auto* exit_row = new QHBoxLayout(exit_card);
        exit_row->setContentsMargins(14, 9, 14, 9);
        exit_row->setSpacing(12);

        auto* exit_info = new QVBoxLayout;
        exit_info->setSpacing(1);
        auto* exit_title = new QLabel(tr("Console Exit-Game Shortcut (Xbox Controller)"), exit_card);
        exit_title->setStyleSheet(
            QStringLiteral("font-size: 13px; font-weight: 600; color: #fde68a;"));
        auto* exit_desc = new QLabel(
            tr("Highlighted in orange on the controller above. Press while playing to exit back to "
               "Genesis Home or GameVault."),
            exit_card);
        exit_desc->setStyleSheet(QStringLiteral("font-size: 11px; color: #d6c5a3;"));
        exit_info->addWidget(exit_title);
        exit_info->addWidget(exit_desc);
        exit_row->addLayout(exit_info, 1);

        exit_shortcut_combo = new QComboBox(exit_card);
        exit_shortcut_combo->setMinimumWidth(290);
        const auto current_exit = UISettings::values.controller_exit_shortcut.GetValue();
        int exit_idx = 0;
        const auto shortcuts = ControllerExitWatcher::AllShortcuts();
        for (int i = 0; i < static_cast<int>(shortcuts.size()); ++i) {
            exit_shortcut_combo->addItem(shortcuts[i].second, static_cast<int>(shortcuts[i].first));
            if (static_cast<u8>(shortcuts[i].first) == current_exit) {
                exit_idx = i;
            }
        }
        exit_shortcut_combo->setCurrentIndex(exit_idx);
        connect(exit_shortcut_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { RefreshDiagram(); });
        exit_row->addWidget(exit_shortcut_combo);
        root->addWidget(exit_card);

        // Quick Presets Bar
        auto* preset_row = new QHBoxLayout;
        preset_row->setSpacing(8);

        auto* presets_lbl = new QLabel(tr("Quick Swap:"), this);
        presets_lbl->setStyleSheet(
            QStringLiteral("font-size: 12px; font-weight: 600; color: #9a9aa8;"));
        preset_row->addWidget(presets_lbl);

        auto* swap_xy = new QPushButton(tr("Swap X %1 Y").arg(arrows), this);
        swap_xy->setObjectName(QStringLiteral("PresetButton"));
        swap_xy->setToolTip(
            tr("Swaps Switch X and Switch Y (e.g. moves Mario Kart Look Back from Xbox Y to Xbox X)."));
        connect(swap_xy, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        });
        preset_row->addWidget(swap_xy);

        auto* swap_ab = new QPushButton(tr("Swap A %1 B").arg(arrows), this);
        swap_ab->setObjectName(QStringLiteral("PresetButton"));
        connect(swap_ab, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
        });
        preset_row->addWidget(swap_ab);

        auto* xbox_face = new QPushButton(tr("Swap All Face (A/B & X/Y)"), this);
        xbox_face->setObjectName(QStringLiteral("PresetButton"));
        connect(xbox_face, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
            SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        });
        preset_row->addWidget(xbox_face);

        preset_row->addStretch(1);

        auto* reset_btn = new QPushButton(tr("Reset Player to Global"), this);
        connect(reset_btn, &QPushButton::clicked, this, [this] { ResetCurrentPlayer(); });
        preset_row->addWidget(reset_btn);
        root->addLayout(preset_row);

        // Scrollable 2-Column Grid of In-Game Actions -> Xbox Button Picker
        action_scroll = new QScrollArea(this);
        action_scroll->setWidgetResizable(true);
        action_scroll->setFrameShape(QFrame::NoFrame);
        action_scroll->setStyleSheet(
            QStringLiteral("QScrollArea { background: transparent; border: none; }"));

        auto* grid_host = new QWidget(action_scroll);
        grid_host->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* grid = new QGridLayout(grid_host);
        grid->setContentsMargins(0, 0, 6, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(7);

        row_cards.resize(kSwitchButtons.size(), nullptr);
        row_combos.resize(kSwitchButtons.size(), nullptr);
        row_press_btns.resize(kSwitchButtons.size(), nullptr);

        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            const auto& spec = kSwitchButtons[i];
            auto* card = new QFrame(grid_host);
            card->setObjectName(QStringLiteral("Card"));
            auto* row = new QHBoxLayout(card);
            row->setContentsMargins(11, 7, 10, 7);
            row->setSpacing(8);

            auto* left = new QVBoxLayout;
            left->setSpacing(1);
            auto* action_lbl = new QLabel(GameActionLabel(program_id, game_title, spec), card);
            action_lbl->setStyleSheet(
                QStringLiteral("font-size: 13px; font-weight: 600; color: #ececf1;"));
            auto* sw_lbl = new QLabel(QString::fromLatin1(spec.switch_name), card);
            sw_lbl->setStyleSheet(QStringLiteral("font-size: 10px; color: #9a9aa8;"));
            left->addWidget(action_lbl);
            left->addWidget(sw_lbl);
            row->addLayout(left, 1);

            auto* combo = new QComboBox(card);
            combo->setMinimumWidth(175);
            for (std::size_t opt_i = 0; opt_i < kXboxOptions.size(); ++opt_i) {
                combo->addItem(QString::fromLatin1(kXboxOptions[opt_i].label),
                               static_cast<int>(opt_i));
            }
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, i](int combo_idx) { OnComboChanged(i, combo_idx); });
            row->addWidget(combo);
            row_combos[i] = combo;

            auto* press_btn = new QPushButton(tr("Press..."), card);
            press_btn->setFixedWidth(74);
            press_btn->setToolTip(tr("Click, then press any button on your Xbox controller."));
            connect(press_btn, &QPushButton::clicked, this, [this, i] { BeginCapture(i); });
            row->addWidget(press_btn);
            row_press_btns[i] = press_btn;

            row_cards[i] = card;
            grid->addWidget(card, static_cast<int>(i / 2), static_cast<int>(i % 2));
        }

        action_scroll->setWidget(grid_host);
        root->addWidget(action_scroll, 1);

        // Footer Save / Cancel
        auto* footer = new QHBoxLayout;
        auto* tip = new QLabel(
            tr("Changes save per game and work automatically from both Genesis Home and GameVault."),
            this);
        tip->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
        footer->addWidget(tip, 1);

        auto* cancel_btn = new QPushButton(tr("Cancel"), this);
        connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
        footer->addWidget(cancel_btn);

        auto* save_btn = new QPushButton(tr("Save Layout"), this);
        save_btn->setObjectName(QStringLiteral("PrimaryButton"));
        connect(save_btn, &QPushButton::clicked, this, &ButtonRemapperDialog::SaveAndAccept);
        footer->addWidget(save_btn);
        root->addLayout(footer);
    }

    void SelectRow(std::size_t row_idx) {
        if (row_idx >= row_cards.size()) {
            return;
        }
        selected_row = row_idx;
        for (std::size_t i = 0; i < row_cards.size(); ++i) {
            auto* card = row_cards[i];
            card->setObjectName(i == row_idx ? QStringLiteral("CardSelected")
                                             : QStringLiteral("Card"));
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
        if (action_scroll && row_cards[row_idx]) {
            action_scroll->ensureWidgetVisible(row_cards[row_idx], 16, 16);
        }
        if (row_combos[row_idx]) {
            row_combos[row_idx]->setFocus();
        }
        RefreshDiagram();
    }

    void RefreshDiagram() {
        if (diagram) {
            diagram->UpdateState(working_players[current_player], program_id, game_title,
                                 SelectedExitShortcut(), selected_row);
        }
    }

    void UpdateStatusBanner() {
        const QChar check(0x2713);
        const bool custom = !ButtonsEqualGlobal(working_players[current_player],
                                                global_players[current_player]);
        is_custom[current_player] = custom;
        if (custom) {
            status_label->setText(
                tr("%1 Custom per-game layout active for Player %2")
                    .arg(check)
                    .arg(current_player + 1));
            status_label->setStyleSheet(
                QStringLiteral("font-size: 12px; font-weight: 600; color: #6ee7b7;"));
        } else {
            status_label->setText(
                tr("Player %1 is using your global Xbox layout - pick any action below or click "
                   "the controller to change it.")
                    .arg(current_player + 1));
            status_label->setStyleSheet(QStringLiteral("font-size: 12px; color: #9a9aa8;"));
        }
    }

    void RefreshAll() {
        const auto& player = working_players[current_player];
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            auto* combo = row_combos[i];
            const QSignalBlocker blocker(combo);

            while (combo->count() > static_cast<int>(kXboxOptions.size())) {
                combo->removeItem(combo->count() - 1);
            }

            const std::string& binding = player.buttons[kSwitchButtons[i].id];
            if (const auto opt = MatchXboxOption(binding); opt.has_value()) {
                combo->setCurrentIndex(static_cast<int>(*opt));
            } else {
                combo->addItem(DescribeCustomBinding(binding), -1);
                combo->setCurrentIndex(combo->count() - 1);
            }
        }
        UpdateStatusBanner();
        RefreshDiagram();
    }

    void OnComboChanged(std::size_t row_index, int combo_idx) {
        if (row_index >= kSwitchButtons.size() || combo_idx < 0) {
            return;
        }
        auto* combo = row_combos[row_index];
        const int opt_idx = combo->itemData(combo_idx).toInt();
        if (opt_idx < 0 || static_cast<std::size_t>(opt_idx) >= kXboxOptions.size()) {
            return;
        }
        const auto& opt = kXboxOptions[static_cast<std::size_t>(opt_idx)];
        working_players[current_player].buttons[kSwitchButtons[row_index].id] =
            BuildXboxBinding(opt, sdl_ids[current_player], current_player);
        selected_row = row_index;
        UpdateStatusBanner();
        RefreshDiagram();
    }

    void SwapButtons(Settings::NativeButton::Values a, Settings::NativeButton::Values b) {
        CancelCapture();
        auto& buttons = working_players[current_player].buttons;
        std::swap(buttons[a], buttons[b]);
        RefreshAll();
    }

    void ResetCurrentPlayer() {
        CancelCapture();
        working_players[current_player] = global_players[current_player];
        working_players[current_player].profile_name.clear();
        selected_row.reset();
        RefreshAll();
    }

    void BeginCapture(std::size_t row_index) {
        if (!input_subsystem || row_index >= row_press_btns.size()) {
            return;
        }
        if (capturing_row.has_value() && *capturing_row == row_index) {
            CancelCapture();
            return;
        }
        CancelCapture();
        SelectRow(row_index);
        capturing_row = row_index;
        auto* btn = row_press_btns[row_index];
        btn->setText(tr("Waiting..."));
        btn->setObjectName(QStringLiteral("PressActive"));
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);

        input_subsystem->BeginMapping(InputCommon::Polling::InputType::Button);
        poll_timer->start();
        timeout_timer->start(4000);
    }

    void CancelCapture() {
        if (poll_timer) {
            poll_timer->stop();
        }
        if (timeout_timer) {
            timeout_timer->stop();
        }
        if (capturing_row.has_value()) {
            if (input_subsystem) {
                input_subsystem->StopMapping();
            }
            auto* btn = row_press_btns[*capturing_row];
            btn->setText(tr("Press..."));
            btn->setObjectName(QString());
            btn->style()->unpolish(btn);
            btn->style()->polish(btn);
            capturing_row.reset();
        }
    }

    void PollNextInput() {
        if (!input_subsystem || !capturing_row.has_value()) {
            CancelCapture();
            return;
        }
        const auto params = input_subsystem->GetNextInput();
        if (!params.Has("engine") || params.Get("engine", "") == "keyboard") {
            if (params.Get("engine", "") == "keyboard" && params.Get("code", 0) == Qt::Key_Escape) {
                CancelCapture();
            }
            return;
        }
        const std::size_t row_index = *capturing_row;
        const std::string serialized = params.Serialize();
        CancelCapture();

        working_players[current_player].buttons[kSwitchButtons[row_index].id] = serialized;
        if (!sdl_ids[current_player].valid) {
            sdl_ids[current_player] = ExtractSdlIdentity(working_players[current_player],
                                                         current_player);
        }
        RefreshAll();
    }

    void SaveAndAccept() {
        CancelCapture();

        if (exit_shortcut_combo) {
            UISettings::values.controller_exit_shortcut.SetValue(
                static_cast<u8>(exit_shortcut_combo->currentData().toInt()));
        }

        const bool was_global = Settings::values.players.UsingGlobal();
        const auto saved_custom = Settings::values.players.GetValue(false);

        {
            QtConfig per_game_cfg(config_name, Config::ConfigType::PerGameConfig);
            Settings::values.players.SetGlobal(false);
            auto& custom_players = Settings::values.players.GetValue(false);
            for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
                const bool custom = !ButtonsEqualGlobal(working_players[p], global_players[p]);
                if (custom) {
                    custom_players[p] = working_players[p];
                    custom_players[p].profile_name = CustomPerGameLayoutProfile;
                    custom_players[p].connected = global_players[p].connected;
                } else if (custom_players[p].profile_name == CustomPerGameLayoutProfile ||
                           custom_players[p].profile_name.empty()) {
                    custom_players[p] = global_players[p];
                    custom_players[p].profile_name.clear();
                }
            }
            per_game_cfg.ClearControlPlayerValues();
            per_game_cfg.SaveAllValues();
        }

        if (system.IsPoweredOn() && system.GetApplicationProcessProgramID() == program_id) {
            Settings::values.players.SetGlobal(false);
            system.HIDCore().ReloadInputDevices();
        } else {
            Settings::values.players.GetValue(false) = saved_custom;
            Settings::values.players.SetGlobal(was_global);
        }

        accept();
    }

    Core::System& system;
    InputCommon::InputSubsystem* input_subsystem = nullptr;
    u64 program_id = 0;
    QString game_title;
    std::string config_name;

    using PlayersList = std::remove_cvref_t<decltype(Settings::values.players.GetValue(true))>;
    PlayersList global_players{};
    PlayersList working_players{};
    std::array<SdlDeviceIdentity, NumRemappablePlayers> sdl_ids{};
    std::array<bool, NumRemappablePlayers> is_custom{};
    std::size_t current_player = 0;
    std::optional<std::size_t> selected_row;

    QComboBox* player_combo = nullptr;
    QLabel* status_label = nullptr;
    XboxControllerDiagram* diagram = nullptr;
    QComboBox* exit_shortcut_combo = nullptr;
    QScrollArea* action_scroll = nullptr;
    std::vector<QFrame*> row_cards;
    std::vector<QComboBox*> row_combos;
    std::vector<QPushButton*> row_press_btns;

    std::optional<std::size_t> capturing_row;
    QTimer* poll_timer = nullptr;
    QTimer* timeout_timer = nullptr;
};

} // namespace

bool ShowButtonRemapperDialog(QWidget* parent, Core::System& system,
                              InputCommon::InputSubsystem* input_subsystem, u64 program_id,
                              const QString& game_title, const std::string& game_path) {
    ButtonRemapperDialog dialog(parent, system, input_subsystem, program_id, game_title, game_path);
    return dialog.exec() == QDialog::Accepted;
}

} // namespace FritchiiUi
