// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/home.h"

#include <algorithm>
#include <set>
#include <vector>

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/scan_index.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/saves_window.h"

namespace FritchiiUi {
namespace {

constexpr int CoverSize = 200;
constexpr int TileIcon = 72;

struct Game {
    QString title;
    QString path;
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
    const auto add = [&](const QStandardItem* item) {
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
                add(top->child(child));
            }
        } else {
            add(top);
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
    system_badge = new QLabel;
    system_badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    system_badge->hide();
    side->addWidget(system_badge, 0, Qt::AlignLeft);
    side->addSpacing(10);
    title = new QLabel;
    title->setObjectName(QStringLiteral("FritchiiHeroTitle"));
    title->setWordWrap(true);
    title->setMaximumWidth(680);
    side->addWidget(title);
    side->addSpacing(20);
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
    side->addWidget(play, 0, Qt::AlignLeft);
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
                      bool is_switch2, const QPixmap& icon) {
    hero_path = path;
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
    title->setText(game_title);
    play->setText(tr("Play"));
    play->setIcon(Glyph(QStringLiteral("play"), 0x1c1433, 18));
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
                   hero->generation == GameListIndex::ConsoleGeneration::Switch2, hero->icon);
    } else {
        hero_path.clear();
        hero_id = 0;
        cover->hide();
        system_badge->hide();
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
            button->setToolTip(game->title);
            const bool is_sw2 = game->generation == GameListIndex::ConsoleGeneration::Switch2;
            connect(button, &QToolButton::clicked, this,
                    [this, path = game->path, id = game->program_id, game_title = game->title,
                     is_sw2, icon = game->icon] {
                        UpdateHero(path, id, game_title, is_sw2, icon);
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
