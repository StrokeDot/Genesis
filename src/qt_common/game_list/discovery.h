// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GameListDiscovery {

struct Result {
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
    std::size_t skipped_romfs{};
    std::size_t errors{};
};

struct SearchRoot {
    std::filesystem::path path;
    std::size_t max_depth{6};
    std::vector<std::string> skip_top;
};

inline bool IsExeFS(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path / "main", ec) &&
           std::filesystem::is_regular_file(path / "main.npdm", ec);
}

inline std::string ToLowerAscii(std::string_view text) {
    std::string out(text.size(), '\0');
    std::transform(text.begin(), text.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

inline std::string NormalizeComparablePath(const std::filesystem::path& path) {
    auto u8 = path.lexically_normal().u8string();
    std::string s(reinterpret_cast<const char*>(u8.data()), u8.size());
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '\\', '/');
    s = ToLowerAscii(s);
#endif
    while (s.size() > 1 && s.back() == '/') {
#ifdef _WIN32
        if (s.size() == 3 && s[1] == ':') {
            break;
        }
#endif
        s.pop_back();
    }
    return s;
}

inline bool IsPathSameOrChild(const std::filesystem::path& candidate,
                              const std::filesystem::path& parent) {
    const std::string c = NormalizeComparablePath(candidate);
    const std::string p = NormalizeComparablePath(parent);
    if (c.empty() || p.empty()) {
        return false;
    }
    if (c == p) {
        return true;
    }
    if (c.size() > p.size() && c.compare(0, p.size(), p) == 0) {
        return p.back() == '/' || c[p.size()] == '/';
    }
    return false;
}

inline bool IsPathCoveredByAny(const std::filesystem::path& candidate,
                               const std::vector<std::filesystem::path>& roots) {
    return std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path& root) {
        return IsPathSameOrChild(candidate, root);
    });
}

inline bool IsSwitchContainerExtension(std::string_view ext_lower) {
    if (!ext_lower.empty() && ext_lower.front() == '.') {
        ext_lower.remove_prefix(1);
    }
    return ext_lower == "nsp" || ext_lower == "xci" || ext_lower == "nsz" || ext_lower == "xcz" ||
           ext_lower == "nup";
}

inline bool ShouldSkipAutoDetectDir(std::string_view name_lower) {
    if (name_lower.empty() || name_lower.front() == '.' || name_lower.front() == '$') {
        return true;
    }
    static constexpr std::array<std::string_view, 54> skip_dirs = {
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
        "saves",
        "save games",
        "save",
        "screenshots",
        "dlc",
        "updates",
        "dlc + updates",
        "keys + firmware",
        "firmware",
        "keys",
        "proc",
        "sys",
        "dev",
        "tmp",
        "var",
        "usr",
        "snap",
    };
    return std::find(skip_dirs.begin(), skip_dirs.end(), name_lower) != skip_dirs.end();
}

/// Parses `Paths\gamedirs\N\path=...` lines from a yuzu-family `qt-config.ini`.
inline std::vector<std::filesystem::path> ParseQtGameDirs(std::string_view ini_text) {
    std::vector<std::filesystem::path> out;
    std::size_t line_start = 0;
    while (line_start < ini_text.size()) {
        std::size_t line_end = ini_text.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = ini_text.size();
        }
        std::string_view line = ini_text.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto trim = [](std::string_view sv) {
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
                sv.remove_prefix(1);
            }
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
                sv.remove_suffix(1);
            }
            return sv;
        };
        std::string key = ToLowerAscii(trim(line.substr(0, eq)));
        std::replace(key.begin(), key.end(), '/', '\\');
        const bool is_gamedir_path =
            (key.rfind("paths\\gamedirs\\", 0) == 0 || key.rfind("gamedirs\\", 0) == 0) &&
            key.size() >= 5 && key.compare(key.size() - 5, 5, "\\path") == 0;
        if (!is_gamedir_path) {
            continue;
        }
        const std::string_view val = trim(line.substr(eq + 1));
        if (val.empty()) {
            continue;
        }
        const std::string val_lower = ToLowerAscii(val);
        if (val_lower == "sdmc" || val_lower == "usernand" || val_lower == "sysnand") {
            continue;
        }
        out.emplace_back(std::u8string_view(reinterpret_cast<const char8_t*>(val.data()),
                                            val.size()));
    }
    return out;
}

inline std::string GetEnvVar(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    if (const char* value = std::getenv(name); value && *value) {
        return std::string(value);
    }
    return {};
#endif
}

/// Collects existing game directories from other yuzu-family emulator configs on this machine.
inline std::vector<std::filesystem::path> CollectEmulatorConfigGameDirs() {
    static constexpr std::array<const char*, 8> family = {
        "yuzu", "suyu", "sudachi", "citron", "eden", "torzu", "fritchii", "lichii",
    };

    std::vector<std::filesystem::path> config_roots;
#ifdef _WIN32
    if (const std::string appdata = GetEnvVar("APPDATA"); !appdata.empty()) {
        config_roots.emplace_back(appdata);
    }
#else
    if (const std::string xdg = GetEnvVar("XDG_CONFIG_HOME"); !xdg.empty()) {
        config_roots.emplace_back(xdg);
    } else if (const std::string home = GetEnvVar("HOME"); !home.empty()) {
        config_roots.emplace_back(std::filesystem::path(home) / ".config");
    }
#endif

    std::vector<std::filesystem::path> found;
    for (const auto& root : config_roots) {
        for (const char* emu : family) {
            const auto ini_path = root / emu / "config" / "qt-config.ini";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(ini_path, ec)) {
                continue;
            }
            std::ifstream in(ini_path, std::ios::binary);
            if (!in.is_open()) {
                continue;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            for (auto& dir : ParseQtGameDirs(ss.str())) {
                if (std::filesystem::is_directory(dir, ec)) {
                    found.push_back(std::move(dir));
                }
            }
        }
    }
    return found;
}

/// Removes duplicates and subdirectories already covered by a parent directory in the list.
inline std::vector<std::filesystem::path> CollapseNestedDirs(
    std::vector<std::filesystem::path> dirs) {
    std::vector<std::pair<std::string, std::filesystem::path>> keyed;
    keyed.reserve(dirs.size());
    for (auto& d : dirs) {
        std::string norm = NormalizeComparablePath(d);
        if (!norm.empty()) {
            keyed.emplace_back(std::move(norm), d.lexically_normal());
        }
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        if (a.first.size() != b.first.size()) {
            return a.first.size() < b.first.size();
        }
        return a.first < b.first;
    });

    std::vector<std::filesystem::path> out;
    for (auto& [_, candidate] : keyed) {
        if (!IsPathCoveredByAny(candidate, out)) {
            out.push_back(std::move(candidate));
        }
    }
    return out;
}

/// Builds the default set of filesystem roots to search for Switch game folders.
/// Mirrors GameVault's strategy: non-system drives are walked deeper, while on the
/// system drive Windows/Program Files are skipped and the user home folder is walked separately.
inline std::vector<SearchRoot> DefaultSearchRoots() {
    std::vector<SearchRoot> roots;
#ifdef _WIN32
    std::string sys_drive = "C:";
    if (const std::string env_sys = GetEnvVar("SystemDrive"); !env_sys.empty()) {
        sys_drive = ToLowerAscii(env_sys);
        for (char& c : sys_drive) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1U << i)) == 0) {
            continue;
        }
        const char letter = static_cast<char>('A' + i);
        wchar_t drive_w[4] = {static_cast<wchar_t>(letter), L':', L'\\', L'\0'};
        const UINT type = GetDriveTypeW(drive_w);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            continue;
        }
        const std::string drive = std::string(1, letter) + ":";
        const std::filesystem::path drive_path(drive + "\\");
        if (drive == sys_drive) {
            roots.push_back(SearchRoot{
                .path = drive_path,
                .max_depth = 5,
                .skip_top = {"users", "program files", "program files (x86)"},
            });
        } else {
            roots.push_back(SearchRoot{
                .path = drive_path,
                .max_depth = 7,
                .skip_top = {},
            });
        }
    }

    if (const std::string profile = GetEnvVar("USERPROFILE"); !profile.empty()) {
        std::error_code ec;
        std::filesystem::path home(profile);
        if (std::filesystem::is_directory(home, ec)) {
            roots.push_back(SearchRoot{.path = std::move(home), .max_depth = 6, .skip_top = {}});
        }
    }
#else
    if (const std::string home_env = GetEnvVar("HOME"); !home_env.empty()) {
        std::error_code ec;
        std::filesystem::path home_path(home_env);
        if (std::filesystem::is_directory(home_path, ec)) {
            roots.push_back(
                SearchRoot{.path = std::move(home_path), .max_depth = 6, .skip_top = {}});
        }
    }
#endif
    return roots;
}

/// Searches all given roots in parallel, each with its own time limit, returning
/// collapsed directories that contain Switch game files or loose ExeFS dumps.
template <typename Stopped>
std::vector<std::filesystem::path> AutoDetectRoots(const std::vector<SearchRoot>& roots,
                                                   std::chrono::milliseconds per_root_time_limit,
                                                   Stopped stopped) {
    namespace fs = std::filesystem;
    if (roots.empty() || stopped()) {
        return {};
    }

    const auto search_one = [&](const SearchRoot& root) -> std::vector<fs::path> {
        const auto started = std::chrono::steady_clock::now();
        std::vector<fs::path> found_dirs;
        std::unordered_set<std::string> seen_dirs;

        std::vector<std::string> skip_top_lower;
        skip_top_lower.reserve(root.skip_top.size());
        for (const auto& s : root.skip_top) {
            skip_top_lower.push_back(ToLowerAscii(s));
        }

        const auto record_dir = [&](const fs::path& dir) {
            std::string key = NormalizeComparablePath(dir);
            if (!key.empty() && seen_dirs.insert(std::move(key)).second) {
                found_dirs.push_back(dir);
            }
        };

        std::vector<std::pair<fs::path, std::size_t>> stack;
        stack.emplace_back(root.path, 0);

        std::size_t visited = 0;
        while (!stack.empty() && !stopped()) {
            if ((++visited & 63U) == 0 &&
                std::chrono::steady_clock::now() - started > per_root_time_limit) {
                break;
            }

            auto [dir, depth] = std::move(stack.back());
            stack.pop_back();

            if (depth > 0 && IsExeFS(dir)) {
                if (dir.has_parent_path()) {
                    record_dir(dir.parent_path());
                } else {
                    record_dir(dir);
                }
                continue;
            }

            std::error_code ec;
            bool dir_has_game = false;
            for (fs::directory_iterator it{dir, fs::directory_options::skip_permission_denied, ec};
                 !ec && it != fs::directory_iterator{} && !stopped(); it.increment(ec)) {
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
                    const auto name_u8 = entry.path().filename().u8string();
                    const std::string name_lower = ToLowerAscii(std::string_view(
                        reinterpret_cast<const char*>(name_u8.data()), name_u8.size()));
                    if (name_lower == "exefs") {
                        if (!dir_has_game && IsExeFS(entry.path())) {
                            dir_has_game = true;
                            record_dir(dir);
                        }
                        continue;
                    }
                    if (depth + 1 > root.max_depth) {
                        continue;
                    }
                    if (ShouldSkipAutoDetectDir(name_lower)) {
                        continue;
                    }
                    if (depth == 0 &&
                        std::find(skip_top_lower.begin(), skip_top_lower.end(), name_lower) !=
                            skip_top_lower.end()) {
                        continue;
                    }
                    stack.emplace_back(entry.path(), depth + 1);
                } else if (!dir_has_game && entry.is_regular_file(entry_ec)) {
                    const auto ext_u8 = entry.path().extension().u8string();
                    const std::string ext_lower = ToLowerAscii(std::string_view(
                        reinterpret_cast<const char*>(ext_u8.data()), ext_u8.size()));
                    if (IsSwitchContainerExtension(ext_lower)) {
                        dir_has_game = true;
                        record_dir(dir);
                    }
                }
            }
        }
        return found_dirs;
    };

    std::vector<std::vector<fs::path>> partials(roots.size());
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

    std::vector<fs::path> merged;
    for (auto& part : partials) {
        merged.insert(merged.end(), std::make_move_iterator(part.begin()),
                      std::make_move_iterator(part.end()));
    }
    return CollapseNestedDirs(std::move(merged));
}

template <typename Supported, typename Stopped>
Result Discover(const std::filesystem::path& root, bool deep, Supported supported,
                Stopped stopped) {
    namespace fs = std::filesystem;
    Result result;
    std::unordered_set<fs::path> files;
    std::unordered_set<fs::path> directories;
    const auto add_file = [&](const fs::path& path) {
        if (files.insert(path).second) {
            result.files.push_back(path);
        }
    };
    const auto watch = [&](const fs::path& path) {
        if (directories.insert(path).second) {
            result.directories.push_back(path);
        }
    };
    const auto probe = [&](const fs::path& path) {
        if (IsExeFS(path)) {
            watch(path);
            add_file(path / "main");
        }
    };
    const auto probe_layouts = [&](const fs::path& path) {
        for (const auto& content : {path, path / "base"}) {
            probe(content / "exefs");
            const auto updates = content / "updates";
            std::error_code ec;
            if (!fs::is_directory(updates, ec)) {
                continue;
            }
            watch(updates);
            for (fs::directory_iterator it{updates, fs::directory_options::skip_permission_denied,
                                           ec};
                 !ec && it != fs::directory_iterator{} && !stopped(); it.increment(ec)) {
                probe(it->path() / "exefs");
            }
        }
    };

    std::vector<std::pair<fs::path, unsigned>> pending{{root, 0}};
    while (!pending.empty() && !stopped()) {
        auto [directory, depth] = std::move(pending.back());
        pending.pop_back();
        watch(directory);
        if (IsExeFS(directory)) {
            add_file(directory / "main");
            continue;
        }
        if (!deep) {
            probe_layouts(directory);
        }
        std::error_code ec;
        for (fs::directory_iterator it{directory, fs::directory_options::skip_permission_denied,
                                       ec};
             !ec && it != fs::directory_iterator{} && !stopped(); it.increment(ec)) {
            const auto& entry = *it;
            std::error_code entry_ec;
            if (entry.is_directory(entry_ec)) {
                if (entry.is_symlink(entry_ec)) {
                    continue;
                }
                auto name = entry.path().filename().u8string();
                std::transform(name.begin(), name.end(), name.begin(), [](char8_t c) {
                    return static_cast<char8_t>(std::tolower(static_cast<unsigned char>(c)));
                });
                if (!name.empty() && (name.front() == u8'.' || name.front() == u8'$')) {
                    continue;
                }
                if (name == u8"romfs") {
                    ++result.skipped_romfs;
                    continue;
                }
                if ((deep && depth < 6) || depth == 0) {
                    pending.emplace_back(entry.path(), depth + 1);
                }
            } else if (entry.is_regular_file(entry_ec) && depth == 0 && !deep) {
                if (supported(entry.path())) {
                    add_file(entry.path());
                }
            } else if (entry.is_regular_file(entry_ec) && deep && supported(entry.path())) {
                add_file(entry.path());
            }
        }
        result.errors += ec ? 1 : 0;
    }
    return result;
}

} // namespace GameListDiscovery
