// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include <QString>

#include "common/common_types.h"

class QWidget;

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace FritchiiUi {

/// Sentinel profile_name written into per-game custom/<TITLEID>.ini when a player has custom
/// per-game button overrides. QtConfig inherits sticks/vibration/controller identity from the
/// global config and applies only the per-game button overrides on top.
inline constexpr const char* CustomPerGameLayoutProfile = "__genesis_custom__";

/// Opens a friendly, console-style per-game button layout remapper dialog for the selected game.
/// Returns true if the user saved changes.
bool ShowButtonRemapperDialog(QWidget* parent, Core::System& system,
                              InputCommon::InputSubsystem* input_subsystem, u64 program_id,
                              const QString& game_title, const std::string& game_path);

} // namespace FritchiiUi
