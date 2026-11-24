// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/controller_exit_watcher.h"

#include <QMetaObject>
#include <QTimer>

#include "common/settings_input.h"
#include "hid_core/hid_core.h"

namespace FritchiiUi {

namespace {
constexpr int HoldGuideDurationMs = 500;
} // namespace

ControllerExitWatcher::ControllerExitWatcher(Core::HID::HIDCore& hid_core, QObject* parent)
    : QObject(parent), hold_timer(new QTimer(this)) {
    hold_timer->setSingleShot(true);
    hold_timer->setInterval(HoldGuideDurationMs);
    connect(hold_timer, &QTimer::timeout, this, [this] {
        if (active.load(std::memory_order_acquire)) {
            TriggerOnce();
        }
    });

    player1_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);

    Core::HID::ControllerUpdateCallback callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { OnControllerUpdate(type); },
        .is_npad_service = false,
    };
    player1_callback_key = player1_controller->SetCallback(callback);
    handheld_callback_key = handheld_controller->SetCallback(callback);
    is_registered = true;
}

ControllerExitWatcher::~ControllerExitWatcher() {
    UnloadController();
}

void ControllerExitWatcher::SetActive(bool is_active) {
    active.store(is_active, std::memory_order_release);
    triggered.store(false, std::memory_order_release);
    {
        std::scoped_lock lock{state_mutex};
        prev_combo_down = false;
        prev_guide_down = false;
    }
    if (hold_timer) {
        QMetaObject::invokeMethod(hold_timer, &QTimer::stop, Qt::QueuedConnection);
    }
}

void ControllerExitWatcher::UnloadController() {
    SetActive(false);
    if (is_registered) {
        if (player1_controller) {
            player1_controller->DeleteCallback(player1_callback_key);
        }
        if (handheld_controller) {
            handheld_controller->DeleteCallback(handheld_callback_key);
        }
        is_registered = false;
    }
}

QString ControllerExitWatcher::ShortcutLabel(UISettings::ControllerExitShortcut shortcut) {
    switch (shortcut) {
    case UISettings::ControllerExitShortcut::Off:
        return tr("Off (Disabled)");
    case UISettings::ControllerExitShortcut::ViewAndMenu:
        return tr("View + Menu (2 small center buttons)");
    case UISettings::ControllerExitShortcut::GuideButton:
        return tr("Xbox Guide Button (center Xbox logo - Tap)");
    case UISettings::ControllerExitShortcut::HoldGuideButton:
        return tr("Xbox Guide Button (center Xbox logo - Hold 0.5s)");
    case UISettings::ControllerExitShortcut::LbRbViewMenu:
        return tr("LB + RB + View + Menu (4-button combo)");
    case UISettings::ControllerExitShortcut::ClickBothSticks:
        return tr("Click Both Sticks (press LS + RS down)");
    }
    return tr("View + Menu (2 small center buttons)");
}

std::vector<std::pair<UISettings::ControllerExitShortcut, QString>>
ControllerExitWatcher::AllShortcuts() {
    return {
        {UISettings::ControllerExitShortcut::ViewAndMenu,
         ShortcutLabel(UISettings::ControllerExitShortcut::ViewAndMenu)},
        {UISettings::ControllerExitShortcut::GuideButton,
         ShortcutLabel(UISettings::ControllerExitShortcut::GuideButton)},
        {UISettings::ControllerExitShortcut::HoldGuideButton,
         ShortcutLabel(UISettings::ControllerExitShortcut::HoldGuideButton)},
        {UISettings::ControllerExitShortcut::LbRbViewMenu,
         ShortcutLabel(UISettings::ControllerExitShortcut::LbRbViewMenu)},
        {UISettings::ControllerExitShortcut::ClickBothSticks,
         ShortcutLabel(UISettings::ControllerExitShortcut::ClickBothSticks)},
        {UISettings::ControllerExitShortcut::Off,
         ShortcutLabel(UISettings::ControllerExitShortcut::Off)},
    };
}

void ControllerExitWatcher::TriggerOnce() {
    if (!active.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (triggered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
            this, [this] { emit ExitGameRequested(); }, Qt::QueuedConnection);
    }
}

void ControllerExitWatcher::OnControllerUpdate(Core::HID::ControllerTriggerType type) {
    if (type != Core::HID::ControllerTriggerType::Button) {
        return;
    }
    if (!active.load(std::memory_order_acquire) || triggered.load(std::memory_order_acquire)) {
        return;
    }

    const auto mode = static_cast<UISettings::ControllerExitShortcut>(
        UISettings::values.controller_exit_shortcut.GetValue());
    if (mode == UISettings::ControllerExitShortcut::Off) {
        return;
    }

    const auto p1 = player1_controller->GetButtonsValues();
    const auto hh = handheld_controller->GetButtonsValues();
    const auto is_down = [&](Settings::NativeButton::Values btn) {
        return p1[btn].value || hh[btn].value;
    };

    std::scoped_lock lock{state_mutex};

    if (mode == UISettings::ControllerExitShortcut::HoldGuideButton) {
        const bool guide_down = is_down(Settings::NativeButton::Home);
        if (guide_down && !prev_guide_down) {
            QMetaObject::invokeMethod(
                hold_timer, [this] { hold_timer->start(HoldGuideDurationMs); },
                Qt::QueuedConnection);
        } else if (!guide_down && prev_guide_down) {
            QMetaObject::invokeMethod(hold_timer, &QTimer::stop, Qt::QueuedConnection);
        }
        prev_guide_down = guide_down;
        return;
    }

    bool combo_down = false;
    switch (mode) {
    case UISettings::ControllerExitShortcut::ViewAndMenu:
        combo_down = is_down(Settings::NativeButton::Minus) &&
                     is_down(Settings::NativeButton::Plus);
        break;
    case UISettings::ControllerExitShortcut::GuideButton:
        combo_down = is_down(Settings::NativeButton::Home);
        break;
    case UISettings::ControllerExitShortcut::LbRbViewMenu:
        combo_down = is_down(Settings::NativeButton::L) && is_down(Settings::NativeButton::R) &&
                     is_down(Settings::NativeButton::Minus) &&
                     is_down(Settings::NativeButton::Plus);
        break;
    case UISettings::ControllerExitShortcut::ClickBothSticks:
        combo_down = is_down(Settings::NativeButton::LStick) &&
                     is_down(Settings::NativeButton::RStick);
        break;
    case UISettings::ControllerExitShortcut::Off:
    case UISettings::ControllerExitShortcut::HoldGuideButton:
        break;
    }

    const bool rising_edge = combo_down && !prev_combo_down;
    prev_combo_down = combo_down;
    if (rising_edge) {
        TriggerOnce();
    }
}

} // namespace FritchiiUi
