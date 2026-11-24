// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "core/core.h"
#include "frontend_common/config.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "ui_configure_input_per_game.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/input_profiles.h"
#include "yuzu/fritchii/button_remapper.h"

ConfigureInputPerGame::ConfigureInputPerGame(Core::System& system_, QtConfig* config_,
                                             QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInputPerGame>()),
      profiles(std::make_unique<InputProfiles>()), system{system_}, config{config_} {
    ui->setupUi(this);
    const std::array labels = {
        ui->label_player_1, ui->label_player_2, ui->label_player_3, ui->label_player_4,
        ui->label_player_5, ui->label_player_6, ui->label_player_7, ui->label_player_8,
    };
    profile_comboboxes = {
        ui->profile_player_1, ui->profile_player_2, ui->profile_player_3, ui->profile_player_4,
        ui->profile_player_5, ui->profile_player_6, ui->profile_player_7, ui->profile_player_8,
    };

    Settings::values.players.SetGlobal(false);

    const auto& profile_names = profiles->GetInputProfileNames();
    const auto populate_profiles = [this, &profile_names](size_t player_index) {
        const auto previous_profile =
            Settings::values.players.GetValue()[player_index].profile_name;

        auto* const player_combobox = profile_comboboxes[player_index];
        player_combobox->addItem(tr("Use global input configuration"), QString());

        const bool has_custom_layout =
            previous_profile == FritchiiUi::CustomPerGameLayoutProfile;
        if (has_custom_layout) {
            player_combobox->addItem(
                tr("Custom button layout (this game)"),
                QString::fromLatin1(FritchiiUi::CustomPerGameLayoutProfile));
            player_combobox->setCurrentIndex(1);
        }

        for (size_t index = 0; index < profile_names.size(); ++index) {
            const auto& profile_name = profile_names[index];
            const QString qname = QString::fromStdString(profile_name);
            player_combobox->addItem(qname, qname);
            if (!has_custom_layout && profile_name == previous_profile) {
                player_combobox->setCurrentIndex(player_combobox->count() - 1);
            }
        }
    };
    for (size_t index = 0; index < profile_comboboxes.size(); ++index) {
        labels[index]->setText(tr("Player %1 profile").arg(index + 1));
        populate_profiles(index);
    }

    LoadConfiguration();
}

void ConfigureInputPerGame::ApplyConfiguration() {
    LoadConfiguration();
    SaveConfiguration();
}

void ConfigureInputPerGame::LoadConfiguration() {
    static constexpr size_t HANDHELD_INDEX = 8;

    auto& hid_core = system.HIDCore();
    for (size_t player_index = 0; player_index < profile_comboboxes.size(); ++player_index) {
        Settings::values.players.SetGlobal(false);

        auto* emulated_controller = hid_core.GetEmulatedControllerByIndex(player_index);
        auto* const player_combobox = profile_comboboxes[player_index];

        const auto selection_index = player_combobox->currentIndex();
        if (selection_index == 0) {
            Settings::values.players.GetValue()[player_index].profile_name = "";
            if (player_index == 0) {
                Settings::values.players.GetValue()[HANDHELD_INDEX] = {};
            }
            Settings::values.players.SetGlobal(true);
            emulated_controller->ReloadFromSettings();
            continue;
        }
        const auto profile_name =
            player_combobox->itemData(selection_index).toString().toStdString();
        if (profile_name.empty()) {
            continue;
        }
        auto& player = Settings::values.players.GetValue()[player_index];
        if (profile_name == FritchiiUi::CustomPerGameLayoutProfile) {
            // Preserve the custom per-game button bindings already loaded for this player.
            player.profile_name = profile_name;
            emulated_controller->ReloadFromSettings();
            continue;
        }
        player.profile_name = profile_name;
        // Read from the profile into the custom player settings
        profiles->LoadProfile(profile_name, player_index);
        // Make sure the controller is connected
        player.connected = true;

        emulated_controller->ReloadFromSettings();

        if (player_index > 0) {
            continue;
        }
        // Handle Handheld cases
        auto& handheld_player = Settings::values.players.GetValue()[HANDHELD_INDEX];
        auto* handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        if (player.controller_type == Settings::ControllerType::Handheld) {
            handheld_player = player;
        } else {
            handheld_player = {};
        }
        handheld_controller->ReloadFromSettings();
    }
}

void ConfigureInputPerGame::SaveConfiguration() {
    Settings::values.players.SetGlobal(false);

    // Clear all controls from the config in case the user reverted back to globals
    config->ClearControlPlayerValues();
    for (size_t index = 0; index < Settings::values.players.GetValue().size(); ++index) {
        config->SaveQtControlPlayerValues(index);
    }
}
