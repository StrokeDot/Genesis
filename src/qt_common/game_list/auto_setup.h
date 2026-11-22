// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "qt_common/game_list/discovery.h"

namespace GameListAutoSetup {

struct SemVer {
    u32 major{0};
    u32 minor{0};
    u32 patch{0};

    [[nodiscard]] bool Valid() const noexcept {
        return major > 0;
    }

    auto operator<=>(const SemVer&) const = default;
};

/// Parses a Switch firmware/key version like `20.3.0`, `v19.0.1`, or `18.-1-0` from a path/name.
[[nodiscard]] inline SemVer ExtractSemanticVersion(std::string_view text) {
    SemVer best{};
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto is_sep = [](char c) { return c == '.' || c == '-' || c == '_'; };

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!is_digit(text[i])) {
            continue;
        }
        if (i > 0 && is_digit(text[i - 1])) {
            continue;
        }

        auto parse_num = [&](std::size_t& pos) -> u32 {
            u32 val = 0;
            int digits = 0;
            while (pos < text.size() && is_digit(text[pos]) && digits < 3) {
                val = val * 10 + static_cast<u32>(text[pos] - '0');
                ++pos;
                ++digits;
            }
            return digits > 0 ? val : 1000U;
        };

        std::size_t p = i;
        const u32 maj = parse_num(p);
        if (maj < 1 || maj > 99 || p >= text.size() || !is_sep(text[p])) {
            continue;
        }
        while (p < text.size() && is_sep(text[p])) {
            ++p;
        }
        const u32 min = parse_num(p);
        if (min > 99 || p >= text.size() || !is_sep(text[p])) {
            continue;
        }
        while (p < text.size() && is_sep(text[p])) {
            ++p;
        }
        const u32 pat = parse_num(p);
        if (pat > 99) {
            continue;
        }
        if (p < text.size() && is_digit(text[p])) {
            continue;
        }

        const SemVer candidate{maj, min, pat};
        if (candidate > best) {
            best = candidate;
        }
    }
    return best;
}

inline bool ShouldSkipAutoSetupDir(std::string_view name_lower) {
    if (name_lower.empty() || name_lower.front() == '.' || name_lower.front() == '$') {
        return true;
    }
    // Note: unlike game discovery, we deliberately do NOT skip `keys`, `firmware`,
    // `keys + firmware`, or `nand` because that is where keys and firmware live.
    static constexpr std::array<std::string_view, 46> skip_dirs = {
        "windows",
        "windows.old",
        "system volume information",
        "programdata",
        "appdata",
        "windowsapps",
        "winsxs",
        "steamapps",
        "epic games",
        "xboxgames",
        "gog galaxy",
        "ea games",
        "origin games",
        "ubisoft game launcher",
        "romfs",
        "exefs",
        "cache",
        "shadercache",
        "shaders",
        "logs",
        "node_modules",
        "__pycache__",
        "site-packages",
        "update-old",
        "windows kits",
        "microsoft visual studio",
        "microsoft",
        "common files",
        "nvidia corporation",
        "amd",
        "intel",
        "windowspowershell",
        "dotnet",
        "recovery",
        "perflogs",
        "msocache",
        "drivers",
        "screenshots",
        "proc",
        "sys",
        "dev",
        "tmp",
        "var",
        "usr",
        "snap",
        "save",
    };
    return std::find(skip_dirs.begin(), skip_dirs.end(), name_lower) != skip_dirs.end();
}

struct KeyCandidate {
    std::filesystem::path prod_keys_path;
    std::filesystem::path title_keys_path;
    SemVer version{};
    u64 file_size{0};
    s64 mtime_ns{0};
};

struct FirmwareCandidate {
    std::filesystem::path directory;
    SemVer version{};
    std::size_t nca_count{0};
    bool from_emulator_nand{false};
};

struct DetectedAssets {
    std::optional<KeyCandidate> best_keys;
    std::optional<FirmwareCandidate> best_firmware;
};

inline bool IsBetterKeys(const KeyCandidate& a, const KeyCandidate& b) {
    return std::tie(a.version, a.file_size, a.mtime_ns) >
           std::tie(b.version, b.file_size, b.mtime_ns);
}

inline bool IsBetterFirmware(const FirmwareCandidate& a, const FirmwareCandidate& b) {
    // Prefer an explicit higher semantic version (e.g. 20.3.0 > 19.0.1).
    // When neither has a version in the path, prefer an existing emulator NAND or larger NCA count.
    if (a.version != b.version) {
        return a.version > b.version;
    }
    if (a.from_emulator_nand != b.from_emulator_nand) {
        return a.from_emulator_nand;
    }
    return a.nca_count > b.nca_count;
}

inline std::size_t CountFirmwareNcas(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it{
             dir, std::filesystem::directory_options::skip_permission_denied, ec};
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec)) {
            continue;
        }
        const auto ext_u8 = it->path().extension().u8string();
        const std::string ext = GameListDiscovery::ToLowerAscii(
            std::string_view(reinterpret_cast<const char*>(ext_u8.data()), ext_u8.size()));
        if (ext == ".nca") {
            ++count;
        }
    }
    return count;
}

inline KeyCandidate MakeKeyCandidate(const std::filesystem::path& prod_keys) {
    KeyCandidate cand;
    cand.prod_keys_path = prod_keys;

    std::error_code ec;
    const auto sibling_title = prod_keys.parent_path() / "title.keys";
    if (std::filesystem::is_regular_file(sibling_title, ec)) {
        cand.title_keys_path = sibling_title;
    }

    cand.file_size = std::filesystem::file_size(prod_keys, ec);
    if (ec) {
        cand.file_size = 0;
    }
    const auto mtime = std::filesystem::last_write_time(prod_keys, ec);
    if (!ec) {
        cand.mtime_ns = static_cast<s64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count());
    }

    const auto parent_u8 = prod_keys.parent_path().u8string();
    cand.version = ExtractSemanticVersion(
        std::string_view(reinterpret_cast<const char*>(parent_u8.data()), parent_u8.size()));
    return cand;
}

/// Checks other installed Switch emulators' data folders for existing keys and firmware.
inline DetectedAssets DetectFromExistingEmulators(const std::filesystem::path& exclude_dir = {}) {
    static constexpr std::array<const char*, 8> family = {
        "fritchii", "lichii", "eden", "citron", "sudachi", "suyu", "yuzu", "torzu",
    };

    std::vector<std::filesystem::path> appdata_roots;
#ifdef _WIN32
    if (const std::string appdata = GameListDiscovery::GetEnvVar("APPDATA"); !appdata.empty()) {
        appdata_roots.emplace_back(appdata);
    }
#else
    if (const std::string xdg = GameListDiscovery::GetEnvVar("XDG_DATA_HOME"); !xdg.empty()) {
        appdata_roots.emplace_back(xdg);
    } else if (const std::string home = GameListDiscovery::GetEnvVar("HOME"); !home.empty()) {
        appdata_roots.emplace_back(std::filesystem::path(home) / ".local" / "share");
    }
#endif

    DetectedAssets result;
    const std::string exclude_norm =
        exclude_dir.empty() ? std::string{} : GameListDiscovery::NormalizeComparablePath(exclude_dir);

    for (const auto& base : appdata_roots) {
        for (const char* emu : family) {
            const auto emu_dir = base / emu;
            if (!exclude_norm.empty() &&
                GameListDiscovery::NormalizeComparablePath(emu_dir) == exclude_norm) {
                continue;
            }
            std::error_code ec;
            const auto keys_file = emu_dir / "keys" / "prod.keys";
            if (std::filesystem::is_regular_file(keys_file, ec)) {
                KeyCandidate cand = MakeKeyCandidate(keys_file);
                if (!result.best_keys || IsBetterKeys(cand, *result.best_keys)) {
                    result.best_keys = std::move(cand);
                }
            }

            const auto fw_dir = emu_dir / "nand" / "system" / "Contents" / "registered";
            const std::size_t ncas = CountFirmwareNcas(fw_dir);
            if (ncas >= 50) {
                FirmwareCandidate cand{
                    .directory = fw_dir,
                    .version = {},
                    .nca_count = ncas,
                    .from_emulator_nand = true,
                };
                if (!result.best_firmware || IsBetterFirmware(cand, *result.best_firmware)) {
                    result.best_firmware = std::move(cand);
                }
            }
        }

        // Also check Ryujinx keys if present.
        std::error_code ec;
        const auto ryu_keys = base / "Ryujinx" / "system" / "prod.keys";
        if (std::filesystem::is_regular_file(ryu_keys, ec)) {
            KeyCandidate cand = MakeKeyCandidate(ryu_keys);
            if (!result.best_keys || IsBetterKeys(cand, *result.best_keys)) {
                result.best_keys = std::move(cand);
            }
        }
    }

    return result;
}

/// Searches the given roots in parallel for `prod.keys` and Switch firmware folders (`>= 50` NCAs).
inline DetectedAssets DetectInRoots(const std::vector<GameListDiscovery::SearchRoot>& roots,
                                    bool need_keys, bool need_firmware,
                                    std::chrono::milliseconds per_root_time_limit) {
    namespace fs = std::filesystem;
    if (roots.empty() || (!need_keys && !need_firmware)) {
        return {};
    }

    const auto search_one = [&](const GameListDiscovery::SearchRoot& root) -> DetectedAssets {
        const auto started = std::chrono::steady_clock::now();
        DetectedAssets local;

        std::vector<std::string> skip_top_lower;
        skip_top_lower.reserve(root.skip_top.size());
        for (const auto& s : root.skip_top) {
            skip_top_lower.push_back(GameListDiscovery::ToLowerAscii(s));
        }

        std::vector<std::pair<fs::path, std::size_t>> stack;
        stack.emplace_back(root.path, 0);

        std::size_t visited = 0;
        while (!stack.empty()) {
            if ((++visited & 63U) == 0 &&
                std::chrono::steady_clock::now() - started > per_root_time_limit) {
                break;
            }

            auto [dir, depth] = std::move(stack.back());
            stack.pop_back();

            std::error_code ec;
            std::size_t nca_count = 0;
            bool has_cnmt_nca = false;

            for (fs::directory_iterator it{dir, fs::directory_options::skip_permission_denied, ec};
                 !ec && it != fs::directory_iterator{}; it.increment(ec)) {
                if ((++visited & 255U) == 0 &&
                    std::chrono::steady_clock::now() - started > per_root_time_limit) {
                    break;
                }
                const auto& entry = *it;
                std::error_code entry_ec;
                if (entry.is_symlink(entry_ec)) {
                    continue;
                }
                if (entry.is_directory(entry_ec)) {
                    if (depth + 1 > root.max_depth) {
                        continue;
                    }
                    const auto name_u8 = entry.path().filename().u8string();
                    const std::string name_lower = GameListDiscovery::ToLowerAscii(std::string_view(
                        reinterpret_cast<const char*>(name_u8.data()), name_u8.size()));
                    if (ShouldSkipAutoSetupDir(name_lower)) {
                        continue;
                    }
                    if (depth == 0 &&
                        std::find(skip_top_lower.begin(), skip_top_lower.end(), name_lower) !=
                            skip_top_lower.end()) {
                        continue;
                    }
                    stack.emplace_back(entry.path(), depth + 1);
                } else if (entry.is_regular_file(entry_ec)) {
                    const auto fname_u8 = entry.path().filename().u8string();
                    const std::string fname_lower =
                        GameListDiscovery::ToLowerAscii(std::string_view(
                            reinterpret_cast<const char*>(fname_u8.data()), fname_u8.size()));
                    if (need_keys && fname_lower == "prod.keys") {
                        KeyCandidate cand = MakeKeyCandidate(entry.path());
                        if (cand.file_size > 0 &&
                            (!local.best_keys || IsBetterKeys(cand, *local.best_keys))) {
                            local.best_keys = std::move(cand);
                        }
                    } else if (need_firmware && fname_lower.size() > 4 &&
                               fname_lower.compare(fname_lower.size() - 4, 4, ".nca") == 0) {
                        ++nca_count;
                        if (fname_lower.size() > 9 &&
                            fname_lower.compare(fname_lower.size() - 9, 9, ".cnmt.nca") == 0) {
                            has_cnmt_nca = true;
                        }
                    }
                }
            }

            if (need_firmware && nca_count >= 50 && has_cnmt_nca) {
                const auto dir_u8 = dir.u8string();
                const SemVer ver = ExtractSemanticVersion(
                    std::string_view(reinterpret_cast<const char*>(dir_u8.data()), dir_u8.size()));
                FirmwareCandidate cand{
                    .directory = dir,
                    .version = ver,
                    .nca_count = nca_count,
                    .from_emulator_nand = false,
                };
                if (!local.best_firmware || IsBetterFirmware(cand, *local.best_firmware)) {
                    local.best_firmware = std::move(cand);
                }
            }
        }
        return local;
    };

    std::vector<DetectedAssets> partials(roots.size());
    std::vector<std::thread> threads;
    threads.reserve(roots.size());
    for (std::size_t i = 0; i < roots.size(); ++i) {
        threads.emplace_back([&, i] { partials[i] = search_one(roots[i]); });
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    DetectedAssets merged;
    for (auto& part : partials) {
        if (part.best_keys && (!merged.best_keys || IsBetterKeys(*part.best_keys, *merged.best_keys))) {
            merged.best_keys = std::move(part.best_keys);
        }
        if (part.best_firmware &&
            (!merged.best_firmware || IsBetterFirmware(*part.best_firmware, *merged.best_firmware))) {
            merged.best_firmware = std::move(part.best_firmware);
        }
    }
    return merged;
}

/// Copies all `.nca` files from `source_dir` into `dest_registered_dir`.
/// Returns the number of `.nca` files copied.
inline std::size_t CopyFirmwareNcas(const std::filesystem::path& source_dir,
                                    const std::filesystem::path& dest_registered_dir) {
    std::error_code ec;
    std::filesystem::create_directories(dest_registered_dir, ec);
    if (ec) {
        return 0;
    }

    std::size_t copied = 0;
    for (std::filesystem::directory_iterator it{
             source_dir, std::filesystem::directory_options::skip_permission_denied, ec};
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec)) {
            continue;
        }
        const auto ext_u8 = it->path().extension().u8string();
        const std::string ext = GameListDiscovery::ToLowerAscii(
            std::string_view(reinterpret_cast<const char*>(ext_u8.data()), ext_u8.size()));
        if (ext != ".nca") {
            continue;
        }
        const auto dst = dest_registered_dir / it->path().filename();
        std::filesystem::copy_file(it->path(), dst,
                                   std::filesystem::copy_options::overwrite_existing, entry_ec);
        if (!entry_ec) {
            ++copied;
        }
    }
    return copied;
}

} // namespace GameListAutoSetup
