/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "switcher-dock.hpp"

#include <plugin-support.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace streambeast {

namespace {

constexpr const char *kSaveKey = "streambeast_srt_switcher";
constexpr const char *kConfigFileName = "config.json";

struct SourceListItem {
	std::string name;
	bool isStreamBeast = false;
};

bool enumMediaSourcesProc(void *param, obs_source_t *source)
{
	auto *items = static_cast<std::vector<SourceListItem> *>(param);
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_CONTROLLABLE_MEDIA) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	if (!name || !*name)
		return true;

	bool isSb = false;
	obs_data_t *settings = obs_source_get_settings(source);
	if (settings) {
		const char *input = obs_data_get_string(settings, "input");
		if (input && isStreamBeastSrtUrl(input)) {
			isSb = true;
		}
		obs_data_release(settings);
	}

	items->push_back(SourceListItem{name, isSb});
	return true;
}

const char *mediaHealthText(MediaHealth health)
{
	switch (health) {
	case MediaHealth::Playing:
		return "Playing";
	case MediaHealth::Opening:
		return "Connecting / Buffering";
	case MediaHealth::Stalled:
		return "Stalled (frozen frames)";
	case MediaHealth::Error:
		return "Error";
	case MediaHealth::Offline:
		return "Offline / Stopped";
	case MediaHealth::Unknown:
	default:
		return "Unknown";
	}
}

QString badgeStyleFor(const std::string &badge)
{
	if (badge == "LIVE") {
		return QStringLiteral(
			"QLabel { background: #14532d; color: #53fc18; border: 1px solid #22c55e; "
			"border-radius: 4px; padding: 2px 8px; font-weight: 700; }");
	}
	if (badge == "DEGRADED" || badge == "DROPPING") {
		return QStringLiteral(
			"QLabel { background: #713f12; color: #fde047; border: 1px solid #eab308; "
			"border-radius: 4px; padding: 2px 8px; font-weight: 700; }");
	}
	if (badge == "RECOVERING") {
		return QStringLiteral(
			"QLabel { background: #1e3a8a; color: #93c5fd; border: 1px solid #3b82f6; "
			"border-radius: 4px; padding: 2px 8px; font-weight: 700; }");
	}
	if (badge == "OFFLINE") {
		return QStringLiteral(
			"QLabel { background: #7f1d1d; color: #fca5a5; border: 1px solid #ef4444; "
			"border-radius: 4px; padding: 2px 8px; font-weight: 700; }");
	}
	return QStringLiteral(
		"QLabel { background: #27272a; color: #d4d4d8; border: 1px solid #52525b; "
		"border-radius: 4px; padding: 2px 8px; font-weight: 700; }");
}

} // namespace

SwitcherDock::SwitcherDock(QWidget *parent)
	: QWidget(parent),
	  apiClient_(new StreamBeastApiClient(this)),
	  tickTimer_(new QTimer(this))
{
	buildUi();
	loadGlobalConfigFile();
	applyConfigToUi();

	connect(apiClient_, &StreamBeastApiClient::telemetryUpdated, this,
		&SwitcherDock::onTelemetryUpdated);

	tickTimer_->setInterval(500);
	connect(tickTimer_, &QTimer::timeout, this, &SwitcherDock::onTick);
}

SwitcherDock::~SwitcherDock()
{
	onObsExiting();
}

void SwitcherDock::buildUi()
{
	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(6, 6, 6, 6);
	rootLayout->setSpacing(6);

	auto *tabs = new QTabWidget(this);
	rootLayout->addWidget(tabs);

	buildMonitorTab(tabs);
	buildSettingsTab(tabs);
	connectUiSignals();
}

void SwitcherDock::buildMonitorTab(QTabWidget *tabs)
{
	auto *monitorScroll = new QScrollArea(tabs);
	monitorScroll->setWidgetResizable(true);
	monitorScroll->setFrameShape(QFrame::NoFrame);

	auto *monitorPage = new QWidget(monitorScroll);
	auto *monLayout = new QVBoxLayout(monitorPage);
	monLayout->setContentsMargins(6, 6, 6, 6);
	monLayout->setSpacing(8);

	// Status card
	auto *statusBox = new QGroupBox(QStringLiteral("SRT Feed Status"), monitorPage);
	auto *statusLayout = new QVBoxLayout(statusBox);
	statusLayout->setSpacing(5);

	auto *topRow = new QHBoxLayout();
	badgeLabel_ = new QLabel(QStringLiteral("STANDBY"), statusBox);
	badgeLabel_->setStyleSheet(badgeStyleFor("STANDBY"));
	badgeLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

	summaryLabel_ = new QLabel(QStringLiteral("Waiting for OBS to finish loading..."),
				   statusBox);
	summaryLabel_->setWordWrap(true);

	topRow->addWidget(badgeLabel_);
	topRow->addWidget(summaryLabel_, 1);
	statusLayout->addLayout(topRow);

	localStateLabel_ = new QLabel(QStringLiteral("OBS Source: —"), statusBox);
	localStateLabel_->setWordWrap(true);
	statusLayout->addWidget(localStateLabel_);

	telemetryLabel_ = new QLabel(QStringLiteral("StreamBeast API: —"), statusBox);
	telemetryLabel_->setWordWrap(true);
	statusLayout->addWidget(telemetryLabel_);

	tipLabel_ = new QLabel(statusBox);
	tipLabel_->setWordWrap(true);
	tipLabel_->setStyleSheet(QStringLiteral("QLabel { color: #facc15; font-size: 11px; }"));
	tipLabel_->setVisible(false);
	statusLayout->addWidget(tipLabel_);

	enableToggleBtn_ = new QPushButton(QStringLiteral("Auto-Switching: ON"), statusBox);
	enableToggleBtn_->setCheckable(true);
	enableToggleBtn_->setChecked(true);
	connect(enableToggleBtn_, &QPushButton::toggled, this, [this](bool checked) {
		if (updatingUi_)
			return;
		config_.enabled = checked;
		enableToggleBtn_->setText(checked ? QStringLiteral("Auto-Switching: ON")
						  : QStringLiteral("Auto-Switching: OFF"));
		engine_.reset();
		saveGlobalConfigFile();
		onTick();
	});
	statusLayout->addWidget(enableToggleBtn_);
	monLayout->addWidget(statusBox);

	// Routing & Sources card
	auto *routingBox = new QGroupBox(QStringLiteral("Source & Scene Mapping"), monitorPage);
	auto *routingForm = new QFormLayout(routingBox);
	routingForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	routingForm->setSpacing(6);

	auto *sourceRow = new QHBoxLayout();
	mediaSourceCombo_ = new QComboBox(routingBox);
	mediaSourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	auto *refreshBtn = new QPushButton(QStringLiteral("Refresh"), routingBox);
	refreshBtn->setToolTip(QStringLiteral("Rescan OBS Media Sources and Scenes"));
	connect(refreshBtn, &QPushButton::clicked, this,
		&SwitcherDock::refreshSceneAndSourceLists);
	sourceRow->addWidget(mediaSourceCombo_, 1);
	sourceRow->addWidget(refreshBtn);
	routingForm->addRow(QStringLiteral("SRT Media Source:"), sourceRow);

	liveSceneCombo_ = new QComboBox(routingBox);
	fallbackSceneCombo_ = new QComboBox(routingBox);
	lowBitrateSceneCombo_ = new QComboBox(routingBox);
	lowBitrateSceneCombo_->setToolTip(
		QStringLiteral("Optional scene shown when StreamBeast API reports low bitrate"));

	routingForm->addRow(QStringLiteral("Live Scene:"), liveSceneCombo_);
	routingForm->addRow(QStringLiteral("Fallback / BRB:"), fallbackSceneCombo_);
	routingForm->addRow(QStringLiteral("Low-Bitrate Scene:"), lowBitrateSceneCombo_);
	monLayout->addWidget(routingBox);

	// Manual actions
	auto *actionsRow = new QHBoxLayout();
	auto *restartBtn = new QPushButton(QStringLiteral("Restart SRT"), monitorPage);
	auto *liveBtn = new QPushButton(QStringLiteral("Go Live"), monitorPage);
	auto *fallbackBtn = new QPushButton(QStringLiteral("Go Fallback"), monitorPage);
	connect(restartBtn, &QPushButton::clicked, this, &SwitcherDock::onRestartSourceClicked);
	connect(liveBtn, &QPushButton::clicked, this, &SwitcherDock::onManualLiveClicked);
	connect(fallbackBtn, &QPushButton::clicked, this, &SwitcherDock::onManualFallbackClicked);
	actionsRow->addWidget(restartBtn);
	actionsRow->addWidget(liveBtn);
	actionsRow->addWidget(fallbackBtn);
	monLayout->addLayout(actionsRow);

	auto *bridgeNote = new QLabel(
		QStringLiteral("Built exclusively for StreamBeast OBS Bridge (streambeast.app)"),
		monitorPage);
	bridgeNote->setWordWrap(true);
	bridgeNote->setStyleSheet(QStringLiteral("QLabel { color: #9ca3af; font-size: 11px; }"));
	monLayout->addWidget(bridgeNote);
	monLayout->addStretch(1);

	monitorScroll->setWidget(monitorPage);
	tabs->addTab(monitorScroll, QStringLiteral("Monitor"));
}

void SwitcherDock::buildSettingsTab(QTabWidget *tabs)
{
	auto *settingsScroll = new QScrollArea(tabs);
	settingsScroll->setWidgetResizable(true);
	settingsScroll->setFrameShape(QFrame::NoFrame);

	auto *settingsPage = new QWidget(settingsScroll);
	auto *setLayout = new QVBoxLayout(settingsPage);
	setLayout->setContentsMargins(6, 6, 6, 6);
	setLayout->setSpacing(8);

	// Timings
	auto *timingBox = new QGroupBox(QStringLiteral("Detection Timings"), settingsPage);
	auto *timingForm = new QFormLayout(timingBox);

	dropDelaySpin_ = new QDoubleSpinBox(timingBox);
	dropDelaySpin_->setRange(0.5, 60.0);
	dropDelaySpin_->setSingleStep(0.5);
	dropDelaySpin_->setSuffix(QStringLiteral(" s"));

	recoverDelaySpin_ = new QDoubleSpinBox(timingBox);
	recoverDelaySpin_->setRange(0.5, 60.0);
	recoverDelaySpin_->setSingleStep(0.5);
	recoverDelaySpin_->setSuffix(QStringLiteral(" s"));

	stallTimeoutSpin_ = new QDoubleSpinBox(timingBox);
	stallTimeoutSpin_->setRange(1.0, 30.0);
	stallTimeoutSpin_->setSingleStep(0.5);
	stallTimeoutSpin_->setSuffix(QStringLiteral(" s"));

	restartCooldownSpin_ = new QDoubleSpinBox(timingBox);
	restartCooldownSpin_->setRange(2.0, 60.0);
	restartCooldownSpin_->setSingleStep(1.0);
	restartCooldownSpin_->setSuffix(QStringLiteral(" s"));

	timingForm->addRow(QStringLiteral("Drop Delay:"), dropDelaySpin_);
	timingForm->addRow(QStringLiteral("Recover Delay:"), recoverDelaySpin_);
	timingForm->addRow(QStringLiteral("Frame Stall Timeout:"), stallTimeoutSpin_);
	timingForm->addRow(QStringLiteral("Restart Cooldown:"), restartCooldownSpin_);
	setLayout->addWidget(timingBox);

	// Low-bitrate settings
	auto *lowBrBox = new QGroupBox(QStringLiteral("Low-Bitrate Detection (API)"), settingsPage);
	auto *lowBrForm = new QFormLayout(lowBrBox);

	lowBitrateThresholdSpin_ = new QSpinBox(lowBrBox);
	lowBitrateThresholdSpin_->setRange(100, 15000);
	lowBitrateThresholdSpin_->setSingleStep(100);
	lowBitrateThresholdSpin_->setSuffix(QStringLiteral(" kbps"));

	lowBitrateDelaySpin_ = new QDoubleSpinBox(lowBrBox);
	lowBitrateDelaySpin_->setRange(1.0, 60.0);
	lowBitrateDelaySpin_->setSingleStep(0.5);
	lowBitrateDelaySpin_->setSuffix(QStringLiteral(" s"));

	lowBitrateHysteresisSpin_ = new QSpinBox(lowBrBox);
	lowBitrateHysteresisSpin_->setRange(50, 5000);
	lowBitrateHysteresisSpin_->setSingleStep(50);
	lowBitrateHysteresisSpin_->setSuffix(QStringLiteral(" kbps"));

	lowBrForm->addRow(QStringLiteral("Trigger Below:"), lowBitrateThresholdSpin_);
	lowBrForm->addRow(QStringLiteral("Hold Before Switch:"), lowBitrateDelaySpin_);
	lowBrForm->addRow(QStringLiteral("Recovery Buffer:"), lowBitrateHysteresisSpin_);
	setLayout->addWidget(lowBrBox);

	// StreamBeast API settings
	auto *apiBox =
		new QGroupBox(QStringLiteral("StreamBeast OBS Bridge Telemetry"), settingsPage);
	auto *apiLayout = new QVBoxLayout(apiBox);

	useApiCheck_ =
		new QCheckBox(QStringLiteral("Enable StreamBeast API hybrid monitoring"), apiBox);
	apiLayout->addWidget(useApiCheck_);

	auto *apiForm = new QFormLayout();
	apiUrlEdit_ = new QLineEdit(apiBox);
	apiUrlEdit_->setPlaceholderText(QStringLiteral("https://streambeast.app"));

	auto *keyRow = new QHBoxLayout();
	streamKeyEdit_ = new QLineEdit(apiBox);
	streamKeyEdit_->setEchoMode(QLineEdit::Password);
	streamKeyEdit_->setPlaceholderText(
		QStringLiteral("Auto-detected from StreamBeast SRT Source"));
	showKeyBtn_ = new QPushButton(QStringLiteral("Show"), apiBox);
	showKeyBtn_->setCheckable(true);
	connect(showKeyBtn_, &QPushButton::toggled, this, [this](bool shown) {
		streamKeyEdit_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		showKeyBtn_->setText(shown ? QStringLiteral("Hide") : QStringLiteral("Show"));
	});
	keyRow->addWidget(streamKeyEdit_, 1);
	keyRow->addWidget(showKeyBtn_);

	apiForm->addRow(QStringLiteral("API URL:"), apiUrlEdit_);
	apiForm->addRow(QStringLiteral("Stream Key:"), keyRow);
	apiLayout->addLayout(apiForm);

	auto *testRow = new QHBoxLayout();
	testApiBtn_ = new QPushButton(QStringLiteral("Test Connection"), apiBox);
	apiStatusLabel_ = new QLabel(QStringLiteral("Not tested"), apiBox);
	apiStatusLabel_->setWordWrap(true);
	connect(testApiBtn_, &QPushButton::clicked, this, &SwitcherDock::onTestApiClicked);
	testRow->addWidget(testApiBtn_);
	testRow->addWidget(apiStatusLabel_, 1);
	apiLayout->addLayout(testRow);
	setLayout->addWidget(apiBox);

	// Safety behaviors
	auto *safetyBox = new QGroupBox(QStringLiteral("Switching Behavior"), settingsPage);
	auto *safetyLayout = new QVBoxLayout(safetyBox);

	onlyStreamingCheck_ = new QCheckBox(
		QStringLiteral("Only auto-switch while Streaming or Recording"), safetyBox);
	onlyManagedScenesCheck_ = new QCheckBox(
		QStringLiteral("Only switch when on Live / Fallback / Low-Bitrate scenes"),
		safetyBox);
	autoReturnCheck_ = new QCheckBox(
		QStringLiteral("Auto-return to Live scene when SRT recovers"), safetyBox);
	autoRestartCheck_ = new QCheckBox(
		QStringLiteral("Auto-restart stuck OBS Media Source"), safetyBox);

	safetyLayout->addWidget(onlyStreamingCheck_);
	safetyLayout->addWidget(onlyManagedScenesCheck_);
	safetyLayout->addWidget(autoReturnCheck_);
	safetyLayout->addWidget(autoRestartCheck_);
	setLayout->addWidget(safetyBox);
	setLayout->addStretch(1);

	settingsScroll->setWidget(settingsPage);
	tabs->addTab(settingsScroll, QStringLiteral("Settings"));
}

void SwitcherDock::connectUiSignals()
{
	connect(mediaSourceCombo_, &QComboBox::currentTextChanged, this,
		&SwitcherDock::onUiConfigChanged);
	connect(liveSceneCombo_, &QComboBox::currentTextChanged, this,
		&SwitcherDock::onUiConfigChanged);
	connect(fallbackSceneCombo_, &QComboBox::currentTextChanged, this,
		&SwitcherDock::onUiConfigChanged);
	connect(lowBitrateSceneCombo_, &QComboBox::currentTextChanged, this,
		&SwitcherDock::onUiConfigChanged);

	connect(dropDelaySpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);
	connect(recoverDelaySpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);
	connect(stallTimeoutSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);
	connect(restartCooldownSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);

	connect(lowBitrateThresholdSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);
	connect(lowBitrateDelaySpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);
	connect(lowBitrateHysteresisSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
		&SwitcherDock::onUiConfigChanged);

	connect(useApiCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
	connect(apiUrlEdit_, &QLineEdit::editingFinished, this, &SwitcherDock::onUiConfigChanged);
	connect(streamKeyEdit_, &QLineEdit::editingFinished, this, &SwitcherDock::onUiConfigChanged);
	connect(onlyStreamingCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
	connect(onlyManagedScenesCheck_, &QCheckBox::toggled, this,
		&SwitcherDock::onUiConfigChanged);
	connect(autoReturnCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
	connect(autoRestartCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
}

void SwitcherDock::onObsFinishedLoading()
{
	obsReady_ = true;
	obsExiting_ = false;
	refreshSceneAndSourceLists();
	if (!tickTimer_->isActive()) {
		tickTimer_->start();
	}
	onTick();
}

void SwitcherDock::onObsExiting()
{
	obsExiting_ = true;
	obsReady_ = false;
	if (tickTimer_->isActive()) {
		tickTimer_->stop();
	}
	apiClient_->stop();
}

void SwitcherDock::refreshSceneAndSourceLists()
{
	if (obsExiting_)
		return;

	updatingUi_ = true;
	{
		QSignalBlocker b1(mediaSourceCombo_);
		QSignalBlocker b2(liveSceneCombo_);
		QSignalBlocker b3(fallbackSceneCombo_);
		QSignalBlocker b4(lowBitrateSceneCombo_);

		std::vector<SourceListItem> sources;
		obs_enum_sources(enumMediaSourcesProc, &sources);
		std::sort(sources.begin(), sources.end(),
			  [](const SourceListItem &a, const SourceListItem &b) {
				  return a.name < b.name;
			  });

		mediaSourceCombo_->clear();
		mediaSourceCombo_->addItem(QStringLiteral("(None)"), QString());

		std::string firstStreamBeastSource;
		for (const auto &item : sources) {
			QString qName = QString::fromStdString(item.name);
			QString label = item.isStreamBeast
						? (qName + QStringLiteral(" [StreamBeast SRT]"))
						: qName;
			mediaSourceCombo_->addItem(label, qName);
			if (item.isStreamBeast && firstStreamBeastSource.empty()) {
				firstStreamBeastSource = item.name;
			}
		}

		if (config_.mediaSourceName.empty() && !firstStreamBeastSource.empty()) {
			config_.mediaSourceName = firstStreamBeastSource;
		}

		int srcIdx = mediaSourceCombo_->findData(
			QString::fromStdString(config_.mediaSourceName));
		if (srcIdx < 0 && !config_.mediaSourceName.empty()) {
			QString qSaved = QString::fromStdString(config_.mediaSourceName);
			mediaSourceCombo_->addItem(qSaved + QStringLiteral(" (Missing)"), qSaved);
			srcIdx = mediaSourceCombo_->count() - 1;
		}
		mediaSourceCombo_->setCurrentIndex(std::max(0, srcIdx));

		// Scenes
		liveSceneCombo_->clear();
		fallbackSceneCombo_->clear();
		lowBitrateSceneCombo_->clear();

		liveSceneCombo_->addItem(QStringLiteral("(Select Scene)"), QString());
		fallbackSceneCombo_->addItem(QStringLiteral("(Select Scene)"), QString());
		lowBitrateSceneCombo_->addItem(QStringLiteral("(Disabled)"), QString());

		char **sceneNames = obs_frontend_get_scene_names();
		if (sceneNames) {
			for (size_t i = 0; sceneNames[i] != nullptr; ++i) {
				QString qScene = QString::fromUtf8(sceneNames[i]);
				liveSceneCombo_->addItem(qScene, qScene);
				fallbackSceneCombo_->addItem(qScene, qScene);
				lowBitrateSceneCombo_->addItem(qScene, qScene);
			}
			bfree(sceneNames);
		}

		auto selectScene = [](QComboBox *combo, const std::string &target) {
			if (target.empty()) {
				combo->setCurrentIndex(0);
				return;
			}
			int idx = combo->findData(QString::fromStdString(target));
			if (idx < 0) {
				QString qTarget = QString::fromStdString(target);
				combo->addItem(qTarget + QStringLiteral(" (Missing)"), qTarget);
				idx = combo->count() - 1;
			}
			combo->setCurrentIndex(idx);
		};

		selectScene(liveSceneCombo_, config_.liveSceneName);
		selectScene(fallbackSceneCombo_, config_.fallbackSceneName);
		selectScene(lowBitrateSceneCombo_, config_.lowBitrateSceneName);
	}
	updatingUi_ = false;
}

void SwitcherDock::applyConfigToUi()
{
	updatingUi_ = true;
	{
		QSignalBlocker bToggle(enableToggleBtn_);
		enableToggleBtn_->setChecked(config_.enabled);
		enableToggleBtn_->setText(config_.enabled
						  ? QStringLiteral("Auto-Switching: ON")
						  : QStringLiteral("Auto-Switching: OFF"));

		dropDelaySpin_->setValue(config_.dropDelayMs / 1000.0);
		recoverDelaySpin_->setValue(config_.recoverDelayMs / 1000.0);
		stallTimeoutSpin_->setValue(config_.mediaStallTimeoutMs / 1000.0);
		restartCooldownSpin_->setValue(config_.mediaRestartCooldownMs / 1000.0);

		lowBitrateThresholdSpin_->setValue(config_.lowBitrateKbps);
		lowBitrateDelaySpin_->setValue(config_.lowBitrateDelayMs / 1000.0);
		lowBitrateHysteresisSpin_->setValue(config_.lowBitrateRecoverHysteresisKbps);

		useApiCheck_->setChecked(config_.useStreamBeastApi);
		apiUrlEdit_->setText(QString::fromStdString(config_.apiBaseUrl));
		streamKeyEdit_->setText(QString::fromStdString(config_.streamKeyOverride));

		onlyStreamingCheck_->setChecked(config_.onlyWhenStreamingOrRecording);
		onlyManagedScenesCheck_->setChecked(config_.onlySwitchManagedScenes);
		autoReturnCheck_->setChecked(config_.autoReturnToLive);
		autoRestartCheck_->setChecked(config_.autoRestartMediaSource);
	}
	updatingUi_ = false;
	refreshSceneAndSourceLists();
}

void SwitcherDock::readConfigFromUi()
{
	config_.enabled = enableToggleBtn_->isChecked();
	config_.mediaSourceName = mediaSourceCombo_->currentData().toString().toStdString();
	config_.liveSceneName = liveSceneCombo_->currentData().toString().toStdString();
	config_.fallbackSceneName = fallbackSceneCombo_->currentData().toString().toStdString();
	config_.lowBitrateSceneName =
		lowBitrateSceneCombo_->currentData().toString().toStdString();

	config_.dropDelayMs = static_cast<int>(std::lround(dropDelaySpin_->value() * 1000.0));
	config_.recoverDelayMs =
		static_cast<int>(std::lround(recoverDelaySpin_->value() * 1000.0));
	config_.mediaStallTimeoutMs =
		static_cast<int>(std::lround(stallTimeoutSpin_->value() * 1000.0));
	config_.mediaRestartCooldownMs =
		static_cast<int>(std::lround(restartCooldownSpin_->value() * 1000.0));

	config_.lowBitrateKbps = lowBitrateThresholdSpin_->value();
	config_.lowBitrateDelayMs =
		static_cast<int>(std::lround(lowBitrateDelaySpin_->value() * 1000.0));
	config_.lowBitrateRecoverHysteresisKbps = lowBitrateHysteresisSpin_->value();

	config_.useStreamBeastApi = useApiCheck_->isChecked();
	config_.apiBaseUrl = normalizeApiBaseUrl(apiUrlEdit_->text().toStdString());
	config_.streamKeyOverride = extractStreamBeastKey(streamKeyEdit_->text().toStdString());

	config_.onlyWhenStreamingOrRecording = onlyStreamingCheck_->isChecked();
	config_.onlySwitchManagedScenes = onlyManagedScenesCheck_->isChecked();
	config_.autoReturnToLive = autoReturnCheck_->isChecked();
	config_.autoRestartMediaSource = autoRestartCheck_->isChecked();
}

void SwitcherDock::onUiConfigChanged()
{
	if (updatingUi_)
		return;
	readConfigFromUi();
	engine_.reset();
	saveGlobalConfigFile();
	if (obsReady_) {
		onTick();
	}
}

void SwitcherDock::writeConfigToObsData(obs_data_t *data) const
{
	obs_data_set_bool(data, "enabled", config_.enabled);
	obs_data_set_bool(data, "only_streaming", config_.onlyWhenStreamingOrRecording);
	obs_data_set_bool(data, "only_managed_scenes", config_.onlySwitchManagedScenes);
	obs_data_set_bool(data, "auto_return", config_.autoReturnToLive);
	obs_data_set_bool(data, "auto_restart", config_.autoRestartMediaSource);
	obs_data_set_bool(data, "use_api", config_.useStreamBeastApi);

	obs_data_set_string(data, "api_base_url", config_.apiBaseUrl.c_str());
	obs_data_set_string(data, "stream_key_override", config_.streamKeyOverride.c_str());
	obs_data_set_string(data, "media_source", config_.mediaSourceName.c_str());
	obs_data_set_string(data, "live_scene", config_.liveSceneName.c_str());
	obs_data_set_string(data, "fallback_scene", config_.fallbackSceneName.c_str());
	obs_data_set_string(data, "low_bitrate_scene", config_.lowBitrateSceneName.c_str());

	obs_data_set_int(data, "drop_delay_ms", config_.dropDelayMs);
	obs_data_set_int(data, "recover_delay_ms", config_.recoverDelayMs);
	obs_data_set_int(data, "stall_timeout_ms", config_.mediaStallTimeoutMs);
	obs_data_set_int(data, "restart_cooldown_ms", config_.mediaRestartCooldownMs);
	obs_data_set_int(data, "low_bitrate_kbps", config_.lowBitrateKbps);
	obs_data_set_int(data, "low_bitrate_delay_ms", config_.lowBitrateDelayMs);
	obs_data_set_int(data, "low_bitrate_hysteresis_kbps",
			 config_.lowBitrateRecoverHysteresisKbps);
}

void SwitcherDock::applyObsDataToConfig(obs_data_t *data)
{
	auto getStr = [data](const char *key) -> std::string {
		const char *val = obs_data_get_string(data, key);
		return val ? std::string(val) : std::string{};
	};

	if (obs_data_has_user_value(data, "enabled"))
		config_.enabled = obs_data_get_bool(data, "enabled");
	if (obs_data_has_user_value(data, "only_streaming"))
		config_.onlyWhenStreamingOrRecording = obs_data_get_bool(data, "only_streaming");
	if (obs_data_has_user_value(data, "only_managed_scenes"))
		config_.onlySwitchManagedScenes = obs_data_get_bool(data, "only_managed_scenes");
	if (obs_data_has_user_value(data, "auto_return"))
		config_.autoReturnToLive = obs_data_get_bool(data, "auto_return");
	if (obs_data_has_user_value(data, "auto_restart"))
		config_.autoRestartMediaSource = obs_data_get_bool(data, "auto_restart");
	if (obs_data_has_user_value(data, "use_api"))
		config_.useStreamBeastApi = obs_data_get_bool(data, "use_api");

	if (obs_data_has_user_value(data, "api_base_url"))
		config_.apiBaseUrl = normalizeApiBaseUrl(getStr("api_base_url"));
	if (obs_data_has_user_value(data, "stream_key_override"))
		config_.streamKeyOverride = getStr("stream_key_override");
	if (obs_data_has_user_value(data, "media_source"))
		config_.mediaSourceName = getStr("media_source");
	if (obs_data_has_user_value(data, "live_scene"))
		config_.liveSceneName = getStr("live_scene");
	if (obs_data_has_user_value(data, "fallback_scene"))
		config_.fallbackSceneName = getStr("fallback_scene");
	if (obs_data_has_user_value(data, "low_bitrate_scene"))
		config_.lowBitrateSceneName = getStr("low_bitrate_scene");

	if (obs_data_has_user_value(data, "drop_delay_ms"))
		config_.dropDelayMs = static_cast<int>(obs_data_get_int(data, "drop_delay_ms"));
	if (obs_data_has_user_value(data, "recover_delay_ms"))
		config_.recoverDelayMs =
			static_cast<int>(obs_data_get_int(data, "recover_delay_ms"));
	if (obs_data_has_user_value(data, "stall_timeout_ms"))
		config_.mediaStallTimeoutMs =
			static_cast<int>(obs_data_get_int(data, "stall_timeout_ms"));
	if (obs_data_has_user_value(data, "restart_cooldown_ms"))
		config_.mediaRestartCooldownMs =
			static_cast<int>(obs_data_get_int(data, "restart_cooldown_ms"));
	if (obs_data_has_user_value(data, "low_bitrate_kbps"))
		config_.lowBitrateKbps =
			static_cast<int>(obs_data_get_int(data, "low_bitrate_kbps"));
	if (obs_data_has_user_value(data, "low_bitrate_delay_ms"))
		config_.lowBitrateDelayMs =
			static_cast<int>(obs_data_get_int(data, "low_bitrate_delay_ms"));
	if (obs_data_has_user_value(data, "low_bitrate_hysteresis_kbps"))
		config_.lowBitrateRecoverHysteresisKbps =
			static_cast<int>(obs_data_get_int(data, "low_bitrate_hysteresis_kbps"));
}

void SwitcherDock::saveGlobalConfigFile() const
{
	char *path = obs_module_config_path(kConfigFileName);
	if (!path)
		return;

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	writeConfigToObsData(data);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

void SwitcherDock::loadGlobalConfigFile()
{
	char *path = obs_module_config_path(kConfigFileName);
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (data) {
		applyObsDataToConfig(data);
		obs_data_release(data);
	}
	bfree(path);
}

void SwitcherDock::saveConfig(obs_data_t *saveData, bool saving)
{
	if (!saveData)
		return;

	if (saving) {
		readConfigFromUi();
		obs_data_t *obj = obs_data_create();
		writeConfigToObsData(obj);
		obs_data_set_obj(saveData, kSaveKey, obj);
		obs_data_release(obj);
		saveGlobalConfigFile();
	} else {
		config_ = SwitcherConfig{};
		loadGlobalConfigFile();
		obs_data_t *obj = obs_data_get_obj(saveData, kSaveKey);
		if (obj) {
			applyObsDataToConfig(obj);
			obs_data_release(obj);
		}
		engine_.reset();
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		applyConfigToUi();
	}
}

MediaHealth SwitcherDock::sampleLocalMediaHealth(int64_t nowMs, bool &outFound,
						 bool &outCloseWhenInactive,
						 std::string &outExtractedKey)
{
	outFound = false;
	outCloseWhenInactive = false;
	outExtractedKey.clear();

	if (config_.mediaSourceName.empty()) {
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		return MediaHealth::Unknown;
	}

	obs_source_t *source = obs_get_source_by_name(config_.mediaSourceName.c_str());
	if (!source) {
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		return MediaHealth::Unknown;
	}

	outFound = true;
	obs_data_t *settings = obs_source_get_settings(source);
	if (settings) {
		const char *input = obs_data_get_string(settings, "input");
		if (input) {
			outExtractedKey = extractStreamBeastKey(input);
		}
		outCloseWhenInactive = obs_data_get_bool(settings, "close_when_inactive");
		obs_data_release(settings);
	}

	const enum obs_media_state state = obs_source_media_get_state(source);
	const int64_t mediaTime = obs_source_media_get_time(source);
	obs_source_release(source);

	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		if (lastMediaAdvanceMs_ < 0 || mediaTime != lastMediaTime_) {
			lastMediaTime_ = mediaTime;
			lastMediaAdvanceMs_ = nowMs;
			return MediaHealth::Playing;
		}
		if ((nowMs - lastMediaAdvanceMs_) >= config_.mediaStallTimeoutMs) {
			return MediaHealth::Stalled;
		}
		return MediaHealth::Playing;

	case OBS_MEDIA_STATE_OPENING:
	case OBS_MEDIA_STATE_BUFFERING:
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		return MediaHealth::Opening;

	case OBS_MEDIA_STATE_ERROR:
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		return MediaHealth::Error;

	case OBS_MEDIA_STATE_PAUSED:
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ENDED:
	case OBS_MEDIA_STATE_NONE:
	default:
		lastMediaTime_ = -1;
		lastMediaAdvanceMs_ = -1;
		return MediaHealth::Offline;
	}
}

std::string SwitcherDock::currentObsSceneName() const
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return {};
	const char *name = obs_source_get_name(scene);
	std::string result = name ? std::string(name) : std::string{};
	obs_source_release(scene);
	return result;
}

bool SwitcherDock::isObsStreamingOrRecording() const
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active();
}

void SwitcherDock::switchObsScene(const std::string &sceneName)
{
	if (sceneName.empty() || obsExiting_)
		return;

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	obs_source_t *target = nullptr;
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *s = scenes.sources.array[i];
		const char *name = obs_source_get_name(s);
		if (name && sceneName == name) {
			target = s;
			break;
		}
	}

	if (target) {
		obs_log(LOG_INFO, "Switching scene to '%s'", sceneName.c_str());
		obs_frontend_set_current_scene(target);
	} else {
		obs_log(LOG_WARNING, "Target scene '%s' not found", sceneName.c_str());
	}

	obs_frontend_source_list_free(&scenes);
}

void SwitcherDock::restartObsMediaSource()
{
	if (config_.mediaSourceName.empty() || obsExiting_)
		return;

	obs_source_t *source = obs_get_source_by_name(config_.mediaSourceName.c_str());
	if (!source)
		return;

	obs_log(LOG_INFO, "Restarting SRT Media Source '%s'", config_.mediaSourceName.c_str());
	obs_source_media_restart(source);
	obs_source_release(source);

	lastMediaTime_ = -1;
	lastMediaAdvanceMs_ = -1;
}

int SwitcherDock::maxApiAgeMs() const
{
	return std::max(10000, config_.apiPollIntervalMs * 4);
}

void SwitcherDock::reconfigureApiClient(const std::string &effectiveKey)
{
	if (!obsReady_ || obsExiting_)
		return;

	apiClient_->configure(config_.useStreamBeastApi,
			      QString::fromStdString(config_.apiBaseUrl),
			      QString::fromStdString(effectiveKey), config_.apiPollIntervalMs);
}

void SwitcherDock::onTick()
{
	if (!obsReady_ || obsExiting_)
		return;

	const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();

	bool mediaFound = false;
	bool closeWhenInactive = false;
	std::string extractedKey;
	const MediaHealth localHealth =
		sampleLocalMediaHealth(nowMs, mediaFound, closeWhenInactive, extractedKey);

	lastAutoDetectedKey_ = extractedKey;
	const std::string effectiveKey =
		!config_.streamKeyOverride.empty() ? config_.streamKeyOverride : extractedKey;

	reconfigureApiClient(effectiveKey);

	const ApiTelemetrySnapshot &apiSnap = apiClient_->latestSnapshot();
	const bool apiFresh =
		apiClient_->isConfigured() && apiClient_->isFresh(nowMs, maxApiAgeMs());

	SignalSnapshot snap;
	snap.nowMs = nowMs;
	snap.isStreamingOrRecording = isObsStreamingOrRecording();
	snap.currentSceneName = currentObsSceneName();
	snap.mediaSourceFound = mediaFound;
	snap.localMediaHealth = localHealth;
	snap.apiConfigured = apiClient_->isConfigured();
	snap.apiReachable = apiFresh;
	snap.apiIsLive = apiSnap.isLive;
	snap.apiBitrateKbps = apiSnap.bitrateKbps;
	snap.apiQuality = apiSnap.quality.toStdString();

	const SwitcherDecision decision = engine_.evaluate(snap, config_);

	if (decision.requestMediaRestart) {
		restartObsMediaSource();
	}

	if (!decision.switchToSceneName.empty() &&
	    decision.switchToSceneName != snap.currentSceneName) {
		switchObsScene(decision.switchToSceneName);
	}

	if (decision.statusSummary != lastLogSummary_) {
		lastLogSummary_ = decision.statusSummary;
		obs_log(LOG_INFO, "Status [%s]: %s", decision.healthBadge.c_str(),
			decision.statusSummary.c_str());
	}

	updateStatusUi(decision, localHealth, mediaFound, closeWhenInactive, effectiveKey);
}

void SwitcherDock::updateStatusUi(const SwitcherDecision &decision, MediaHealth localHealth,
				  bool mediaFound, bool closeWhenInactive,
				  const std::string &effectiveKey)
{
	badgeLabel_->setText(QString::fromStdString(decision.healthBadge));
	badgeLabel_->setStyleSheet(badgeStyleFor(decision.healthBadge));
	summaryLabel_->setText(QString::fromStdString(decision.statusSummary));

	if (config_.mediaSourceName.empty()) {
		localStateLabel_->setText(QStringLiteral("OBS Source: Not selected"));
	} else if (!mediaFound) {
		localStateLabel_->setText(
			QStringLiteral("OBS Source: '%1' not found")
				.arg(QString::fromStdString(config_.mediaSourceName)));
	} else {
		localStateLabel_->setText(
			QStringLiteral("OBS Source: %1 (%2)")
				.arg(QString::fromStdString(config_.mediaSourceName),
				     QString::fromUtf8(mediaHealthText(localHealth))));
	}

	const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
	const ApiTelemetrySnapshot &apiSnap = apiClient_->latestSnapshot();

	if (!config_.useStreamBeastApi) {
		telemetryLabel_->setText(
			QStringLiteral("StreamBeast API: Disabled (local OBS mode)"));
	} else if (effectiveKey.empty()) {
		telemetryLabel_->setText(
			QStringLiteral("StreamBeast API: Waiting for StreamBeast SRT key"));
	} else if (!apiClient_->isFresh(nowMs, maxApiAgeMs())) {
		if (!apiSnap.errorMessage.isEmpty()) {
			telemetryLabel_->setText(
				QStringLiteral("StreamBeast API: %1").arg(apiSnap.errorMessage));
		} else {
			telemetryLabel_->setText(QStringLiteral("StreamBeast API: Polling..."));
		}
	} else if (!apiSnap.isLive) {
		telemetryLabel_->setText(
			QStringLiteral("StreamBeast API: Offline (standby)"));
	} else {
		QString rateText = apiSnap.bitrateKbps > 0
					   ? QStringLiteral("%1 kbps").arg(apiSnap.bitrateKbps)
					   : QStringLiteral("Measuring");
		QString meta;
		if (!apiSnap.resolution.isEmpty()) {
			meta += QStringLiteral(" · %1").arg(apiSnap.resolution);
			if (apiSnap.fps > 0) {
				meta += QStringLiteral(" @ %1fps")
						.arg(QString::number(apiSnap.fps, 'g', 4));
			}
		}
		if (!apiSnap.quality.isEmpty()) {
			meta += QStringLiteral(" [%1]").arg(apiSnap.quality);
		}
		telemetryLabel_->setText(
			QStringLiteral("StreamBeast API: Live · %1%2").arg(rateText, meta));
	}

	if (mediaFound && closeWhenInactive && !config_.useStreamBeastApi) {
		tipLabel_->setText(QStringLiteral(
			"Tip: Uncheck 'Close file when inactive' in your Media Source "
			"or enable StreamBeast API so recovery can be detected while on Fallback."));
		tipLabel_->setVisible(true);
	} else if (mediaFound && effectiveKey.empty() && config_.useStreamBeastApi) {
		tipLabel_->setText(QStringLiteral(
			"No StreamBeast OBS Bridge key detected. This plugin only works with "
			"the OBS Bridge from streambeast.app (or paste your key in Settings)."));
		tipLabel_->setVisible(true);
	} else {
		tipLabel_->setVisible(false);
	}
}

void SwitcherDock::onTelemetryUpdated(const ApiTelemetrySnapshot &snapshot, bool fromManualTest)
{
	if (fromManualTest) {
		testApiBtn_->setEnabled(true);
		if (snapshot.reachable) {
			apiStatusLabel_->setText(
				snapshot.isLive
					? QStringLiteral("Connected (Feed LIVE · %1 kbps)")
						  .arg(std::max(0, snapshot.bitrateKbps))
					: QStringLiteral("Connected (Feed currently offline)"));
		} else {
			apiStatusLabel_->setText(
				QStringLiteral("Failed: %1 (HTTP %2)")
					.arg(snapshot.errorMessage.isEmpty()
						     ? QStringLiteral("Network error")
						     : snapshot.errorMessage)
					.arg(snapshot.httpStatus));
		}
	}

	if (obsReady_ && !obsExiting_) {
		onTick();
	}
}

void SwitcherDock::onTestApiClicked()
{
	readConfigFromUi();
	const std::string effectiveKey =
		!config_.streamKeyOverride.empty() ? config_.streamKeyOverride
						   : lastAutoDetectedKey_;
	if (effectiveKey.empty()) {
		apiStatusLabel_->setText(QStringLiteral(
			"No key found. Select your StreamBeast Media Source or paste a Stream Key."));
		return;
	}

	testApiBtn_->setEnabled(false);
	apiStatusLabel_->setText(QStringLiteral("Testing..."));
	apiClient_->configure(config_.useStreamBeastApi,
			      QString::fromStdString(config_.apiBaseUrl),
			      QString::fromStdString(effectiveKey), config_.apiPollIntervalMs);
	apiClient_->pollNow(true);
}

void SwitcherDock::onRestartSourceClicked()
{
	readConfigFromUi();
	restartObsMediaSource();
}

void SwitcherDock::onManualLiveClicked()
{
	readConfigFromUi();
	switchObsScene(config_.liveSceneName);
	engine_.reset();
	onTick();
}

void SwitcherDock::onManualFallbackClicked()
{
	readConfigFromUi();
	switchObsScene(config_.fallbackSceneName);
	engine_.reset();
	onTick();
}

} // namespace streambeast
