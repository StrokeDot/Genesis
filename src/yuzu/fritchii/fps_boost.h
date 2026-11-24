// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include "common/common_types.h"

class QWidget;

namespace FritchiiUi {

/// Returns true if a managed "60 FPS" patch folder exists for this Title ID.
bool Is60FpsPatchInstalled(u64 program_id);

/// Returns true if the "60 FPS" patch or experimental swap-interval unlock is currently active.
bool Is60FpsBoostActive(u64 program_id);

/// Returns true if the per-game experimental 60 Hz swap-interval override is enabled.
bool HasForce60FpsSwapInterval(u64 program_id);

/// Opens a friendly 60 FPS Boost dialog for the selected game where the user can:
///  - Toggle an installed 60 FPS patch ON/OFF in 1 click
///  - Auto-fetch and install a verified community [60 FPS] patch for any 30 FPS Switch title
///  - Optionally toggle the experimental 60 Hz presentation swap-interval unlocker
/// Returns true if settings/mods were modified.
bool Show60FpsBoostDialog(QWidget* parent, u64 program_id, const QString& game_title);

} // namespace FritchiiUi
