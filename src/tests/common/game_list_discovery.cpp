// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "qt_common/game_list/discovery.h"
#include "qt_common/game_list/scan_index.h"

namespace {

struct ScopedTempDir {
    std::filesystem::path path;

    ScopedTempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("genesis_discovery_test_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void TouchFile(const std::filesystem::path& file_path, std::size_t size_bytes = 16) {
    std::filesystem::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (size_bytes > 0) {
        std::string buf(size_bytes, '\0');
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }
}

} // namespace

TEST_CASE("Console generation classification distinguishes Switch 1 and Switch 2",
          "[game_list_discovery]") {
    using namespace GameListIndex;

    CHECK(ClassifyConsoleGeneration(0x0100152000022000ULL) == ConsoleGeneration::Switch1);
    CHECK(ClassifyConsoleGeneration(0x01006A800016E000ULL) == ConsoleGeneration::Switch1);
    CHECK(ClassifyConsoleGeneration(0x0000000000000000ULL) == ConsoleGeneration::Switch1);
    CHECK(ClassifyConsoleGeneration(0x0400C3F00006E000ULL) == ConsoleGeneration::Switch2);
    CHECK(ClassifyConsoleGeneration(0x040074A01BF12000ULL) == ConsoleGeneration::Switch2);

    CHECK(MatchesSystemFilter(SystemFilter::All, ConsoleGeneration::Switch1));
    CHECK(MatchesSystemFilter(SystemFilter::All, ConsoleGeneration::Switch2));
    CHECK(MatchesSystemFilter(SystemFilter::Switch1, ConsoleGeneration::Switch1));
    CHECK_FALSE(MatchesSystemFilter(SystemFilter::Switch1, ConsoleGeneration::Switch2));
    CHECK(MatchesSystemFilter(SystemFilter::Switch2, ConsoleGeneration::Switch2));
    CHECK_FALSE(MatchesSystemFilter(SystemFilter::Switch2, ConsoleGeneration::Switch1));
}

TEST_CASE("Filename title ID extraction and extra-file priority heuristic",
          "[game_list_discovery]") {
    using namespace GameListIndex;

    CHECK(ExtractTitleIdFromFilename("Mario Tennis Aces[0100BDE00862A000][US][v0].nsp") ==
          0x0100BDE00862A000ULL);
    CHECK(ExtractTitleIdFromFilename("Mario Tennis Aces[0100BDE00862A800][US][v786432].nsp") ==
          0x0100BDE00862A800ULL);
    CHECK(ExtractTitleIdFromFilename("Super Smash Bros Ultimate [DLC-73] [01006A800016F049].nsp") ==
          0x01006A800016F049ULL);
    CHECK_FALSE(ExtractTitleIdFromFilename("Kirby and the Forgotten Land.xci").has_value());

    constexpr u64 BigGame = 2ULL << 30;
    constexpr u64 TinyDlc = 128ULL << 10;

    CHECK_FALSE(IsLikelyDeferredExtra("Games/Kirby and the Forgotten Land.xci", BigGame));
    CHECK_FALSE(
        IsLikelyDeferredExtra("Games/Mario Tennis Aces[0100BDE00862A000][US][v0].nsp", BigGame));
    CHECK(IsLikelyDeferredExtra("Games/Mario Tennis Aces[0100BDE00862A800][US][v786432].nsp",
                                BigGame));
    CHECK(IsLikelyDeferredExtra("Games/DLC + Updates/Something.nsp", BigGame));
    CHECK(IsLikelyDeferredExtra("Games/Zelda Expansion Pass DLC 2.nsp", TinyDlc));
    CHECK(IsLikelyDeferredExtra("Games/UnknownUnlocker.nsp", TinyDlc));
}

TEST_CASE("PersistentScanIndex saves, reloads, validates stamps, and prunes missing entries",
          "[game_list_discovery]") {
    using namespace GameListIndex;

    ScopedTempDir tmp;
    const auto index_file = tmp.path / "scan_index_v1.bin";
    const auto game_file = tmp.path / "Switch Games" / "BotW.xci";
    const auto upd_file = tmp.path / "Switch Games" / "BotW_Update.nsp";

    PersistentScanIndex writer;
    writer.SetAutoDetectedDirs({(tmp.path / "Switch Games").string()});

    FileIndexEntry base_entry;
    base_entry.path = game_file.string();
    base_entry.file_size = 14880000000ULL;
    base_entry.mtime_ns = 1700000000000000000LL;
    base_entry.file_type = 4;
    base_entry.is_bootable = true;
    base_entry.titles.push_back(TitleRecord{
        .program_id = 0x01007EF00011E000ULL,
        .name = "The Legend of Zelda: Breath of the Wild",
        .file_type_str = "XCI",
        .romfs_updatable = true,
        .generation = ConsoleGeneration::Switch1,
    });
    base_entry.referenced_base_ids = {0x01007EF00011E000ULL};
    writer.Upsert(game_file, base_entry);

    FileIndexEntry upd_entry;
    upd_entry.path = upd_file.string();
    upd_entry.file_size = 930000000ULL;
    upd_entry.mtime_ns = 1700000000000000001LL;
    upd_entry.file_type = 5;
    upd_entry.is_bootable = false;
    upd_entry.has_updates_or_dlc = true;
    upd_entry.referenced_base_ids = {0x01007EF00011E000ULL};
    writer.Upsert(upd_file, upd_entry);

    REQUIRE(writer.Save(index_file));

    PersistentScanIndex reader;
    REQUIRE(reader.Load(index_file));
    REQUIRE(reader.AutoDetectedDirs().size() == 1);
    CHECK(reader.Size() == 2);

    const auto* hit = reader.FindMatching(game_file, base_entry.file_size, base_entry.mtime_ns);
    REQUIRE(hit != nullptr);
    CHECK(hit->is_bootable);
    REQUIRE(hit->titles.size() == 1);
    CHECK(hit->titles[0].program_id == 0x01007EF00011E000ULL);
    CHECK(hit->titles[0].name == "The Legend of Zelda: Breath of the Wild");

    // Modified size or mtime must miss cache.
    CHECK(reader.FindMatching(game_file, base_entry.file_size + 1, base_entry.mtime_ns) == nullptr);
    CHECK(reader.FindMatching(game_file, base_entry.file_size, base_entry.mtime_ns + 1) == nullptr);

    std::unordered_set<std::string> live{NormalizePathKey(game_file)};
    reader.PruneMissing(live);
    CHECK(reader.Size() == 1);
    CHECK(reader.FindMatching(upd_file, upd_entry.file_size, upd_entry.mtime_ns) == nullptr);
}

TEST_CASE("DeferredContentRegistry tracks updates and DLC by base title ID",
          "[game_list_discovery]") {
    using namespace GameListIndex;

    auto& reg = DeferredContentRegistry::Instance();
    reg.Clear();

    const u64 botw_base = 0x01007EF00011E000ULL;
    const u64 botw_update = 0x01007EF00011E800ULL;
    const u64 botw_dlc = 0x01007EF00011F001ULL;
    const u64 mk8_update = 0x0100152000022800ULL;

    reg.TrackCandidate("D:/Games/botw_upd.nsp", {botw_update});
    reg.TrackCandidate("D:/Games/botw_dlc.nsp", {botw_dlc});
    reg.TrackCandidate("D:/Games/mk8_upd.nsp", {mk8_update});
    reg.TrackCandidate("D:/Games/unlabeled_extra.nsp", {});

    auto pending = reg.CollectPendingForTitle(botw_base);
    CHECK(pending.size() == 3);
    CHECK(std::find(pending.begin(), pending.end(), "D:/Games/mk8_upd.nsp") == pending.end());

    reg.MarkRegistered("D:/Games/botw_upd.nsp");
    reg.MarkRegistered("D:/Games/unlabeled_extra.nsp");
    pending = reg.CollectPendingForTitle(botw_base);
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == "D:/Games/botw_dlc.nsp");

    reg.Clear();
}

TEST_CASE("AutoDetectRoots finds Switch folders, collapses nested dirs, and skips system paths",
          "[game_list_discovery]") {
    ScopedTempDir tmp;
    const auto root = tmp.path;

    const auto games = root / "Switch Games" / "Switch Games";
    TouchFile(games / "Kirby and the Forgotten Land.xci");
    TouchFile(games / "Zelda" / "Breath of the Wild.xci");
    TouchFile(games / "DLC + Updates" / "Update [0100BDE00862A800].nsp");
    TouchFile(games / "LooseGame" / "romfs" / "ignored.nsp");
    TouchFile(root / "Windows" / "System32" / "fake.nsp");
    TouchFile(root / "ExcludedTop" / "secret.xci");

    const auto loose = root / "LooseDumpTitle";
    TouchFile(loose / "exefs" / "main");
    TouchFile(loose / "exefs" / "main.npdm");

    GameListDiscovery::SearchRoot sr{
        .path = root,
        .max_depth = 6,
        .skip_top = {"excludedtop"},
    };
    const auto found = GameListDiscovery::AutoDetectRoots(
        {sr}, std::chrono::milliseconds{5000}, [] { return false; });

    // Switch Games/Switch Games collapses Zelda underneath it, LooseDumpTitle is found via exefs,
    // Windows and ExcludedTop are skipped.
    REQUIRE(found.size() == 2);
    CHECK(GameListDiscovery::IsPathCoveredByAny(games / "Zelda", found));
    CHECK(GameListDiscovery::IsPathCoveredByAny(loose, found));
    CHECK_FALSE(GameListDiscovery::IsPathCoveredByAny(root / "Windows", found));
    CHECK_FALSE(GameListDiscovery::IsPathCoveredByAny(root / "ExcludedTop", found));
}

TEST_CASE("ParseQtGameDirs extracts custom paths and skips virtual NAND entries",
          "[game_list_discovery]") {
    const std::string ini =
        "[UI]\n"
        "Paths\\gamedirs\\1\\path=SDMC\n"
        "Paths\\gamedirs\\2\\path=UserNAND\n"
        "Paths\\gamedirs\\3\\path=SysNAND\n"
        "Paths\\gamedirs\\4\\path=D:/Switch Games/Switch Games\n"
        "Paths\\gamedirs\\4\\path\\default=false\n"
        "Paths\\gamedirs\\size=4\n";

    const auto dirs = GameListDiscovery::ParseQtGameDirs(ini);
    REQUIRE(dirs.size() == 1);
    CHECK(dirs[0] == std::filesystem::path("D:/Switch Games/Switch Games"));
}
