// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QListView>
#include <QString>

#include "qt_common/game_list/scan_index.h"

class GameCard;
class GameListModel;

class GameGrid : public QListView {
    Q_OBJECT

public:
    explicit GameGrid(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model,
                     GameListIndex::SystemFilter system_filter =
                         GameListIndex::SystemFilter::All);
    void UpdateIconSize();

    QModelIndex indexAt(const QPoint& point) const override;

private:
    GameCard* m_gameCard = nullptr;
};
