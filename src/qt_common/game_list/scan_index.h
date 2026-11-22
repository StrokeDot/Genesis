// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace GameListIndex {

enum class ConsoleGeneration : u8 {
    Switch1 = 1,
    Switch2 = 2,
};

enum class SystemFilter : u8 {
    All = 0,
    Switch1 = 1,
    Switch2 = 2,
};

[[nodiscard]] constexpr ConsoleGeneration ClassifyConsoleGeneration(u64 program_id) noexcept {
    // Switch 2 application title IDs begin with 0x04xxxxxxxxxxxxxx.
    return ((program_id >> 56) == 0x04ULL) ? ConsoleGeneration::Switch2
                                           : ConsoleGeneration::Switch1;
}

[[nodiscard]] constexpr const char* ConsoleGenerationLabel(ConsoleGeneration gen) noexcept {
    return gen == ConsoleGeneration::Switch2 ? "Switch 2" : "Switch 1";
}

[[nodiscard]] constexpr u64 BaseTitleIdMask(u64 title_id) noexcept {
    return title_id & 0xFFFFFFFFFFFFE000ULL;
}

[[nodiscard]] inline bool MatchesSystemFilter(SystemFilter filter, ConsoleGeneration gen) noexcept {
    switch (filter) {
    case SystemFilter::Switch1:
        return gen == ConsoleGeneration::Switch1;
    case SystemFilter::Switch2:
        return gen == ConsoleGeneration::Switch2;
    case SystemFilter::All:
    default:
        return true;
    }
}

/// Extracts a 16-hex-digit title ID inside brackets such as `[0100BDE00862A000]`.
/// When multiple bracketed IDs are present, prefers a base-game ID ending in `000`.
[[nodiscard]] inline std::optional<u64> ExtractTitleIdFromFilename(std::string_view name) {
    std::optional<u64> first_id;
    std::size_t pos = 0;
    while (pos < name.size()) {
        const std::size_t open = name.find('[', pos);
        if (open == std::string_view::npos || open + 17 >= name.size()) {
            break;
        }
        if (name[open + 17] == ']') {
            u64 id = 0;
            bool valid = true;
            for (std::size_t i = 0; i < 16; ++i) {
                const unsigned char ch = static_cast<unsigned char>(name[open + 1 + i]);
                u64 digit = 0;
                if (ch >= '0' && ch <= '9') {
                    digit = ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    digit = 10 + (ch - 'a');
                } else if (ch >= 'A' && ch <= 'F') {
                    digit = 10 + (ch - 'A');
                } else {
                    valid = false;
                    break;
                }
                id = (id << 4) | digit;
            }
            if (valid && id != 0) {
                if ((id & 0xFFFULL) == 0) {
                    return id;
                }
                if (!first_id.has_value()) {
                    first_id = id;
                }
            }
        }
        pos = open + 1;
    }
    return first_id;
}

[[nodiscard]] inline std::string ToLowerAscii(std::string_view s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

[[nodiscard]] inline bool NameLooksLikeExtra(std::string_view name) {
    const std::string lower = ToLowerAscii(name);
    static constexpr std::array<std::string_view, 8> tokens = {
        "dlc", "update", "[upd]", "expansion pass", "booster course", "firmware", "keys", "save",
    };
    return std::any_of(tokens.begin(), tokens.end(), [&](std::string_view tok) {
        return lower.find(tok) != std::string::npos;
    });
}

/// Priority heuristic: returns true when a file looks like an update or DLC container
/// so base games can be parsed and displayed first while extras register in the background.
[[nodiscard]] inline bool IsLikelyDeferredExtra(const std::filesystem::path& path, u64 file_size) {
    const auto filename_u8 = path.filename().u8string();
    const std::string filename(reinterpret_cast<const char*>(filename_u8.data()),
                               filename_u8.size());
    const auto ext_u8 = path.extension().u8string();
    const std::string ext =
        ToLowerAscii(std::string_view(reinterpret_cast<const char*>(ext_u8.data()), ext_u8.size()));

    if (ext != ".nsp" && ext != ".nsz" && ext != ".xci" && ext != ".xcz" && ext != ".nca") {
        return false;
    }

    const auto filename_id = ExtractTitleIdFromFilename(filename);
    if (filename_id.has_value()) {
        // Base games end in 000; updates end in 800 and DLC uses other non-zero endings.
        return (*filename_id & 0xFFFULL) != 0;
    }

    if (NameLooksLikeExtra(filename)) {
        return true;
    }

    if (path.has_parent_path()) {
        const auto parent_u8 = path.parent_path().filename().u8string();
        const std::string parent(reinterpret_cast<const char*>(parent_u8.data()), parent_u8.size());
        if (NameLooksLikeExtra(parent)) {
            return true;
        }
    }

    // Tiny NSP/NSZ files without a base title ID are almost always DLC unlockers.
    constexpr u64 SmallExtraThresholdBytes = 50ULL * 1024ULL * 1024ULL;
    if ((ext == ".nsp" || ext == ".nsz") && file_size > 0 && file_size < SmallExtraThresholdBytes) {
        return true;
    }

    return false;
}

[[nodiscard]] inline std::string NormalizePathKey(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal().u8string();
    std::string out(reinterpret_cast<const char*>(normalized.data()), normalized.size());
#ifdef _WIN32
    std::replace(out.begin(), out.end(), '\\', '/');
    out = ToLowerAscii(out);
#endif
    while (out.size() > 1 && out.back() == '/') {
#ifdef _WIN32
        if (out.size() == 3 && out[1] == ':') {
            break;
        }
#endif
        out.pop_back();
    }
    return out;
}

[[nodiscard]] inline std::pair<u64, s64> QueryFileStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const u64 size = std::filesystem::file_size(path, ec);
    if (ec) {
        return {0, 0};
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {size, 0};
    }
    const s64 ticks = static_cast<s64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count());
    return {size, ticks};
}

struct TitleRecord {
    u64 program_id{0};
    std::string name;
    std::string file_type_str;
    bool romfs_updatable{true};
    ConsoleGeneration generation{ConsoleGeneration::Switch1};
};

struct FileIndexEntry {
    std::string path;
    u64 file_size{0};
    s64 mtime_ns{0};
    u8 file_type{0};
    bool is_bootable{false};
    bool has_updates_or_dlc{false};
    bool is_loose_dump{false};
    std::vector<TitleRecord> titles;
    std::vector<u64> referenced_base_ids;
};

class PersistentScanIndex {
public:
    static constexpr std::array<char, 4> Magic{'G', 'N', 'S', 'I'};
    static constexpr u32 FormatVersion = 1;

    bool Load(const std::filesystem::path& file_path) {
        entries.clear();
        auto_detected_dirs.clear();

        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open()) {
            return false;
        }

        std::array<char, 4> magic{};
        u32 version = 0;
        if (!ReadRaw(in, magic) || magic != Magic || !ReadRaw(in, version) ||
            version != FormatVersion) {
            return false;
        }

        u32 dir_count = 0;
        if (!ReadRaw(in, dir_count) || dir_count > 4096) {
            return false;
        }
        auto_detected_dirs.reserve(dir_count);
        for (u32 i = 0; i < dir_count; ++i) {
            std::string dir;
            if (!ReadString(in, dir)) {
                return false;
            }
            auto_detected_dirs.push_back(std::move(dir));
        }

        u32 entry_count = 0;
        if (!ReadRaw(in, entry_count) || entry_count > 1000000) {
            return false;
        }
        entries.reserve(entry_count);
        for (u32 i = 0; i < entry_count; ++i) {
            FileIndexEntry e;
            u8 flags = 0;
            u32 title_count = 0;
            u32 ref_count = 0;
            if (!ReadString(in, e.path) || !ReadRaw(in, e.file_size) || !ReadRaw(in, e.mtime_ns) ||
                !ReadRaw(in, e.file_type) || !ReadRaw(in, flags) || !ReadRaw(in, title_count) ||
                title_count > 256) {
                return false;
            }
            e.is_bootable = (flags & 0x01) != 0;
            e.has_updates_or_dlc = (flags & 0x02) != 0;
            e.is_loose_dump = (flags & 0x04) != 0;

            e.titles.reserve(title_count);
            for (u32 t = 0; t < title_count; ++t) {
                TitleRecord rec;
                u8 tflags = 0;
                u8 gen = 1;
                if (!ReadRaw(in, rec.program_id) || !ReadString(in, rec.name) ||
                    !ReadString(in, rec.file_type_str) || !ReadRaw(in, tflags) || !ReadRaw(in, gen)) {
                    return false;
                }
                rec.romfs_updatable = (tflags & 0x01) != 0;
                rec.generation =
                    gen == 2 ? ConsoleGeneration::Switch2 : ConsoleGeneration::Switch1;
                e.titles.push_back(std::move(rec));
            }

            if (!ReadRaw(in, ref_count) || ref_count > 1024) {
                return false;
            }
            e.referenced_base_ids.resize(ref_count);
            for (u32 r = 0; r < ref_count; ++r) {
                if (!ReadRaw(in, e.referenced_base_ids[r])) {
                    return false;
                }
            }

            std::string key = NormalizePathKey(
                std::filesystem::path(std::u8string_view(
                    reinterpret_cast<const char8_t*>(e.path.data()), e.path.size())));
            entries.insert_or_assign(std::move(key), std::move(e));
        }

        return true;
    }

    bool Save(const std::filesystem::path& file_path) const {
        std::error_code ec;
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path(), ec);
        }

        auto tmp_path = file_path;
        tmp_path += ".tmp";

        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }

            WriteRaw(out, Magic);
            WriteRaw(out, FormatVersion);

            const u32 dir_count = static_cast<u32>(auto_detected_dirs.size());
            WriteRaw(out, dir_count);
            for (const auto& dir : auto_detected_dirs) {
                WriteString(out, dir);
            }

            const u32 entry_count = static_cast<u32>(entries.size());
            WriteRaw(out, entry_count);
            for (const auto& [_, e] : entries) {
                WriteString(out, e.path);
                WriteRaw(out, e.file_size);
                WriteRaw(out, e.mtime_ns);
                WriteRaw(out, e.file_type);
                const u8 flags = static_cast<u8>((e.is_bootable ? 0x01 : 0) |
                                                 (e.has_updates_or_dlc ? 0x02 : 0) |
                                                 (e.is_loose_dump ? 0x04 : 0));
                WriteRaw(out, flags);

                const u32 title_count = static_cast<u32>(e.titles.size());
                WriteRaw(out, title_count);
                for (const auto& rec : e.titles) {
                    WriteRaw(out, rec.program_id);
                    WriteString(out, rec.name);
                    WriteString(out, rec.file_type_str);
                    const u8 tflags = rec.romfs_updatable ? 0x01 : 0x00;
                    WriteRaw(out, tflags);
                    WriteRaw(out, static_cast<u8>(rec.generation));
                }

                const u32 ref_count = static_cast<u32>(e.referenced_base_ids.size());
                WriteRaw(out, ref_count);
                for (const u64 base_id : e.referenced_base_ids) {
                    WriteRaw(out, base_id);
                }
            }

            if (!out.good()) {
                return false;
            }
        }

        std::filesystem::rename(tmp_path, file_path, ec);
        if (ec) {
            std::error_code rm_ec;
            std::filesystem::remove(file_path, rm_ec);
            ec.clear();
            std::filesystem::rename(tmp_path, file_path, ec);
            if (ec) {
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const FileIndexEntry* FindMatching(const std::filesystem::path& path,
                                                     u64 file_size, s64 mtime_ns) const {
        const auto it = entries.find(NormalizePathKey(path));
        if (it == entries.end()) {
            return nullptr;
        }
        if (it->second.file_size != file_size || it->second.mtime_ns != mtime_ns) {
            return nullptr;
        }
        return &it->second;
    }

    void Upsert(const std::filesystem::path& path, FileIndexEntry entry) {
        entries.insert_or_assign(NormalizePathKey(path), std::move(entry));
    }

    void PruneMissing(const std::unordered_set<std::string>& live_keys) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (!live_keys.contains(it->first)) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] const std::vector<std::string>& AutoDetectedDirs() const {
        return auto_detected_dirs;
    }

    void SetAutoDetectedDirs(std::vector<std::string> dirs) {
        auto_detected_dirs = std::move(dirs);
    }

    [[nodiscard]] std::size_t Size() const {
        return entries.size();
    }

private:
    template <typename T>
    static bool ReadRaw(std::ifstream& in, T& value) {
        return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
    }

    template <typename T>
    static void WriteRaw(std::ofstream& out, const T& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    static bool ReadString(std::ifstream& in, std::string& out_str) {
        u32 len = 0;
        if (!ReadRaw(in, len) || len > (16U << 20)) {
            return false;
        }
        out_str.resize(len);
        if (len == 0) {
            return true;
        }
        return static_cast<bool>(in.read(out_str.data(), len));
    }

    static void WriteString(std::ofstream& out, const std::string& str) {
        const u32 len = static_cast<u32>(str.size());
        WriteRaw(out, len);
        if (len > 0) {
            out.write(str.data(), len);
        }
    }

    std::vector<std::string> auto_detected_dirs;
    std::unordered_map<std::string, FileIndexEntry> entries;
};

/// Tracks discovered update/DLC container files so that they can register in the
/// background without delaying the game list, and can also be registered on demand
/// right before a title boots.
class DeferredContentRegistry {
public:
    static DeferredContentRegistry& Instance() {
        static DeferredContentRegistry instance;
        return instance;
    }

    void Clear() {
        std::scoped_lock lk(mutex);
        by_base_id.clear();
        unknown_paths.clear();
        registered_paths.clear();
    }

    void TrackCandidate(const std::string& path, const std::vector<u64>& base_ids) {
        std::scoped_lock lk(mutex);
        if (base_ids.empty()) {
            unknown_paths.insert(path);
            return;
        }
        for (const u64 base_id : base_ids) {
            if (base_id != 0) {
                by_base_id[BaseTitleIdMask(base_id)].insert(path);
            }
        }
    }

    void MarkRegistered(const std::string& path) {
        std::scoped_lock lk(mutex);
        registered_paths.insert(path);
        unknown_paths.erase(path);
    }

    [[nodiscard]] bool IsRegistered(const std::string& path) const {
        std::scoped_lock lk(mutex);
        return registered_paths.contains(path);
    }

    /// Returns all unregistered candidate files that may hold updates/DLC for `program_id`.
    [[nodiscard]] std::vector<std::string> CollectPendingForTitle(u64 program_id) const {
        std::scoped_lock lk(mutex);
        std::vector<std::string> result;
        const u64 base_id = BaseTitleIdMask(program_id);
        if (const auto it = by_base_id.find(base_id); it != by_base_id.end()) {
            for (const auto& p : it->second) {
                if (!registered_paths.contains(p)) {
                    result.push_back(p);
                }
            }
        }
        for (const auto& p : unknown_paths) {
            if (!registered_paths.contains(p)) {
                result.push_back(p);
            }
        }
        return result;
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<u64, std::unordered_set<std::string>> by_base_id;
    std::unordered_set<std::string> unknown_paths;
    std::unordered_set<std::string> registered_paths;
};

} // namespace GameListIndex
