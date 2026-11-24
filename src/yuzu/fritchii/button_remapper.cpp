// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/button_remapper.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <QComboBox>
#include <QDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/fritchii/controller_exit_watcher.h"

namespace FritchiiUi {

namespace {

constexpr std::size_t NumRemappablePlayers = 4;

struct SwitchButtonSpec {
    Settings::NativeButton::Values id;
    const char* switch_name;
    const char* default_hint;
};

constexpr std::array<SwitchButtonSpec, 17> kSwitchButtons{{
    {Settings::NativeButton::A, "Switch A", "Right face button (Confirm)"},
    {Settings::NativeButton::B, "Switch B", "Bottom face button (Back / Cancel)"},
    {Settings::NativeButton::X, "Switch X", "Top face button"},
    {Settings::NativeButton::Y, "Switch Y", "Left face button"},
    {Settings::NativeButton::L, "Switch L", "Left bumper"},
    {Settings::NativeButton::R, "Switch R", "Right bumper"},
    {Settings::NativeButton::ZL, "Switch ZL", "Left trigger"},
    {Settings::NativeButton::ZR, "Switch ZR", "Right trigger"},
    {Settings::NativeButton::Plus, "Switch Plus (+)", "Pause / Start"},
    {Settings::NativeButton::Minus, "Switch Minus (-)", "Map / Select"},
    {Settings::NativeButton::LStick, "Switch L-Stick Click", "Press left stick"},
    {Settings::NativeButton::RStick, "Switch R-Stick Click", "Press right stick"},
    {Settings::NativeButton::DUp, "Switch D-Pad Up", "Directional up"},
    {Settings::NativeButton::DDown, "Switch D-Pad Down", "Directional down"},
    {Settings::NativeButton::DLeft, "Switch D-Pad Left", "Directional left"},
    {Settings::NativeButton::DRight, "Switch D-Pad Right", "Directional right"},
    {Settings::NativeButton::Screenshot, "Switch Capture", "Screenshot button"},
}};

QString GameButtonHint(u64 program_id, const SwitchButtonSpec& spec) {
    // Mario Kart 8 Deluxe (0100152000022000)
    if (program_id == 0x0100152000022000ULL) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Accelerate / Confirm");
        case Settings::NativeButton::B:
            return QObject::tr("Brake / Reverse / Back");
        case Settings::NativeButton::X:
            return QObject::tr("Look Back (Rear View)");
        case Settings::NativeButton::Y:
            return QObject::tr("Accelerate (Alt)");
        case Settings::NativeButton::L:
        case Settings::NativeButton::ZL:
            return QObject::tr("Use / Hold Item");
        case Settings::NativeButton::R:
        case Settings::NativeButton::ZR:
            return QObject::tr("Hop / Drift / Trick");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause Menu");
        default:
            break;
        }
    }
    // Super Smash Bros. Ultimate (01006A800016E000)
    if (program_id == 0x01006A800016E000ULL) {
        switch (spec.id) {
        case Settings::NativeButton::A:
            return QObject::tr("Attack / Confirm");
        case Settings::NativeButton::B:
            return QObject::tr("Special Move / Back");
        case Settings::NativeButton::X:
        case Settings::NativeButton::Y:
            return QObject::tr("Jump");
        case Settings::NativeButton::L:
        case Settings::NativeButton::R:
            return QObject::tr("Grab");
        case Settings::NativeButton::ZL:
        case Settings::NativeButton::ZR:
            return QObject::tr("Shield");
        case Settings::NativeButton::Plus:
            return QObject::tr("Pause");
        default:
            break;
        }
    }
    return QString::fromLatin1(spec.default_hint);
}

enum class XboxInputKind {
    Button,
    AxisPositive,
    Hat,
    Unbound,
};

struct XboxOption {
    const char* key;
    const char* label;
    XboxInputKind kind;
    int code;
    const char* direction;
};

constexpr std::array<XboxOption, 18> kXboxOptions{{
    {"btn_0", "Xbox A", XboxInputKind::Button, 0, ""},
    {"btn_1", "Xbox B", XboxInputKind::Button, 1, ""},
    {"btn_2", "Xbox X", XboxInputKind::Button, 2, ""},
    {"btn_3", "Xbox Y", XboxInputKind::Button, 3, ""},
    {"btn_4", "LB (Left Bumper)", XboxInputKind::Button, 4, ""},
    {"btn_5", "RB (Right Bumper)", XboxInputKind::Button, 5, ""},
    {"axis_2", "LT (Left Trigger)", XboxInputKind::AxisPositive, 2, ""},
    {"axis_5", "RT (Right Trigger)", XboxInputKind::AxisPositive, 5, ""},
    {"btn_6", "View / Back", XboxInputKind::Button, 6, ""},
    {"btn_7", "Menu / Start", XboxInputKind::Button, 7, ""},
    {"btn_8", "Left Stick Click (LS)", XboxInputKind::Button, 8, ""},
    {"btn_9", "Right Stick Click (RS)", XboxInputKind::Button, 9, ""},
    {"hat_up", "D-Pad Up", XboxInputKind::Hat, 0, "up"},
    {"hat_down", "D-Pad Down", XboxInputKind::Hat, 0, "down"},
    {"hat_left", "D-Pad Left", XboxInputKind::Hat, 0, "left"},
    {"hat_right", "D-Pad Right", XboxInputKind::Hat, 0, "right"},
    {"btn_10", "Xbox Guide", XboxInputKind::Button, 10, ""},
    {"unbound", "Unbound (None)", XboxInputKind::Unbound, -1, ""},
}};

struct SdlDeviceIdentity {
    std::string guid;
    int port = 0;
    bool valid = false;
};

SdlDeviceIdentity ExtractSdlIdentity(const Settings::PlayerInput& player, std::size_t player_index) {
    for (const auto& binding : player.buttons) {
        if (binding.empty() || binding == "[empty]") {
            continue;
        }
        Common::ParamPackage param(binding);
        if (param.Get("engine", "") == "sdl") {
            SdlDeviceIdentity id;
            id.guid = param.Get("guid", "");
            id.port = param.Get("port", static_cast<int>(player_index));
            id.valid = !id.guid.empty();
            if (id.valid) {
                return id;
            }
        }
    }
    return {};
}

std::optional<std::size_t> MatchXboxOption(const std::string& binding) {
    if (binding.empty() || binding == "[empty]") {
        return kXboxOptions.size() - 1; // Unbound
    }
    Common::ParamPackage param(binding);
    if (param.Get("engine", "") != "sdl") {
        return std::nullopt;
    }
    if (param.Has("button")) {
        const int btn = param.Get("button", -1);
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::Button && kXboxOptions[i].code == btn) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("axis")) {
        const int axis = param.Get("axis", -1);
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::AxisPositive &&
                kXboxOptions[i].code == axis) {
                return i;
            }
        }
        return std::nullopt;
    }
    if (param.Has("hat")) {
        const std::string dir = param.Get("direction", "");
        for (std::size_t i = 0; i < kXboxOptions.size(); ++i) {
            if (kXboxOptions[i].kind == XboxInputKind::Hat && dir == kXboxOptions[i].direction) {
                return i;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string BuildXboxBinding(const XboxOption& opt, const SdlDeviceIdentity& id,
                             std::size_t player_index) {
    if (opt.kind == XboxInputKind::Unbound) {
        return "[empty]";
    }
    Common::ParamPackage param;
    param.Set("engine", "sdl");
    param.Set("port", id.valid ? id.port : static_cast<int>(player_index));
    if (id.valid && !id.guid.empty()) {
        param.Set("guid", id.guid);
    }
    switch (opt.kind) {
    case XboxInputKind::Button:
        param.Set("button", opt.code);
        break;
    case XboxInputKind::AxisPositive:
        param.Set("axis", opt.code);
        param.Set("invert", "+");
        param.Set("threshold", "0.500000");
        break;
    case XboxInputKind::Hat:
        param.Set("hat", 0);
        param.Set("direction", opt.direction);
        break;
    case XboxInputKind::Unbound:
        return "[empty]";
    }
    return param.Serialize();
}

QString DescribeCustomBinding(const std::string& binding) {
    if (binding.empty() || binding == "[empty]") {
        return QObject::tr("Unbound (None)");
    }
    if (const auto idx = MatchXboxOption(binding); idx.has_value()) {
        return QString::fromLatin1(kXboxOptions[*idx].label);
    }
    Common::ParamPackage param(binding);
    const std::string engine = param.Get("engine", "");
    if (engine == "keyboard") {
        return QObject::tr("Keyboard (Key %1)").arg(param.Get("code", 0));
    }
    if (engine == "sdl") {
        if (param.Has("button")) {
            return QObject::tr("Controller Button %1").arg(param.Get("button", 0));
        }
        if (param.Has("axis")) {
            return QObject::tr("Controller Axis %1").arg(param.Get("axis", 0));
        }
    }
    return QObject::tr("Custom Binding");
}

std::string ConfigNameForGame(u64 program_id, const std::string& game_path) {
    if (program_id != 0) {
        return fmt::format("{:016X}", program_id);
    }
    return QFileInfo(QString::fromStdString(game_path)).fileName().toStdString();
}

bool ButtonsEqualGlobal(const Settings::PlayerInput& current, const Settings::PlayerInput& global) {
    for (const auto& spec : kSwitchButtons) {
        if (current.buttons[spec.id] != global.buttons[spec.id]) {
            return false;
        }
    }
    return true;
}

class ButtonRemapperDialog : public QDialog {
public:
    ButtonRemapperDialog(QWidget* parent, Core::System& system_,
                         InputCommon::InputSubsystem* input_subsystem_, u64 program_id_,
                         const QString& game_title, const std::string& game_path_)
        : QDialog(parent), system(system_), input_subsystem(input_subsystem_),
          program_id(program_id_), config_name(ConfigNameForGame(program_id_, game_path_)) {
        const QChar emdash(0x2014);
        setWindowTitle(tr("Button Layout %1 %2").arg(emdash).arg(game_title));
        setMinimumSize(640, 620);
        resize(680, 680);
        setStyleSheet(QStringLiteral(
            "QDialog { background: #141418; color: #ececf1; }"
            "QLabel { color: #ececf1; }"
            "QFrame#Card { background: #1b1b22; border: 1px solid #2b2b36; border-radius: 10px; }"
            "QComboBox { background: #23232e; color: #ececf1; border: 1px solid #353545; "
            "border-radius: 6px; padding: 5px 10px; min-height: 22px; }"
            "QComboBox::drop-down { border: none; width: 22px; }"
            "QComboBox QAbstractItemView { background: #1e1e27; color: #ececf1; "
            "selection-background-color: #7c3aed; border: 1px solid #353545; }"
            "QPushButton { background: #262633; color: #ececf1; border: 1px solid #3a3a4a; "
            "border-radius: 7px; padding: 6px 12px; font-weight: 500; }"
            "QPushButton:hover { background: #313142; border-color: #55556e; }"
            "QPushButton#PrimaryButton { background: #7c3aed; border-color: #8b5cf6; "
            "color: #ffffff; font-weight: 600; padding: 8px 18px; }"
            "QPushButton#PrimaryButton:hover { background: #8b5cf6; }"
            "QPushButton#PresetButton { background: #1f1f2b; border: 1px solid #3c3456; "
            "color: #d8b4fe; padding: 6px 11px; }"
            "QPushButton#PresetButton:hover { background: #2d2544; border-color: #7c3aed; }"
            "QPushButton#PressActive { background: #5b21b6; border-color: #a78bfa; color: #ffffff; }"));

        LoadState();
        BuildUi(game_title);
        RefreshPlayerRows();

        poll_timer = new QTimer(this);
        poll_timer->setInterval(25);
        connect(poll_timer, &QTimer::timeout, this, &ButtonRemapperDialog::PollNextInput);

        timeout_timer = new QTimer(this);
        timeout_timer->setSingleShot(true);
        connect(timeout_timer, &QTimer::timeout, this, [this] { CancelCapture(); });
    }

    ~ButtonRemapperDialog() override {
        CancelCapture();
    }

private:
    void LoadState() {
        // Snapshot global player configs first.
        global_players = Settings::values.players.GetValue(true);

        // Read existing per-game overrides (temporarily switches custom players view).
        const bool was_global = Settings::values.players.UsingGlobal();
        const auto saved_custom = Settings::values.players.GetValue(false);
        {
            QtConfig per_game_cfg(config_name, Config::ConfigType::PerGameConfig);
            working_players = Settings::values.players.GetValue(false);
        }
        // Restore Settings::values.players immediately so cancelling leaves live state untouched.
        Settings::values.players.GetValue(false) = saved_custom;
        Settings::values.players.SetGlobal(was_global);

        for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
            sdl_ids[p] = ExtractSdlIdentity(global_players[p], p);
            if (!sdl_ids[p].valid) {
                sdl_ids[p] = ExtractSdlIdentity(working_players[p], p);
            }
            // If this player has no per-game override yet, working_players[p] already equals
            // global_players[p] with empty profile_name.
            is_custom[p] = !working_players[p].profile_name.empty();
        }
    }

    void BuildUi(const QString& game_title) {
        const QChar arrows(0x2194);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 18, 22, 18);
        root->setSpacing(12);

        // Header
        auto* header_row = new QHBoxLayout;
        auto* title_col = new QVBoxLayout;
        title_col->setSpacing(2);
        auto* heading = new QLabel(game_title, this);
        heading->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #ececf1;"));
        heading->setWordWrap(true);
        title_col->addWidget(heading);

        auto* sub = new QLabel(
            tr("Remap what each button does for this game (works from Home and GameVault)."), this);
        sub->setStyleSheet(QStringLiteral("font-size: 12px; color: #9a9aa8;"));
        title_col->addWidget(sub);
        header_row->addLayout(title_col, 1);

        player_combo = new QComboBox(this);
        for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
            QString label = tr("Player %1").arg(p + 1);
            if (!global_players[p].profile_name.empty()) {
                label += QStringLiteral(" (%1)")
                             .arg(QString::fromStdString(global_players[p].profile_name));
            }
            player_combo->addItem(label, static_cast<int>(p));
        }
        connect(player_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
                    CancelCapture();
                    current_player = static_cast<std::size_t>(std::clamp(idx, 0, 3));
                    RefreshPlayerRows();
                });
        header_row->addWidget(player_combo);
        root->addLayout(header_row);

        // Status + Quick presets card
        auto* preset_card = new QFrame(this);
        preset_card->setObjectName(QStringLiteral("Card"));
        auto* preset_outer = new QVBoxLayout(preset_card);
        preset_outer->setContentsMargins(14, 10, 14, 10);
        preset_outer->setSpacing(8);

        status_label = new QLabel(preset_card);
        status_label->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600;"));
        preset_outer->addWidget(status_label);

        auto* preset_row = new QHBoxLayout;
        preset_row->setSpacing(8);

        auto* swap_xy = new QPushButton(tr("Swap X %1 Y").arg(arrows), preset_card);
        swap_xy->setObjectName(QStringLiteral("PresetButton"));
        swap_xy->setToolTip(
            tr("Swaps Switch X and Switch Y (e.g. moves Mario Kart Look Back from Xbox Y to Xbox X)."));
        connect(swap_xy, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        });
        preset_row->addWidget(swap_xy);

        auto* swap_ab = new QPushButton(tr("Swap A %1 B").arg(arrows), preset_card);
        swap_ab->setObjectName(QStringLiteral("PresetButton"));
        swap_ab->setToolTip(tr("Swaps Switch A and Switch B."));
        connect(swap_ab, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
        });
        preset_row->addWidget(swap_ab);

        auto* xbox_face = new QPushButton(tr("Swap All Face (A/B & X/Y)"), preset_card);
        xbox_face->setObjectName(QStringLiteral("PresetButton"));
        connect(xbox_face, &QPushButton::clicked, this, [this] {
            SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
            SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        });
        preset_row->addWidget(xbox_face);

        preset_row->addStretch(1);

        auto* reset_btn = new QPushButton(tr("Reset to Global"), preset_card);
        connect(reset_btn, &QPushButton::clicked, this, [this] { ResetCurrentPlayer(); });
        preset_row->addWidget(reset_btn);

        preset_outer->addLayout(preset_row);
        root->addWidget(preset_card);

        // Scrollable button list
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

        auto* list_host = new QWidget(scroll);
        list_host->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* list_layout = new QVBoxLayout(list_host);
        list_layout->setContentsMargins(0, 0, 6, 0);
        list_layout->setSpacing(6);

        row_combos.resize(kSwitchButtons.size(), nullptr);
        row_press_btns.resize(kSwitchButtons.size(), nullptr);

        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            const auto& spec = kSwitchButtons[i];
            auto* row_card = new QFrame(list_host);
            row_card->setObjectName(QStringLiteral("Card"));
            auto* row = new QHBoxLayout(row_card);
            row->setContentsMargins(12, 8, 12, 8);
            row->setSpacing(10);

            auto* left = new QVBoxLayout;
            left->setSpacing(1);
            auto* btn_name = new QLabel(QString::fromLatin1(spec.switch_name), row_card);
            btn_name->setStyleSheet(
                QStringLiteral("font-size: 13px; font-weight: 600; color: #ececf1;"));
            auto* hint = new QLabel(GameButtonHint(program_id, spec), row_card);
            hint->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
            left->addWidget(btn_name);
            left->addWidget(hint);
            row->addLayout(left, 1);

            auto* combo = new QComboBox(row_card);
            combo->setMinimumWidth(195);
            for (std::size_t opt_i = 0; opt_i < kXboxOptions.size(); ++opt_i) {
                combo->addItem(QString::fromLatin1(kXboxOptions[opt_i].label),
                               static_cast<int>(opt_i));
            }
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, i](int combo_idx) { OnComboChanged(i, combo_idx); });
            row->addWidget(combo);
            row_combos[i] = combo;

            auto* press_btn = new QPushButton(tr("Press..."), row_card);
            press_btn->setFixedWidth(92);
            press_btn->setToolTip(tr("Click, then press any button on your Xbox controller."));
            connect(press_btn, &QPushButton::clicked, this, [this, i] { BeginCapture(i); });
            row->addWidget(press_btn);
            row_press_btns[i] = press_btn;

            list_layout->addWidget(row_card);
        }
        list_layout->addStretch(1);
        scroll->setWidget(list_host);
        root->addWidget(scroll, 1);

        // Console Exit-Game Shortcut bar
        auto* exit_card = new QFrame(this);
        exit_card->setObjectName(QStringLiteral("Card"));
        auto* exit_row = new QHBoxLayout(exit_card);
        exit_row->setContentsMargins(14, 10, 14, 10);
        exit_row->setSpacing(10);

        auto* exit_info = new QVBoxLayout;
        exit_info->setSpacing(1);
        auto* exit_title = new QLabel(tr("Xbox Controller Exit-Game Shortcut"), exit_card);
        exit_title->setStyleSheet(
            QStringLiteral("font-size: 13px; font-weight: 600; color: #ececf1;"));
        auto* exit_desc = new QLabel(
            tr("Press this while playing to exit straight back to Genesis Home or GameVault."),
            exit_card);
        exit_desc->setStyleSheet(QStringLiteral("font-size: 11px; color: #9a9aa8;"));
        exit_info->addWidget(exit_title);
        exit_info->addWidget(exit_desc);
        exit_row->addLayout(exit_info, 1);

        exit_shortcut_combo = new QComboBox(exit_card);
        exit_shortcut_combo->setMinimumWidth(220);
        const auto current_exit = UISettings::values.controller_exit_shortcut.GetValue();
        int exit_idx = 0;
        const auto shortcuts = ControllerExitWatcher::AllShortcuts();
        for (int i = 0; i < static_cast<int>(shortcuts.size()); ++i) {
            exit_shortcut_combo->addItem(shortcuts[i].second, static_cast<int>(shortcuts[i].first));
            if (static_cast<u8>(shortcuts[i].first) == current_exit) {
                exit_idx = i;
            }
        }
        exit_shortcut_combo->setCurrentIndex(exit_idx);
        exit_row->addWidget(exit_shortcut_combo);
        root->addWidget(exit_card);

        // Dialog footer
        auto* footer = new QHBoxLayout;
        footer->addStretch(1);
        auto* cancel_btn = new QPushButton(tr("Cancel"), this);
        connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
        footer->addWidget(cancel_btn);

        auto* save_btn = new QPushButton(tr("Save Layout"), this);
        save_btn->setObjectName(QStringLiteral("PrimaryButton"));
        connect(save_btn, &QPushButton::clicked, this, &ButtonRemapperDialog::SaveAndAccept);
        footer->addWidget(save_btn);
        root->addLayout(footer);
    }

    void UpdateStatusBanner() {
        const QChar check(0x2713);
        const bool custom = !ButtonsEqualGlobal(working_players[current_player],
                                                global_players[current_player]);
        is_custom[current_player] = custom;
        if (custom) {
            status_label->setText(
                tr("%1 Custom per-game layout active for Player %2")
                    .arg(check)
                    .arg(current_player + 1));
            status_label->setStyleSheet(
                QStringLiteral("font-size: 12px; font-weight: 600; color: #6ee7b7;"));
        } else {
            status_label->setText(
                tr("Player %1 is currently using your global controller layout")
                    .arg(current_player + 1));
            status_label->setStyleSheet(
                QStringLiteral("font-size: 12px; font-weight: 500; color: #9a9aa8;"));
        }
    }

    void RefreshPlayerRows() {
        const auto& player = working_players[current_player];
        for (std::size_t i = 0; i < kSwitchButtons.size(); ++i) {
            auto* combo = row_combos[i];
            const QSignalBlocker blocker(combo);

            // Remove any temporary custom trailing item from a previous player/capture.
            while (combo->count() > static_cast<int>(kXboxOptions.size())) {
                combo->removeItem(combo->count() - 1);
            }

            const std::string& binding = player.buttons[kSwitchButtons[i].id];
            if (const auto opt = MatchXboxOption(binding); opt.has_value()) {
                combo->setCurrentIndex(static_cast<int>(*opt));
            } else {
                combo->addItem(DescribeCustomBinding(binding), -1);
                combo->setCurrentIndex(combo->count() - 1);
            }
        }
        UpdateStatusBanner();
    }

    void OnComboChanged(std::size_t row_index, int combo_idx) {
        if (row_index >= kSwitchButtons.size() || combo_idx < 0) {
            return;
        }
        auto* combo = row_combos[row_index];
        const int opt_idx = combo->itemData(combo_idx).toInt();
        if (opt_idx < 0 || static_cast<std::size_t>(opt_idx) >= kXboxOptions.size()) {
            return;
        }
        const auto& opt = kXboxOptions[static_cast<std::size_t>(opt_idx)];
        working_players[current_player].buttons[kSwitchButtons[row_index].id] =
            BuildXboxBinding(opt, sdl_ids[current_player], current_player);
        UpdateStatusBanner();
    }

    void SwapButtons(Settings::NativeButton::Values a, Settings::NativeButton::Values b) {
        CancelCapture();
        auto& buttons = working_players[current_player].buttons;
        std::swap(buttons[a], buttons[b]);
        RefreshPlayerRows();
    }

    void ResetCurrentPlayer() {
        CancelCapture();
        working_players[current_player] = global_players[current_player];
        working_players[current_player].profile_name.clear();
        RefreshPlayerRows();
    }

    void BeginCapture(std::size_t row_index) {
        if (!input_subsystem || row_index >= row_press_btns.size()) {
            return;
        }
        if (capturing_row.has_value() && *capturing_row == row_index) {
            CancelCapture();
            return;
        }
        CancelCapture();
        capturing_row = row_index;
        auto* btn = row_press_btns[row_index];
        btn->setText(tr("Waiting..."));
        btn->setObjectName(QStringLiteral("PressActive"));
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);

        input_subsystem->BeginMapping(InputCommon::Polling::InputType::Button);
        poll_timer->start();
        timeout_timer->start(4000);
    }

    void CancelCapture() {
        if (poll_timer) {
            poll_timer->stop();
        }
        if (timeout_timer) {
            timeout_timer->stop();
        }
        if (capturing_row.has_value()) {
            if (input_subsystem) {
                input_subsystem->StopMapping();
            }
            auto* btn = row_press_btns[*capturing_row];
            btn->setText(tr("Press..."));
            btn->setObjectName(QString());
            btn->style()->unpolish(btn);
            btn->style()->polish(btn);
            capturing_row.reset();
        }
    }

    void PollNextInput() {
        if (!input_subsystem || !capturing_row.has_value()) {
            CancelCapture();
            return;
        }
        const auto params = input_subsystem->GetNextInput();
        if (!params.Has("engine") || params.Get("engine", "") == "keyboard") {
            // Wait for controller press (or user cancels).
            if (params.Get("engine", "") == "keyboard" && params.Get("code", 0) == Qt::Key_Escape) {
                CancelCapture();
            }
            return;
        }
        const std::size_t row_index = *capturing_row;
        const std::string serialized = params.Serialize();
        CancelCapture();

        working_players[current_player].buttons[kSwitchButtons[row_index].id] = serialized;
        if (!sdl_ids[current_player].valid) {
            sdl_ids[current_player] = ExtractSdlIdentity(working_players[current_player],
                                                         current_player);
        }
        RefreshPlayerRows();
    }

    void SaveAndAccept() {
        CancelCapture();

        // Save global controller exit shortcut immediately.
        if (exit_shortcut_combo) {
            UISettings::values.controller_exit_shortcut.SetValue(
                static_cast<u8>(exit_shortcut_combo->currentData().toInt()));
        }

        const bool was_global = Settings::values.players.UsingGlobal();
        const auto saved_custom = Settings::values.players.GetValue(false);

        {
            QtConfig per_game_cfg(config_name, Config::ConfigType::PerGameConfig);
            Settings::values.players.SetGlobal(false);
            auto& custom_players = Settings::values.players.GetValue(false);
            for (std::size_t p = 0; p < NumRemappablePlayers; ++p) {
                const bool custom = !ButtonsEqualGlobal(working_players[p], global_players[p]);
                if (custom) {
                    custom_players[p] = working_players[p];
                    custom_players[p].profile_name = CustomPerGameLayoutProfile;
                    custom_players[p].connected = global_players[p].connected;
                } else if (custom_players[p].profile_name == CustomPerGameLayoutProfile ||
                           custom_players[p].profile_name.empty()) {
                    custom_players[p] = global_players[p];
                    custom_players[p].profile_name.clear();
                }
            }
            per_game_cfg.ClearControlPlayerValues();
            per_game_cfg.SaveAllValues();
        }

        // If this exact game is currently running, apply live; otherwise restore prior state.
        if (system.IsPoweredOn() && system.GetApplicationProcessProgramID() == program_id) {
            Settings::values.players.SetGlobal(false);
            system.HIDCore().ReloadInputDevices();
        } else {
            Settings::values.players.GetValue(false) = saved_custom;
            Settings::values.players.SetGlobal(was_global);
        }

        accept();
    }

    Core::System& system;
    InputCommon::InputSubsystem* input_subsystem = nullptr;
    u64 program_id = 0;
    std::string config_name;

    using PlayersList = std::remove_cvref_t<decltype(Settings::values.players.GetValue(true))>;
    PlayersList global_players{};
    PlayersList working_players{};
    std::array<SdlDeviceIdentity, NumRemappablePlayers> sdl_ids{};
    std::array<bool, NumRemappablePlayers> is_custom{};
    std::size_t current_player = 0;

    QComboBox* player_combo = nullptr;
    QLabel* status_label = nullptr;
    QComboBox* exit_shortcut_combo = nullptr;
    std::vector<QComboBox*> row_combos;
    std::vector<QPushButton*> row_press_btns;

    std::optional<std::size_t> capturing_row;
    QTimer* poll_timer = nullptr;
    QTimer* timeout_timer = nullptr;
};

} // namespace

bool ShowButtonRemapperDialog(QWidget* parent, Core::System& system,
                              InputCommon::InputSubsystem* input_subsystem, u64 program_id,
                              const QString& game_title, const std::string& game_path) {
    ButtonRemapperDialog dialog(parent, system, input_subsystem, program_id, game_title, game_path);
    return dialog.exec() == QDialog::Accepted;
}

} // namespace FritchiiUi
