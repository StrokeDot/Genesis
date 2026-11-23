// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "common/settings.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 Switch2ProgramIdMin = 0x0400000000000000ULL;

inline std::atomic<bool> g_switch1_session{false};

inline constexpr bool IsSwitch1ProgramId(u64 program_id) {
    return program_id != 0 && program_id < Switch2ProgramIdMin;
}

inline void SetSwitch1Session(bool is_switch1) {
    g_switch1_session.store(is_switch1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool IsSwitch1Session() {
    return g_switch1_session.load(std::memory_order_relaxed);
}

/// Automatic console-style stability profile for Nintendo Switch 1 games.
/// Disables experimental Switch 2 / OoT graphics paths (such as Graphics Pipeline Library
/// independent descriptor sets and Low GPU accuracy) for the session without modifying saved
/// global settings or overriding explicit per-game customizations.
inline bool ApplySwitch1StabilityPreset(Values& settings, u64 program_id) {
    if (!IsSwitch1ProgramId(program_id)) {
        return false;
    }
    const auto session_if_global = [](auto& setting, auto value) {
        if (!setting.UsingGlobal()) {
            return;
        }
        setting.SetGlobal(false);
        setting.SetValue(value);
    };
    session_if_global(settings.use_graphics_pipeline_library, false);
    if (settings.gpu_accuracy.UsingGlobal() &&
        settings.gpu_accuracy.GetValue(true) == GpuAccuracy::Low) {
        session_if_global(settings.gpu_accuracy, GpuAccuracy::Normal);
    }
    session_if_global(settings.use_reactive_flushing, true);
    return true;
}

inline void SetMarioKartWorldRecommendedSettings(Values& settings, bool global) {
    const auto session = [global](auto& setting, auto value) {
        setting.SetGlobal(global);
        setting.SetValue(value);
    };
    session(settings.renderer_backend, RendererBackend::Vulkan);
    session(settings.resolution_setup, ResolutionSetup::Res1X);
    session(settings.gpu_accuracy, GpuAccuracy::Low);
    session(settings.use_asynchronous_shaders, true);
    session(settings.astc_recompression, AstcRecompression::Bc3);
    session(settings.vram_usage_mode, VramUsageMode::Conservative);
    session(settings.use_reactive_flushing, false);
    session(settings.vsync_mode, VSyncMode::FifoRelaxed);
    session(settings.use_docked_mode, ConsoleMode::Handheld);
    session(settings.scaling_filter, ScalingFilter::ScaleForce);
    session(settings.anti_aliasing, AntiAliasing::Fxaa);
}

inline bool ApplyMarioKartWorldPreset(Values& settings, u64 program_id, bool accepted) {
    if (!accepted || program_id != MarioKartWorldProgramId) {
        return false;
    }
    SetMarioKartWorldRecommendedSettings(settings, false);
    return true;
}

} // namespace Settings
