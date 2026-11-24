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

enum class ControllerFamily {
    Xbox,
    PlayStation,
    SwitchPro,
};

struct SwitchButtonSpec {
    Settings::NativeButton::Values id;
    const char* switch_name;
    const char* default_action;
    bool is_core; // Core gameplay actions stay visible; extra/D-Pad toggle cleanly
};

constexpr std::array<SwitchButtonSpec, 17> kSwitchButtons{{
    {Settings::NativeButton::A, "Switch A", "Confirm / Primary Action", true},
    {Settings::NativeButton::B, "Switch B", "Back / Secondary Action", true},
    {Settings::NativeButton::X, "Switch X", "Top Face Action", true},
    {Settings::NativeButton::Y, "Switch Y", "Left Face Action", true},
    {Settings::NativeButton::L, "Switch L", "Left Bumper", true},
    {Settings::NativeButton::R, "Switch R", "Right Bumper", true},
    {Settings::NativeButton::ZL, "Switch ZL", "Left Trigger", true},
    {Settings::NativeButton::ZR, "Switch ZR", "Right Trigger", true},
    {Settings::NativeButton::Plus, "Switch Plus (+)", "Pause / Menu", true},
    {Settings::NativeButton::Minus, "Switch Minus (-)", "Map / Select", true},
    {Settings::NativeButton::LStick, "Switch L-Stick Click", "Left Stick Press", false},
    {Settings::NativeButton::RStick, "Switch R-Stick Click", "Right Stick Press", false},
    {Settings::NativeButton::DUp, "Switch D-Pad Up", "D-Pad Up", false},
    {Settings::NativeButton::DDown, "Switch D-Pad Down", "D-Pad Down", false},
    {Settings::NativeButton::DLeft, "Switch D-Pad Left", "D-Pad Left", false},
    {Settings::NativeButton::DRight, "Switch D-Pad Right", "D-Pad Right", false},
    {Settings::NativeButton::Screenshot, "Switch Capture", "Screenshot", false},
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
            return QObject::tr("Look Back (Rear View)");
        case Settings::NativeButton::Y:
            return QObject::tr("Accelerate (Alt)");
        case Settings::NativeButton::L:
        case Settings::NativeButton::ZL:
            return QObject::tr("Use / Hold Item");
        case Settings::NativeButton::R:
        case Settings::NativeButton::ZR:
            return QObject::tr("Hop / Drift / Trick");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause Menu");
        case Settings::NativeButton::Minus:
            return QObject::tr("Mini-Map / Info");
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
            return QObject::tr("Lob / Drop Shot");
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

enum class PadInputKind {
    Button,
    AxisPositive,
    Hat,
    Unbound,
};

enum PadOptionIndex : std::size_t {
    PAD_SOUTH = 0, // Xbox A / PS Cross / Switch B
    PAD_EAST = 1,  // Xbox B / PS Circle / Switch A
    PAD_WEST = 2,  // Xbox X / PS Square / Switch Y
    PAD_NORTH = 3, // Xbox Y / PS Triangle / Switch X
    PAD_LB = 4,
    PAD_RB = 5,
    PAD_LT = 6,
    PAD_RT = 7,
    PAD_VIEW = 8,
    PAD_MENU = 9,
    PAD_LS = 10,
    PAD_RS = 11,
    PAD_DUP = 12,
    PAD_DDOWN = 13,
    PAD_DLEFT = 14,
    PAD_DRIGHT = 15,
    PAD_GUIDE = 16,
    PAD_UNBOUND = 17,
};

struct PadOption {
    PadInputKind kind;
    int code;
    const char* direction;
    const char* xbox_label;
    const char* xbox_short;
    const char* ps_label;
    const char* ps_short;
    const char* sw_label;
    const char* sw_short;
};

constexpr std::array<PadOption, 18> kPadOptions{{
    {PadInputKind::Button, 0, "", "Xbox A (Green - Bottom)", "A", "Cross (Bottom)", "X",
     "Switch B (Bottom)", "B"},
    {PadInputKind::Button, 1, "", "Xbox B (Red - Right)", "B", "Circle (Right)", "O",
     "Switch A (Right)", "A"},
    {PadInputKind::Button, 2, "", "Xbox X (Blue - Left)", "X", "Square (Left)", "Sq",
     "Switch Y (Left)", "Y"},
    {PadInputKind::Button, 3, "", "Xbox Y (Yellow - Top)", "Y", "Triangle (Top)", "Tri",
     "Switch X (Top)", "X"},
    {PadInputKind::Button, 4, "", "LB (Left Bumper)", "LB", "L1 (Left Bumper)", "L1",
     "L (Left Bumper)", "L"},
    {PadInputKind::Button, 5, "", "RB (Right Bumper)", "RB", "R1 (Right Bumper)", "R1",
     "R (Right Bumper)", "R"},
    {PadInputKind::AxisPositive, 2, "", "LT (Left Trigger)", "LT", "L2 (Left Trigger)", "L2",
     "ZL (Left Trigger)", "ZL"},
    {PadInputKind::AxisPositive, 5, "", "RT (Right Trigger)", "RT", "R2 (Right Trigger)", "R2",
     "ZR (Right Trigger)", "ZR"},
    {PadInputKind::Button, 6, "", "View (Small Center-Left)", "View", "Create / Share", "Create",
     "Minus (-)", "-"},
    {PadInputKind::Button, 7, "", "Menu (Small Center-Right)", "Menu", "Options", "Options",
     "Plus (+)", "+"},
    {PadInputKind::Button, 8, "", "Left Stick Click (LS)", "LS", "L3 (Left Stick Click)", "L3",
     "Left Stick Click", "LS"},
    {PadInputKind::Button, 9, "", "Right Stick Click (RS)", "RS", "R3 (Right Stick Click)", "R3",
     "Right Stick Click", "RS"},
    {PadInputKind::Hat, 0, "up", "D-Pad Up", "D-Up", "D-Pad Up", "D-Up", "D-Pad Up", "D-Up"},
    {PadInputKind::Hat, 0, "down", "D-Pad Down", "D-Down", "D-Pad Down", "D-Down", "D-Pad Down",
     "D-Down"},
    {PadInputKind::Hat, 0, "left", "D-Pad Left", "D-Left", "D-Pad Left", "D-Left", "D-Pad Left",
     "D-Left"},
    {PadInputKind::Hat, 0, "right", "D-Pad Right", "D-Right", "D-Pad Right", "D-Right",
     "D-Pad Right", "D-Right"},
    {PadInputKind::Button, 10, "", "Xbox Guide (Center Logo)", "Guide", "PS Button (Center)", "PS",
     "Home Button", "Home"},
    {PadInputKind::Unbound, -1, "", "Unbound (None)", "None", "Unbound (None)", "None",
     "Unbound (None)", "None"},
}};

QString OptionLabel(std::size_t idx, ControllerFamily family) {
    const auto& opt = kPadOptions[idx];
    switch (family) {
    case ControllerFamily::PlayStation:
        return QString::fromLatin1(opt.ps_label);
    case ControllerFamily::SwitchPro:
        return QString::fromLatin1(opt.sw_label);
    case ControllerFamily::Xbox:
        return QString::fromLatin1(opt.xbox_label);
    }
    return QString::fromLatin1(opt.xbox_label);
}

QString OptionShort(std::size_t idx, ControllerFamily family) {
    const auto& opt = kPadOptions[idx];
    switch (family) {
    case ControllerFamily::PlayStation:
        return QString::fromLatin1(opt.ps_short);
    case ControllerFamily::SwitchPro:
        return QString::fromLatin1(opt.sw_short);
    case ControllerFamily::Xbox:
        return QString::fromLatin1(opt.xbox_short);
    }
    return QString::fromLatin1(opt.xbox_short);
}

QString FamilyDisplayName(ControllerFamily family) {
    switch (family) {
    case ControllerFamily::Xbox:
        return QObject::tr("Xbox Wireless Controller");
    case ControllerFamily::PlayStation:
        return QObject::tr("PlayStation Controller");
    case ControllerFamily::SwitchPro:
        return QObject::tr("Nintendo Switch Pro Controller");
    }
    return QObject::tr("Xbox Wireless Controller");
}

struct SdlDeviceIdentity {
    std::string guid;
    int port = 0;
    bool valid = false;
    ControllerFamily family = ControllerFamily::Xbox;
    QString device_name;
};

ControllerFamily DetectFamilyFromGuidOrName(const std::string& guid, const QString& name) {
    // SDL GUID bytes 4-5 (chars 8..11) store USB Vendor ID in little-endian hex:
    //  0x045E -> "5e04" (Microsoft Xbox)
    //  0x054C -> "4c05" (Sony PlayStation)
    //  0x057E -> "7e05" (Nintendo Switch Pro / Joy-Con)
    if (guid.size() >= 12) {
        std::string vid_le = guid.substr(8, 4);
        std::transform(vid_le.begin(), vid_le.end(), vid_le.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (vid_le == "4c05") {
            return ControllerFamily::PlayStation;
        }
        if (vid_le == "7e05") {
            return ControllerFamily::SwitchPro;
        }
        if (vid_le == "5e04") {
            return ControllerFamily::Xbox;
        }
    }
    const QString lower = name.toLower();
    if (lower.contains(QStringLiteral("dualsense")) ||
        lower.contains(QStringLiteral("dualshock")) ||
        lower.contains(QStringLiteral("playstation")) ||
        lower.contains(QStringLiteral("ps4")) || lower.contains(QStringLiteral("ps5"))) {
        return ControllerFamily::PlayStation;
    }
    if (lower.contains(QStringLiteral("pro controller")) ||
        lower.contains(QStringLiteral("nintendo")) || lower.contains(QStringLiteral("joy-con"))) {
        return ControllerFamily::SwitchPro;
    }
    return ControllerFamily::Xbox;
}

SdlDeviceIdentity ExtractSdlIdentity(const Settings::PlayerInput& player, std::size_t player_index,
                                     InputCommon::InputSubsystem* input_subsystem) {
    SdlDeviceIdentity id;
    id.port = static_cast<int>(player_index);

    for (const auto& binding : player.buttons) {
        if (binding.empty() || binding == "[empty]") {
            continue;
        }
        Common::ParamPackage param(binding);
        if (param.Get("engine", "") == "sdl") {
            id.guid = param.Get("guid", "");
            id.port = param.Get("port", static_cast<int>(player_index));
            id.valid = !id.guid.empty();
            if (id.valid) {
                break;
            }
        }
    }

    // Match against currently connected SDL devices for live name & family detection
    if (input_subsystem != nullptr) {
        const auto devices = input_subsystem->GetInputDevices();
        for (const auto& dev : devices) {
            if (dev.Get("engine", "") != "sdl") {
                continue;
            }
            const std::string dev_guid = dev.Get("guid", "");
            const int dev_port = dev.Get("port", -1);
            const bool match = (id.valid && dev_guid == id.guid && dev_port == id.port) ||
                               (!id.valid && dev_port == static_cast<int>(player_index));
            if (match) {
                id.guid = dev_guid;
                id.port = dev_port;
                id.valid = !dev_guid.empty();
                id.device_name = QString::fromStdString(dev.Get("display", ""));
                break;
            }
        }
    }

    id.family = DetectFamilyFromGuidOrName(id.guid, id.device_name);
    if (id.device_name.isEmpty()) {
        id.device_name = FamilyDisplayName(id.family);
    }
    return id;
}

std::optional<std::size_t> MatchPadOption(const std::string& binding) {
    if (binding.empty() || binding == "[empty]") {
        return PAD_UNBOUND;
    }
    Common::ParamPackage param(binding);
    if (param.Get("engine", "") != "sdl") {
        return std::nullopt;
    }
    if (param.Has("button")) {
        const int btn = param.Get("button", -1);
        for (std::size_t i = 0; i < kPadOptions.size(); ++i) {
            if (kPadOptions[i].kind == PadInputKind::Button && kPadOptions[i].code == btn) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("axis")) {
        const int axis = param.Get("axis", -1);
        for (std::size_t i = 0; i < kPadOptions.size(); ++i) {
            if (kPadOptions[i].kind == PadInputKind::AxisPositive && kPadOptions[i].code == axis) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("hat")) {
        const std::string dir = param.Get("direction", "");
        for (std::size_t i = 0; i < kPadOptions.size(); ++i) {
            if (kPadOptions[i].kind == PadInputKind::Hat && dir == kPadOptions[i].direction) {
                return i;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string BuildPadBinding(const PadOption& opt, const SdlDeviceIdentity& id,
                            std::size_t player_index) {
    if (opt.kind == PadInputKind::Unbound) {
        return "[empty]";
    }
    Common::ParamPackage param;
    param.Set("engine", "sdl");
    param.Set("port", id.valid ? id.port : static_cast<int>(player_index));
    if (id.valid && !id.guid.empty()) {
        param.Set("guid", id.guid);
    }
    switch (opt.kind) {
    case PadInputKind::Button:
        param.Set("button", opt.code);
        break;
    case PadInputKind::AxisPositive:
        param.Set("axis", opt.code);
        param.Set("invert", "+");
        param.Set("threshold", "0.500000");
        break;
    case PadInputKind::Hat:
        param.Set("hat", 0);
        param.Set("direction", opt.direction);
        break;
    case PadInputKind::Unbound:
        return "[empty]";
    }
    return param.Serialize();
}

QString DescribeCustomBinding(const std::string& binding, ControllerFamily family) {
    if (binding.empty() || binding == "[empty]") {
        return QObject::tr("Unbound (None)");
    }
    if (const auto idx = MatchPadOption(binding); idx.has_value()) {
        return OptionLabel(*idx, family);
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

/// Official-proportion interactive controller visual that adapts automatically to the connected
/// player's controller (Xbox Series X|S, PlayStation DualSense, or Nintendo Switch Pro).
/// Clean, spiderweb-free layout: Top-edge shoulders (LT/LB/RB/RT) above an official front view,
/// with purple selection glow and orange Exit-Game shortcut glow. Clicking any physical button
/// highlights and selects its in-game action card on the right.
class OfficialControllerVisual : public QWidget {
public:
    struct ButtonBindingSummary {
        QString action_text;
        QString switch_name;
        std::optional<std::size_t> row_index;
    };

    explicit OfficialControllerVisual(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(410, 385);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    void SetClickHandler(std::function<void(std::size_t row_index)> handler) {
        on_row_clicked = std::move(handler);
    }

    void UpdateState(const Settings::PlayerInput& player, ControllerFamily family_,
                     const QString& profile_badge_, u64 program_id, const QString& game_title,
                     UISettings::ControllerExitShortcut exit_shortcut_,
                     std::optional<std::size_t> selected_row_) {
        family = family_;
        profile_badge = profile_badge_;
        exit_shortcut = exit_shortcut_;
        selected_row = selected_row_;
        for (auto& info : pad_actions) {
            info = {};
        }

        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            const auto& spec = kSwitchButtons[i];
            const auto opt = MatchPadOption(player.buttons[spec.id]);
            if (!opt.has_value() || *opt >= PAD_UNBOUND) {
                continue;
            }
            auto& slot = pad_actions[*opt];
            const QString act = GameActionLabel(program_id, game_title, spec);
            const QString sw = QString::fromLatin1(spec.switch_name);
            if (slot.action_text.isEmpty()) {
                slot.action_text = act;
                slot.switch_name = sw;
                slot.row_index = i;
            } else {
                if (!slot.action_text.contains(act)) {
                    slot.action_text += QStringLiteral(" / ") + act;
                }
            }
        }
        update();
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        const QPointF pt = event->position();
        std::optional<PadOptionIndex> next_hover;
        for (const auto& hit : hit_regions) {
            if (hit.rect.contains(pt)) {
                next_hover = hit.pad_index;
                break;
            }
        }
        if (next_hover != hovered_pad) {
            hovered_pad = next_hover;
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        if (hovered_pad.has_value()) {
            hovered_pad.reset();
            update();
        }
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        const QPointF pt = event->position();
        for (const auto& hit : hit_regions) {
            if (hit.rect.contains(pt)) {
                if (hit.row_index.has_value() && on_row_clicked) {
                    on_row_clicked(*hit.row_index);
                }
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

        // Card container
        p.setPen(QPen(QColor(0x2b, 0x2b, 0x36), 1.0));
        p.setBrush(QColor(0x18, 0x18, 0x20));
        p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 12.0, 12.0);

        // Top badge showing detected profile + controller family
        {
            QFont hdr = font();
            hdr.setPixelSize(11);
            hdr.setWeight(QFont::DemiBold);
            p.setFont(hdr);
            p.setPen(QColor(0xd8, 0xb4, 0xfe));
            p.drawText(QRectF(16, 10, w - 32, 18), Qt::AlignCenter, profile_badge);
        }

        const auto is_exit_pad = [&](PadOptionIndex idx) {
            switch (exit_shortcut) {
            case UISettings::ControllerExitShortcut::ViewAndMenu:
                return idx == PAD_VIEW || idx == PAD_MENU;
            case UISettings::ControllerExitShortcut::GuideButton:
            case UISettings::ControllerExitShortcut::HoldGuideButton:
                return idx == PAD_GUIDE;
            case UISettings::ControllerExitShortcut::LbRbViewMenu:
                return idx == PAD_LB || idx == PAD_RB || idx == PAD_VIEW || idx == PAD_MENU;
            case UISettings::ControllerExitShortcut::ClickBothSticks:
                return idx == PAD_LS || idx == PAD_RS;
            case UISettings::ControllerExitShortcut::Off:
                return false;
            }
            return false;
        };

        const auto is_selected_pad = [&](PadOptionIndex idx) {
            return selected_row.has_value() && pad_actions[idx].row_index.has_value() &&
                   *pad_actions[idx].row_index == *selected_row;
        };

        const auto is_hovered_pad = [&](PadOptionIndex idx) {
            return hovered_pad.has_value() && *hovered_pad == idx;
        };

        const auto draw_ring_rect = [&](const QRectF& r, qreal rad, PadOptionIndex idx) {
            if (is_exit_pad(idx)) {
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b, 175), 5.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2.5, -2.5, 2.5, 2.5), rad + 2.5, rad + 2.5);
            }
            if (is_selected_pad(idx) || is_hovered_pad(idx)) {
                p.setPen(QPen(QColor(0xa8, 0x55, 0xf7, 210), 3.6));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2.0, -2.0, 2.0, 2.0), rad + 2.0, rad + 2.0);
            }
        };

        const auto draw_ring_circle = [&](QPointF c, qreal r, PadOptionIndex idx) {
            if (is_exit_pad(idx)) {
                p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b, 175), 5.0));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, r + 3.2, r + 3.2);
            }
            if (is_selected_pad(idx) || is_hovered_pad(idx)) {
                p.setPen(QPen(QColor(0xa8, 0x55, 0xf7, 215), 3.6));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, r + 2.5, r + 2.5);
            }
        };

        QFont btn_font = font();
        btn_font.setPixelSize(11);
        btn_font.setWeight(QFont::Bold);

        QFont tiny_font = font();
        tiny_font.setPixelSize(9);
        tiny_font.setWeight(QFont::DemiBold);

        // =========================================================================
        // 1. TOP EDGE VIEW: Triggers & Bumpers (LT/LB - USB-C - RB/RT)
        // =========================================================================
        const qreal top_y = 38.0;
        const QRectF lt_rect(cx - 148, top_y, 58, 24);
        const QRectF lb_rect(cx - 82, top_y + 2, 54, 22);
        const QRectF rb_rect(cx + 28, top_y + 2, 54, 22);
        const QRectF rt_rect(cx + 90, top_y, 58, 24);

        const auto draw_top_btn = [&](const QRectF& rect, PadOptionIndex idx) {
            draw_ring_rect(rect, 8.0, idx);
            const bool exit_hl = is_exit_pad(idx);
            const bool sel_hl = is_selected_pad(idx) || is_hovered_pad(idx);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x4b, 0x4b, 0x60)),
                          1.4));
            p.setBrush(exit_hl ? QColor(0x52, 0x2f, 0x0d)
                               : (sel_hl ? QColor(0x32, 0x23, 0x50) : QColor(0x28, 0x28, 0x34)));
            p.drawRoundedRect(rect, 8.0, 8.0);
            p.setFont(btn_font);
            p.setPen(QColor(0xec, 0xec, 0xf1));
            p.drawText(rect, Qt::AlignCenter, OptionShort(idx, family));
            hit_regions.push_back({rect, idx, pad_actions[idx].row_index});
        };

        draw_top_btn(lt_rect, PAD_LT);
        draw_top_btn(lb_rect, PAD_LB);
        // Subtle USB-C port indicator in center
        p.setPen(QPen(QColor(0x45, 0x45, 0x56), 1.2));
        p.setBrush(QColor(0x13, 0x13, 0x19));
        p.drawRoundedRect(QRectF(cx - 12, top_y + 8, 24, 9), 4.0, 4.0);
        draw_top_btn(rb_rect, PAD_RB);
        draw_top_btn(rt_rect, PAD_RT);

        // =========================================================================
        // 2. OFFICIAL FRONT VIEW SILHOUETTE (centered around cx, cy)
        // =========================================================================
        const qreal cy = 186.0;
        {
            QPainterPath body;
            if (family == ControllerFamily::PlayStation) {
                // Sleek DualSense silhouette
                body.moveTo(cx - 108, cy - 82);
                body.cubicTo(cx - 56, cy - 90, cx + 56, cy - 90, cx + 108, cy - 82);
                body.cubicTo(cx + 150, cy - 72, cx + 172, cy - 16, cx + 176, cy + 52);
                body.cubicTo(cx + 178, cy + 98, cx + 150, cy + 122, cx + 124, cy + 114);
                body.cubicTo(cx + 100, cy + 106, cx + 88, cy + 58, cx + 62, cy + 54);
                body.lineTo(cx - 62, cy + 54);
                body.cubicTo(cx - 88, cy + 58, cx - 100, cy + 106, cx - 124, cy + 114);
                body.cubicTo(cx - 150, cy + 122, cx - 178, cy + 98, cx - 176, cy + 52);
                body.cubicTo(cx - 172, cy - 16, cx - 150, cy - 72, cx - 108, cy - 82);
            } else {
                // Official Xbox Series X|S ergonomic sculpted silhouette (also fits Switch Pro)
                body.moveTo(cx - 110, cy - 80);
                body.cubicTo(cx - 56, cy - 94, cx + 56, cy - 94, cx + 110, cy - 80);
                body.cubicTo(cx + 148, cy - 68, cx + 174, cy - 18, cx + 178, cy + 44);
                body.cubicTo(cx + 181, cy + 96, cx + 156, cy + 122, cx + 130, cy + 116);
                body.cubicTo(cx + 102, cy + 109, cx + 88, cy + 62, cx + 58, cy + 56);
                body.cubicTo(cx + 22, cy + 52, cx - 22, cy + 52, cx - 58, cy + 56);
                body.cubicTo(cx - 88, cy + 62, cx - 102, cy + 109, cx - 130, cy + 116);
                body.cubicTo(cx - 156, cy + 122, cx - 181, cy + 96, cx - 178, cy + 44);
                body.cubicTo(cx - 174, cy - 18, cx - 148, cy - 68, cx - 110, cy - 80);
            }
            body.closeSubpath();

            QLinearGradient body_grad(cx, cy - 94, cx, cy + 120);
            body_grad.setColorAt(0.0, QColor(0x2d, 0x2d, 0x39));
            body_grad.setColorAt(0.55, QColor(0x23, 0x23, 0x2e));
            body_grad.setColorAt(1.0, QColor(0x1b, 0x1b, 0x24));
            p.setPen(QPen(QColor(0x4a, 0x4a, 0x5f), 1.8));
            p.setBrush(body_grad);
            p.drawPath(body);

            // Subtle ergonomic grip shading
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 7));
            p.drawEllipse(QPointF(cx, cy - 42), 112.0, 44.0);
        }

        // =========================================================================
        // 3. CENTER CLUSTER (Official Xbox: Guide top, View lower-left, Menu lower-right, Share bottom)
        // =========================================================================
        if (family == ControllerFamily::PlayStation) {
            // PlayStation center touchpad
            p.setPen(QPen(QColor(0x4c, 0x4c, 0x62), 1.3));
            p.setBrush(QColor(0x1d, 0x1d, 0x27));
            p.drawRoundedRect(QRectF(cx - 46, cy - 78, 92, 50), 8.0, 8.0);
        }

        const QPointF guide_pt = (family == ControllerFamily::PlayStation)
                                     ? QPointF(cx, cy + 20)
                                     : QPointF(cx, cy - 52);
        const QPointF view_pt = (family == ControllerFamily::PlayStation)
                                    ? QPointF(cx - 60, cy - 58)
                                    : QPointF(cx - 32, cy - 22);
        const QPointF menu_pt = (family == ControllerFamily::PlayStation)
                                    ? QPointF(cx + 60, cy - 58)
                                    : QPointF(cx + 32, cy - 22);

        // Guide / Home / PS Button
        {
            const qreal gr = (family == ControllerFamily::Xbox) ? 17.5 : 13.0;
            draw_ring_circle(guide_pt, gr + 0.5, PAD_GUIDE);
            const bool exit_hl = is_exit_pad(PAD_GUIDE);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24) : QColor(0x88, 0x88, 0xa0), 2.0));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e) : QColor(0x20, 0x20, 0x2a));
            p.drawEllipse(guide_pt, gr, gr);
            if (family == ControllerFamily::Xbox) {
                // Iconic Xbox Guide X orb
                p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xe6, 0xe6, 0xf2), 2.6,
                              Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(guide_pt.x() - 7.0, guide_pt.y() - 7.0),
                           QPointF(guide_pt.x() + 7.0, guide_pt.y() + 7.0));
                p.drawLine(QPointF(guide_pt.x() + 7.0, guide_pt.y() - 7.0),
                           QPointF(guide_pt.x() - 7.0, guide_pt.y() + 7.0));
            } else {
                p.setFont(tiny_font);
                p.setPen(QColor(0xec, 0xec, 0xf1));
                p.drawText(QRectF(guide_pt.x() - 16, guide_pt.y() - 8, 32, 16), Qt::AlignCenter,
                           OptionShort(PAD_GUIDE, family));
            }
            hit_regions.push_back(
                {QRectF(guide_pt.x() - gr, guide_pt.y() - gr, gr * 2, gr * 2), PAD_GUIDE,
                 pad_actions[PAD_GUIDE].row_index});
        }

        // View (Xbox double-window) / Create (PS) / Minus (Switch)
        {
            draw_ring_circle(view_pt, 12.5, PAD_VIEW);
            const bool exit_hl = is_exit_pad(PAD_VIEW);
            const bool sel_hl = is_selected_pad(PAD_VIEW) || is_hovered_pad(PAD_VIEW);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x6c, 0x6c, 0x82)),
                          1.5));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e)
                               : (sel_hl ? QColor(0x34, 0x25, 0x54) : QColor(0x28, 0x28, 0x35)));
            p.drawEllipse(view_pt, 12.0, 12.0);
            if (family == ControllerFamily::Xbox) {
                p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xe2, 0xe2, 0xec), 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawRect(QRectF(view_pt.x() - 5.0, view_pt.y() - 4.2, 6.0, 5.2));
                p.drawRect(QRectF(view_pt.x() - 1.2, view_pt.y() - 1.0, 6.0, 5.2));
            } else {
                p.setFont(tiny_font);
                p.setPen(QColor(0xec, 0xec, 0xf1));
                p.drawText(QRectF(view_pt.x() - 12, view_pt.y() - 8, 24, 16), Qt::AlignCenter,
                           OptionShort(PAD_VIEW, family));
            }
            p.setFont(tiny_font);
            p.setPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xa0, 0xa0, 0xb2));
            p.drawText(QRectF(view_pt.x() - 22, view_pt.y() + 13, 44, 13), Qt::AlignCenter,
                       OptionShort(PAD_VIEW, family));
            hit_regions.push_back(
                {QRectF(view_pt.x() - 14, view_pt.y() - 14, 28, 28), PAD_VIEW,
                 pad_actions[PAD_VIEW].row_index});
        }

        // Menu (Xbox 3-line hamburger) / Options (PS) / Plus (Switch)
        {
            draw_ring_circle(menu_pt, 12.5, PAD_MENU);
            const bool exit_hl = is_exit_pad(PAD_MENU);
            const bool sel_hl = is_selected_pad(PAD_MENU) || is_hovered_pad(PAD_MENU);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x6c, 0x6c, 0x82)),
                          1.5));
            p.setBrush(exit_hl ? QColor(0x5c, 0x34, 0x0e)
                               : (sel_hl ? QColor(0x34, 0x25, 0x54) : QColor(0x28, 0x28, 0x35)));
            p.drawEllipse(menu_pt, 12.0, 12.0);
            if (family == ControllerFamily::Xbox) {
                p.setPen(QPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xe2, 0xe2, 0xec), 1.6,
                              Qt::SolidLine, Qt::RoundCap));
                for (int dy : {-3, 0, 3}) {
                    p.drawLine(QPointF(menu_pt.x() - 5.0, menu_pt.y() + dy),
                               QPointF(menu_pt.x() + 5.0, menu_pt.y() + dy));
                }
            } else {
                p.setFont(tiny_font);
                p.setPen(QColor(0xec, 0xec, 0xf1));
                p.drawText(QRectF(menu_pt.x() - 12, menu_pt.y() - 8, 24, 16), Qt::AlignCenter,
                           OptionShort(PAD_MENU, family));
            }
            p.setFont(tiny_font);
            p.setPen(exit_hl ? QColor(0xfd, 0xe6, 0x8a) : QColor(0xa0, 0xa0, 0xb2));
            p.drawText(QRectF(menu_pt.x() - 22, menu_pt.y() + 13, 44, 13), Qt::AlignCenter,
                       OptionShort(PAD_MENU, family));
            hit_regions.push_back(
                {QRectF(menu_pt.x() - 14, menu_pt.y() - 14, 28, 28), PAD_MENU,
                 pad_actions[PAD_MENU].row_index});
        }

        // Official Xbox Series X|S Share button (centered directly below View & Menu)
        if (family == ControllerFamily::Xbox) {
            const QRectF share_rect(cx - 10, cy - 4, 20, 14);
            p.setPen(QPen(QColor(0x52, 0x52, 0x64), 1.1));
            p.setBrush(QColor(0x23, 0x23, 0x2e));
            p.drawRoundedRect(share_rect, 6.0, 6.0);
            p.setPen(QPen(QColor(0x9a, 0x9a, 0xaa), 1.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(cx, share_rect.top() + 3.5), QPointF(cx, share_rect.bottom() - 3.5));
            p.drawLine(QPointF(cx - 3.0, share_rect.top() + 6.0),
                       QPointF(cx, share_rect.top() + 3.0));
            p.drawLine(QPointF(cx + 3.0, share_rect.top() + 6.0),
                       QPointF(cx, share_rect.top() + 3.0));
        }

        // =========================================================================
        // 4. ANALOG STICKS & D-PAD (Staggered for Xbox/Switch, Symmetrical for PlayStation)
        // =========================================================================
        const QPointF ls_pt = (family == ControllerFamily::PlayStation)
                                  ? QPointF(cx - 56, cy + 20)
                                  : QPointF(cx - 102, cy - 26);
        const QPointF dpad_pt = (family == ControllerFamily::PlayStation)
                                    ? QPointF(cx - 104, cy - 26)
                                    : QPointF(cx - 56, cy + 24);
        const QPointF rs_pt(cx + 56, cy + 20);

        const auto draw_stick = [&](QPointF c, PadOptionIndex idx) {
            draw_ring_circle(c, 25.0, idx);
            const bool exit_hl = is_exit_pad(idx);
            const bool sel_hl = is_selected_pad(idx) || is_hovered_pad(idx);
            p.setPen(QPen(exit_hl ? QColor(0xfb, 0xbf, 0x24)
                                  : (sel_hl ? QColor(0xc0, 0x84, 0xfc) : QColor(0x52, 0x52, 0x68)),
                          2.0));
            p.setBrush(QColor(0x1a, 0x1a, 0x23));
            p.drawEllipse(c, 24.0, 24.0);
            p.setPen(QPen(QColor(0x3f, 0x3f, 0x52), 1.4));
            p.setBrush(exit_hl ? QColor(0x52, 0x2f, 0x0e)
                               : (sel_hl ? QColor(0x32, 0x24, 0x4f) : QColor(0x2a, 0x2a, 0x37)));
            p.drawEllipse(c, 16.0, 16.0);
            p.setFont(btn_font);
            p.setPen(QColor(0xe0, 0xe0, 0xeb));
            p.drawText(QRectF(c.x() - 16, c.y() - 9, 32, 18), Qt::AlignCenter,
                       OptionShort(idx, family));
            hit_regions.push_back(
                {QRectF(c.x() - 24, c.y() - 24, 48, 48), idx, pad_actions[idx].row_index});
        };

        draw_stick(ls_pt, PAD_LS);
        draw_stick(rs_pt, PAD_RS);

        // D-Pad (Official Xbox Series X|S hybrid dish + cross)
        {
            if (family == ControllerFamily::Xbox) {
                p.setPen(QPen(QColor(0x46, 0x46, 0x5a), 1.3));
                p.setBrush(QColor(0x1c, 0x1c, 0x25));
                p.drawEllipse(dpad_pt, 23.0, 23.0);
            }
            p.setPen(QPen(QColor(0x58, 0x58, 0x6e), 1.2));
            p.setBrush(QColor(0x2b, 0x2b, 0x38));
            p.drawRoundedRect(QRectF(dpad_pt.x() - 7.5, dpad_pt.y() - 21.0, 15.0, 42.0), 3.0, 3.0);
            p.drawRoundedRect(QRectF(dpad_pt.x() - 21.0, dpad_pt.y() - 7.5, 42.0, 15.0), 3.0, 3.0);
            p.setPen(Qt::NoPen);
            p.drawRect(QRectF(dpad_pt.x() - 6.5, dpad_pt.y() - 6.5, 13.0, 13.0));
            hit_regions.push_back(
                {QRectF(dpad_pt.x() - 8, dpad_pt.y() - 22, 16, 16), PAD_DUP,
                 pad_actions[PAD_DUP].row_index});
            hit_regions.push_back(
                {QRectF(dpad_pt.x() - 8, dpad_pt.y() + 6, 16, 16), PAD_DDOWN,
                 pad_actions[PAD_DDOWN].row_index});
            hit_regions.push_back(
                {QRectF(dpad_pt.x() - 22, dpad_pt.y() - 8, 16, 16), PAD_DLEFT,
                 pad_actions[PAD_DLEFT].row_index});
            hit_regions.push_back(
                {QRectF(dpad_pt.x() + 6, dpad_pt.y() - 8, 16, 16), PAD_DRIGHT,
                 pad_actions[PAD_DRIGHT].row_index});
        }

        // =========================================================================
        // 5. FACE BUTTON DIAMOND (Upper-Right)
        // =========================================================================
        const QPointF face_c(cx + 104, cy - 26);
        const QPointF north_pt(face_c.x(), face_c.y() - 28);
        const QPointF west_pt(face_c.x() - 28, face_c.y());
        const QPointF east_pt(face_c.x() + 28, face_c.y());
        const QPointF south_pt(face_c.x(), face_c.y() + 28);

        const auto draw_face = [&](QPointF pt, PadOptionIndex idx, QColor ring, QColor fill) {
            draw_ring_circle(pt, 15.0, idx);
            const bool sel = is_selected_pad(idx) || is_hovered_pad(idx);
            p.setPen(QPen(sel ? QColor(0xf3, 0xe8, 0xff) : ring, 2.2));
            p.setBrush(fill);
            p.drawEllipse(pt, 14.0, 14.0);
            QFont f = font();
            f.setPixelSize(13);
            f.setWeight(QFont::Bold);
            p.setFont(f);
            p.setPen(ring.lighter(135));
            p.drawText(QRectF(pt.x() - 14, pt.y() - 14, 28, 28), Qt::AlignCenter,
                       OptionShort(idx, family));
            hit_regions.push_back(
                {QRectF(pt.x() - 15, pt.y() - 15, 30, 30), idx, pad_actions[idx].row_index});
        };

        if (family == ControllerFamily::PlayStation) {
            draw_face(north_pt, PAD_NORTH, QColor(0x34, 0xd3, 0x99), QColor(0x12, 0x36, 0x2b));
            draw_face(west_pt, PAD_WEST, QColor(0xf4, 0x72, 0xb6), QColor(0x3f, 0x16, 0x2d));
            draw_face(east_pt, PAD_EAST, QColor(0xf8, 0x71, 0x71), QColor(0x45, 0x16, 0x19));
            draw_face(south_pt, PAD_SOUTH, QColor(0x60, 0xa5, 0xfa), QColor(0x13, 0x29, 0x4b));
        } else if (family == ControllerFamily::SwitchPro) {
            draw_face(north_pt, PAD_NORTH, QColor(0xa7, 0x8b, 0xfa), QColor(0x28, 0x20, 0x3f));
            draw_face(west_pt, PAD_WEST, QColor(0xa7, 0x8b, 0xfa), QColor(0x28, 0x20, 0x3f));
            draw_face(east_pt, PAD_EAST, QColor(0xa7, 0x8b, 0xfa), QColor(0x28, 0x20, 0x3f));
            draw_face(south_pt, PAD_SOUTH, QColor(0xa7, 0x8b, 0xfa), QColor(0x28, 0x20, 0x3f));
        } else {
            // Authentic official Xbox face button colors: Y=Yellow, X=Blue, B=Red, A=Green
            draw_face(north_pt, PAD_NORTH, QColor(0xea, 0xb3, 0x08), QColor(0x3b, 0x31, 0x10));
            draw_face(west_pt, PAD_WEST, QColor(0x3b, 0x82, 0xf6), QColor(0x13, 0x29, 0x4b));
            draw_face(east_pt, PAD_EAST, QColor(0xef, 0x44, 0x44), QColor(0x45, 0x16, 0x19));
            draw_face(south_pt, PAD_SOUTH, QColor(0x22, 0xc5, 0x5e), QColor(0x11, 0x36, 0x20));
        }

        // =========================================================================
        // 6. CLEAN INSPECTOR & EXIT-GAME STATUS FOOTER INSIDE THE CARD
        // =========================================================================
        // Determine which button to spotlight in the bottom inspector pill (hovered > selected)
        std::optional<PadOptionIndex> focus_pad = hovered_pad;
        if (!focus_pad.has_value() && selected_row.has_value()) {
            for (std::size_t idx = 0; idx < PAD_UNBOUND; ++idx) {
                if (pad_actions[idx].row_index.has_value() &&
                    *pad_actions[idx].row_index == *selected_row) {
                    focus_pad = static_cast<PadOptionIndex>(idx);
                    break;
                }
            }
        }

        const QRectF inspector_box(16, h - 76, w - 32, 34);
        p.setPen(QPen(focus_pad.has_value() ? QColor(0xa8, 0x55, 0xf7) : QColor(0x2d, 0x2d, 0x3b),
                      1.2));
        p.setBrush(focus_pad.has_value() ? QColor(0x25, 0x1d, 0x3a) : QColor(0x1f, 0x1f, 0x28));
        p.drawRoundedRect(inspector_box, 8.0, 8.0);

        QFont insp_font = font();
        insp_font.setPixelSize(12);
        insp_font.setWeight(QFont::DemiBold);
        p.setFont(insp_font);
        if (focus_pad.has_value()) {
            const auto& info = pad_actions[*focus_pad];
            const QString pad_name = OptionLabel(*focus_pad, family);
            const QString act =
                info.action_text.isEmpty() ? tr("Not assigned") : info.action_text;
            const QChar arrow(0x2192);
            p.setPen(QColor(0xec, 0xec, 0xf1));
            p.drawText(inspector_box.adjusted(10, 0, -10, 0), Qt::AlignCenter,
                       QStringLiteral("%1  %2  %3").arg(pad_name).arg(arrow).arg(act));
        } else {
            p.setPen(QColor(0x9a, 0x9a, 0xaa));
            p.drawText(inspector_box.adjusted(10, 0, -10, 0), Qt::AlignCenter,
                       tr("Hover or click any button on the controller to see what it does"));
        }

        // Orange Exit-Game shortcut legend strip
        const QRectF exit_pill(16, h - 34, w - 32, 24);
        if (exit_shortcut != UISettings::ControllerExitShortcut::Off) {
            p.setPen(QPen(QColor(0xf5, 0x9e, 0x0b), 1.0));
            p.setBrush(QColor(0x3a, 0x22, 0x0a));
            p.drawRoundedRect(exit_pill, 7.0, 7.0);
            QFont ef = font();
            ef.setPixelSize(10);
            ef.setWeight(QFont::Bold);
            p.setFont(ef);
            p.setPen(QColor(0xfd, 0xe6, 0x8a));
            p.drawText(exit_pill, Qt::AlignCenter,
                       tr("Orange Glow = Exit Current Game: %1")
                           .arg(ControllerExitWatcher::ShortcutLabel(exit_shortcut)));
        } else {
            p.setPen(QPen(QColor(0x2b, 0x2b, 0x36), 1.0));
            p.setBrush(QColor(0x1c, 0x1c, 0x24));
            p.drawRoundedRect(exit_pill, 7.0, 7.0);
            p.setFont(tiny_font);
            p.setPen(QColor(0x88, 0x88, 0x9a));
            p.drawText(exit_pill, Qt::AlignCenter, tr("Controller Exit-Game Shortcut: Off"));
        }
    }

private:
    struct HitRegion {
        QRectF rect;
        PadOptionIndex pad_index;
        std::optional<std::size_t> row_index;
    };

    ControllerFamily family = ControllerFamily::Xbox;
    QString profile_badge;
    std::array<ButtonBindingSummary, kPadOptions.size()> pad_actions{};
    UISettings::ControllerExitShortcut exit_shortcut =
        UISettings::ControllerExitShortcut::ViewAndMenu;
    std::optional<std::size_t> selected_row;
    std::optional<PadOptionIndex> hovered_pad;
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
        setWindowTitle(tr("Controls %1 %2").arg(emdash).arg(game_title));
        setMinimumSize(940, 650);
        resize(980, 690);
        setStyleSheet(QStringLiteral(
            "QDialog { background: #141418; color: #ececf1; }"
            "QLabel { color: #ececf1; }"
            "QFrame#Card { background: #1b1b22; border: 1px solid #2b2b36; border-radius: 9px; }"
            "QFrame#CardSelected { background: #281f3f; border: 1.5px solid #a855f7; border-radius: 9px; }"
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
            "color: #d8b4fe; padding: 5px 10px; font-size: 12px; }"
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
            sdl_ids[p] = ExtractSdlIdentity(global_players[p], p, input_subsystem);
            if (!sdl_ids[p].valid) {
                sdl_ids[p] = ExtractSdlIdentity(working_players[p], p, input_subsystem);
            }
            is_custom[p] = !working_players[p].profile_name.empty();
        }
    }

    ControllerFamily CurrentFamily() const {
        return sdl_ids[current_player].family;
    }

    QString CurrentProfileBadge() const {
        const QChar bullet(0x00B7);
        QString profile = global_players[current_player].profile_name.empty()
                              ? tr("Default Profile")
                              : QString::fromStdString(global_players[current_player].profile_name);
        return tr("Player %1 (%2)  %3  %4")
            .arg(current_player + 1)
            .arg(profile)
            .arg(bullet)
            .arg(sdl_ids[current_player].device_name);
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
        root->setSpacing(12);

        // =====================================================================
        // Header: Game Title + Profile / Player Switcher + Quick Swaps
        // =====================================================================
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
        player_combo->setMinimumWidth(210);
        for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
            QString label = tr("Player %1").arg(p + 1);
            if (!global_players[p].profile_name.empty()) {
                label += QStringLiteral(" - %1")
                             .arg(QString::fromStdString(global_players[p].profile_name));
            }
            player_combo->addItem(label, static_cast<int>(p));
        }
        connect(player_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
                    CancelCapture();
                    current_player = static_cast<std::size_t>(std::clamp(idx, 0, 3));
                    selected_row.reset();
                    RebuildComboLabelsForFamily();
                    RefreshAll();
                });
        header_row->addWidget(player_combo);
        root->addLayout(header_row);

        // =====================================================================
        // Main Coherent 2-Pane Split: Left = Official Controller | Right = Game Actions
        // =====================================================================
        auto* split = new QHBoxLayout;
        split->setSpacing(16);

        // LEFT PANE: Official Controller Visual + Quick Swap Row
        auto* left_pane = new QVBoxLayout;
        left_pane->setSpacing(10);

        diagram = new OfficialControllerVisual(this);
        diagram->SetClickHandler([this](std::size_t row_idx) { SelectRow(row_idx); });
        left_pane->addWidget(diagram, 1);

        auto* quick_row = new QHBoxLayout;
        quick_row->setSpacing(6);
        auto* swap_xy = new QPushButton(tr("Swap X %1 Y").arg(arrows), this);
        swap_xy->setObjectName(QStringLiteral("PresetButton"));
        swap_xy->setToolTip(
            tr("Swaps Switch X and Switch Y (e.g. moves Mario Kart Look Back between Y and X)."));
        connect(swap_xy, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        });
        quick_row->addWidget(swap_xy);

        auto* swap_ab = new QPushButton(tr("Swap A %1 B").arg(arrows), this);
        swap_ab->setObjectName(QStringLiteral("PresetButton"));
        connect(swap_ab, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
        });
        quick_row->addWidget(swap_ab);

        auto* reset_btn = new QPushButton(tr("Reset to Global"), this);
        connect(reset_btn, &QPushButton::clicked, this, [this] { ResetCurrentPlayer(); });
        quick_row->addWidget(reset_btn);
        left_pane->addLayout(quick_row);

        split->addLayout(left_pane, 5);

        // RIGHT PANE: Clean, Focused In-Game Actions List
        auto* right_pane = new QVBoxLayout;
        right_pane->setSpacing(8);

        auto* right_hdr = new QLabel(tr("In-Game Actions (click any row to highlight on controller)"),
                                     this);
        right_hdr->setStyleSheet(
            QStringLiteral("font-size: 12px; font-weight: 600; color: #b8b8c8;"));
        right_pane->addWidget(right_hdr);

        action_scroll = new QScrollArea(this);
        action_scroll->setWidgetResizable(true);
        action_scroll->setFrameShape(QFrame::NoFrame);
        action_scroll->setStyleSheet(
            QStringLiteral("QScrollArea { background: transparent; border: none; }"));

        auto* list_host = new QWidget(action_scroll);
        list_host->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* list_layout = new QVBoxLayout(list_host);
        list_layout->setContentsMargins(0, 0, 6, 0);
        list_layout->setSpacing(6);

        row_cards.resize(kSwitchButtons.size(), nullptr);
        row_combos.resize(kSwitchButtons.size(), nullptr);
        row_press_btns.resize(kSwitchButtons.size(), nullptr);

        const auto add_action_row = [&](std::size_t i, QVBoxLayout* target_layout) {
            const auto& spec = kSwitchButtons[i];
            auto* card = new QFrame(list_host);
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
            sw_lbl->setStyleSheet(QStringLiteral("font-size: 10px; color: #8c8c9e;"));
            left->addWidget(action_lbl);
            left->addWidget(sw_lbl);
            row->addLayout(left, 1);

            auto* combo = new QComboBox(card);
            combo->setMinimumWidth(185);
            for (std::size_t opt_i = 0; opt_i < kPadOptions.size(); ++opt_i) {
                combo->addItem(OptionLabel(opt_i, CurrentFamily()), static_cast<int>(opt_i));
            }
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, i](int combo_idx) { OnComboChanged(i, combo_idx); });
            row->addWidget(combo);
            row_combos[i] = combo;

            auto* press_btn = new QPushButton(tr("Press"), card);
            press_btn->setFixedWidth(64);
            press_btn->setToolTip(tr("Click, then press a button on your controller."));
            connect(press_btn, &QPushButton::clicked, this, [this, i] { BeginCapture(i); });
            row->addWidget(press_btn);
            row_press_btns[i] = press_btn;

            row_cards[i] = card;
            target_layout->addWidget(card);
        };

        // Core gameplay buttons first
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            if (kSwitchButtons[i].is_core) {
                add_action_row(i, list_layout);
            }
        }

        // Collapsible secondary section (D-Pad, Stick Clicks, Capture)
        more_toggle_btn = new QPushButton(tr("Show D-Pad, Stick Clicks & Extra Buttons..."), list_host);
        more_toggle_btn->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: 1px dashed #3a3a4c; "
                           "color: #a78bfa; padding: 6px; font-size: 11px; }"
                           "QPushButton:hover { background: #1e1c2b; }"));
        list_layout->addWidget(more_toggle_btn);

        extra_container = new QWidget(list_host);
        auto* extra_layout = new QVBoxLayout(extra_container);
        extra_layout->setContentsMargins(0, 0, 0, 0);
        extra_layout->setSpacing(6);
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            if (!kSwitchButtons[i].is_core) {
                add_action_row(i, extra_layout);
            }
        }
        extra_container->hide();
        list_layout->addWidget(extra_container);

        connect(more_toggle_btn, &QPushButton::clicked, this, [this] {
            const bool show_now = !extra_container->isVisible();
            extra_container->setVisible(show_now);
            more_toggle_btn->setText(
                show_now ? tr("Hide D-Pad, Stick Clicks & Extra Buttons")
                         : tr("Show D-Pad, Stick Clicks & Extra Buttons..."));
        });

        list_layout->addStretch(1);
        action_scroll->setWidget(list_host);
        right_pane->addWidget(action_scroll, 1);
        split->addLayout(right_pane, 6);

        root->addLayout(split, 1);

        // =====================================================================
        // Bottom Console Exit-Game Shortcut Bar + Save/Cancel
        // =====================================================================
        auto* exit_card = new QFrame(this);
        exit_card->setObjectName(QStringLiteral("ExitCard"));
        auto* exit_row = new QHBoxLayout(exit_card);
        exit_row->setContentsMargins(14, 9, 14, 9);
        exit_row->setSpacing(12);

        auto* exit_info = new QVBoxLayout;
        exit_info->setSpacing(1);
        auto* exit_title = new QLabel(tr("Exit Current Game Shortcut (Controller)"), exit_card);
        exit_title->setStyleSheet(
            QStringLiteral("font-size: 13px; font-weight: 600; color: #fde68a;"));
        auto* exit_desc = new QLabel(
            tr("Closes only the running game and returns to Genesis Home (highlighted in orange "
               "above)."),
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

        auto* footer = new QHBoxLayout;
        auto* tip = new QLabel(
            tr("Saved per game. Works automatically from both Genesis Home and GameVault."), this);
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

    void RebuildComboLabelsForFamily() {
        const auto fam = CurrentFamily();
        for (std::size_t i = 0; i < row_combos.size(); ++i) {
            auto* combo = row_combos[i];
            const QSignalBlocker blocker(combo);
            combo->clear();
            for (std::size_t opt_i = 0; opt_i < kPadOptions.size(); ++opt_i) {
                combo->addItem(OptionLabel(opt_i, fam), static_cast<int>(opt_i));
            }
        }
    }

    void SelectRow(std::size_t row_idx) {
        if (row_idx >= row_cards.size()) {
            return;
        }
        if (!kSwitchButtons[row_idx].is_core && extra_container && !extra_container->isVisible()) {
            extra_container->show();
            if (more_toggle_btn) {
                more_toggle_btn->setText(tr("Hide D-Pad, Stick Clicks & Extra Buttons"));
            }
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
            diagram->UpdateState(working_players[current_player], CurrentFamily(),
                                 CurrentProfileBadge(), program_id, game_title,
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
                tr("%1 Custom layout saved for this game (Player %2)")
                    .arg(check)
                    .arg(current_player + 1));
            status_label->setStyleSheet(
                QStringLiteral("font-size: 12px; font-weight: 600; color: #6ee7b7;"));
        } else {
            status_label->setText(
                tr("Using your global controller profile (%1). Change any action on the right.")
                    .arg(sdl_ids[current_player].device_name));
            status_label->setStyleSheet(QStringLiteral("font-size: 12px; color: #9a9aa8;"));
        }
    }

    void RefreshAll() {
        const auto& player = working_players[current_player];
        const auto fam = CurrentFamily();
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            auto* combo = row_combos[i];
            const QSignalBlocker blocker(combo);

            while (combo->count() > static_cast<int>(kPadOptions.size())) {
                combo->removeItem(combo->count() - 1);
            }

            const std::string& binding = player.buttons[kSwitchButtons[i].id];
            if (const auto opt = MatchPadOption(binding); opt.has_value()) {
                combo->setCurrentIndex(static_cast<int>(*opt));
            } else {
                combo->addItem(DescribeCustomBinding(binding, fam), -1);
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
        if (opt_idx < 0 || static_cast<std::size_t>(opt_idx) >= kPadOptions.size()) {
            return;
        }
        const auto& opt = kPadOptions[static_cast<std::size_t>(opt_idx)];
        working_players[current_player].buttons[kSwitchButtons[row_index].id] =
            BuildPadBinding(opt, sdl_ids[current_player], current_player);
        SelectRow(row_index);
        UpdateStatusBanner();
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
        btn->setText(tr("Wait..."));
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
            btn->setText(tr("Press"));
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
            sdl_ids[current_player] =
                ExtractSdlIdentity(working_players[current_player], current_player, input_subsystem);
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
    OfficialControllerVisual* diagram = nullptr;
    QComboBox* exit_shortcut_combo = nullptr;
    QScrollArea* action_scroll = nullptr;
    QPushButton* more_toggle_btn = nullptr;
    QWidget* extra_container = nullptr;
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
