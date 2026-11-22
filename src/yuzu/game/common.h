// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStandardItem>
#include <QStringList>

#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/scan_index.h"

namespace Yuzu {

inline bool ContainsAllWords(const QString& haystack, const QString& userinput) {
    const QStringList userinput_split = userinput.split(QLatin1Char{' '}, Qt::SkipEmptyParts);
    return std::all_of(userinput_split.begin(), userinput_split.end(), [&haystack](const QString& s) {
        return haystack.contains(s);
    });
}

inline bool FilterMatches(const QString& filter, const QStandardItem* item,
                          GameListIndex::SystemFilter system_filter =
                              GameListIndex::SystemFilter::All) {
    const auto program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
    const int gen_raw = item->data(GameListItemPath::ConsoleGenerationRole).toInt();
    const auto gen =
        gen_raw == static_cast<int>(GameListIndex::ConsoleGeneration::Switch2)
            ? GameListIndex::ConsoleGeneration::Switch2
            : (gen_raw == static_cast<int>(GameListIndex::ConsoleGeneration::Switch1)
                   ? GameListIndex::ConsoleGeneration::Switch1
                   : GameListIndex::ClassifyConsoleGeneration(program_id));

    if (!GameListIndex::MatchesSystemFilter(system_filter, gen)) {
        return false;
    }

    if (filter.isEmpty())
        return true;

    auto const sluggify = [](const QString& s) {
        QString o(s.normalized(QString::NormalizationForm_D).toLower());
        return o.replace(QRegularExpression(QStringLiteral("[^a-z0-9\\s]")), QStringLiteral(""));
    };

    const auto norm_filter = sluggify(filter);
    const QString file_path = sluggify(item->data(GameListItemPath::FullPathRole).toString());
    const QString file_title = sluggify(item->data(GameListItemPath::TitleRole).toString());
    const QString system_str =
        sluggify(QString::fromLatin1(GameListIndex::ConsoleGenerationLabel(gen)));
    const QString file_program_id = QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char{'0'});
    const QString file_name = sluggify(file_path.mid(file_path.lastIndexOf(QLatin1Char{'/'}) + 1) +
                                       QLatin1Char{' '} + file_title + QLatin1Char{' '} +
                                       system_str);
    return (ContainsAllWords(file_name, norm_filter) ||
            ContainsAllWords(file_title, norm_filter)) ||
           (file_program_id.size() == 16 && file_program_id.contains(norm_filter));
}

} // namespace Yuzu
