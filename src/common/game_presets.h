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

struct HardwareDisplayProfile {
    bool discrete_gpu = false;
    bool supports_mailbox = false;
    u64 vram_bytes = 0;
    int display_width = 1920;
    int display_height = 1080;
};

[[nodiscard]] inline constexpr ResolutionSetup PickAutoResolution(const HardwareDisplayProfile& hw,
                                                                  bool is_switch1) {
    constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;
    const bool high_end = hw.discrete_gpu && hw.vram_bytes >= 10ULL * GiB;
    const bool ultra_vram = hw.discrete_gpu && hw.vram_bytes >= 12ULL * GiB;
    const bool mid_range = hw.discrete_gpu && hw.vram_bytes >= 6ULL * GiB;

    if (is_switch1) {
        if (high_end) {
            if (hw.display_height >= 1440) {
                return ResolutionSetup::Res2X;
            }
            if (hw.display_height >= 1080) {
                return ResolutionSetup::Res3_2X;
            }
            return ResolutionSetup::Res1X;
        }
        if (mid_range && hw.display_height >= 1440) {
            return ResolutionSetup::Res3_2X;
        }
        return ResolutionSetup::Res1X;
    }

    // Switch 2 titles are heavier internally; scale up only on 12+ GiB discrete GPUs.
    if (ultra_vram) {
        if (hw.display_height >= 2160) {
            return ResolutionSetup::Res2X;
        }
        if (hw.display_height >= 1440) {
            return ResolutionSetup::Res3_2X;
        }
    }
    return ResolutionSetup::Res1X;
}

/// Automatically detects GPU tier and physical display resolution to configure optimal
/// resolution, texture filtering, VRAM budget, and latency settings for the session
/// without altering saved global settings or overriding explicit per-game overrides.
inline bool ApplyAutoHardwareQualityPreset(Values& settings, u64 program_id,
                                           const HardwareDisplayProfile& hw) {
    if (program_id == 0) {
        return false;
    }
    constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;
    const bool is_switch1 = IsSwitch1ProgramId(program_id);
    const bool high_end = hw.discrete_gpu && hw.vram_bytes >= 10ULL * GiB;
    const bool ultra_vram = hw.discrete_gpu && hw.vram_bytes >= 12ULL * GiB;
    const bool mid_range = hw.discrete_gpu && hw.vram_bytes >= 6ULL * GiB;

    const auto session_if_global = [](auto& setting, auto value) {
        if (!setting.UsingGlobal()) {
            return;
        }
        setting.SetGlobal(false);
        setting.SetValue(value);
    };

    session_if_global(settings.resolution_setup, PickAutoResolution(hw, is_switch1));
    session_if_global(settings.use_asynchronous_shaders, true);
    session_if_global(settings.async_presentation, true);
    session_if_global(settings.use_disk_shader_cache, true);
    session_if_global(settings.use_vulkan_driver_pipeline_cache, true);
    session_if_global(settings.vsync_mode,
                      hw.supports_mailbox ? VSyncMode::Mailbox : VSyncMode::Fifo);

    if (high_end) {
        session_if_global(settings.max_anisotropy, AnisotropyMode::X16);
        session_if_global(settings.vram_usage_mode, VramUsageMode::Aggressive);
        session_if_global(settings.accelerate_astc, AstcDecodeMode::Gpu);
        session_if_global(settings.astc_recompression,
                          ultra_vram ? AstcRecompression::Uncompressed : AstcRecompression::Bc3);
        session_if_global(settings.scaling_filter, ScalingFilter::Fsr);
        if (is_switch1) {
            session_if_global(settings.anti_aliasing, AntiAliasing::Smaa);
        }
    } else if (mid_range) {
        session_if_global(settings.max_anisotropy, AnisotropyMode::X8);
        session_if_global(settings.accelerate_astc, AstcDecodeMode::Gpu);
        session_if_global(settings.scaling_filter, ScalingFilter::Fsr);
    }

    return true;
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
