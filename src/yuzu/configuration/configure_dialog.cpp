// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <QBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_debug_tab.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/configuration/configure_filesystem.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_hotkeys.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_profile_manager.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/configure_ui.h"
#include "yuzu/configuration/configure_web.h"
#include "yuzu/hotkeys.h"
#include "yuzu/fritchii/fritchii_ui.h"

ConfigureDialog::ConfigureDialog(QWidget* parent, HotkeyRegistry& registry_,
                                 InputCommon::InputSubsystem* input_subsystem_,
                                 std::vector<VkDeviceInfo::Record>& vk_device_records,
                                 Core::System& system_, bool enable_web_config)
    : QDialog(parent), ui{std::make_unique<Ui::ConfigureDialog>()}, registry(registry_),
      input_subsystem{input_subsystem_},
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      applets_tab{std::make_unique<ConfigureApplets>(system_, nullptr, *builder, this)},
      audio_tab{std::make_unique<ConfigureAudio>(system_, nullptr, *builder, this)},
      cpu_tab{std::make_unique<ConfigureCpu>(system_, nullptr, *builder, this)},
      debug_tab_tab{std::make_unique<ConfigureDebugTab>(system_, this)},
      filesystem_tab{std::make_unique<ConfigureFilesystem>(this)},
      general_tab{std::make_unique<ConfigureGeneral>(system_, nullptr, *builder, this)},
      graphics_advanced_tab{
          std::make_unique<ConfigureGraphicsAdvanced>(system_, nullptr, *builder, this)},
      graphics_extensions_tab{
          std::make_unique<ConfigureGraphicsExtensions>(system_, nullptr, *builder, this)},
      ui_tab{std::make_unique<ConfigureUi>(system_, this)},
      graphics_tab{std::make_unique<ConfigureGraphics>(
          system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
          [this](Settings::AspectRatio ratio, Settings::ResolutionSetup setup) {
              ui_tab->UpdateScreenshotInfo(ratio, setup);
          },
          nullptr, *builder, this)},
      hotkeys_tab{std::make_unique<ConfigureHotkeys>(system_.HIDCore(), this)},
      network_tab{std::make_unique<ConfigureNetwork>(system_, this)},
      profile_tab{std::make_unique<ConfigureProfileManager>(system_, this)},
      system_tab{std::make_unique<ConfigureSystem>(system_, nullptr, *builder, this)},
      web_tab{std::make_unique<ConfigureWeb>(this)} {
    Settings::SetConfiguringGlobal(true);

    ui->setupUi(this);

    // Replace the single clear-and-rebuild QTabWidget with a QStackedWidget holding one cached
    // QTabWidget per sidebar category so switching categories is instant (<1 ms).
    ui->tabWidget->hide();
    category_stack = new QStackedWidget(this);
    ui->horizontalLayout->addWidget(category_stack, 1);

    // Hide eager tabs until their category page is lazily shown.
    for (QWidget* w : std::initializer_list<QWidget*>{
             applets_tab.get(), audio_tab.get(), cpu_tab.get(), debug_tab_tab.get(),
             filesystem_tab.get(), general_tab.get(), graphics_tab.get(),
             graphics_advanced_tab.get(), graphics_extensions_tab.get(), hotkeys_tab.get(),
             profile_tab.get(), network_tab.get(), system_tab.get(), ui_tab.get(), web_tab.get()}) {
        w->hide();
    }

    web_tab->SetWebServiceConfigEnabled(enable_web_config);
    hotkeys_tab->Populate(registry);

    general_tab->SetResetCallback([&] { this->close(); });

    SetConfiguration();
    PopulateSelectionList();

    connect(ui_tab.get(), &ConfigureUi::LanguageChanged, this, &ConfigureDialog::OnLanguageChanged);
    connect(general_tab.get(), &ConfigureGeneral::ExternalContentDirsChanged, this,
            &ConfigureDialog::ExternalContentDirsChanged);
    connect(ui->selectorList, &QListWidget::itemSelectionChanged, this,
            &ConfigureDialog::UpdateVisibleTabs);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigureDialog::HandleApplyButtonClicked);
    }

    adjustSize();
    ui->selectorList->setCurrentRow(0);
    if (FritchiiUi::Enabled()) {
        // After the first group is shown: its pages sit in scroll areas by then, so the window
        // can take a normal size.
        setWindowTitle(tr("Settings"));
        ui->label->setText(tr("Some settings can't change while a game runs."));
        ui->label->setVisible(system.IsPoweredOn());
        ui->selectorList->setFixedWidth(180);
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        setMinimumSize(640, 460);
        // Wide enough for the Controls page's button columns and controller preview, but never
        // bigger than the screen it opens on (2026-11-16: it opened taller than a 1080p screen):
        // after the first layout pass, fit it into 90% x 85% of that screen and center it on
        // the main window.
        resize(1080, 700);
        QTimer::singleShot(0, this, [this] {
            QScreen* const screen = parentWidget() != nullptr && parentWidget()->screen() != nullptr
                                        ? parentWidget()->screen()
                                        : this->screen();
            if (screen == nullptr) {
                return;
            }
            const QRect area = screen->availableGeometry();
            const int w = std::min({1080, area.width() * 9 / 10, std::max(this->width(), 640)});
            const int h = std::min({760, area.height() * 85 / 100, std::max(this->height(), 460)});
            resize(w, h);
            const QRect anchor =
                parentWidget() != nullptr ? parentWidget()->frameGeometry() : area;
            QPoint top_left = anchor.center() - QPoint(w / 2, h / 2);
            top_left.setX(std::clamp(top_left.x(), area.left(), area.right() - w));
            top_left.setY(std::clamp(top_left.y(), area.top(), area.bottom() - h));
            move(top_left);
        });

        // OoT's own values (Configure Game) beat these, the usual "my settings didn't apply".
        if (const QStringList overrides = FritchiiUi::OcarinaGameOverrides();
            !overrides.isEmpty()) {
            auto* const banner = new QWidget(this);
            auto* const row = new QHBoxLayout(banner);
            row->setContentsMargins(0, 0, 0, 8);
            auto* const text = new QLabel(
                tr("Ocarina of Time keeps its own value for %n of these settings, so changes "
                   "here don't reach it.",
                   nullptr, static_cast<int>(overrides.size())),
                banner);
            text->setWordWrap(true);
            text->setToolTip(overrides.join(QLatin1Char('\n')));
            auto* const use_global = new QPushButton(tr("Use these for OoT too"), banner);
            row->addWidget(text, 1);
            row->addWidget(use_global);
            if (auto* const main = qobject_cast<QBoxLayout*>(layout())) {
                main->insertWidget(0, banner);
            }
            connect(use_global, &QPushButton::clicked, this, [banner] {
                if (FritchiiUi::ClearOcarinaGameOverrides()) {
                    banner->hide();
                }
            });
        }
    }

    // Selects the leftmost button on the bottom bar (Cancel as of writing)
    ui->buttonBox->setFocus();
}

ConfigureDialog::~ConfigureDialog() = default;

void ConfigureDialog::SetConfiguration() {}

void ConfigureDialog::ApplyConfiguration() {
    general_tab->ApplyConfiguration();
    ui_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    profile_tab->ApplyConfiguration();
    filesystem_tab->ApplyConfiguration();
    if (input_initialized && input_tab) {
        input_tab->ApplyConfiguration();
    }
    hotkeys_tab->ApplyConfiguration(registry);
    cpu_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    graphics_extensions_tab->ApplyConfiguration();
    audio_tab->ApplyConfiguration();
    debug_tab_tab->ApplyConfiguration();
    web_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();
    system.ApplySettings();
    Settings::LogSettings();
}

void ConfigureDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureDialog::RetranslateUI() {
    const int old_row = ui->selectorList->currentRow();
    int old_index = -1;
    if (old_row >= 0 && old_row < static_cast<int>(category_tab_widgets.size()) &&
        category_tab_widgets[old_row] != nullptr) {
        old_index = category_tab_widgets[old_row]->currentIndex();
    }

    ui->retranslateUi(this);

    PopulateSelectionList();
    for (int cat = 0; cat < static_cast<int>(category_tab_widgets.size()); ++cat) {
        auto* tw = category_tab_widgets[cat];
        if (!tw) {
            continue;
        }
        const auto tabs = GetCategorySubTabs(cat);
        for (int i = 0; i < tw->count() && i < tabs.size(); ++i) {
            tw->setTabText(i, tabs[i]->accessibleName());
        }
    }

    ui->selectorList->setCurrentRow(old_row);
    UpdateVisibleTabs();
    if (old_row >= 0 && old_row < static_cast<int>(category_tab_widgets.size()) &&
        category_tab_widgets[old_row] != nullptr && old_index >= 0) {
        category_tab_widgets[old_row]->setCurrentIndex(old_index);
    }
}

void ConfigureDialog::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigureDialog::EnsureInputInitialized() {
    if (input_initialized) {
        return;
    }
    input_tab = std::make_unique<ConfigureInput>(system, this);
    input_tab->hide();
    input_tab->Initialize(input_subsystem);
    input_initialized = true;
}

QList<QWidget*> ConfigureDialog::GetCategorySubTabs(int category_index) {
    if (FritchiiUi::Enabled()) {
        switch (category_index) {
        case 0:
            return {general_tab.get(), ui_tab.get(), hotkeys_tab.get()};
        case 1:
            return {graphics_tab.get(), graphics_advanced_tab.get(), graphics_extensions_tab.get()};
        case 2:
            EnsureInputInitialized();
            return input_tab->GetSubTabs();
        case 3:
            return {audio_tab.get()};
        case 4:
            return {system_tab.get(), profile_tab.get()};
        case 5:
            return {cpu_tab.get(), network_tab.get(), filesystem_tab.get(), applets_tab.get(),
                    web_tab.get(), debug_tab_tab.get()};
        default:
            return {};
        }
    }

    switch (category_index) {
    case 0:
        return {general_tab.get(), hotkeys_tab.get(), ui_tab.get(), web_tab.get(),
                debug_tab_tab.get()};
    case 1:
        return {system_tab.get(), profile_tab.get(), network_tab.get(), filesystem_tab.get(),
                applets_tab.get()};
    case 2:
        return {cpu_tab.get()};
    case 3:
        return {graphics_tab.get(), graphics_advanced_tab.get(), graphics_extensions_tab.get()};
    case 4:
        return {audio_tab.get()};
    case 5:
        EnsureInputInitialized();
        return input_tab->GetSubTabs();
    default:
        return {};
    }
}

void ConfigureDialog::PopulateSelectionList() {
    const std::array<QString, 6> classic_labels{
        tr("General"), tr("System"), tr("CPU"), tr("Graphics"), tr("Audio"), tr("Controls"),
    };
    const std::array<QString, 6> modern_labels{
        tr("General"), tr("Graphics"), tr("Controls"), tr("Audio"), tr("System"), tr("Advanced"),
    };
    if (FritchiiUi::Enabled()) {
        ui_tab->setAccessibleName(tr("Interface"));
        hotkeys_tab->setAccessibleName(tr("Shortcuts"));
        profile_tab->setAccessibleName(tr("Users"));
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->selectorList);

    ui->selectorList->clear();
    const auto& labels = FritchiiUi::Enabled() ? modern_labels : classic_labels;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        auto* const item = new QListWidgetItem(labels[i]);
        item->setData(Qt::UserRole, i);
        ui->selectorList->addItem(item);
    }
}

void ConfigureDialog::OnLanguageChanged(const QString& locale) {
    emit LanguageChanged(locale);
    //  Reloading the game list is needed to force retranslation.
    UISettings::values.is_game_list_reload_pending = true;
    // first apply the configuration, and then restore the display
    ApplyConfiguration();
    RetranslateUI();
    SetConfiguration();
}

void ConfigureDialog::UpdateVisibleTabs() {
    const auto items = ui->selectorList->selectedItems();
    if (items.isEmpty()) {
        return;
    }

    const int cat = items[0]->data(Qt::UserRole).toInt();
    if (cat < 0 || cat >= static_cast<int>(category_tab_widgets.size())) {
        return;
    }

    QTabWidget* tw = category_tab_widgets[cat];
    if (tw == nullptr) {
        category_stack->setUpdatesEnabled(false);
        tw = new QTabWidget(category_stack);
        tw->setUpdatesEnabled(false);
        const auto tabs = GetCategorySubTabs(cat);
        for (auto* const tab : tabs) {
            LOG_DEBUG(Frontend, "{}", tab->accessibleName().toStdString());
            QWidget* page = FritchiiUi::SettingsPage(tab);
            tab->show();
            tw->addTab(page, tab->accessibleName());
        }
        connect(tw, &QTabWidget::currentChanged, this, [this](int index) {
            if (index != -1) {
                debug_tab_tab->SetCurrentIndex(0);
            }
        });
        tw->setUpdatesEnabled(true);
        category_stack->addWidget(tw);
        category_tab_widgets[cat] = tw;
        category_stack->setUpdatesEnabled(true);
    }

    category_stack->setCurrentWidget(tw);
}
