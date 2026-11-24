// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"

#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/game_list/worker.h"

GameListModel::GameListModel(std::shared_ptr<FileSys::VfsFilesystem> vfs_,
                             FileSys::ManualContentProvider* provider_,
                             const PlayTime::PlayTimeManager& play_time_manager_,
                             Core::System& system_, QObject* parent)
    : QStandardItemModel{parent}, vfs{std::move(vfs_)}, provider{provider_},
      play_time_manager{play_time_manager_}, system{system_} {
    watcher = new QFileSystemWatcher(this);
    external_watcher = new QFileSystemWatcher(this);
    refresh_debounce_timer = new QTimer(this);
    refresh_debounce_timer->setSingleShot(true);
    refresh_debounce_timer->setInterval(350);

    connect(watcher, &QFileSystemWatcher::directoryChanged, this,
            &GameListModel::RefreshGameDirectory);
    connect(external_watcher, &QFileSystemWatcher::directoryChanged, this,
            &GameListModel::RefreshExternalContent);
    connect(refresh_debounce_timer, &QTimer::timeout, this, [this] {
        if (!scan_enabled || system.IsPoweredOn()) {
            return;
        }
        if (repopulate_reset_metadata) {
            repopulate_reset_metadata = false;
            QtCommon::Game::ResetMetadata(false);
        }
        Repopulate();
    });

    ResetExternalWatcher();

    insertColumns(0, COLUMN_COUNT);
    RetranslateUI();

    setSortRole(GameListItemPath::SortRole);
}

GameListModel::~GameListModel() {
    if (refresh_debounce_timer) {
        refresh_debounce_timer->stop();
    }
    scan_enabled = false;
    repopulate_pending = false;
    StopWorker();
    // Wait briefly for retiring workers; if a background worker is wedged on a sleeping/removed
    // external drive, release it so process exit never hangs in "(Not Responding)".
    for (auto& w : retiring_workers) {
        if (!w) {
            continue;
        }
        w->disconnect();
        if (!w->WaitForStop(std::chrono::milliseconds{150})) {
            w.release();
        }
    }
    retiring_workers.clear();
}

void GameListModel::SetScanEnabled(bool enabled) {
    scan_enabled = enabled;
    if (!enabled) {
        if (refresh_debounce_timer) {
            refresh_debounce_timer->stop();
        }
        repopulate_pending = false;
        StopWorker();
        CleanupRetiringWorkers();
    }
}

void GameListModel::PopulateAsync(QVector<UISettings::GameDir>& game_dirs) {
    if (!scan_enabled || system.IsPoweredOn()) {
        return;
    }

    StopWorker();
    CleanupRetiringWorkers();

    // If a previous worker is still unwinding a slow drive read, defer starting the new scan
    // until it finishes asynchronously instead of freezing the UI thread.
    if (!retiring_workers.empty()) {
        repopulate_pending = true;
        return;
    }

    repopulate_pending = false;
    emit PopulatingStarted();

    removeRows(0, rowCount());

    current_worker = std::make_unique<GameListWorker>(vfs, provider, game_dirs,
                                                      play_time_manager, system);

    connect(current_worker.get(), &GameListWorker::DataAvailable, this, &GameListModel::WorkerEvent,
            Qt::QueuedConnection);
    connect(current_worker.get(), &GameListWorker::Finished, this,
            &GameListModel::CleanupRetiringWorkers, Qt::QueuedConnection);

    QThreadPool::globalInstance()->start(current_worker.get());
}

void GameListModel::WorkerEvent() {
    if (current_worker) {
        current_worker->ProcessEvents(this);
    }
}

void GameListModel::AddDirEntry(GameListDir* entry_items) {
    if (m_flat) {
        delete entry_items;
        return;
    }
    invisibleRootItem()->appendRow(entry_items);
}

void GameListModel::AddEntry(const QList<QStandardItem*>& entry_items, GameListDir* parent) {
    if (m_flat) {
        invisibleRootItem()->appendRow(entry_items);
    } else {
        parent->appendRow(entry_items);
    }
}

void GameListModel::AddDeferredEntry(const QList<QStandardItem*>& entry_items,
                                     const QString& dir_path, bool is_auto_detected) {
    if (m_flat) {
        invisibleRootItem()->appendRow(entry_items);
        emit ShowList(true);
        return;
    }

    QStandardItem* target_dir = nullptr;
    auto* root = invisibleRootItem();
    for (int i = 0; i < root->rowCount(); ++i) {
        auto* row_item = root->child(i);
        if (!row_item) {
            continue;
        }
        const auto type = static_cast<GameListItemType>(row_item->type());
        if (is_auto_detected && type == GameListItemType::AutoDetectedDir) {
            if (row_item->data(GameListDir::AutoDirPathRole).toString() == dir_path) {
                target_dir = row_item;
                break;
            }
        } else if (!is_auto_detected && type == GameListItemType::CustomDir) {
            if (row_item->data(Qt::DisplayRole).toString() == dir_path) {
                target_dir = row_item;
                break;
            }
        }
    }

    if (!target_dir) {
        int insert_pos = root->rowCount();
        if (insert_pos > 0) {
            const auto last_type =
                static_cast<GameListItemType>(root->child(insert_pos - 1)->type());
            if (last_type == GameListItemType::AddDir) {
                --insert_pos;
            }
        }
        auto* new_dir = new GameListDir(dir_path.toStdString());
        root->insertRow(insert_pos, new_dir);
        target_dir = new_dir;
    }

    target_dir->appendRow(entry_items);
    emit ShowList(true);
}

void GameListModel::UpdateTitleAddOns(u64 program_id, const QString& patch_versions) {
    if (program_id == 0) {
        return;
    }
    const auto update_children = [&](QStandardItem* parent) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            auto* item = parent->child(row, COLUMN_NAME);
            if (!item || static_cast<GameListItemType>(item->type()) != GameListItemType::Game) {
                continue;
            }
            if (item->data(GameListItemPath::ProgramIdRole).toULongLong() != program_id) {
                continue;
            }
            if (auto* addons_item = parent->child(row, COLUMN_ADD_ONS)) {
                addons_item->setText(patch_versions);
                addons_item->setData(patch_versions, GameListItem::SortRole);
            }
        }
    };

    auto* root = invisibleRootItem();
    if (m_flat) {
        update_children(root);
    } else {
        for (int i = 0; i < root->rowCount(); ++i) {
            if (auto* folder = root->child(i)) {
                update_children(folder);
            }
        }
    }
}

void GameListModel::UpdateTitleIcon(u64 program_id, const std::vector<u8>& picture_data) {
    if (program_id == 0 || picture_data.empty()) {
        return;
    }
    const u32 size = UISettings::values.game_icon_size.GetValue();
    const QPixmap pixmap = GameListItemPath::MakeDisplayPixmap(picture_data, size);
    const QByteArray raw_bytes(reinterpret_cast<const char*>(picture_data.data()),
                               static_cast<qsizetype>(picture_data.size()));

    const auto update_children = [&](QStandardItem* parent) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            auto* item = parent->child(row, COLUMN_NAME);
            if (!item || static_cast<GameListItemType>(item->type()) != GameListItemType::Game) {
                continue;
            }
            if (item->data(GameListItemPath::ProgramIdRole).toULongLong() != program_id) {
                continue;
            }
            item->setData(raw_bytes, GameListItemPath::IconDataRole);
            item->setData(pixmap, Qt::DecorationRole);
        }
    };

    auto* root = invisibleRootItem();
    if (m_flat) {
        update_children(root);
    } else {
        for (int i = 0; i < root->rowCount(); ++i) {
            if (auto* folder = root->child(i)) {
                update_children(folder);
            }
        }
    }
}

void GameListModel::DonePopulating(const QStringList& watch_list) {
    emit ShowList(!IsEmpty());

    if (!m_flat) {
        invisibleRootItem()->appendRow(new GameListAddDir());
        invisibleRootItem()->insertRow(0, new GameListFavorites());

        for (const auto id : std::as_const(UISettings::values.favorited_ids)) {
            AddFavorite(id);
        }
    }

    emit PopulatingCompleted(watch_list);
}

bool GameListModel::IsEmpty() const {
    for (int i = 0; i < rowCount(); i++) {
        const QStandardItem* child = invisibleRootItem()->child(i);
        const auto type = static_cast<GameListItemType>(child->type());

        if (!child->hasChildren() &&
            (type == GameListItemType::SdmcDir || type == GameListItemType::UserNandDir ||
             type == GameListItemType::SysNandDir || type == GameListItemType::AutoDetectedDir)) {
            invisibleRootItem()->removeRow(child->row());
            i--;
        }
    }

    return !invisibleRootItem()->hasChildren();
}

void GameListModel::ToggleFavorite(u64 program_id) {
    if (!UISettings::values.favorited_ids.contains(program_id)) {
        UISettings::values.favorited_ids.append(program_id);
        AddFavorite(program_id);
    } else {
        UISettings::values.favorited_ids.removeOne(program_id);
        RemoveFavorite(program_id);
    }
    emit SaveConfig();
}

void GameListModel::AddFavorite(u64 program_id) {
    auto* favorites_row = item(0);

    for (int i = 1; i < rowCount() - 1; i++) {
        const auto* folder = item(i);
        for (int j = 0; j < folder->rowCount(); j++) {
            if (folder->child(j)->data(GameListItemPath::ProgramIdRole).toULongLong() ==
                program_id) {
                QList<QStandardItem*> list;
                for (int k = 0; k < COLUMN_COUNT; k++) {
                    list.append(folder->child(j, k)->clone());
                }
                list[0]->setData(folder->child(j)->data(GameListItem::SortRole),
                                 GameListItem::SortRole);
                list[0]->setText(folder->child(j)->data(Qt::DisplayRole).toString());

                favorites_row->appendRow(list);
                return;
            }
        }
    }
}

void GameListModel::RemoveFavorite(u64 program_id) {
    auto* favorites_row = item(0);

    for (int i = 0; i < favorites_row->rowCount(); i++) {
        const auto* game = favorites_row->child(i);
        if (game->data(GameListItemPath::ProgramIdRole).toULongLong() == program_id) {
            favorites_row->removeRow(i);
            return;
        }
    }
}

void GameListModel::PruneMissingFiles() {
    const auto is_local_missing = [](const QString& path) {
        if (path.isEmpty()) {
            return false;
        }
        // Only check real filesystem paths (skip virtual NAND/SDMC placeholders if any).
        if (!path.contains(QLatin1Char('/')) && !path.contains(QLatin1Char('\\'))) {
            return false;
        }
        return !QFileInfo::exists(path);
    };

    int removed_count = 0;
    const auto prune_parent = [&](QStandardItem* parent) {
        for (int row = parent->rowCount() - 1; row >= 0; --row) {
            auto* item = parent->child(row, COLUMN_NAME);
            if (!item || static_cast<GameListItemType>(item->type()) != GameListItemType::Game) {
                continue;
            }
            const QString full_path = item->data(GameListItemPath::FullPathRole).toString();
            if (is_local_missing(full_path)) {
                parent->removeRow(row);
                ++removed_count;
            }
        }
    };

    auto* root = invisibleRootItem();
    if (m_flat) {
        prune_parent(root);
    } else {
        for (int i = 0; i < root->rowCount(); ++i) {
            if (auto* folder = root->child(i)) {
                prune_parent(folder);
            }
        }
    }

    // Also drop any deleted folders from QFileSystemWatcher so Windows doesn't hold stale handles.
    if (watcher) {
        QStringList dead_dirs;
        for (const QString& dir : watcher->directories()) {
            if (!QFileInfo::exists(dir)) {
                dead_dirs.append(dir);
            }
        }
        if (!dead_dirs.isEmpty()) {
            watcher->removePaths(dead_dirs);
        }
    }

    if (removed_count > 0) {
        LOG_INFO(Frontend, "Pruned {} deleted game file(s) from game list immediately.",
                 removed_count);
    }
}

void GameListModel::ScheduleRepopulate(bool reset_metadata) {
    if (!scan_enabled || system.IsPoweredOn()) {
        return;
    }
    if (reset_metadata) {
        repopulate_reset_metadata = true;
    }
    if (refresh_debounce_timer) {
        refresh_debounce_timer->start();
    } else {
        Repopulate();
    }
}

void GameListModel::CleanupRetiringWorkers() {
    for (auto it = retiring_workers.begin(); it != retiring_workers.end();) {
        if (!(*it) || (*it)->IsCompleted()) {
            if (*it) {
                (*it)->disconnect();
                (*it).release()->deleteLater();
            }
            it = retiring_workers.erase(it);
        } else {
            ++it;
        }
    }

    if (retiring_workers.empty() && repopulate_pending && scan_enabled && !system.IsPoweredOn()) {
        repopulate_pending = false;
        Repopulate();
    }
}

void GameListModel::StopWorker() {
    if (!current_worker) {
        return;
    }

    // Request cancellation asynchronously without blocking the GUI thread.
    // Keep the worker alive in retiring_workers until its Finished() signal fires.
    current_worker->disconnect(this);
    current_worker->RequestStop();
    if (current_worker->IsCompleted()) {
        current_worker.release()->deleteLater();
        return;
    }

    GameListWorker* retiring = current_worker.get();
    connect(retiring, &GameListWorker::Finished, this, &GameListModel::CleanupRetiringWorkers,
            Qt::QueuedConnection);
    retiring_workers.push_back(std::move(current_worker));
    // Handle the race where the worker finished right before connect().
    if (retiring->IsCompleted()) {
        CleanupRetiringWorkers();
    }
}

void GameListModel::Repopulate() {
    // CreateFactories replaces the content providers a running game reads from. Stopping
    // emulation repopulates the list anyway, so skip watcher-driven reloads until then.
    if (!scan_enabled || QtCommon::system->IsPoweredOn()) {
        LOG_INFO(Frontend, "Emulation is running, deferring game list reload.");
        return;
    }

    StopWorker();
    CleanupRetiringWorkers();
    if (!retiring_workers.empty()) {
        repopulate_pending = true;
        return;
    }

    QtCommon::system->GetFileSystemController().CreateFactories(*QtCommon::vfs);
    PopulateAsync(UISettings::values.game_dirs);
}

void GameListModel::RefreshGameDirectory() {
    ResetExternalWatcher();
    // Immediately remove any deleted files from the UI so Home updates instantaneously without
    // freezing or crashing, then debounce the background rescan for newly added/changed files.
    PruneMissingFiles();
    if ((!UISettings::values.game_dirs.empty() || UISettings::values.auto_detect_games.GetValue()) &&
        scan_enabled && !system.IsPoweredOn()) {
        LOG_INFO(Frontend, "Change detected in the games directory. Scheduling non-blocking reload.");
        ScheduleRepopulate(false);
    }
}

void GameListModel::RefreshExternalContent() {
    if ((!UISettings::values.game_dirs.empty() || UISettings::values.auto_detect_games.GetValue()) &&
        scan_enabled && !system.IsPoweredOn()) {
        LOG_INFO(Frontend, "External content directory changed. Scheduling metadata refresh.");
        ScheduleRepopulate(true);
    }
}

void GameListModel::ResetExternalWatcher() {
    auto watch_dirs = external_watcher->directories();
    if (!watch_dirs.isEmpty()) {
        external_watcher->removePaths(watch_dirs);
    }

    for (const std::string& dir : Settings::values.external_content_dirs) {
        external_watcher->addPath(QString::fromStdString(dir));
    }
}

void GameListModel::RetranslateUI() {
    setHeaderData(COLUMN_NAME, Qt::Horizontal, tr("Name"));
    setHeaderData(COLUMN_SYSTEM, Qt::Horizontal, tr("System"));
    setHeaderData(COLUMN_ADD_ONS, Qt::Horizontal, tr("Add-ons"));
    setHeaderData(COLUMN_FILE_TYPE, Qt::Horizontal, tr("File type"));
    setHeaderData(COLUMN_SIZE, Qt::Horizontal, tr("Size"));
    setHeaderData(COLUMN_PLAY_TIME, Qt::Horizontal, tr("Play time"));
}

QFileSystemWatcher* GameListModel::GetWatcher() const {
    return watcher;
}

void GameListModel::SetFlat(bool flat) {
    m_flat = flat;
}
