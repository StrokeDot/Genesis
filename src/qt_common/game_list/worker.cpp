// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"
#include "core/loader/nx.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

#include "qt_common/game_list/artwork.h"
#include "qt_common/game_list/discovery.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/game_list/scan_index.h"
#include "qt_common/game_list/worker.h"

namespace {

constexpr auto AutoDetectPerRootTimeout = std::chrono::milliseconds{2000};

struct GameRowPayload {
    std::string path;
    std::string name;
    std::size_t size{0};
    std::vector<u8> icon;
    std::string file_type_string;
    u64 program_id{0};
    u64 play_time{0};
    QString patch_versions;
    bool loose_dump{false};
};

struct FileScanTask {
    std::filesystem::path path;
    std::string physical_name;
    u64 file_size{0};
    s64 mtime_ns{0};
    GameListDir* parent_dir{nullptr};
    std::string dir_path;
    bool is_auto_detected{false};
    bool deferred_emit{false};
};

std::filesystem::path GameListCacheDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list";
}

std::filesystem::path ScanIndexPath() {
    return GameListCacheDir() / "scan_index_v1.bin";
}

std::mutex& CacheIOMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<u8> ReadCachedIcon(u64 program_id) {
    if (!UISettings::values.cache_game_list || program_id == 0) {
        return {};
    }
    std::scoped_lock lk(CacheIOMutex());
    const auto path = Common::FS::PathToUTF8String(
        GameListCacheDir() / fmt::format("{:016X}.jpeg", program_id));
    QFile file(QString::fromStdString(path));
    if (!file.open(QFile::ReadOnly)) {
        return {};
    }
    std::vector<u8> vec(static_cast<std::size_t>(file.size()));
    if (!vec.empty() && file.read(reinterpret_cast<char*>(vec.data()),
                                  static_cast<qint64>(vec.size())) !=
                            static_cast<qint64>(vec.size())) {
        return {};
    }
    return vec;
}

void WriteCachedIconAndName(u64 program_id, const std::vector<u8>& icon, const std::string& name) {
    if (!UISettings::values.cache_game_list || program_id == 0) {
        return;
    }
    std::scoped_lock lk(CacheIOMutex());
    const auto cache_dir = GameListCacheDir();
    void(Common::FS::CreateDirs(cache_dir));

    if (!icon.empty()) {
        const auto icon_path =
            Common::FS::PathToUTF8String(cache_dir / fmt::format("{:016X}.jpeg", program_id));
        const QFileInfo existing{QString::fromStdString(icon_path)};
        if (!existing.exists() || existing.size() == 0) {
            QFile file1{QString::fromStdString(icon_path)};
            if (file1.open(QFile::WriteOnly | QFile::Truncate)) {
                file1.write(reinterpret_cast<const char*>(icon.data()),
                            static_cast<qint64>(icon.size()));
            }
        }
    }

    if (!name.empty()) {
        const auto name_path = Common::FS::PathToUTF8String(
            cache_dir / fmt::format("{:016X}.appname.txt", program_id));
        if (!Common::FS::Exists(name_path)) {
            QFile file2{QString::fromStdString(name_path)};
            if (file2.open(QFile::WriteOnly)) {
                file2.write(name.data(), static_cast<qint64>(name.size()));
            }
        }
    }
}

QString ReadCachedPatchVersions(u64 program_id) {
    if (!UISettings::values.cache_game_list || program_id == 0) {
        return {};
    }
    std::scoped_lock lk(CacheIOMutex());
    const auto path = Common::FS::PathToUTF8String(
        GameListCacheDir() / fmt::format("{:016X}.pv.txt", program_id));
    QFile file{QString::fromStdString(path)};
    if (!file.open(QFile::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void WriteCachedPatchVersions(u64 program_id, const QString& patch_versions) {
    if (!UISettings::values.cache_game_list || program_id == 0) {
        return;
    }
    std::scoped_lock lk(CacheIOMutex());
    const auto cache_dir = GameListCacheDir();
    void(Common::FS::CreateDirs(cache_dir));
    const auto path =
        Common::FS::PathToUTF8String(cache_dir / fmt::format("{:016X}.pv.txt", program_id));
    QFile file{QString::fromStdString(path)};
    if (file.open(QFile::WriteOnly | QFile::Truncate)) {
        file.write(patch_versions.toUtf8());
    }
}

QString GetGameListCachedObject(const std::string& filename, const std::string& ext,
                                const std::function<QString()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto path = Common::FS::PathToUTF8String(
        GameListCacheDir() / fmt::format("{}.{}", filename, ext));

    {
        std::scoped_lock lk(CacheIOMutex());
        if (Common::FS::Exists(path)) {
            QFile file{QString::fromStdString(path)};
            if (file.open(QFile::ReadOnly)) {
                return QString::fromUtf8(file.readAll());
            }
        }
    }

    const auto str = generator();
    {
        std::scoped_lock lk(CacheIOMutex());
        void(Common::FS::CreateParentDirs(path));
        QFile file{QString::fromStdString(path)};
        if (file.open(QFile::WriteOnly)) {
            file.write(str.toUtf8());
        }
    }
    return str;
}

std::pair<std::vector<u8>, std::string> GetGameListCachedObject(
    const std::string& filename, const std::string& /*ext*/,
    const std::function<std::pair<std::vector<u8>, std::string>()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto game_list_dir = GameListCacheDir();
    const auto jpeg_name = fmt::format("{}.jpeg", filename);
    const auto app_name = fmt::format("{}.appname.txt", filename);

    const auto path1 = Common::FS::PathToUTF8String(game_list_dir / jpeg_name);
    const auto path2 = Common::FS::PathToUTF8String(game_list_dir / app_name);

    void(Common::FS::CreateParentDirs(path1));

    if (!Common::FS::Exists(path1) || !Common::FS::Exists(path2)) {
        const auto [icon, nacp] = generator();

        QFile file1{QString::fromStdString(path1)};
        if (!file1.open(QFile::WriteOnly)) {
            LOG_ERROR(Frontend, "Failed to open cache file.");
            return std::make_pair(icon, nacp);
        }

        if (!file1.resize(static_cast<qint64>(icon.size()))) {
            LOG_ERROR(Frontend, "Failed to resize cache file to necessary size.");
            return std::make_pair(icon, nacp);
        }

        if (file1.write(reinterpret_cast<const char*>(icon.data()),
                        static_cast<qint64>(icon.size())) != static_cast<s64>(icon.size())) {
            LOG_ERROR(Frontend, "Failed to write data to cache file.");
            return std::make_pair(icon, nacp);
        }

        QFile file2{QString::fromStdString(path2)};
        if (file2.open(QFile::WriteOnly)) {
            file2.write(nacp.data(), static_cast<qint64>(nacp.size()));
        }

        return std::make_pair(icon, nacp);
    }

    QFile file1(QString::fromStdString(path1));
    QFile file2(QString::fromStdString(path2));

    if (!file1.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    if (!file2.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    std::vector<u8> vec(static_cast<std::size_t>(file1.size()));
    if (file1.read(reinterpret_cast<char*>(vec.data()), static_cast<qint64>(vec.size())) !=
        static_cast<s64>(vec.size())) {
        return generator();
    }

    const auto data = file2.readAll();
    return std::make_pair(vec, data.toStdString());
}

void GetMetadataFromControlNCA(const FileSys::PatchManager& patch_manager, const FileSys::NCA& nca,
                               std::vector<u8>& icon, std::string& name) {
    std::tie(icon, name) = GetGameListCachedObject(
        fmt::format("{:016X}", patch_manager.GetTitleID()), {}, [&patch_manager, &nca] {
            const auto [nacp, icon_f] = patch_manager.ParseControlNCA(nca);
            return std::make_pair(icon_f->ReadAllBytes(), nacp->GetApplicationName());
        });
}

bool HasSupportedFileExtension(const std::string& file_name) {
    const QFileInfo file = QFileInfo(QString::fromStdString(file_name));
    return QtCommon::supported_file_extensions.contains(file.suffix(), Qt::CaseInsensitive);
}

bool IsExtractedNCAMain(const std::string& file_name) {
    return QFileInfo(QString::fromStdString(file_name)).fileName() == QStringLiteral("main");
}

bool IsContainerExtension(const std::string& file_name) {
    const auto ext = Common::ToLower(std::string(Common::FS::GetExtensionFromFilename(file_name)));
    return ext == "nsp" || ext == "nsz" || ext == "xci" || ext == "xcz";
}

QString FormatGameName(const std::string& physical_name) {
    const QString physical_name_as_qstring = QString::fromStdString(physical_name);
    const QFileInfo file_info(physical_name_as_qstring);

    if (IsExtractedNCAMain(physical_name)) {
        return file_info.dir().path();
    }

    return physical_name_as_qstring;
}

// Loose ExeFS dumps (<game>/exefs/main, <game>/base/exefs/main, updates in
// <game>/updates/<title id>/exefs/main) have no control data, so they are named after their
// game folder.
QString LooseExeFSName(const std::string& physical_name) {
    QDir dir = QFileInfo(QString::fromStdString(physical_name)).dir();
    if (dir.dirName().compare(QStringLiteral("exefs"), Qt::CaseInsensitive) != 0 || !dir.cdUp()) {
        return {};
    }

    const auto is_storage_level = [](const QString& name) {
        if (name.compare(QStringLiteral("base"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            return true;
        }
        return !name.isEmpty() &&
               std::all_of(name.cbegin(), name.cend(), [](QChar c) {
                   return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
                          (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
               });
    };

    bool is_update = false;
    while (is_storage_level(dir.dirName())) {
        if (dir.dirName().compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            dir.dirName().compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            is_update = true;
        }
        if (!dir.cdUp()) {
            return {};
        }
    }

    const QString name = dir.dirName();
    if (name.isEmpty()) {
        return {};
    }
    if (is_update && !name.contains(QStringLiteral("update"), Qt::CaseInsensitive)) {
        return QObject::tr("%1 (update)").arg(name);
    }
    return name;
}

QString FormatPatchEntries(const std::vector<FileSys::Patch>& patches,
                           const std::string& container_type_string, bool updatable = true) {
    QString out;
    for (const auto& patch : patches) {
        const bool is_update = patch.name == "Update";
        if (!updatable && is_update) {
            continue;
        }

        const QString type =
            QString::fromStdString(patch.enabled ? patch.name : "[D] " + patch.name);

        if (patch.version.empty()) {
            out.append(QStringLiteral("%1\n").arg(type));
        } else {
            auto ver = patch.version;
            if (is_update && ver == "PACKED") {
                ver = container_type_string;
            }
            out.append(QStringLiteral("%1 (%2)\n").arg(type, QString::fromStdString(ver)));
        }
    }

    out.chop(1);
    return out;
}

QString FormatPatchNameVersions(const FileSys::PatchManager& patch_manager,
                                Loader::AppLoader& loader, bool updatable = true) {
    FileSys::VirtualFile update_raw;
    loader.ReadUpdateRaw(update_raw);
    return FormatPatchEntries(patch_manager.GetPatches(update_raw),
                              Loader::GetFileTypeString(loader.GetFileType()), updatable);
}

QList<QStandardItem*> BuildRowItems(const GameRowPayload& row) {
    const auto gen = GameListIndex::ClassifyConsoleGeneration(row.program_id);
    const QString system_label = QString::fromLatin1(GameListIndex::ConsoleGenerationLabel(gen));
    const QString file_type_qstr = QString::fromStdString(row.file_type_string);

    auto* size_item = new GameListItemSize(row.size);
    if (row.loose_dump) {
        size_item->setText(QObject::tr("Not scanned"));
        size_item->setToolTip(QObject::tr("Folder size is not calculated during game discovery."));
    }

    return QList<QStandardItem*>{
        new GameListItemPath(FormatGameName(row.path), row.icon, QString::fromStdString(row.name),
                             file_type_qstr, row.program_id, row.play_time, row.patch_versions),
        new GameListItem(system_label),
        new GameListItem(file_type_qstr),
        size_item,
        new GameListItemPlayTime(row.play_time),
        new GameListItem(row.patch_versions),
    };
}

bool LooksLikeImageBytes(std::string_view data) {
    if (data.size() < 12) {
        return false;
    }
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(data[i]); };
    // JPEG
    if (b(0) == 0xFF && b(1) == 0xD8 && b(2) == 0xFF) {
        return true;
    }
    // PNG
    if (b(0) == 0x89 && b(1) == 0x50 && b(2) == 0x4E && b(3) == 0x47) {
        return true;
    }
    // WebP
    if (data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WEBP") {
        return true;
    }
    return false;
}

std::vector<u8> FetchOnlineSwitchIcon(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    try {
        httplib::Client client("https://api.nlib.cc");
        client.set_connection_timeout(3, 0);
        client.set_read_timeout(5, 0);
        client.set_write_timeout(3, 0);
        client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
        const std::string path = fmt::format("/nx/{:016X}/icon", program_id);
        httplib::Headers headers{{"User-Agent", "Genesis"}};
        auto res = client.Get(path, headers);
        if (!res || res->status != 200 || res->body.empty() ||
            res->body.size() > 4ULL * 1024ULL * 1024ULL) {
            return {};
        }
        if (!LooksLikeImageBytes(res->body)) {
            return {};
        }
        return std::vector<u8>(res->body.begin(), res->body.end());
    } catch (...) {
        return {};
    }
}

template <typename Item, typename Fn>
void ParallelForEach(const std::vector<Item>& items, const std::atomic_bool& stop_requested,
                     Fn&& fn) {
    if (items.empty() || stop_requested.load()) {
        return;
    }
    if (items.size() == 1) {
        fn(items[0]);
        return;
    }
    const std::size_t hw = std::thread::hardware_concurrency();
    const std::size_t thread_count =
        std::min<std::size_t>(items.size(), std::clamp<std::size_t>(hw == 0 ? 4 : hw, 2, 8));

    std::atomic<std::size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        workers.emplace_back([&] {
            while (!stop_requested.load()) {
                const std::size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
                if (idx >= items.size()) {
                    break;
                }
                fn(items[idx]);
            }
        });
    }
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }
}

} // Anonymous namespace

GameListWorker::GameListWorker(FileSys::VirtualFilesystem vfs_,
                               FileSys::ManualContentProvider* provider_,
                               QVector<UISettings::GameDir>& game_dirs_,
                               const PlayTime::PlayTimeManager& play_time_manager_,
                               Core::System& system_)
    : vfs{std::move(vfs_)}, provider{provider_}, game_dirs{game_dirs_},
      play_time_manager{play_time_manager_}, system{system_} {
    // We want the game list to manage our lifetime.
    setAutoDelete(false);
}

GameListWorker::~GameListWorker() {
    this->disconnect();
    Stop();
}

void GameListWorker::Stop() {
    if (!stop_requested.exchange(true)) {
        processing_completed.Wait();
    }
}

void GameListWorker::ProcessEvents(GameListModel* model) {
    // An event may spin a nested event loop that replaces this worker, in which case stop.
    while (model->current_worker.get() == this) {
        std::function<void(GameListModel*)> func;
        {
            std::scoped_lock lk(lock);
            if (queued_events.empty()) {
                return;
            }
            func = std::move(queued_events.back());
            queued_events.pop_back();
        }
        func(model);
    }
}

template <typename F>
void GameListWorker::RecordEvent(F&& func) {
    {
        std::scoped_lock lk(lock);
        queued_events.emplace_front(std::forward<F>(func));
    }
    emit DataAvailable();
}

void GameListWorker::AddTitlesToGameList(GameListDir* parent_dir, GameListItemType parent_type) {
    using namespace FileSys;

    const auto& cache = system.GetContentProviderUnion();

    auto installed_games = cache.ListEntriesFilterOrigin(std::nullopt, TitleType::Application,
                                                         ContentRecordType::Program);

    if (parent_type == GameListItemType::SdmcDir) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SDMC, TitleType::Application, ContentRecordType::Program);
    } else if (parent_type == GameListItemType::UserNandDir) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::UserNAND, TitleType::Application, ContentRecordType::Program);
    } else if (parent_type == GameListItemType::SysNandDir) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SysNAND, TitleType::Application, ContentRecordType::Program);
    }

    for (const auto& [slot, game] : installed_games) {
        if (stop_requested.load()) {
            break;
        }
        if (slot == ContentProviderUnionSlot::FrontendManual) {
            continue;
        }

        const auto file = cache.GetEntryUnparsed(game.title_id, game.type);
        std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(system, file);
        if (!loader) {
            continue;
        }

        std::vector<u8> icon;
        std::string name;
        u64 program_id = 0;
        if (loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {
            continue;
        }

        const PatchManager patch{program_id, system.GetFileSystemController(),
                                 system.GetContentProvider()};
        const auto control = cache.GetEntry(game.title_id, ContentRecordType::Control);
        if (control != nullptr) {
            GetMetadataFromControlNCA(patch, *control, icon, name);
        }

        QString patch_versions = GetGameListCachedObject(
            fmt::format("{:016X}", patch.GetTitleID()), "pv.txt", [&patch, &loader] {
                return FormatPatchNameVersions(patch, *loader, loader->IsRomFSUpdatable());
            });

        GameRowPayload payload{
            .path = file->GetFullPath(),
            .name = std::move(name),
            .size = file->GetSize(),
            .icon = std::move(icon),
            .file_type_string = Loader::GetFileTypeString(loader->GetFileType()),
            .program_id = program_id,
            .play_time = play_time_manager.GetPlayTime(program_id),
            .patch_versions = std::move(patch_versions),
            .loose_dump = false,
        };
        RecordEvent([payload = std::move(payload), parent_dir](GameListModel* model) {
            model->AddEntry(BuildRowItems(payload), parent_dir);
        });
    }
}

void GameListWorker::run() {
    const auto started = std::chrono::steady_clock::now();
    watch_list.clear();
    // Preserve previously registered updates/DLC so a cancelled scan restores them.
    FileSys::ManualContentProvider previous;
    provider->SwapEntries(previous);
    auto& deferred_registry = GameListIndex::DeferredContentRegistry::Instance();
    deferred_registry.Clear();

    const bool use_cache = UISettings::values.cache_game_list.GetValue();
    const bool ext_content_enabled = Settings::values.ext_content_from_game_dirs.GetValue();
    const bool auto_detect_enabled = UISettings::values.auto_detect_games.GetValue();
    const auto art_cache = QtCommon::GameList::GameVaultArtCache::LoadDefault();

    GameListIndex::PersistentScanIndex scan_index;
    const auto index_path = ScanIndexPath();
    if (use_cache) {
        scan_index.Load(index_path);
    }

    std::mutex index_mutex;
    bool index_dirty = false;
    std::unordered_set<std::string> live_keys;
    std::unordered_set<std::string> seen_file_keys;
    std::unordered_set<u64> emitted_program_ids;
    std::unordered_set<u64> missing_icon_ids;
    std::mutex emitted_mutex;

    const auto resolve_fallback_art = [&](u64 program_id, std::string_view title,
                                          const std::filesystem::path& game_path)
        -> std::vector<u8> {
        if (const auto art_path = art_cache.FindArt(program_id, title, game_path)) {
            auto bytes = QtCommon::GameList::GameVaultArtCache::ReadArtBytes(*art_path);
            if (!bytes.empty()) {
                WriteCachedIconAndName(program_id, bytes, {});
                return bytes;
            }
        }
        return {};
    };

    const auto DirEntryReady = [&](GameListDir* game_list_dir) {
        auto pending = std::make_shared<std::unique_ptr<GameListDir>>(game_list_dir);
        RecordEvent([pending](GameListModel* model) { model->AddDirEntry(pending->release()); });
    };

    const auto emit_row = [&](GameRowPayload payload, GameListDir* parent_dir,
                              const std::string& dir_path, bool is_auto_detected,
                              bool deferred_emit) {
        if (payload.program_id != 0) {
            std::scoped_lock lk(emitted_mutex);
            emitted_program_ids.insert(payload.program_id);
            if (payload.icon.empty()) {
                missing_icon_ids.insert(payload.program_id);
            }
        }
        if (deferred_emit) {
            const QString qdir = QString::fromStdString(dir_path);
            RecordEvent([payload = std::move(payload), qdir, is_auto_detected](
                            GameListModel* model) {
                model->AddDeferredEntry(BuildRowItems(payload), qdir, is_auto_detected);
            });
        } else {
            RecordEvent([payload = std::move(payload), parent_dir](GameListModel* model) {
                model->AddEntry(BuildRowItems(payload), parent_dir);
            });
        }
    };

    std::vector<FileScanTask> priority_parse_queue;
    std::vector<FileScanTask> deferred_parse_queue;
    std::vector<FileScanTask> deferred_register_queue;

    std::size_t cache_hits = 0;
    std::size_t cache_misses = 0;

    // Collects files in one directory, emits cached bootable titles immediately without
    // opening any container, and routes uncached / extra files to the priority or deferred queues.
    const auto collect_directory = [&](const std::filesystem::path& dir_fs_path, bool deep_scan,
                                       GameListDir* parent_dir, const std::string& dir_str,
                                       bool is_auto_detected, bool deferred_emit) {
        const auto discovery = GameListDiscovery::Discover(
            dir_fs_path, deep_scan,
            [](const auto& path) {
                const auto name = Common::FS::PathToUTF8String(path);
                return HasSupportedFileExtension(name) || IsExtractedNCAMain(name);
            },
            [this] { return stop_requested.load(); });

        for (const auto& directory : discovery.directories) {
            watch_list.append(QString::fromStdString(Common::FS::PathToUTF8String(directory)));
        }
        LOG_INFO(Frontend,
                 "Discovery ({}): candidates={} directories={} skipped_romfs={} errors={} cancelled={}",
                 dir_str, discovery.files.size(), discovery.directories.size(),
                 discovery.skipped_romfs, discovery.errors, stop_requested.load());

        for (const auto& path : discovery.files) {
            if (stop_requested.load()) {
                break;
            }
            const std::string key = GameListIndex::NormalizePathKey(path);
            if (!seen_file_keys.insert(key).second) {
                continue;
            }
            live_keys.insert(key);

            const std::string physical_name = Common::FS::PathToUTF8String(path);
            const auto [file_size, mtime_ns] = GameListIndex::QueryFileStamp(path);

            const GameListIndex::FileIndexEntry* cached =
                use_cache ? scan_index.FindMatching(path, file_size, mtime_ns) : nullptr;

            if (cached != nullptr) {
                ++cache_hits;
                if (cached->is_bootable) {
                    for (const auto& rec : cached->titles) {
                        std::vector<u8> icon = ReadCachedIcon(rec.program_id);
                        if (icon.empty()) {
                            icon = resolve_fallback_art(rec.program_id, rec.name, path);
                        }
                        GameRowPayload payload{
                            .path = physical_name,
                            .name = rec.name,
                            .size = cached->is_loose_dump ? 0 : static_cast<std::size_t>(file_size),
                            .icon = std::move(icon),
                            .file_type_string = rec.file_type_str,
                            .program_id = rec.program_id,
                            .play_time = play_time_manager.GetPlayTime(rec.program_id),
                            .patch_versions = ReadCachedPatchVersions(rec.program_id),
                            .loose_dump = cached->is_loose_dump,
                        };
                        emit_row(std::move(payload), parent_dir, dir_str, is_auto_detected,
                                 deferred_emit);
                    }
                }

                const auto cached_ft = static_cast<Loader::FileType>(cached->file_type);
                const bool needs_content_reg =
                    !cached->is_loose_dump &&
                    ((ext_content_enabled && IsContainerExtension(physical_name) &&
                      (cached->has_updates_or_dlc || !cached->is_bootable)) ||
                     cached_ft == Loader::FileType::NCA || cached_ft == Loader::FileType::NX);
                if (needs_content_reg) {
                    deferred_registry.TrackCandidate(physical_name, cached->referenced_base_ids);
                    deferred_register_queue.push_back(FileScanTask{
                        .path = path,
                        .physical_name = physical_name,
                        .file_size = file_size,
                        .mtime_ns = mtime_ns,
                        .parent_dir = parent_dir,
                        .dir_path = dir_str,
                        .is_auto_detected = is_auto_detected,
                        .deferred_emit = true,
                    });
                }
                continue;
            }

            ++cache_misses;
            FileScanTask task{
                .path = path,
                .physical_name = physical_name,
                .file_size = file_size,
                .mtime_ns = mtime_ns,
                .parent_dir = parent_dir,
                .dir_path = dir_str,
                .is_auto_detected = is_auto_detected,
                .deferred_emit = deferred_emit,
            };

            if (!IsExtractedNCAMain(physical_name) &&
                GameListIndex::IsLikelyDeferredExtra(path, file_size)) {
                std::vector<u64> hint_base_ids;
                const auto fname_u8 = path.filename().u8string();
                const std::string_view fname_sv(reinterpret_cast<const char*>(fname_u8.data()),
                                                fname_u8.size());
                if (const auto tid = GameListIndex::ExtractTitleIdFromFilename(fname_sv)) {
                    hint_base_ids.push_back(FileSys::GetBaseTitleID(*tid));
                }
                deferred_registry.TrackCandidate(physical_name, hint_base_ids);
                task.deferred_emit = true;
                deferred_parse_queue.push_back(std::move(task));
            } else {
                priority_parse_queue.push_back(std::move(task));
            }
        }
    };

    // Parses one uncached file in a single pass: registers container entries once,
    // reads title/icon only if bootable, caches index entry, and emits UI rows.
    const auto parse_uncached_task = [&](const FileScanTask& task,
                                         std::unordered_set<u64>* updated_base_ids,
                                         std::mutex* updated_mutex) {
        const auto file = vfs->OpenFile(task.physical_name, FileSys::OpenMode::Read);
        if (!file) {
            return;
        }

        const bool loose_dump = IsExtractedNCAMain(task.physical_name);
        GameListIndex::FileIndexEntry index_entry;
        index_entry.path = task.physical_name;
        index_entry.file_size = task.file_size;
        index_entry.mtime_ns = task.mtime_ns;
        index_entry.is_loose_dump = loose_dump;

        const bool is_container_ext = !loose_dump && IsContainerExtension(task.physical_name);
        if (is_container_ext) {
            const auto summary = provider->ScanAndRegisterContainer(file, ext_content_enabled);
            deferred_registry.MarkRegistered(task.physical_name);

            index_entry.is_bootable = summary.is_bootable;
            index_entry.has_updates_or_dlc = summary.has_updates_or_dlc;
            index_entry.referenced_base_ids = summary.referenced_base_ids;

            if (summary.has_updates_or_dlc && updated_base_ids && updated_mutex) {
                std::scoped_lock lk(*updated_mutex);
                for (const u64 base_id : summary.referenced_base_ids) {
                    updated_base_ids->insert(base_id);
                }
            }

            if (!summary.is_bootable) {
                const auto guessed = Loader::GuessFromFilename(task.physical_name);
                index_entry.file_type = static_cast<u8>(guessed);
                std::scoped_lock lk(index_mutex);
                scan_index.Upsert(task.path, std::move(index_entry));
                index_dirty = true;
                return;
            }
        }

        auto loader = Loader::GetLoader(system, file);
        if (!loader) {
            std::scoped_lock lk(index_mutex);
            scan_index.Upsert(task.path, std::move(index_entry));
            index_dirty = true;
            return;
        }

        const auto file_type = loader->GetFileType();
        index_entry.file_type = static_cast<u8>(file_type);
        if (file_type == Loader::FileType::Unknown || file_type == Loader::FileType::Error) {
            std::scoped_lock lk(index_mutex);
            scan_index.Upsert(task.path, std::move(index_entry));
            index_dirty = true;
            return;
        }

        u64 program_id = 0;
        const auto res2 = loader->ReadProgramId(program_id);

        if (!is_container_ext) {
            if (res2 == Loader::ResultStatus::Success && file_type == Loader::FileType::NCA) {
                provider->AddEntry(FileSys::TitleType::Application,
                                   FileSys::GetCRTypeFromNCAType(FileSys::NCA{file}.GetType()),
                                   program_id, file);
                deferred_registry.MarkRegistered(task.physical_name);
                if (program_id != 0) {
                    index_entry.referenced_base_ids.push_back(FileSys::GetBaseTitleID(program_id));
                }
            } else if (file_type == Loader::FileType::NX) {
                if (!Loader::IsBootableGameContainer(file, file_type)) {
                    index_entry.is_bootable = false;
                    std::scoped_lock lk(index_mutex);
                    scan_index.Upsert(task.path, std::move(index_entry));
                    index_dirty = true;
                    return;
                }
                static_cast<Loader::AppLoader_NX&>(*loader).RegisterContent(*provider);
                deferred_registry.MarkRegistered(task.physical_name);
                if (program_id != 0) {
                    index_entry.referenced_base_ids.push_back(FileSys::GetBaseTitleID(program_id));
                }
            } else if (file_type == Loader::FileType::NUP &&
                       !Loader::IsBootableGameContainer(file, file_type)) {
                index_entry.is_bootable = false;
                std::scoped_lock lk(index_mutex);
                scan_index.Upsert(task.path, std::move(index_entry));
                index_dirty = true;
                return;
            }
        }

        index_entry.is_bootable = true;

        std::vector<u64> program_ids;
        loader->ReadProgramIds(program_ids);

        const auto emit_and_record = [&](std::unique_ptr<Loader::AppLoader>& app_loader,
                                         const u64 id) {
            std::vector<u8> icon;
            [[maybe_unused]] const auto res1 = app_loader->ReadIcon(icon);

            std::string name = " ";
            const auto res3 = app_loader->ReadTitle(name);
            const bool title_missing =
                res3 != Loader::ResultStatus::Success || name.empty() || name == " ";
            const auto app_file_type = app_loader->GetFileType();
            if (title_missing && loose_dump) {
                if (const QString loose = LooseExeFSName(task.physical_name); !loose.isEmpty()) {
                    name = loose.toStdString();
                }
            } else if (title_missing && (app_file_type == Loader::FileType::NX ||
                                         app_file_type == Loader::FileType::NUP)) {
                name = QFileInfo(QString::fromStdString(task.physical_name))
                           .completeBaseName()
                           .toStdString();
            }

            // Skip NRO/NSO/KIP plugin libraries (e.g. Skyline hooks) that have no title ID,
            // no NACP title, and no icon — they are shared modules, not playable games.
            if ((app_file_type == Loader::FileType::NRO ||
                 app_file_type == Loader::FileType::NSO ||
                 app_file_type == Loader::FileType::KIP) &&
                id == 0 && title_missing && icon.empty()) {
                return;
            }

            if (icon.empty()) {
                icon = ReadCachedIcon(id);
            }
            if (icon.empty()) {
                icon = resolve_fallback_art(id, name, task.path);
            }

            WriteCachedIconAndName(id, icon, name);

            const FileSys::PatchManager patch{id, system.GetFileSystemController(),
                                              system.GetContentProvider()};
            const bool updatable = app_loader->IsRomFSUpdatable();
            QString patch_versions = GetGameListCachedObject(
                fmt::format("{:016X}", patch.GetTitleID()), "pv.txt",
                [&patch, &app_loader, updatable, app_file_type] {
                    QString pv = FormatPatchNameVersions(patch, *app_loader, updatable);
                    FileSys::NACP nacp{};
                    if ((app_file_type == Loader::FileType::NUP ||
                         (app_file_type == Loader::FileType::NX && !updatable)) &&
                        app_loader->ReadControlData(nacp) == Loader::ResultStatus::Success) {
                        const std::string version_string = nacp.GetVersionString();
                        if (!version_string.empty()) {
                            if (!pv.isEmpty()) {
                                pv += QLatin1Char('\n');
                            }
                            pv += QStringLiteral("Update (%1)")
                                      .arg(QString::fromStdString(version_string));
                        }
                    }
                    return pv;
                });

            const std::string type_str = Loader::GetFileTypeString(app_file_type);
            const u64 size = loose_dump ? 0 : task.file_size;

            index_entry.titles.push_back(GameListIndex::TitleRecord{
                .program_id = id,
                .name = name,
                .file_type_str = type_str,
                .romfs_updatable = updatable,
                .generation = GameListIndex::ClassifyConsoleGeneration(id),
            });

            GameRowPayload payload{
                .path = task.physical_name,
                .name = std::move(name),
                .size = static_cast<std::size_t>(size),
                .icon = std::move(icon),
                .file_type_string = type_str,
                .program_id = id,
                .play_time = play_time_manager.GetPlayTime(id),
                .patch_versions = std::move(patch_versions),
                .loose_dump = loose_dump,
            };
            emit_row(std::move(payload), task.parent_dir, task.dir_path, task.is_auto_detected,
                     task.deferred_emit);
        };

        if (res2 == Loader::ResultStatus::Success && program_ids.size() > 1 &&
            (file_type == Loader::FileType::XCI || file_type == Loader::FileType::XCZ ||
             file_type == Loader::FileType::NSP || file_type == Loader::FileType::NSZ)) {
            for (const auto id : program_ids) {
                if ((id & 0xFFF) != 0) {
                    continue;
                }
                auto sub_loader = Loader::GetLoader(system, file, id);
                if (!sub_loader) {
                    continue;
                }
                emit_and_record(sub_loader, id);
            }
        } else {
            emit_and_record(loader, program_id);
        }

        index_entry.is_bootable = !index_entry.titles.empty();

        std::scoped_lock lk(index_mutex);
        scan_index.Upsert(task.path, std::move(index_entry));
        index_dirty = true;
    };

    // 1. Collect user-configured directories and NAND/SDMC titles.
    std::vector<std::filesystem::path> manual_roots;
    std::vector<std::filesystem::path> excluded_roots;
    for (const auto& excluded : UISettings::values.excluded_auto_game_dirs) {
        if (!excluded.empty()) {
            excluded_roots.emplace_back(Common::FS::ToU8String(excluded));
        }
    }

    for (UISettings::GameDir& game_dir : game_dirs) {
        if (stop_requested.load()) {
            break;
        }

        GameListDir* game_list_dir = nullptr;
        bool scan = false;

        if (game_dir.path == std::string("SDMC")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SdmcDir);
        } else if (game_dir.path == std::string("UserNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::UserNandDir);
        } else if (game_dir.path == std::string("SysNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SysNandDir);
        } else {
            const QString qpath = QString::fromStdString(game_dir.path);
            if (QDir(qpath).exists()) {
                watch_list.append(qpath);
            }
            manual_roots.emplace_back(Common::FS::ToU8String(game_dir.path));
            game_list_dir = new GameListDir(game_dir);
            scan = true;
        }

        const auto parent_type = static_cast<GameListItemType>(game_list_dir->type());
        DirEntryReady(game_list_dir);
        if (scan) {
            collect_directory(std::filesystem::path(Common::FS::ToU8String(game_dir.path)),
                              game_dir.deep_scan, game_list_dir, game_dir.path, false, false);
        } else {
            AddTitlesToGameList(game_list_dir, parent_type);
        }
    }

    // 2. Auto-detect Switch game folders across drives without manual setup.
    //    Use remembered auto-detected folders immediately on warm launches; on cold
    //    launches (when no cached auto-dirs exist yet), run the parallel drive search now.
    std::vector<std::filesystem::path> scanned_auto_dirs;
    bool ran_drive_search = false;
    if (auto_detect_enabled && !stop_requested.load()) {
        std::vector<std::filesystem::path> initial_auto_dirs;
        for (const auto& cached_dir : scan_index.AutoDetectedDirs()) {
            std::error_code ec;
            std::filesystem::path p(Common::FS::ToU8String(cached_dir));
            if (std::filesystem::is_directory(p, ec) &&
                !GameListDiscovery::IsPathCoveredByAny(p, manual_roots) &&
                !GameListDiscovery::IsPathCoveredByAny(p, excluded_roots)) {
                initial_auto_dirs.push_back(std::move(p));
            }
        }

        if (initial_auto_dirs.empty()) {
            ran_drive_search = true;
            auto detected = GameListDiscovery::AutoDetectRoots(
                GameListDiscovery::DefaultSearchRoots(), AutoDetectPerRootTimeout,
                [this] { return stop_requested.load(); });
            auto from_configs = GameListDiscovery::CollectEmulatorConfigGameDirs();
            detected.insert(detected.end(), std::make_move_iterator(from_configs.begin()),
                            std::make_move_iterator(from_configs.end()));
            detected = GameListDiscovery::CollapseNestedDirs(std::move(detected));

            std::vector<std::string> stored_dirs;
            for (auto& dir : detected) {
                stored_dirs.push_back(Common::FS::PathToUTF8String(dir));
                if (!GameListDiscovery::IsPathCoveredByAny(dir, manual_roots) &&
                    !GameListDiscovery::IsPathCoveredByAny(dir, excluded_roots)) {
                    initial_auto_dirs.push_back(std::move(dir));
                }
            }
            if (!stop_requested.load() && stored_dirs != scan_index.AutoDetectedDirs()) {
                scan_index.SetAutoDetectedDirs(std::move(stored_dirs));
                index_dirty = true;
            }
        }

        for (const auto& auto_dir : initial_auto_dirs) {
            if (stop_requested.load()) {
                break;
            }
            const std::string dir_str = Common::FS::PathToUTF8String(auto_dir);
            watch_list.append(QString::fromStdString(dir_str));
            auto* auto_item = new GameListDir(dir_str);
            DirEntryReady(auto_item);
            collect_directory(auto_dir, true, auto_item, dir_str, true, false);
            scanned_auto_dirs.push_back(auto_dir);
        }
    }

    // 3. Stage 1: Parse uncached base games in parallel (1 pass per file).
    ParallelForEach(priority_parse_queue, stop_requested,
                    [&](const FileScanTask& task) { parse_uncached_task(task, nullptr, nullptr); });

    watch_list.removeDuplicates();
    const auto list_ready_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
    LOG_INFO(Frontend,
             "Game list ready: elapsed_ms={} cache_hits={} cache_misses={} priority={} "
             "deferred_parse={} deferred_reg={} watched={} cancelled={}",
             list_ready_ms, cache_hits, cache_misses, priority_parse_queue.size(),
             deferred_parse_queue.size(), deferred_register_queue.size(), watch_list.size(),
             stop_requested.load());

    // Unlock and display the game list right away while updates/DLC finish in the background.
    const QStringList ready_watch_list = watch_list;
    RecordEvent([ready_watch_list](GameListModel* model) {
        model->DonePopulating(ready_watch_list);
    });

    // 4. Stage 2 (background): Refresh auto-detection on warm starts (catches newly added
    //    folders on any drive) and register deferred updates/DLC into ManualContentProvider.
    if (auto_detect_enabled && !ran_drive_search && !stop_requested.load()) {
        auto detected = GameListDiscovery::AutoDetectRoots(
            GameListDiscovery::DefaultSearchRoots(), AutoDetectPerRootTimeout,
            [this] { return stop_requested.load(); });
        auto from_configs = GameListDiscovery::CollectEmulatorConfigGameDirs();
        detected.insert(detected.end(), std::make_move_iterator(from_configs.begin()),
                        std::make_move_iterator(from_configs.end()));
        detected = GameListDiscovery::CollapseNestedDirs(std::move(detected));

        std::vector<std::string> stored_dirs;
        std::vector<FileScanTask> new_priority_tasks;
        for (auto& dir : detected) {
            stored_dirs.push_back(Common::FS::PathToUTF8String(dir));
            if (GameListDiscovery::IsPathCoveredByAny(dir, manual_roots) ||
                GameListDiscovery::IsPathCoveredByAny(dir, excluded_roots) ||
                GameListDiscovery::IsPathCoveredByAny(dir, scanned_auto_dirs)) {
                continue;
            }
            const std::size_t prev_priority = priority_parse_queue.size();
            collect_directory(dir, true, nullptr, Common::FS::PathToUTF8String(dir), true, true);
            for (std::size_t i = prev_priority; i < priority_parse_queue.size(); ++i) {
                priority_parse_queue[i].deferred_emit = true;
                new_priority_tasks.push_back(priority_parse_queue[i]);
            }
            scanned_auto_dirs.push_back(std::move(dir));
        }

        if (!stop_requested.load() && stored_dirs != scan_index.AutoDetectedDirs()) {
            std::scoped_lock lk(index_mutex);
            scan_index.SetAutoDetectedDirs(std::move(stored_dirs));
            index_dirty = true;
        }

        ParallelForEach(new_priority_tasks, stop_requested, [&](const FileScanTask& task) {
            parse_uncached_task(task, nullptr, nullptr);
        });
    }

    std::unordered_set<u64> updated_base_ids;
    std::mutex updated_mutex;

    // Parse uncached deferred extras (updates / DLC) in parallel.
    ParallelForEach(deferred_parse_queue, stop_requested, [&](const FileScanTask& task) {
        parse_uncached_task(task, &updated_base_ids, &updated_mutex);
    });

    // Register cached deferred updates/DLC into ManualContentProvider in parallel.
    ParallelForEach(deferred_register_queue, stop_requested, [&](const FileScanTask& task) {
        if (deferred_registry.IsRegistered(task.physical_name)) {
            return;
        }
        const auto file = vfs->OpenFile(task.physical_name, FileSys::OpenMode::Read);
        if (!file) {
            deferred_registry.MarkRegistered(task.physical_name);
            return;
        }
        const auto ext = Common::ToLower(
            std::string(Common::FS::GetExtensionFromFilename(task.physical_name)));
        if (ext == "nca") {
            const FileSys::NCA nca{file};
            if (nca.GetStatus() == Loader::ResultStatus::Success) {
                provider->AddEntry(FileSys::TitleType::Application,
                                   FileSys::GetCRTypeFromNCAType(nca.GetType()), nca.GetTitleId(),
                                   file);
            }
        } else if (ext == "nx") {
            auto nx_loader = Loader::GetLoader(system, file);
            if (nx_loader && nx_loader->GetFileType() == Loader::FileType::NX) {
                static_cast<Loader::AppLoader_NX&>(*nx_loader).RegisterContent(*provider);
            }
        } else if (ext_content_enabled) {
            const auto summary = provider->ScanAndRegisterContainer(file, true);
            if (summary.has_updates_or_dlc) {
                std::scoped_lock lk(updated_mutex);
                for (const u64 base_id : summary.referenced_base_ids) {
                    updated_base_ids.insert(base_id);
                }
            }
        }
        deferred_registry.MarkRegistered(task.physical_name);
    });

    // Refresh the Add-ons column for any emitted titles whose updates/DLC just registered.
    if (!stop_requested.load() && !updated_base_ids.empty()) {
        std::vector<u64> to_refresh;
        {
            std::scoped_lock lk(emitted_mutex);
            for (const u64 tid : emitted_program_ids) {
                if (updated_base_ids.contains(FileSys::GetBaseTitleID(tid))) {
                    to_refresh.push_back(tid);
                }
            }
        }
        for (const u64 tid : to_refresh) {
            if (stop_requested.load()) {
                break;
            }
            const FileSys::PatchManager patch{tid, system.GetFileSystemController(),
                                              system.GetContentProvider()};
            const QString pv = FormatPatchEntries(patch.GetPatches(), {}, true);
            const QString old_pv = ReadCachedPatchVersions(tid);
            if (pv != old_pv && !pv.isEmpty()) {
                WriteCachedPatchVersions(tid, pv);
                RecordEvent([tid, pv](GameListModel* model) {
                    model->UpdateTitleAddOns(tid, pv);
                });
            }
        }
    }

    // Fetch missing game icons online (Switch icon API) in the background after the list is live.
    if (!stop_requested.load()) {
        std::vector<u64> ids_to_fetch;
        {
            std::scoped_lock lk(emitted_mutex);
            ids_to_fetch.assign(missing_icon_ids.begin(), missing_icon_ids.end());
        }
        for (const u64 tid : ids_to_fetch) {
            if (stop_requested.load()) {
                break;
            }
            std::vector<u8> icon = FetchOnlineSwitchIcon(tid);
            if (!icon.empty()) {
                WriteCachedIconAndName(tid, icon, {});
                RecordEvent([tid, icon = std::move(icon)](GameListModel* model) {
                    model->UpdateTitleIcon(tid, icon);
                });
            }
        }
    }

    if (stop_requested.load()) {
        provider->SwapEntries(previous);
    }

    if (use_cache && !stop_requested.load()) {
        std::scoped_lock lk(index_mutex);
        const std::size_t before = scan_index.Size();
        scan_index.PruneMissing(live_keys);
        if (scan_index.Size() != before) {
            index_dirty = true;
        }
    }

    if (use_cache && index_dirty) {
        std::scoped_lock lk(index_mutex);
        scan_index.Save(index_path);
    }

    LOG_INFO(Frontend, "Game discovery finished: elapsed_ms={} indexed={} cancelled={}",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started)
                 .count(),
             scan_index.Size(), stop_requested.load());
    processing_completed.Set();
}
