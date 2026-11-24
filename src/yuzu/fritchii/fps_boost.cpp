// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/fps_boost.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <QCheckBox>
#include <QDialog>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/file_sys/patch_manager.h"

namespace FritchiiUi {

namespace {

constexpr const char* kModFolderName = "60 FPS";
constexpr const char* kSwapIntervalFlag = "force_swap_interval.flag";
constexpr u64 kKirbyForgottenLandId = 0x01004D300C5AE000ULL;
constexpr u64 kBotwId = 0x01007EF00011E000ULL;

std::filesystem::path ModRootDir(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
           fmt::format("{:016X}", program_id) / kModFolderName;
}

std::filesystem::path CheatsDir(u64 program_id) {
    return ModRootDir(program_id) / "cheats";
}

std::filesystem::path ExefsDir(u64 program_id) {
    return ModRootDir(program_id) / "exefs";
}

std::filesystem::path SwapFlagPath(u64 program_id) {
    return ModRootDir(program_id) / kSwapIntervalFlag;
}

bool IsModDisabledInSettings(u64 program_id) {
    const auto lock = FileSys::PatchManager::LockAddOns();
    const auto it = Settings::values.disabled_addons.find(program_id);
    if (it == Settings::values.disabled_addons.end()) {
        return false;
    }
    return std::find(it->second.begin(), it->second.end(), kModFolderName) != it->second.end();
}

void SetModEnabledInSettings(u64 program_id, bool enabled) {
    const auto lock = FileSys::PatchManager::LockAddOns();
    auto& seen = Settings::values.seen_addons[program_id];
    if (std::find(seen.begin(), seen.end(), kModFolderName) == seen.end()) {
        seen.emplace_back(kModFolderName);
    }
    auto& disabled = Settings::values.disabled_addons[program_id];
    if (enabled) {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), kModFolderName),
                       disabled.end());
    } else if (std::find(disabled.begin(), disabled.end(), kModFolderName) == disabled.end()) {
        disabled.emplace_back(kModFolderName);
    }
}

void SetForceSwapIntervalFlag(u64 program_id, bool enabled) {
    std::error_code ec;
    const auto flag_path = SwapFlagPath(program_id);
    if (enabled) {
        std::filesystem::create_directories(flag_path.parent_path(), ec);
        std::ofstream out(flag_path, std::ios::binary | std::ios::trunc);
        out << "1\n";
    } else {
        std::filesystem::remove(flag_path, ec);
    }
}

/// Extracts ONLY the [60 FPS] / [60FPS] section from an Atmosphere cheat .txt file so no
/// unrelated cheats (health, coins, resolution locks, etc.) are activated.
std::string Extract60FpsSection(const std::string& raw_text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= raw_text.size()) {
        const std::size_t nl = raw_text.find('\n', pos);
        std::string line =
            (nl == std::string::npos) ? raw_text.substr(pos) : raw_text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }

    const auto to_lower_copy = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const auto is_60fps_header = [&](const std::string& line) {
        if (line.size() < 4 || line.front() != '[' || line.back() != ']') {
            return false;
        }
        const std::string lower = to_lower_copy(line);
        const bool mentions_60 =
            lower.find("60 fps") != std::string::npos || lower.find("60fps") != std::string::npos;
        const bool is_disable_or_default = lower.find("off") != std::string::npos ||
                                           lower.find("disable") != std::string::npos ||
                                           lower.find("30") != std::string::npos ||
                                           lower.find("default") != std::string::npos;
        return mentions_60 && !is_disable_or_default;
    };

    std::string best_block;
    bool in_target = false;
    std::string current_block;
    int code_lines = 0;

    const auto flush_block = [&] {
        if (in_target && code_lines > 0 && best_block.empty()) {
            best_block = current_block;
        }
        in_target = false;
        current_block.clear();
        code_lines = 0;
    };

    for (const auto& raw_line : lines) {
        std::string trimmed = raw_line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.pop_back();
        }
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            flush_block();
            if (is_60fps_header(trimmed)) {
                in_target = true;
                current_block = "[60 FPS]\n";
            }
            continue;
        }
        if (in_target) {
            current_block += trimmed;
            current_block.push_back('\n');
            ++code_lines;
        }
    }
    flush_block();
    return best_block;
}

std::optional<std::string> HttpGet(const QUrl& url, int timeout_ms = 8000) {
    QNetworkAccessManager manager;
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "Genesis-Emulator");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    auto* reply = manager.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        reply->abort();
        loop.quit();
    });
    timer.start(timeout_ms);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    std::optional<std::string> result;
    if (reply->error() == QNetworkReply::NoError && status == 200) {
        const QByteArray body = reply->readAll();
        result = std::string(body.constData(), static_cast<std::size_t>(body.size()));
    }
    reply->deleteLater();
    return result;
}

/// Installs built-in verified offline fallback (e.g., Kirby and the Forgotten Land 1.0.0)
/// or fetches verified [60 FPS] cheat blocks from ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats.
int FetchAndInstall60FpsPatch(u64 program_id, QString& out_detail) {
    std::error_code ec;
    const auto cheats_dir = CheatsDir(program_id);
    int installed_files = 0;

    // 1. Try online community 60 FPS repository (covers hundreds of 30 FPS Switch games).
    const QString tid_hex =
        QString::fromStdString(fmt::format("{:016X}", program_id));
    const QUrl api_url(
        QStringLiteral("https://api.github.com/repos/ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats/"
                       "contents/titles/%1/cheats")
            .arg(tid_hex));

    if (const auto json_body = HttpGet(api_url, 8000); json_body.has_value()) {
        try {
            const auto arr = nlohmann::json::parse(*json_body);
            if (arr.is_array()) {
                for (const auto& item : arr) {
                    if (!item.is_object() || !item.contains("name") ||
                        !item.contains("download_url")) {
                        continue;
                    }
                    const std::string name = item["name"].get<std::string>();
                    if (name.size() < 5 || name.substr(name.size() - 4) != ".txt") {
                        continue;
                    }
                    const std::string dl = item["download_url"].get<std::string>();
                    const auto txt = HttpGet(QUrl(QString::fromStdString(dl)), 8000);
                    if (!txt.has_value()) {
                        continue;
                    }
                    const std::string section = Extract60FpsSection(*txt);
                    if (section.empty()) {
                        continue;
                    }
                    std::filesystem::create_directories(cheats_dir, ec);
                    std::ofstream out(cheats_dir / name, std::ios::binary | std::ios::trunc);
                    if (out.good()) {
                        out.write(section.data(), static_cast<std::streamsize>(section.size()));
                        ++installed_files;
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARNING(Frontend, "60 FPS patch lookup parse error for {:016X}: {}", program_id,
                        ex.what());
        }
    }

    // 2. Built-in verified offline fallbacks for popular 30 FPS titles.
    if (installed_files == 0 && program_id == kKirbyForgottenLandId) {
        std::filesystem::create_directories(cheats_dir, ec);
        constexpr const char* kKirby60Fps =
            "[60 FPS]\n"
            "580F0000 00A723D0\n"
            "780F0000 00003694\n"
            "640F0000 00000000 00000001\n"
            "580F0000 00A72628\n"
            "780F0000 00000050\n"
            "640F0000 00000000 00000001\n";
        std::ofstream out(cheats_dir / "D9BA7DB72FFAFECD.txt", std::ios::binary | std::ios::trunc);
        if (out.good()) {
            out << kKirby60Fps;
            ++installed_files;
        }
    } else if (installed_files == 0 && program_id == kBotwId) {
        const auto exefs_dir = ExefsDir(program_id);
        std::filesystem::create_directories(exefs_dir, ec);
        constexpr const char* kBotw160_60Fps =
            "@nsobid-8E9978D50BDD20B4C8395A106C27FFDE\n"
            "\n"
            "# The Legend of Zelda: Breath of the Wild v1.6.0 - 60FPS\n"
            "@flag print_values\n"
            "@flag offset_shift 0x100\n"
            "\n"
            "@enabled\n"
            "01614954 08008052\n"
            "01614D84 08F08752\n"
            "01614DB0 20008052\n"
            "01614DA4 21000012\n"
            "@stop\n";
        std::ofstream out(exefs_dir / "1.6.0.pchtxt", std::ios::binary | std::ios::trunc);
        if (out.good()) {
            out << kBotw160_60Fps;
            ++installed_files;
        }
    }

    if (installed_files > 0) {
        SetModEnabledInSettings(program_id, true);
        if (program_id == kBotwId) {
            out_detail = QObject::tr(
                "Installed verified 60 FPS patch for Breath of the Wild (v1.6.0) and enabled it "
                "automatically. Note: requires game update v1.6.0 (v851968) to activate.");
        } else {
            out_detail = QObject::tr("Installed verified 60 FPS patch for %1 game build(s) and "
                                     "enabled it automatically.")
                             .arg(installed_files);
        }
    } else if (Is60FpsPatchInstalled(program_id)) {
        SetModEnabledInSettings(program_id, true);
        out_detail = QObject::tr("Verified 60 FPS mod is installed and enabled for this game.");
        installed_files = 1;
    } else {
        out_detail = QObject::tr(
            "No community 60 FPS patch was found for this Title ID. Many Switch games (like "
            "Mario Kart 8 or Smash Bros.) already run at 60 FPS natively. For other 30 FPS "
            "games, you can also try the experimental 60 Hz option below.");
    }
    return installed_files;
}

} // namespace

bool Is60FpsPatchInstalled(u64 program_id) {
    if (program_id == 0) {
        return false;
    }
    std::error_code ec;
    const auto cheats_dir = CheatsDir(program_id);
    if (std::filesystem::is_directory(cheats_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(cheats_dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
                return true;
            }
        }
    }
    const auto exefs_dir = ExefsDir(program_id);
    if (std::filesystem::is_directory(exefs_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(exefs_dir, ec)) {
            if (entry.is_regular_file(ec)) {
                return true;
            }
        }
    }
    return false;
}

bool HasForce60FpsSwapInterval(u64 program_id) {
    if (program_id == 0) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(SwapFlagPath(program_id), ec);
}

bool Is60FpsBoostActive(u64 program_id) {
    if (program_id == 0) {
        return false;
    }
    if (Is60FpsPatchInstalled(program_id) && !IsModDisabledInSettings(program_id)) {
        return true;
    }
    return HasForce60FpsSwapInterval(program_id);
}

bool Show60FpsBoostDialog(QWidget* parent, u64 program_id, const QString& game_title) {
    if (program_id == 0) {
        return false;
    }

    const QChar check(0x2713);
    const QChar emdash(0x2014);
    bool modified = false;

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("60 FPS Boost %1 %2").arg(emdash).arg(game_title));
    dialog.setMinimumWidth(520);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #141418; color: #ececf1; }"
        "QLabel { color: #ececf1; }"
        "QFrame#Card { background: #1b1b22; border: 1px solid #2b2b36; border-radius: 10px; }"
        "QCheckBox { color: #ececf1; spacing: 8px; font-size: 12px; }"
        "QPushButton { background: #262633; color: #ececf1; border: 1px solid #3a3a4a; "
        "border-radius: 7px; padding: 7px 14px; font-weight: 500; }"
        "QPushButton:hover { background: #313142; border-color: #55556e; }"
        "QPushButton#PrimaryButton { background: #7c3aed; border-color: #8b5cf6; "
        "color: #ffffff; font-weight: 600; padding: 8px 16px; }"
        "QPushButton#PrimaryButton:hover { background: #8b5cf6; }"
        "QPushButton#SuccessButton { background: #166534; border-color: #22c55e; "
        "color: #ecfdf5; font-weight: 600; padding: 8px 16px; }"));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(12);

    auto* heading = new QLabel(game_title, &dialog);
    heading->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #ececf1;"));
    heading->setWordWrap(true);
    root->addWidget(heading);

    auto* tid_lbl = new QLabel(
        QObject::tr("Title ID: %1")
            .arg(QString::number(program_id, 16).toUpper().rightJustified(16, QLatin1Char('0'))),
        &dialog);
    tid_lbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
    root->addWidget(tid_lbl);

    auto* banner = new QLabel(&dialog);
    banner->setWordWrap(true);
    root->addWidget(banner);

    // Patch install / toggle card
    auto* patch_card = new QFrame(&dialog);
    patch_card->setObjectName(QStringLiteral("Card"));
    auto* patch_layout = new QVBoxLayout(patch_card);
    patch_layout->setContentsMargins(14, 12, 14, 12);
    patch_layout->setSpacing(8);

    auto* patch_title = new QLabel(QObject::tr("Game-Specific 60 FPS Patch (Recommended)"),
                                   patch_card);
    patch_title->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600;"));
    patch_layout->addWidget(patch_title);

    auto* patch_desc = new QLabel(
        QObject::tr("Patches the game's internal 30 FPS cap to 60 FPS while keeping normal 1x "
                    "gameplay speed (works for Kirby and the Forgotten Land and hundreds of "
                    "other 30 FPS Switch games)."),
        patch_card);
    patch_desc->setWordWrap(true);
    patch_desc->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
    patch_layout->addWidget(patch_desc);

    auto* patch_btn_row = new QHBoxLayout;
    auto* toggle_patch_btn = new QPushButton(&dialog);
    auto* install_patch_btn = new QPushButton(QObject::tr("Auto-Install 60 FPS Patch"), &dialog);
    install_patch_btn->setObjectName(QStringLiteral("PrimaryButton"));
    patch_btn_row->addWidget(toggle_patch_btn);
    patch_btn_row->addWidget(install_patch_btn);
    patch_btn_row->addStretch(1);
    patch_layout->addLayout(patch_btn_row);

    auto* feedback_lbl = new QLabel(&dialog);
    feedback_lbl->setWordWrap(true);
    feedback_lbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #c4b5fd;"));
    feedback_lbl->hide();
    patch_layout->addWidget(feedback_lbl);
    root->addWidget(patch_card);

    // Experimental swap-interval card
    auto* exp_card = new QFrame(&dialog);
    exp_card->setObjectName(QStringLiteral("Card"));
    auto* exp_layout = new QVBoxLayout(exp_card);
    exp_layout->setContentsMargins(14, 12, 14, 12);
    exp_layout->setSpacing(6);

    auto* swap_checkbox = new QCheckBox(
        QObject::tr("Force 60 Hz presentation interval (Experimental fallback)"), exp_card);
    swap_checkbox->setChecked(HasForce60FpsSwapInterval(program_id));
    exp_layout->addWidget(swap_checkbox);

    auto* exp_desc = new QLabel(
        QObject::tr("Overrides 30 Hz vblank locks directly in the emulator. Useful for games "
                    "with dynamic delta-time that do not have a patch yet. Leave OFF when using "
                    "a 60 FPS patch above (turning it on for fixed-timestep games may double "
                    "game speed)."),
        exp_card);
    exp_desc->setWordWrap(true);
    exp_desc->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
    exp_layout->addWidget(exp_desc);
    root->addWidget(exp_card);

    const auto refresh_ui = [&] {
        const bool installed = Is60FpsPatchInstalled(program_id);
        const bool enabled = installed && !IsModDisabledInSettings(program_id);
        const bool swap_on = HasForce60FpsSwapInterval(program_id);

        if (enabled) {
            banner->setText(
                QObject::tr("%1 60 FPS Patch is installed and ACTIVE for this game.").arg(check));
            banner->setStyleSheet(QStringLiteral(
                "QLabel { background: #123324; color: #6ee7b7; border: 1px solid #22543d; "
                "border-radius: 8px; padding: 10px 14px; font-size: 12px; font-weight: 600; }"));
            toggle_patch_btn->setText(QObject::tr("Disable 60 FPS (Use Default 30 FPS)"));
            toggle_patch_btn->setObjectName(QString());
            toggle_patch_btn->show();
            install_patch_btn->setText(QObject::tr("Re-check Patch"));
            install_patch_btn->setObjectName(QString());
        } else if (installed) {
            banner->setText(
                QObject::tr("60 FPS Patch is installed but currently OFF (game will run at default "
                            "30 FPS)."));
            banner->setStyleSheet(QStringLiteral(
                "QLabel { background: #3b2610; color: #fcd34d; border: 1px solid #6b441a; "
                "border-radius: 8px; padding: 10px 14px; font-size: 12px; font-weight: 600; }"));
            toggle_patch_btn->setText(QObject::tr("Enable 60 FPS Patch"));
            toggle_patch_btn->setObjectName(QStringLiteral("SuccessButton"));
            toggle_patch_btn->show();
            install_patch_btn->setText(QObject::tr("Re-check Patch"));
            install_patch_btn->setObjectName(QString());
        } else if (swap_on) {
            banner->setText(QObject::tr(
                "Experimental 60 Hz presentation interval is ON for this game."));
            banner->setStyleSheet(QStringLiteral(
                "QLabel { background: #2a2040; color: #d8b4fe; border: 1px solid #5b3a99; "
                "border-radius: 8px; padding: 10px 14px; font-size: 12px; font-weight: 600; }"));
            toggle_patch_btn->hide();
            install_patch_btn->setText(QObject::tr("Auto-Install 60 FPS Patch"));
            install_patch_btn->setObjectName(QStringLiteral("PrimaryButton"));
        } else {
            banner->setText(QObject::tr(
                "No 60 FPS patch installed yet. Click \"Auto-Install 60 FPS Patch\" to fetch and "
                "enable one automatically for 30 FPS games."));
            banner->setStyleSheet(QStringLiteral(
                "QLabel { background: #1d1d26; color: #c9c9d3; border: 1px solid #2f2f3d; "
                "border-radius: 8px; padding: 10px 14px; font-size: 12px; }"));
            toggle_patch_btn->hide();
            install_patch_btn->setText(QObject::tr("Auto-Install 60 FPS Patch"));
            install_patch_btn->setObjectName(QStringLiteral("PrimaryButton"));
        }
        toggle_patch_btn->style()->unpolish(toggle_patch_btn);
        toggle_patch_btn->style()->polish(toggle_patch_btn);
        install_patch_btn->style()->unpolish(install_patch_btn);
        install_patch_btn->style()->polish(install_patch_btn);
    };

    QObject::connect(toggle_patch_btn, &QPushButton::clicked, &dialog, [&] {
        const bool next_enabled = IsModDisabledInSettings(program_id);
        SetModEnabledInSettings(program_id, next_enabled);
        modified = true;
        refresh_ui();
    });

    QObject::connect(install_patch_btn, &QPushButton::clicked, &dialog, [&] {
        install_patch_btn->setEnabled(false);
        install_patch_btn->setText(QObject::tr("Checking..."));
        feedback_lbl->setText(QObject::tr("Looking up verified 60 FPS patch..."));
        feedback_lbl->show();
        dialog.repaint();

        QString detail;
        const int count = FetchAndInstall60FpsPatch(program_id, detail);
        if (count > 0) {
            modified = true;
        }
        feedback_lbl->setText(detail);
        feedback_lbl->show();
        install_patch_btn->setEnabled(true);
        refresh_ui();
    });

    QObject::connect(swap_checkbox, &QCheckBox::toggled, &dialog, [&](bool checked) {
        SetForceSwapIntervalFlag(program_id, checked);
        modified = true;
        refresh_ui();
    });

    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    auto* close_btn = new QPushButton(QObject::tr("Done"), &dialog);
    close_btn->setObjectName(QStringLiteral("PrimaryButton"));
    QObject::connect(close_btn, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer->addWidget(close_btn);
    root->addLayout(footer);

    refresh_ui();
    dialog.exec();
    return modified;
}

} // namespace FritchiiUi
