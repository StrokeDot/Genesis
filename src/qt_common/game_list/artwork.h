// SPDX-FileCopyrightText: Copyright 2026 Genesis Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace QtCommon::GameList {

namespace Detail {

inline std::string ArtGetEnvVar(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
        return {};
    }
    std::string out(raw);
    std::free(raw);
    return out;
#else
    const char* raw = std::getenv(name);
    return raw ? std::string(raw) : std::string{};
#endif
}

inline std::string ArtToLowerAscii(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline std::optional<std::uint64_t> ParseHexTitleId16(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    std::string_view trimmed = text.substr(start, end - start);
    if (trimmed.size() >= 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        trimmed.remove_prefix(2);
    }
    if (trimmed.size() != 16) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char ch : trimmed) {
        value <<= 4;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<std::uint64_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<std::uint64_t>(10 + (ch - 'a'));
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<std::uint64_t>(10 + (ch - 'A'));
        } else {
            return std::nullopt;
        }
    }
    return value;
}

inline std::string SafeGameVaultArtKey(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

inline std::string NormalizeArtTitleKey(std::string_view title) {
    std::string out;
    out.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(title[i]);
        if (c == '+') {
            out.append("plus");
            continue;
        }
        if (c == '&') {
            out.append("and");
            continue;
        }
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

inline std::string NormalizeArtPathKey(const std::filesystem::path& path) {
    std::error_code ec;
    const auto normalized = path.lexically_normal();
    std::string s = normalized.generic_string();
#ifdef _WIN32
    s = ArtToLowerAscii(s);
#endif
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

inline std::string_view KnownSwitchTitleById(std::uint64_t program_id) {
    switch (program_id) {
    case 0x0400C3F00006E000ULL:
        return "Mario Kart World";
    case 0x0400486024DEC000ULL:
        return "The Legend of Zelda: Ocarina of Time";
    case 0x040074A01BF12000ULL:
        return "Donkey Kong Bananza";
    case 0x040077201BE50000ULL:
    case 0x04005B902517C000ULL:
        return "Kirby Air Riders";
    case 0x0400D4201C6EA000ULL:
        return "Mario Tennis Fever";
    case 0x04007C6028AA8000ULL:
        return "Yoshi and the Mysterious Book";
    case 0x0400E88025B1A000ULL:
        return "Star Fox";
    case 0x04007EF01F8AE000ULL:
        return "Fire Emblem: Fortune's Weave";
    case 0x04008E001F098000ULL:
        return "Nintendo Switch Sports Resort";
    case 0x0400A3A01EE4C000ULL:
        return "Pokemon Pokopia";
    case 0x0400A4802437E000ULL:
        return "Drag x Drive";
    case 0x040041E0212AE000ULL:
        return "Fast Fusion";
    case 0x040057C02159A000ULL:
        return "Nintendo Switch 2 Welcome Tour";
    case 0x0400D1D02399A000ULL:
        return "The Duskbloods";
    case 0x0100152000022000ULL:
        return "Mario Kart 8 Deluxe";
    case 0x01007EF00011E000ULL:
        return "The Legend of Zelda: Breath of the Wild";
    case 0x0100F2C0115B6000ULL:
        return "The Legend of Zelda: Tears of the Kingdom";
    case 0x01006A800016E000ULL:
        return "Super Smash Bros. Ultimate";
    case 0x0100000000010000ULL:
        return "Super Mario Odyssey";
    default:
        return {};
    }
}

struct ArtFileCandidate {
    std::filesystem::path path;
    int priority = 0; // higher is better (icon > cover > hero)
};

inline std::optional<ArtFileCandidate> PickBestArtInDir(const std::filesystem::path& dir) {
    static constexpr std::array<std::pair<std::string_view, int>, 11> kCandidates{{
        {"icon.jpg", 30},
        {"icon.png", 30},
        {"icon.jpeg", 30},
        {"icon.webp", 29},
        {"cover.jpg", 20},
        {"cover.png", 20},
        {"cover.jpeg", 20},
        {"cover.webp", 19},
        {"hero.jpg", 10},
        {"hero.png", 10},
        {"hero.webp", 9},
    }};
    std::error_code ec;
    for (const auto& [name, prio] : kCandidates) {
        const auto p = dir / name;
        if (std::filesystem::is_regular_file(p, ec)) {
            const auto sz = std::filesystem::file_size(p, ec);
            if (!ec && sz > 0 && sz <= 16ULL * 1024ULL * 1024ULL) {
                return ArtFileCandidate{p, prio};
            }
        }
    }
    return std::nullopt;
}

// Minimal, strict RFC 8259 string reader for GameVault's library.json.
inline bool ReadJsonString(std::string_view src, std::size_t& pos, std::string& out) {
    out.clear();
    if (pos >= src.size() || src[pos] != '"') {
        return false;
    }
    ++pos;
    while (pos < src.size()) {
        const char c = src[pos++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (pos >= src.size()) {
                return false;
            }
            const char esc = src[pos++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (pos + 4 > src.size()) {
                    return false;
                }
                unsigned int cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = src[pos++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') {
                        cp |= static_cast<unsigned int>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        cp |= static_cast<unsigned int>(10 + (h - 'a'));
                    } else if (h >= 'A' && h <= 'F') {
                        cp |= static_cast<unsigned int>(10 + (h - 'A'));
                    } else {
                        return false;
                    }
                }
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                out.push_back(esc);
                break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

inline void SkipJsonWhitespace(std::string_view src, std::size_t& pos) {
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) {
        ++pos;
    }
}

inline void SkipJsonValue(std::string_view src, std::size_t& pos) {
    SkipJsonWhitespace(src, pos);
    if (pos >= src.size()) {
        return;
    }
    const char c = src[pos];
    if (c == '"') {
        std::string tmp;
        ReadJsonString(src, pos, tmp);
        return;
    }
    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (pos < src.size()) {
            const char cur = src[pos];
            if (cur == '"') {
                std::string tmp;
                if (!ReadJsonString(src, pos, tmp)) {
                    return;
                }
                continue;
            }
            ++pos;
            if (cur == c) {
                ++depth;
            } else if (cur == close) {
                --depth;
                if (depth == 0) {
                    return;
                }
            }
        }
        return;
    }
    while (pos < src.size()) {
        const char cur = src[pos];
        if (cur == ',' || cur == '}' || cur == ']' ||
            std::isspace(static_cast<unsigned char>(cur))) {
            return;
        }
        ++pos;
    }
}

struct LibraryEntry {
    std::string id;
    std::string title;
    std::string rom_path;
};

inline std::vector<LibraryEntry> ParseGameVaultLibraryJson(std::string_view src) {
    std::vector<LibraryEntry> entries;
    std::size_t pos = 0;
    SkipJsonWhitespace(src, pos);
    if (pos >= src.size() || src[pos] != '[') {
        return entries;
    }
    ++pos;
    while (pos < src.size()) {
        SkipJsonWhitespace(src, pos);
        if (pos >= src.size() || src[pos] == ']') {
            break;
        }
        if (src[pos] == ',') {
            ++pos;
            continue;
        }
        if (src[pos] != '{') {
            SkipJsonValue(src, pos);
            continue;
        }
        ++pos;
        LibraryEntry entry;
        while (pos < src.size()) {
            SkipJsonWhitespace(src, pos);
            if (pos >= src.size() || src[pos] == '}') {
                if (pos < src.size()) {
                    ++pos;
                }
                break;
            }
            if (src[pos] == ',') {
                ++pos;
                continue;
            }
            std::string key;
            if (!ReadJsonString(src, pos, key)) {
                break;
            }
            SkipJsonWhitespace(src, pos);
            if (pos >= src.size() || src[pos] != ':') {
                break;
            }
            ++pos;
            SkipJsonWhitespace(src, pos);
            if ((key == "id" || key == "title" || key == "romPath") && pos < src.size() &&
                src[pos] == '"') {
                std::string value;
                if (ReadJsonString(src, pos, value)) {
                    if (key == "id") {
                        entry.id = std::move(value);
                    } else if (key == "title") {
                        entry.title = std::move(value);
                    } else if (key == "romPath") {
                        entry.rom_path = std::move(value);
                    }
                }
            } else {
                SkipJsonValue(src, pos);
            }
        }
        if (!entry.id.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

} // namespace Detail

inline std::optional<std::filesystem::path> FindLocalCoverPath(
    const std::filesystem::path& game_path) {
    if (game_path.empty()) {
        return std::nullopt;
    }
    static constexpr std::array<std::string_view, 4> kExts{".png", ".jpg", ".jpeg", ".webp"};
    std::error_code ec;
    const auto check_dir = [&](const std::filesystem::path& dir, const std::string& stem)
        -> std::optional<std::filesystem::path> {
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
            return std::nullopt;
        }
        const std::array<std::string, 3> bases{stem, "icon", "cover"};
        for (const auto& base : bases) {
            if (base.empty()) {
                continue;
            }
            for (const auto ext : kExts) {
                const auto candidate = dir / (base + std::string(ext));
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    const auto sz = std::filesystem::file_size(candidate, ec);
                    if (!ec && sz > 0 && sz <= 16ULL * 1024ULL * 1024ULL) {
                        return candidate;
                    }
                }
            }
        }
        return std::nullopt;
    };

    const auto parent = game_path.parent_path();
    if (auto found = check_dir(parent, game_path.stem().string())) {
        return found;
    }

    // Loose dumps store their executable at <GameFolder>/exefs/main.
    if (Detail::ArtToLowerAscii(parent.filename().string()) == "exefs") {
        const auto grand = parent.parent_path();
        if (auto found = check_dir(grand, grand.filename().string())) {
            return found;
        }
    }
    return std::nullopt;
}

class GameVaultArtCache {
public:
    static std::filesystem::path DefaultRoot() {
#ifdef _WIN32
        const std::string appdata = Detail::ArtGetEnvVar("APPDATA");
        if (!appdata.empty()) {
            return std::filesystem::path(appdata) / "com.strokedot.gamevault";
        }
        return {};
#elif defined(__APPLE__)
        const std::string home = Detail::ArtGetEnvVar("HOME");
        if (!home.empty()) {
            return std::filesystem::path(home) / "Library" / "Application Support" /
                   "com.strokedot.gamevault";
        }
        return {};
#else
        const std::string xdg = Detail::ArtGetEnvVar("XDG_DATA_HOME");
        if (!xdg.empty()) {
            return std::filesystem::path(xdg) / "com.strokedot.gamevault";
        }
        const std::string home = Detail::ArtGetEnvVar("HOME");
        if (!home.empty()) {
            return std::filesystem::path(home) / ".local" / "share" / "com.strokedot.gamevault";
        }
        return {};
#endif
    }

    static GameVaultArtCache LoadDefault() {
        return LoadFromRoot(DefaultRoot());
    }

    static GameVaultArtCache LoadFromRoot(const std::filesystem::path& root) {
        GameVaultArtCache cache;
        if (root.empty()) {
            return cache;
        }
        std::error_code ec;
        const auto art_root = root / "art";
        if (!std::filesystem::is_directory(art_root, ec)) {
            return cache;
        }

        // 1. Scan every art/* directory for titleid.txt and best image file.
        for (const auto& entry : std::filesystem::directory_iterator(
                 art_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec || !entry.is_directory(ec)) {
                continue;
            }
            const auto best = Detail::PickBestArtInDir(entry.path());
            if (!best) {
                continue;
            }
            const auto tid_file = entry.path() / "titleid.txt";
            if (std::filesystem::is_regular_file(tid_file, ec)) {
                std::ifstream in(tid_file, std::ios::binary);
                std::string text;
                if (std::getline(in, text)) {
                    if (const auto tid = Detail::ParseHexTitleId16(text)) {
                        cache.InsertTitleId(*tid, *best);
                    }
                }
            }
        }

        // 2. Parse library.json to map game titles and ROM paths to their art directories.
        const auto lib_path = root / "library.json";
        if (std::filesystem::is_regular_file(lib_path, ec)) {
            const auto sz = std::filesystem::file_size(lib_path, ec);
            if (!ec && sz > 0 && sz <= 32ULL * 1024ULL * 1024ULL) {
                std::ifstream in(lib_path, std::ios::binary);
                std::string content(static_cast<std::size_t>(sz), '\0');
                if (in.read(content.data(), static_cast<std::streamsize>(sz))) {
                    const auto entries = Detail::ParseGameVaultLibraryJson(content);
                    for (const auto& lib_entry : entries) {
                        const auto dir = art_root / Detail::SafeGameVaultArtKey(lib_entry.id);
                        const auto best = Detail::PickBestArtInDir(dir);
                        if (!best) {
                            continue;
                        }
                        if (!lib_entry.rom_path.empty()) {
                            cache.InsertPath(Detail::NormalizeArtPathKey(lib_entry.rom_path),
                                             *best);
                        }
                        const std::string title_key = Detail::NormalizeArtTitleKey(lib_entry.title);
                        if (!title_key.empty()) {
                            cache.InsertTitleKey(title_key, *best);
                        }
                    }
                }
            }
        }

        return cache;
    }

    [[nodiscard]] bool Empty() const {
        return by_title_id.empty() && by_rom_path.empty() && by_title_key.empty();
    }

    [[nodiscard]] std::optional<std::filesystem::path> FindArt(
        std::uint64_t program_id, std::string_view title,
        const std::filesystem::path& game_path) const {
        if (auto local = FindLocalCoverPath(game_path)) {
            return local;
        }
        if (Empty()) {
            return std::nullopt;
        }

        if (!game_path.empty()) {
            if (auto it = by_rom_path.find(Detail::NormalizeArtPathKey(game_path));
                it != by_rom_path.end()) {
                return it->second.path;
            }
        }

        if (program_id != 0) {
            if (auto it = by_title_id.find(program_id); it != by_title_id.end()) {
                return it->second.path;
            }
            const std::uint64_t base_id = program_id & ~0xFFFULL;
            if (base_id != program_id) {
                if (auto it = by_title_id.find(base_id); it != by_title_id.end()) {
                    return it->second.path;
                }
            }
        }

        if (!title.empty()) {
            const std::string key = Detail::NormalizeArtTitleKey(title);
            if (auto it = by_title_key.find(key); it != by_title_key.end()) {
                return it->second.path;
            }
        }

        if (program_id != 0) {
            const std::string_view known = Detail::KnownSwitchTitleById(program_id);
            if (!known.empty()) {
                const std::string key = Detail::NormalizeArtTitleKey(known);
                if (auto it = by_title_key.find(key); it != by_title_key.end()) {
                    return it->second.path;
                }
            }
        }

        if (!game_path.empty()) {
            const auto parent = game_path.parent_path();
            std::string stem = game_path.stem().string();
            if (Detail::ArtToLowerAscii(parent.filename().string()) == "exefs") {
                stem = parent.parent_path().filename().string();
            }
            const std::string key = Detail::NormalizeArtTitleKey(stem);
            if (!key.empty()) {
                if (auto it = by_title_key.find(key); it != by_title_key.end()) {
                    return it->second.path;
                }
            }
        }

        return std::nullopt;
    }

    static std::vector<std::uint8_t> ReadArtBytes(const std::filesystem::path& path,
                                                  std::size_t max_bytes = 16ULL * 1024ULL *
                                                                          1024ULL) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return {};
        }
        const auto sz = std::filesystem::file_size(path, ec);
        if (ec || sz == 0 || sz > max_bytes) {
            return {};
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return {};
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
        if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz))) {
            return {};
        }
        return data;
    }

private:
    void InsertTitleId(std::uint64_t tid, const Detail::ArtFileCandidate& cand) {
        auto it = by_title_id.find(tid);
        if (it == by_title_id.end() || cand.priority > it->second.priority) {
            by_title_id[tid] = cand;
        }
    }

    void InsertPath(const std::string& path_key, const Detail::ArtFileCandidate& cand) {
        auto it = by_rom_path.find(path_key);
        if (it == by_rom_path.end() || cand.priority > it->second.priority) {
            by_rom_path[path_key] = cand;
        }
    }

    void InsertTitleKey(const std::string& key, const Detail::ArtFileCandidate& cand) {
        auto it = by_title_key.find(key);
        if (it == by_title_key.end() || cand.priority > it->second.priority) {
            by_title_key[key] = cand;
        }
    }

    std::unordered_map<std::uint64_t, Detail::ArtFileCandidate> by_title_id;
    std::unordered_map<std::string, Detail::ArtFileCandidate> by_rom_path;
    std::unordered_map<std::string, Detail::ArtFileCandidate> by_title_key;
};

} // namespace QtCommon::GameList
