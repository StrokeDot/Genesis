// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>

#include "common/common_types.h"
#include "hid_core/frontend/emulated_controller.h"
#include "qt_common/config/uisettings.h"

class QTimer;

namespace Core::HID {
class HIDCore;
}

namespace FritchiiUi {

/// Watches Player 1 / Handheld controller button state while a game is running and emits
/// ExitGameRequested() when the configured console-style Xbox exit shortcut is pressed.
class ControllerExitWatcher : public QObject {
    Q_OBJECT

public:
    explicit ControllerExitWatcher(Core::HID::HIDCore& hid_core, QObject* parent = nullptr);
    ~ControllerExitWatcher() override;

    void SetActive(bool active);
    void UnloadController();

    static QString ShortcutLabel(UISettings::ControllerExitShortcut shortcut);
    static std::vector<std::pair<UISettings::ControllerExitShortcut, QString>> AllShortcuts();

signals:
    void ExitGameRequested();

private:
    void OnControllerUpdate(Core::HID::ControllerTriggerType type);
    void TriggerOnce();

    Core::HID::EmulatedController* player1_controller = nullptr;
    Core::HID::EmulatedController* handheld_controller = nullptr;
    int player1_callback_key = 0;
    int handheld_callback_key = 0;
    bool is_registered = false;

    std::atomic_bool active{false};
    std::atomic_bool triggered{false};
    std::mutex state_mutex;
    bool prev_combo_down = false;
    bool prev_guide_down = false;
    QTimer* hold_timer = nullptr;
};

} // namespace FritchiiUi
