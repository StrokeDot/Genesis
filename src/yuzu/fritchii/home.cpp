// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/home.h"

#include <algorithm>
#include <set>
#include <vector>

#include <QApplication>
#include <QBoxLayout>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/game_list/scan_index.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/saves_window.h"

namespace FritchiiUi {
namespace {

constexpr int CoverSize = 200;
constexpr int TileIcon = 72;

struct AddOnSummary {
    QString enabled_update_version;
    QString disabled_update_version;
    int enabled_dlc_count = 0;
    int disabled_dlc_count = 0;
    int enabled_mod_count = 0;
    int disabled_mod_count = 0;
    QStringList enabled_entries;
    QStringList disabled_entries;

    [[nodiscard]] bool HasEnabledUpdate() const {
        return !enabled_update_version.isEmpty();
    }
    [[nodiscard]] bool HasDisabledUpdate() const {
        return !disabled_update_version.isEmpty();
    }
};

AddOnSummary ParseAddOns(const QString& raw) {
    AddOnSummary summary;
    const QStringList lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const QRegularExpression paren_re{QStringLiteral(R"(\(([^)]+)\)\s*$)")};

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        bool enabled = true;
        if (line.startsWith(QStringLiteral("[D] "))) {
            enabled = false;
            line = line.mid(4).trimmed();
        }
        if (enabled) {
            summary.enabled_entries.push_back(line);
        } else {
            summary.disabled_entries.push_back(line);
        }

        const auto paren_match = paren_re.match(line);
        const QString detail = paren_match.hasMatch() ? paren_match.captured(1).trimmed() : QString{};

        if (line.startsWith(QStringLiteral("Update"), Qt::CaseInsensitive)) {
            const QString ver = detail.isEmpty() ? QStringLiteral("Installed") : detail;
            if (enabled) {
                if (summary.enabled_update_version.isEmpty()) {
                    summary.enabled_update_version = ver;
                }
            } else if (summary.disabled_update_version.isEmpty()) {
                summary.disabled_update_version = ver;
            }
        } else if (line.startsWith(QStringLiteral("DLC"), Qt::CaseInsensitive)) {
            int count = 1;
            if (!detail.isEmpty()) {
                const QStringList parts = detail.split(QLatin1Char(','), Qt::SkipEmptyParts);
                std::set<QString> unique_ids;
                for (const QString& part : parts) {
                    const QString trimmed = part.trimmed();
                    if (!trimmed.isEmpty()) {
                        unique_ids.insert(trimmed);
                    }
                }
                if (!unique_ids.empty()) {
                    count = static_cast<int>(unique_ids.size());
                }
            }
            if (enabled) {
                summary.enabled_dlc_count += count;
            } else {
                summary.disabled_dlc_count += count;
            }
        } else {
            if (enabled) {
                ++summary.enabled_mod_count;
            } else {
                ++summary.disabled_mod_count;
            }
        }
    }
    return summary;
}

QString FormatTileAddOnLine(const AddOnSummary& s) {
    const QChar check(0x2713);
    const QString sep = QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ");
    QStringList parts;
    if (s.HasEnabledUpdate()) {
        parts << QObject::tr("%1 Update %2").arg(check).arg(s.enabled_update_version);
    } else if (s.HasDisabledUpdate()) {
        parts << QObject::tr("! Update %1 (Disabled)").arg(s.disabled_update_version);
    } else {
        parts << QObject::tr("Base v1.0.0");
    }
    if (s.enabled_dlc_count > 0) {
        parts << QObject::tr("%1 %2 DLC").arg(check).arg(s.enabled_dlc_count);
    } else if (s.disabled_dlc_count > 0) {
        parts << QObject::tr("! %1 DLC (Disabled)").arg(s.disabled_dlc_count);
    }
    if (s.enabled_mod_count > 0) {
        parts << QObject::tr("%1 Mod(s)").arg(s.enabled_mod_count);
    }
    return parts.join(sep);
}

struct Game {
    QString title;
    QString path;
    QString addons;
    u64 program_id = 0;
    GameListIndex::ConsoleGeneration generation = GameListIndex::ConsoleGeneration::Switch1;
    QPixmap icon;
};

QPixmap Rounded(const QPixmap& source, int size, int radius) {
    const qreal ratio = qApp->devicePixelRatio();
    const int pixel_size = static_cast<int>(size * ratio);
    QPixmap out(pixel_size, pixel_size);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
        painter.setClipPath(clip);
        if (source.isNull()) {
            painter.fillRect(QRect(0, 0, size, size), QColor(0x24, 0x23, 0x2c));
        } else if (source.width() != source.height()) {
            // Preserve aspect ratio for portrait/landscape covers (e.g. GameVault covers).
            const QPixmap scaled = source.scaled(pixel_size, pixel_size,
                                                 Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
            const int x = (scaled.width() - pixel_size) / 2;
            const int y = scaled.height() > scaled.width() ? (scaled.height() - pixel_size) / 3
                                                           : (scaled.height() - pixel_size) / 2;
            QPixmap cropped = scaled.copy(x, y, pixel_size, pixel_size);
            cropped.setDevicePixelRatio(ratio);
            painter.drawPixmap(0, 0, cropped);
        } else {
            painter.drawPixmap(QRect(0, 0, size, size), source);
        }
    }
    return out;
}

/// True for a missing or fully transparent icon (some dumps have none).
bool IsBlank(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return true;
    }
    const QImage image = pixmap.toImage();
    const int step_x = std::max(1, image.width() / 16);
    const int step_y = std::max(1, image.height() / 16);
    for (int y = 0; y < image.height(); y += step_y) {
        for (int x = 0; x < image.width(); x += step_x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return false;
            }
        }
    }
    return true;
}

/// A cover the player put next to the game file: "<file name>.png" or "icon.png" (the same files
/// Game Settings > Detect Icon looks at first). Read only; the game file is never changed.
QPixmap LocalCover(const QString& game_path) {
    const QFileInfo info(game_path);
    const QDir dir = info.absoluteDir();
    for (const QString& base : {info.completeBaseName(), QStringLiteral("icon")}) {
        for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
            const QFileInfo candidate(dir.filePath(base + QLatin1String(ext)));
            if (!candidate.isFile() || candidate.size() > 16 * 1024 * 1024) {
                continue;
            }
            QPixmap pixmap;
            if (pixmap.load(candidate.filePath())) {
                return pixmap;
            }
        }
    }
    return {};
}

std::vector<Game> CollectGames(QStandardItemModel* model, const QPixmap& fallback,
                               GameListIndex::SystemFilter filter) {
    std::vector<Game> games;
    std::set<QString> seen;
    if (!model) {
        return games;
    }
    const auto add = [&](const QStandardItem* item, const QStandardItem* addons_item) {
        if (item->data(GameListItem::TypeRole).toInt() !=
            static_cast<int>(GameListItemType::Game)) {
            return;
        }
        const QString path = item->data(GameListItemPath::FullPathRole).toString();
        if (path.isEmpty() || !seen.insert(path).second) {
            return;
        }
        Game game;
        game.path = path;
        game.addons = addons_item ? addons_item->text() : QString{};
        game.program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
        game.generation = GameListIndex::ClassifyConsoleGeneration(game.program_id);
        if (!GameListIndex::MatchesSystemFilter(filter, game.generation)) {
            return;
        }
        game.title = item->data(GameListItemPath::TitleRole).toString();
        if (game.title.isEmpty()) {
            game.title = QFileInfo(path).completeBaseName();
        }
        const QByteArray icon_data = item->data(GameListItemPath::IconDataRole).toByteArray();
        if (!game.icon.loadFromData(icon_data)) {
            game.icon = item->data(Qt::DecorationRole).value<QPixmap>();
        }
        if (IsBlank(game.icon)) {
            game.icon = LocalCover(path);
        }
        if (IsBlank(game.icon)) {
            // No icon in the dump and none next to it: Genesis's own icon, not an empty square.
            game.icon = fallback;
        }
        games.push_back(std::move(game));
    };
    for (int row = 0; row < model->rowCount(); ++row) {
        const QStandardItem* top = model->item(row);
        if (!top) {
            continue;
        }
        if (top->hasChildren()) {
            for (int child = 0; child < top->rowCount(); ++child) {
                add(top->child(child, GameListModel::COLUMN_NAME),
                    top->child(child, GameListModel::COLUMN_ADD_ONS));
            }
        } else {
            add(top, model->item(row, GameListModel::COLUMN_ADD_ONS));
        }
    }
    return games;
}

} // namespace

Home::Home(QMainWindow* window_, QBoxLayout* central, QWidget* game_list_, QWidget* placeholder_,
           HomeHooks hooks_)
    : QObject(window_), window{window_}, game_list{game_list_}, placeholder{placeholder_},
      hooks{std::move(hooks_)} {
    // The classic menus live behind the rail's menu button. A zero-height bar keeps every
    // shortcut working and stays out of the way when fullscreen shows the bar again.
    window->menuBar()->setFixedHeight(0);

    rail = MakeRail();
    page = MakePage();
    central->insertWidget(0, rail);
    central->addWidget(page, 1);

    game_list->installEventFilter(this);
    placeholder->installEventFilter(this);

    refresh_timer = new QTimer(this);
    refresh_timer->setSingleShot(true);
    refresh_timer->setInterval(150);
    connect(refresh_timer, &QTimer::timeout, this, &Home::Refresh);
    if (hooks.model) {
        const auto later = [this] {
            sections_dirty = true;
            refresh_timer->start();
        };
        connect(hooks.model, &QAbstractItemModel::rowsInserted, this, later);
        connect(hooks.model, &QAbstractItemModel::rowsRemoved, this, later);
        connect(hooks.model, &QAbstractItemModel::modelReset, this, later);
        connect(hooks.model, &QAbstractItemModel::dataChanged, this, later);
    }
    QTimer::singleShot(0, this, &Home::Restore);
}

QToolButton* Home::RailButton(const QString& icon, const QString& tip) {
    auto* button = new QToolButton(rail);
    button->setObjectName(QStringLiteral("FritchiiRailButton"));
    button->setIcon(Icon(icon, 22));
    button->setIconSize(QSize(22, 22));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QWidget* Home::MakeRail() {
    rail = new QWidget;
    rail->setObjectName(QStringLiteral("FritchiiRail"));
    rail->setAttribute(Qt::WA_StyledBackground);
    rail->setFixedWidth(64);
    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(11, 16, 11, 16);
    layout->setSpacing(8);

    library_button = RailButton(QStringLiteral("library"), tr("Library"));
    library_button->setCheckable(true);
    connect(library_button, &QToolButton::clicked, this, &Home::ShowHome);

    QToolButton* settings = RailButton(QStringLiteral("settings"), tr("Settings"));
    connect(settings, &QToolButton::clicked, this, [this] { hooks.open_settings(); });

    QToolButton* help = RailButton(QStringLiteral("help"), tr("Help"));
    connect(help, &QToolButton::clicked, this, [this, help] {
        if (hooks.help_menu) {
            hooks.help_menu->exec(help->mapToGlobal(QPoint(help->width() + 8, 0)));
        }
    });

    auto* classic = new QMenu(rail);
    for (QMenu* menu : hooks.classic_menus) {
        classic->addMenu(menu);
    }
    QToolButton* menu_button = RailButton(QStringLiteral("menu"), tr("Menu"));
    connect(menu_button, &QToolButton::clicked, this, [classic, menu_button] {
        const QSize size = classic->sizeHint();
        classic->exec(menu_button->mapToGlobal(
            QPoint(menu_button->width() + 8, menu_button->height() - size.height())));
    });

    // Discord and Donate are on the home page itself (MakePage), with their names.
    layout->addWidget(library_button);
    layout->addWidget(settings);
    layout->addWidget(help);
    layout->addStretch(1);
    layout->addWidget(menu_button);
    return rail;
}

QWidget* Home::MakePage() {
    page = new QWidget;
    page->setObjectName(QStringLiteral("FritchiiHome"));
    page->setAttribute(Qt::WA_StyledBackground);
    page->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(page, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (hero_path.isEmpty()) {
            return;
        }
        // The game list's full menu (modders use its folders and dumps), then Delete Shader Cache.
        QMenu menu;
        if (hooks.game_menu) {
            hooks.game_menu(menu, hero_id, hero_path.toStdString());
        } else {
            menu.addAction(tr("Game Settings..."), &menu, [this] {
                hooks.game_settings(hero_id, hero_path.toStdString());
            });
        }
        menu.addSeparator();
        if (hooks.remap_buttons) {
            menu.addAction(tr("Customize Button Layout..."), &menu, [this] {
                hooks.remap_buttons(hero_id, hero_title, hero_path.toStdString());
            });
            menu.addSeparator();
        }
        menu.addAction(tr("Delete Shader Cache..."), &menu,
                       [this] { hooks.delete_shader_cache(hero_id); });
        menu.exec(page->mapToGlobal(pos));
    });

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(56, 24, 56, 36);
    layout->setSpacing(0);

    auto* links = new QHBoxLayout;
    links->setSpacing(10);
    links->addStretch(1);

    system_filter_combo = new QComboBox(page);
    system_filter_combo->setFocusPolicy(Qt::NoFocus);
    system_filter_combo->addItem(tr("All Systems"),
                                 static_cast<int>(GameListIndex::SystemFilter::All));
    system_filter_combo->addItem(tr("Switch 1"),
                                 static_cast<int>(GameListIndex::SystemFilter::Switch1));
    system_filter_combo->addItem(tr("Switch 2"),
                                 static_cast<int>(GameListIndex::SystemFilter::Switch2));
    const int saved_filter =
        std::clamp<int>(static_cast<int>(UISettings::values.system_filter.GetValue()), 0, 2);
    system_filter_combo->setCurrentIndex(saved_filter);
    connect(system_filter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                UISettings::values.system_filter = static_cast<u8>(std::clamp(idx, 0, 2));
                sections_dirty = true;
                Refresh();
            });
    links->addWidget(system_filter_combo);

    auto* all_games_btn = new QPushButton(Icon(QStringLiteral("list"), 18), tr("All games"), page);
    all_games_btn->setObjectName(QStringLiteral("FritchiiLink"));
    all_games_btn->setIconSize(QSize(18, 18));
    all_games_btn->setCursor(Qt::PointingHandCursor);
    all_games_btn->setFocusPolicy(Qt::NoFocus);
    connect(all_games_btn, &QPushButton::clicked, this, &Home::ShowList);
    links->addWidget(all_games_btn);

    auto* add_folder_btn = new QPushButton(Icon(QStringLiteral("plus"), 18), tr("Add folder"), page);
    add_folder_btn->setObjectName(QStringLiteral("FritchiiLink"));
    add_folder_btn->setIconSize(QSize(18, 18));
    add_folder_btn->setCursor(Qt::PointingHandCursor);
    add_folder_btn->setFocusPolicy(Qt::NoFocus);
    connect(add_folder_btn, &QPushButton::clicked, this, [this] { hooks.add_folder(); });
    links->addWidget(add_folder_btn);

    auto* saves = new QPushButton(Icon(QStringLiteral("list"), 18), tr("Saves"), page);
    saves->setObjectName(QStringLiteral("FritchiiLink"));
    saves->setIconSize(QSize(18, 18));
    saves->setCursor(Qt::PointingHandCursor);
    saves->setFocusPolicy(Qt::NoFocus);
    connect(saves, &QPushButton::clicked, this, [this] {
        ShowSavesWindow(window, hooks.emulation_running);
    });
    links->addWidget(saves);

    auto* library_addons =
        new QPushButton(Icon(QStringLiteral("list"), 18), tr("Updates & DLC"), page);
    library_addons->setObjectName(QStringLiteral("FritchiiLink"));
    library_addons->setIconSize(QSize(18, 18));
    library_addons->setCursor(Qt::PointingHandCursor);
    library_addons->setFocusPolicy(Qt::NoFocus);
    connect(library_addons, &QPushButton::clicked, this, &Home::ShowLibraryAddonsDialog);
    links->addWidget(library_addons);

    layout->addLayout(links);
    layout->addSpacing(20);

    auto* hero = new QHBoxLayout;
    hero->setSpacing(36);
    cover = new QLabel;
    cover->setFixedSize(CoverSize, CoverSize);
    hero->addWidget(cover, 0, Qt::AlignTop);

    auto* side = new QVBoxLayout;
    side->setSpacing(0);
    side->addStretch(1);

    auto* badge_row = new QHBoxLayout;
    badge_row->setContentsMargins(0, 0, 0, 0);
    badge_row->setSpacing(8);
    system_badge = new QLabel;
    system_badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    system_badge->hide();
    badge_row->addWidget(system_badge);

    update_badge = new QLabel;
    update_badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    update_badge->hide();
    badge_row->addWidget(update_badge);

    dlc_badge = new QLabel;
    dlc_badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    dlc_badge->hide();
    badge_row->addWidget(dlc_badge);

    mods_badge = new QLabel;
    mods_badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    mods_badge->hide();
    badge_row->addWidget(mods_badge);
    badge_row->addStretch(1);
    side->addLayout(badge_row);

    side->addSpacing(10);
    title = new QLabel;
    title->setObjectName(QStringLiteral("FritchiiHeroTitle"));
    title->setWordWrap(true);
    title->setMaximumWidth(680);
    side->addWidget(title);
    side->addSpacing(20);

    auto* action_row = new QHBoxLayout;
    action_row->setContentsMargins(0, 0, 0, 0);
    action_row->setSpacing(12);
    play = new QPushButton;
    play->setObjectName(QStringLiteral("FritchiiPlay"));
    play->setCursor(Qt::PointingHandCursor);
    play->setIconSize(QSize(18, 18));
    connect(play, &QPushButton::clicked, this, [this] {
        if (hero_path.isEmpty()) {
            hooks.add_folder();
        } else {
            hooks.play(hero_path, hero_id);
        }
    });
    action_row->addWidget(play);

    controls_button = new QPushButton(tr("Controls"), page);
    controls_button->setObjectName(QStringLiteral("FritchiiLink"));
    controls_button->setCursor(Qt::PointingHandCursor);
    controls_button->setFocusPolicy(Qt::NoFocus);
    controls_button->setMinimumHeight(44);
    controls_button->setMaximumHeight(44);
    controls_button->setToolTip(
        tr("Customize per-game button layout and Xbox controller exit shortcut."));
    controls_button->hide();
    connect(controls_button, &QPushButton::clicked, this, [this] {
        if (!hero_path.isEmpty() && hooks.remap_buttons) {
            hooks.remap_buttons(hero_id, hero_title, hero_path.toStdString());
        }
    });
    action_row->addWidget(controls_button);

    addons_button = new QPushButton(tr("Updates & DLC"), page);
    addons_button->setObjectName(QStringLiteral("FritchiiLink"));
    addons_button->setCursor(Qt::PointingHandCursor);
    addons_button->setFocusPolicy(Qt::NoFocus);
    addons_button->setMinimumHeight(44);
    addons_button->setMaximumHeight(44);
    addons_button->hide();
    connect(addons_button, &QPushButton::clicked, this, &Home::ShowHeroAddonsDialog);
    action_row->addWidget(addons_button);
    action_row->addStretch(1);
    side->addLayout(action_row);

    side->addStretch(1);
    hero->addLayout(side, 1);
    layout->addLayout(hero);

    layout->addSpacing(28);
    auto* scroll = new QScrollArea(page);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    sections_box = new QWidget;
    sections_box->setStyleSheet(QStringLiteral("background: transparent;"));
    sections_layout = new QVBoxLayout(sections_box);
    sections_layout->setContentsMargins(0, 0, 0, 0);
    sections_layout->setSpacing(20);
    sections_layout->setAlignment(Qt::AlignTop);
    scroll->setWidget(sections_box);
    layout->addWidget(scroll, 1);
    return page;
}

void Home::UpdateHero(const QString& path, u64 program_id, const QString& game_title,
                      bool is_switch2, const QPixmap& icon, const QString& addons_text) {
    hero_path = path;
    hero_title = game_title;
    hero_addons = addons_text;
    hero_id = program_id;
    cover->setPixmap(Rounded(icon, CoverSize, 18));
    cover->show();
    system_badge->setText(is_switch2 ? tr("SWITCH 2") : tr("SWITCH 1"));
    system_badge->setStyleSheet(
        is_switch2 ? QStringLiteral("QLabel { background-color: #5c2028; color: #fca5a5; "
                                    "border-radius: 6px; padding: 4px 10px; font-size: 11px; "
                                    "font-weight: 600; }")
                   : QStringLiteral("QLabel { background-color: #312e59; color: #c4b5fd; "
                                    "border-radius: 6px; padding: 4px 10px; font-size: 11px; "
                                    "font-weight: 600; }"));
    system_badge->show();

    const QChar check(0x2713);
    const AddOnSummary summary = ParseAddOns(addons_text);
    if (summary.HasEnabledUpdate()) {
        update_badge->setText(tr("%1 Update %2").arg(check).arg(summary.enabled_update_version));
        update_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #143626; color: #6ee7b7; border: 1px solid #22543d; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        update_badge->setToolTip(
            tr("Game update %1 is installed and active.").arg(summary.enabled_update_version));
    } else if (summary.HasDisabledUpdate()) {
        update_badge->setText(tr("! Update Disabled (%1)").arg(summary.disabled_update_version));
        update_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #422b12; color: #fcd34d; border: 1px solid #6b441a; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        update_badge->setToolTip(
            tr("An update is installed but currently disabled in Add-Ons."));
    } else {
        update_badge->setText(tr("Base v1.0.0"));
        update_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #1f1f27; color: #9a9aa8; border: 1px solid #2c2c38; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        update_badge->setToolTip(tr("Running base version (no update installed)."));
    }
    update_badge->show();

    if (summary.enabled_dlc_count > 0) {
        dlc_badge->setText(tr("%1 %2 DLC Installed").arg(check).arg(summary.enabled_dlc_count));
        dlc_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #15314a; color: #7dd3fc; border: 1px solid #1e4b75; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        dlc_badge->setToolTip(
            tr("%1 DLC pack(s) installed and active.").arg(summary.enabled_dlc_count));
        dlc_badge->show();
    } else if (summary.disabled_dlc_count > 0) {
        dlc_badge->setText(tr("! %1 DLC Disabled").arg(summary.disabled_dlc_count));
        dlc_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #422b12; color: #fcd34d; border: 1px solid #6b441a; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        dlc_badge->setToolTip(tr("DLC packs are installed but currently disabled."));
        dlc_badge->show();
    } else {
        dlc_badge->hide();
    }

    if (summary.enabled_mod_count > 0) {
        mods_badge->setText(summary.enabled_mod_count == 1
                                ? tr("1 Mod Active")
                                : tr("%1 Mods Active").arg(summary.enabled_mod_count));
        mods_badge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #2c2142; color: #d8b4fe; border: 1px solid #443166; "
            "border-radius: 6px; padding: 3px 10px; font-size: 11px; font-weight: 600; }"));
        mods_badge->show();
    } else {
        mods_badge->hide();
    }

    title->setText(game_title);
    play->setText(tr("Play"));
    play->setIcon(Glyph(QStringLiteral("play"), 0x1c1433, 18));
    if (controls_button) {
        controls_button->show();
    }
    addons_button->show();
}

void Home::ShowHeroAddonsDialog() {
    if (hero_path.isEmpty()) {
        return;
    }
    const QChar check(0x2713);
    const QChar emdash(0x2014);
    const AddOnSummary summary = ParseAddOns(hero_addons);
    QDialog dialog(window);
    dialog.setWindowTitle(tr("Updates & DLC %1 %2").arg(emdash).arg(hero_title));
    dialog.setMinimumWidth(520);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #141418; color: #ececf1; }"
        "QLabel { color: #ececf1; }"
        "QFrame#StatusCard { background: #1b1b22; border: 1px solid #2b2b36; border-radius: 10px; }"));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);

    auto* header = new QLabel(hero_title, &dialog);
    header->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #ececf1;"));
    header->setWordWrap(true);
    root->addWidget(header);

    auto* tid_label = new QLabel(
        tr("Title ID: %1").arg(QString::number(hero_id, 16).toUpper().rightJustified(16, QLatin1Char('0'))),
        &dialog);
    tid_label->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
    root->addWidget(tid_label);

    // Overall health banner: an older update disabled alongside an active update is normal.
    auto* banner = new QLabel(&dialog);
    banner->setWordWrap(true);
    const bool has_disabled =
        (!summary.HasEnabledUpdate() && summary.HasDisabledUpdate()) ||
        (summary.enabled_dlc_count == 0 && summary.disabled_dlc_count > 0);
    if (has_disabled) {
        banner->setText(tr("! Some installed Updates or DLC are currently disabled. Click \"Manage Add-Ons...\" below to enable them."));
        banner->setStyleSheet(QStringLiteral(
            "QLabel { background: #3b2610; color: #fcd34d; border: 1px solid #6b441a; "
            "border-radius: 8px; padding: 10px 14px; font-size: 12px; font-weight: 500; }"));
    } else if (summary.HasEnabledUpdate() || summary.enabled_dlc_count > 0) {
        banner->setText(tr("%1 All detected Updates & DLC for this game are properly installed and active.").arg(check));
        banner->setStyleSheet(QStringLiteral(
            "QLabel { background: #123324; color: #6ee7b7; border: 1px solid #22543d; "
            "border-radius: 8px; padding: 10px 14px; font-size: 12px; font-weight: 500; }"));
    } else {
        banner->setText(tr("Base game is ready to play. No separate Updates or DLC were found for this title."));
        banner->setStyleSheet(QStringLiteral(
            "QLabel { background: #1d1d26; color: #c9c9d3; border: 1px solid #2f2f3d; "
            "border-radius: 8px; padding: 10px 14px; font-size: 12px; }"));
    }
    root->addWidget(banner);

    const auto add_card = [&](const QString& label_text, const QString& value_text,
                              const QString& value_color) {
        auto* card = new QFrame(&dialog);
        card->setObjectName(QStringLiteral("StatusCard"));
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(14, 12, 14, 12);
        auto* left = new QLabel(label_text, card);
        left->setStyleSheet(QStringLiteral("font-size: 12px; color: #9a9aa8; font-weight: 500;"));
        auto* right = new QLabel(value_text, card);
        right->setStyleSheet(
            QStringLiteral("font-size: 13px; color: %1; font-weight: 600;").arg(value_color));
        row->addWidget(left);
        row->addStretch(1);
        row->addWidget(right);
        root->addWidget(card);
    };

    if (summary.HasEnabledUpdate()) {
        add_card(tr("Game Update"),
                 tr("%1 Installed & Active (%2)").arg(check).arg(summary.enabled_update_version),
                 QStringLiteral("#6ee7b7"));
    } else if (summary.HasDisabledUpdate()) {
        add_card(tr("Game Update"), tr("! Disabled (%1)").arg(summary.disabled_update_version),
                 QStringLiteral("#fcd34d"));
    } else {
        add_card(tr("Game Update"), tr("Base v1.0.0 (None installed)"), QStringLiteral("#9a9aa8"));
    }

    if (summary.enabled_dlc_count > 0) {
        add_card(tr("DLC Content"),
                 tr("%1 %2 DLC Pack(s) Installed & Active").arg(check).arg(summary.enabled_dlc_count),
                 QStringLiteral("#7dd3fc"));
    } else if (summary.disabled_dlc_count > 0) {
        add_card(tr("DLC Content"), tr("! %1 DLC Pack(s) Disabled").arg(summary.disabled_dlc_count),
                 QStringLiteral("#fcd34d"));
    } else {
        add_card(tr("DLC Content"), tr("None detected"), QStringLiteral("#9a9aa8"));
    }

    if (summary.enabled_mod_count > 0 || summary.disabled_mod_count > 0) {
        add_card(tr("Mods & Cheats"),
                 tr("%1 active, %2 disabled")
                     .arg(summary.enabled_mod_count)
                     .arg(summary.disabled_mod_count),
                 QStringLiteral("#d8b4fe"));
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    auto* manage_btn = new QPushButton(tr("Manage Add-Ons..."), &dialog);
    manage_btn->setObjectName(QStringLiteral("FritchiiLink"));
    manage_btn->setCursor(Qt::PointingHandCursor);
    connect(manage_btn, &QPushButton::clicked, &dialog, [&] {
        dialog.accept();
        hooks.game_settings(hero_id, hero_path.toStdString());
    });
    auto* close_btn = new QPushButton(tr("Close"), &dialog);
    close_btn->setObjectName(QStringLiteral("FritchiiPlay"));
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(manage_btn);
    buttons->addStretch(1);
    buttons->addWidget(close_btn);
    root->addLayout(buttons);

    dialog.exec();
}

void Home::ShowLibraryAddonsDialog() {
    std::vector<Game> games = CollectGames(hooks.model, window->windowIcon().pixmap(256),
                                           GameListIndex::SystemFilter::All);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        if (a.generation != b.generation) {
            return static_cast<int>(a.generation) < static_cast<int>(b.generation);
        }
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });

    // Deduplicate by program_id so duplicate dumps don't clutter the overview.
    std::vector<const Game*> unique_games;
    std::set<u64> seen_ids;
    for (const Game& g : games) {
        if (g.program_id == 0 || seen_ids.insert(g.program_id).second) {
            unique_games.push_back(&g);
        }
    }

    int updated_count = 0;
    int total_dlc = 0;
    int disabled_issues = 0;
    for (const Game* g : unique_games) {
        const AddOnSummary s = ParseAddOns(g->addons);
        if (s.HasEnabledUpdate()) {
            ++updated_count;
        }
        total_dlc += s.enabled_dlc_count;
        if ((!s.HasEnabledUpdate() && s.HasDisabledUpdate()) ||
            (s.enabled_dlc_count == 0 && s.disabled_dlc_count > 0)) {
            ++disabled_issues;
        }
    }

    QDialog dialog(window);
    dialog.setWindowTitle(tr("Library Updates & DLC Status"));
    dialog.resize(820, 520);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #141418; color: #ececf1; }"
        "QTableWidget { background: #18181f; color: #ececf1; gridline-color: #262632; "
        "border: 1px solid #2b2b36; border-radius: 8px; selection-background-color: #2b2442; }"
        "QHeaderView::section { background: #1f1f28; color: #9a9aa8; border: none; "
        "padding: 8px 10px; font-weight: 600; font-size: 11px; }"));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);

    const QChar check(0x2713);
    const QChar middot(0x00B7);
    const QChar emdash(0x2014);
    auto* banner = new QLabel(&dialog);
    banner->setWordWrap(true);
    if (disabled_issues > 0) {
        banner->setText(tr("! %1 of %2 games updated %3 %4 active DLC pack(s) %3 %5 game(s) have disabled Updates or DLC")
                            .arg(updated_count)
                            .arg(unique_games.size())
                            .arg(middot)
                            .arg(total_dlc)
                            .arg(disabled_issues));
        banner->setStyleSheet(QStringLiteral(
            "QLabel { background: #3b2610; color: #fcd34d; border: 1px solid #6b441a; "
            "border-radius: 8px; padding: 10px 14px; font-size: 13px; font-weight: 600; }"));
    } else {
        banner->setText(tr("%1 All installed Updates & DLC are active %2 %3 of %4 games updated %5 %6 DLC pack(s) installed")
                            .arg(check)
                            .arg(emdash)
                            .arg(updated_count)
                            .arg(unique_games.size())
                            .arg(middot)
                            .arg(total_dlc));
        banner->setStyleSheet(QStringLiteral(
            "QLabel { background: #123324; color: #6ee7b7; border: 1px solid #22543d; "
            "border-radius: 8px; padding: 10px 14px; font-size: 13px; font-weight: 600; }"));
    }
    root->addWidget(banner);

    auto* table = new QTableWidget(static_cast<int>(unique_games.size()), 5, &dialog);
    table->setHorizontalHeaderLabels(
        {tr("Game"), tr("System"), tr("Update"), tr("DLC"), tr("Status")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    for (int i = 0; i < static_cast<int>(unique_games.size()); ++i) {
        const Game* g = unique_games[static_cast<size_t>(i)];
        const AddOnSummary s = ParseAddOns(g->addons);

        auto* name_item = new QTableWidgetItem(g->title);
        auto* sys_item = new QTableWidgetItem(
            g->generation == GameListIndex::ConsoleGeneration::Switch2 ? tr("Switch 2")
                                                                       : tr("Switch 1"));
        sys_item->setForeground(
            QColor(g->generation == GameListIndex::ConsoleGeneration::Switch2
                       ? QStringLiteral("#fca5a5")
                       : QStringLiteral("#c4b5fd")));

        QTableWidgetItem* upd_item = nullptr;
        if (s.HasEnabledUpdate()) {
            upd_item = new QTableWidgetItem(tr("%1 v%2").arg(check).arg(s.enabled_update_version));
            upd_item->setForeground(QColor(QStringLiteral("#6ee7b7")));
        } else if (s.HasDisabledUpdate()) {
            upd_item = new QTableWidgetItem(tr("! Disabled (%1)").arg(s.disabled_update_version));
            upd_item->setForeground(QColor(QStringLiteral("#fcd34d")));
        } else {
            upd_item = new QTableWidgetItem(tr("Base v1.0.0"));
            upd_item->setForeground(QColor(QStringLiteral("#9a9aa8")));
        }

        QTableWidgetItem* dlc_item = nullptr;
        if (s.enabled_dlc_count > 0) {
            dlc_item = new QTableWidgetItem(tr("%1 %2 DLC").arg(check).arg(s.enabled_dlc_count));
            dlc_item->setForeground(QColor(QStringLiteral("#7dd3fc")));
        } else if (s.disabled_dlc_count > 0) {
            dlc_item = new QTableWidgetItem(tr("! %1 Disabled").arg(s.disabled_dlc_count));
            dlc_item->setForeground(QColor(QStringLiteral("#fcd34d")));
        } else {
            dlc_item = new QTableWidgetItem(QString(emdash));
            dlc_item->setForeground(QColor(QStringLiteral("#6e6e7c")));
        }

        QTableWidgetItem* status_item = nullptr;
        if ((!s.HasEnabledUpdate() && s.HasDisabledUpdate()) ||
            (s.enabled_dlc_count == 0 && s.disabled_dlc_count > 0)) {
            status_item = new QTableWidgetItem(tr("! Check Add-Ons"));
            status_item->setForeground(QColor(QStringLiteral("#fcd34d")));
        } else if (s.HasEnabledUpdate() || s.enabled_dlc_count > 0) {
            status_item = new QTableWidgetItem(tr("%1 Installed & Ready").arg(check));
            status_item->setForeground(QColor(QStringLiteral("#6ee7b7")));
        } else {
            status_item = new QTableWidgetItem(tr("%1 Base Ready").arg(check));
            status_item->setForeground(QColor(QStringLiteral("#c9c9d3")));
        }

        table->setItem(i, 0, name_item);
        table->setItem(i, 1, sys_item);
        table->setItem(i, 2, upd_item);
        table->setItem(i, 3, dlc_item);
        table->setItem(i, 4, status_item);
    }

    connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int) {
        if (row >= 0 && row < static_cast<int>(unique_games.size())) {
            const Game* g = unique_games[static_cast<size_t>(row)];
            dialog.accept();
            hooks.game_settings(g->program_id, g->path.toStdString());
        }
    });
    root->addWidget(table, 1);

    auto* footer = new QHBoxLayout;
    auto* hint = new QLabel(tr("Tip: Double-click any game to manage its Updates, DLC, or Mods."),
                            &dialog);
    hint->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
    auto* close_btn = new QPushButton(tr("Close"), &dialog);
    close_btn->setObjectName(QStringLiteral("FritchiiPlay"));
    close_btn->setCursor(Qt::PointingHandCursor);
    connect(close_btn, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer->addWidget(hint);
    footer->addStretch(1);
    footer->addWidget(close_btn);
    root->addLayout(footer);

    dialog.exec();
}

void Home::Refresh() {
    sections_dirty = false;
    const auto filter = static_cast<GameListIndex::SystemFilter>(
        std::clamp<int>(static_cast<int>(UISettings::values.system_filter.GetValue()), 0, 2));
    if (system_filter_combo && system_filter_combo->currentIndex() != static_cast<int>(filter)) {
        const QSignalBlocker blocker(system_filter_combo);
        system_filter_combo->setCurrentIndex(static_cast<int>(filter));
    }

    std::vector<Game> games =
        CollectGames(hooks.model, window->windowIcon().pixmap(256), filter);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        if (a.generation != b.generation) {
            return static_cast<int>(a.generation) < static_cast<int>(b.generation);
        }
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });

    // The hero: the game picked last, else the first Switch 1 game, else the first game.
    const Game* hero = nullptr;
    for (const Game& game : games) {
        if (!hero_path.isEmpty() && game.path == hero_path) {
            hero = &game;
            break;
        }
    }
    if (!hero) {
        for (const Game& game : games) {
            if (game.generation == GameListIndex::ConsoleGeneration::Switch1) {
                hero = &game;
                break;
            }
        }
    }
    if (!hero && !games.empty()) {
        hero = &games.front();
    }

    if (hero) {
        UpdateHero(hero->path, hero->program_id, hero->title,
                   hero->generation == GameListIndex::ConsoleGeneration::Switch2, hero->icon,
                   hero->addons);
    } else {
        hero_path.clear();
        hero_title.clear();
        hero_addons.clear();
        hero_id = 0;
        cover->hide();
        system_badge->hide();
        update_badge->hide();
        dlc_badge->hide();
        mods_badge->hide();
        if (controls_button) {
            controls_button->hide();
        }
        addons_button->hide();
        title->setText(tr("Add your games"));
        play->setText(tr("Add Folder"));
        play->setIcon(Glyph(QStringLiteral("plus"), 0x1c1433, 18));
    }

    // Rebuild the separated Switch 1 / Switch 2 rows.
    sections_box->setUpdatesEnabled(false);
    while (QLayoutItem* item = sections_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    std::vector<const Game*> sw1_games;
    std::vector<const Game*> sw2_games;
    for (const Game& game : games) {
        if (game.generation == GameListIndex::ConsoleGeneration::Switch2) {
            sw2_games.push_back(&game);
        } else {
            sw1_games.push_back(&game);
        }
    }

    const auto add_section = [this](const QString& heading, const QString& badge_color,
                                    const std::vector<const Game*>& group) {
        if (group.empty()) {
            return;
        }
        auto* section = new QWidget(sections_box);
        section->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* v = new QVBoxLayout(section);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);

        auto* header = new QLabel(QStringLiteral("%1 (%2)").arg(heading).arg(group.size()), section);
        header->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; font-size: 13px; font-weight: 600; letter-spacing: 0.5px; }")
                                  .arg(badge_color));
        v->addWidget(header);

        auto* hscroll = new QScrollArea(section);
        hscroll->setFrameShape(QFrame::NoFrame);
        hscroll->setWidgetResizable(true);
        hscroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hscroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        hscroll->setFixedHeight(TileIcon + 64);
        hscroll->setStyleSheet(
            QStringLiteral("QScrollArea { background: transparent; border: none; }"));

        auto* strip = new QWidget;
        strip->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* row = new QHBoxLayout(strip);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->setAlignment(Qt::AlignLeft);

        for (const Game* game : group) {
            auto* button = new QToolButton(strip);
            button->setObjectName(QStringLiteral("FritchiiTile"));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setIcon(QIcon(Rounded(game->icon, TileIcon, 12)));
            button->setIconSize(QSize(TileIcon, TileIcon));
            button->setFixedWidth(TileIcon + 40);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            const QFontMetrics metrics(button->font());
            button->setText(metrics.elidedText(game->title, Qt::ElideRight, TileIcon + 24));
            button->setToolTip(QStringLiteral("%1\n%2").arg(
                game->title, FormatTileAddOnLine(ParseAddOns(game->addons))));
            const bool is_sw2 = game->generation == GameListIndex::ConsoleGeneration::Switch2;
            connect(button, &QToolButton::clicked, this,
                    [this, path = game->path, id = game->program_id, game_title = game->title,
                     is_sw2, icon = game->icon, addons = game->addons] {
                        UpdateHero(path, id, game_title, is_sw2, icon, addons);
                    });
            row->addWidget(button);
        }
        row->addStretch(1);
        hscroll->setWidget(strip);
        v->addWidget(hscroll);
        sections_layout->addWidget(section);
    };

    add_section(tr("Nintendo Switch"), QStringLiteral("#c4b5fd"), sw1_games);
    add_section(tr("Nintendo Switch 2"), QStringLiteral("#fca5a5"), sw2_games);
    sections_layout->addStretch(1);
    sections_box->setUpdatesEnabled(true);
}

void Home::ShowHome() {
    list_mode = false;
    game_list->hide();
    placeholder->hide();
    rail->show();
    page->show();
    library_button->setChecked(true);
    if (sections_dirty) {
        Refresh();
    }
}

void Home::ShowList() {
    list_mode = true;
    page->hide();
    rail->show();
    library_button->setChecked(false);
    if (hooks.game_list_empty()) {
        placeholder->show();
    } else {
        game_list->show();
    }
}

void Home::Restore() {
    // A restart shows the list for a moment, and nothing may cover a running game.
    if (hooks.single_window() && hooks.emulation_running()) {
        return;
    }
    if (list_mode) {
        rail->show();
        page->hide();
    } else {
        ShowHome();
    }
}

void Home::OnEmulationStarting() {
    if (!hooks.single_window()) {
        return;
    }
    rail->hide();
    page->hide();
}

bool Home::eventFilter(QObject* watched, QEvent* event) {
    // The main window shows its game list (start up, a game stopped, the list filled): show the
    // home page instead, unless the player asked for the full list.
    if (event->type() == QEvent::Show && (watched == game_list || watched == placeholder)) {
        QTimer::singleShot(0, this, &Home::Restore);
    }
    return QObject::eventFilter(watched, event);
}

} // namespace FritchiiUi
