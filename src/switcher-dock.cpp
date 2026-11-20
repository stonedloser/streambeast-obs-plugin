/*
StreamBeast IRL
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
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
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
	  obsRemoteClient_(new ObsRemoteClient(this)),
	  tickTimer_(new QTimer(this)),
	  cpuUsageInfo_(os_cpu_usage_info_start())
{
	buildUi();
	loadGlobalConfigFile();
	applyConfigToUi();

	connect(apiClient_, &StreamBeastApiClient::telemetryUpdated, this,
		&SwitcherDock::onTelemetryUpdated);
	connect(obsRemoteClient_, &ObsRemoteClient::connectionChanged, this,
		&SwitcherDock::onObsRemoteConnectionChanged);
	connect(obsRemoteClient_, &ObsRemoteClient::tokenTested, this,
		&SwitcherDock::onObsRemoteTokenTested);
	connect(obsRemoteClient_, &ObsRemoteClient::commandReceived, this,
		&SwitcherDock::onObsRemoteCommandReceived);
	connect(obsRemoteClient_, &ObsRemoteClient::configReloadRequested, this, [this]() {
		if (obsReady_ && !obsExiting_) {
			pushObsRemoteState(QDateTime::currentMSecsSinceEpoch());
		}
	});

	tickTimer_->setInterval(500);
	connect(tickTimer_, &QTimer::timeout, this, &SwitcherDock::onTick);
}

SwitcherDock::~SwitcherDock()
{
	onObsExiting();
	if (cpuUsageInfo_) {
		os_cpu_usage_info_destroy(cpuUsageInfo_);
		cpuUsageInfo_ = nullptr;
	}
}

void SwitcherDock::buildUi()
{
	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(6, 6, 6, 6);
	rootLayout->setSpacing(6);

	auto *tabs = new QTabWidget(this);
	rootLayout->addWidget(tabs);

	buildAutoSceneTab(tabs);
	buildClipsTab(tabs);
	buildObsRemoteTab(tabs);
	connectUiSignals();
}

void SwitcherDock::buildAutoSceneTab(QTabWidget *tabs)
{
	auto *autoSceneScroll = new QScrollArea(tabs);
	autoSceneScroll->setWidgetResizable(true);
	autoSceneScroll->setFrameShape(QFrame::NoFrame);

	auto *autoScenePage = new QWidget(autoSceneScroll);
	auto *monLayout = new QVBoxLayout(autoScenePage);
	monLayout->setContentsMargins(6, 6, 6, 6);
	monLayout->setSpacing(8);

	// Status card
	auto *statusBox = new QGroupBox(QStringLiteral("SRT Feed Status"), autoScenePage);
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

	clipMonitorLabel_ = new QLabel(statusBox);
	clipMonitorLabel_->setWordWrap(true);
	clipMonitorLabel_->setVisible(false);
	statusLayout->addWidget(clipMonitorLabel_);

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
	auto *routingBox = new QGroupBox(QStringLiteral("Source & Scene Mapping"), autoScenePage);
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
	auto *restartBtn = new QPushButton(QStringLiteral("Restart SRT"), autoScenePage);
	auto *liveBtn = new QPushButton(QStringLiteral("Go Live"), autoScenePage);
	auto *fallbackBtn = new QPushButton(QStringLiteral("Go Fallback"), autoScenePage);
	connect(restartBtn, &QPushButton::clicked, this, &SwitcherDock::onRestartSourceClicked);
	connect(liveBtn, &QPushButton::clicked, this, &SwitcherDock::onManualLiveClicked);
	connect(fallbackBtn, &QPushButton::clicked, this, &SwitcherDock::onManualFallbackClicked);
	actionsRow->addWidget(restartBtn);
	actionsRow->addWidget(liveBtn);
	actionsRow->addWidget(fallbackBtn);
	monLayout->addLayout(actionsRow);

	buildAutoSceneSettings(autoScenePage, monLayout);

	auto *bridgeNote = new QLabel(
		QStringLiteral("Built exclusively for StreamBeast OBS Bridge (streambeast.app)"),
		autoScenePage);
	bridgeNote->setWordWrap(true);
	bridgeNote->setStyleSheet(QStringLiteral("QLabel { color: #9ca3af; font-size: 11px; }"));
	monLayout->addWidget(bridgeNote);
	monLayout->addStretch(1);

	autoSceneScroll->setWidget(autoScenePage);
	tabs->addTab(autoSceneScroll, QStringLiteral("Auto Scene"));
}

void SwitcherDock::buildClipsTab(QTabWidget *tabs)
{
	auto *clipsScroll = new QScrollArea(tabs);
	clipsScroll->setWidgetResizable(true);
	clipsScroll->setFrameShape(QFrame::NoFrame);

	auto *clipsPage = new QWidget(clipsScroll);
	auto *clipsLayout = new QVBoxLayout(clipsPage);
	clipsLayout->setContentsMargins(6, 6, 6, 6);
	clipsLayout->setSpacing(8);

	// Setup instructions at the top
	auto *setupGuideBox = new QGroupBox(QStringLiteral("Setup"), clipsPage);
	auto *setupGuideLayout = new QVBoxLayout(setupGuideBox);
	auto *clipsHint = new QLabel(
		QStringLiteral("1. Select a Media Source on your Fallback / BRB scene (or click "
			       "Create to add one automatically).\n"
			       "2. Browse for your folder of clips (.mp4, .mkv, .mov, .webm, "
			       ".flv, .ts, .avi, etc.).\n"
			       "3. Turn Random Clips ON below to play clips when disconnected."),
		setupGuideBox);
	clipsHint->setWordWrap(true);
	clipsHint->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; }"));
	setupGuideLayout->addWidget(clipsHint);
	clipsLayout->addWidget(setupGuideBox);

	// Status & toggle card
	auto *playerBox = new QGroupBox(QStringLiteral("Random Clip Player"), clipsPage);
	auto *playerLayout = new QVBoxLayout(playerBox);
	playerLayout->setSpacing(6);

	clipEnabledToggleBtn_ = new QPushButton(QStringLiteral("Random Clips: OFF"), playerBox);
	clipEnabledToggleBtn_->setCheckable(true);
	clipEnabledToggleBtn_->setChecked(false);
	connect(clipEnabledToggleBtn_, &QPushButton::toggled, this, [this](bool checked) {
		if (updatingUi_)
			return;
		config_.clipPlayerEnabled = checked;
		clipEnabledToggleBtn_->setText(checked ? QStringLiteral("Random Clips: ON")
						       : QStringLiteral("Random Clips: OFF"));
		saveGlobalConfigFile();
		if (obsReady_) {
			onTick();
		} else {
			updateClipStatusUi();
		}
	});
	playerLayout->addWidget(clipEnabledToggleBtn_);

	clipStatusLabel_ = new QLabel(QStringLiteral("Status: Disabled"), playerBox);
	clipStatusLabel_->setWordWrap(true);
	playerLayout->addWidget(clipStatusLabel_);

	auto *clipActionsRow = new QHBoxLayout();
	nextClipBtn_ = new QPushButton(QStringLiteral("Play Next Clip"), playerBox);
	rescanClipsBtn_ = new QPushButton(QStringLiteral("Rescan Folder"), playerBox);
	connect(nextClipBtn_, &QPushButton::clicked, this, &SwitcherDock::onNextClipClicked);
	connect(rescanClipsBtn_, &QPushButton::clicked, this, &SwitcherDock::onRescanClipsClicked);
	clipActionsRow->addWidget(nextClipBtn_);
	clipActionsRow->addWidget(rescanClipsBtn_);
	playerLayout->addLayout(clipActionsRow);
	clipsLayout->addWidget(playerBox);

	// Source & folder card
	auto *setupBox = new QGroupBox(QStringLiteral("Clip Source & Directory"), clipsPage);
	auto *setupForm = new QFormLayout(setupBox);
	setupForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	setupForm->setSpacing(6);

	auto *clipSourceRow = new QHBoxLayout();
	clipSourceCombo_ = new QComboBox(setupBox);
	clipSourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	clipSourceCombo_->setToolTip(
		QStringLiteral("OBS Media Source in your Fallback / BRB scene that plays clips"));
	createClipSourceBtn_ = new QPushButton(QStringLiteral("Create"), setupBox);
	createClipSourceBtn_->setToolTip(QStringLiteral(
		"Create a new Media Source for Random Clips and add it to your Fallback scene"));
	connect(createClipSourceBtn_, &QPushButton::clicked, this,
		&SwitcherDock::onCreateClipSourceClicked);
	clipSourceRow->addWidget(clipSourceCombo_, 1);
	clipSourceRow->addWidget(createClipSourceBtn_);
	setupForm->addRow(QStringLiteral("Clip Media Source:"), clipSourceRow);

	auto *dirRow = new QHBoxLayout();
	clipDirEdit_ = new QLineEdit(setupBox);
	clipDirEdit_->setPlaceholderText(QStringLiteral("Select folder containing video clips"));
	browseClipDirBtn_ = new QPushButton(QStringLiteral("Browse..."), setupBox);
	connect(browseClipDirBtn_, &QPushButton::clicked, this,
		&SwitcherDock::onBrowseClipDirClicked);
	dirRow->addWidget(clipDirEdit_, 1);
	dirRow->addWidget(browseClipDirBtn_);
	setupForm->addRow(QStringLiteral("Clips Folder:"), dirRow);
	clipsLayout->addWidget(setupBox);

	// Playback options card
	auto *optsBox = new QGroupBox(QStringLiteral("Playback Options"), clipsPage);
	auto *optsLayout = new QVBoxLayout(optsBox);

	clipShuffleCheck_ =
		new QCheckBox(QStringLiteral("Shuffle all clips before repeating"), optsBox);
	clipSubfoldersCheck_ = new QCheckBox(QStringLiteral("Include subfolders"), optsBox);
	clipOnlyFallbackCheck_ = new QCheckBox(
		QStringLiteral("Only play while on Fallback / BRB scene"), optsBox);

	optsLayout->addWidget(clipShuffleCheck_);
	optsLayout->addWidget(clipSubfoldersCheck_);
	optsLayout->addWidget(clipOnlyFallbackCheck_);
	clipsLayout->addWidget(optsBox);
	clipsLayout->addStretch(1);

	clipsScroll->setWidget(clipsPage);
	tabs->addTab(clipsScroll, QStringLiteral("Random Clips"));
}

void SwitcherDock::buildAutoSceneSettings(QWidget *settingsPage, QVBoxLayout *setLayout)
{
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
}

void SwitcherDock::buildObsRemoteTab(QTabWidget *tabs)
{
	auto *remoteScroll = new QScrollArea(tabs);
	remoteScroll->setWidgetResizable(true);
	remoteScroll->setFrameShape(QFrame::NoFrame);

	auto *remotePage = new QWidget(remoteScroll);
	auto *remoteLayout = new QVBoxLayout(remotePage);
	remoteLayout->setContentsMargins(6, 6, 6, 6);
	remoteLayout->setSpacing(8);

	// Setup instructions at top
	auto *setupGuideBox = new QGroupBox(QStringLiteral("Setup"), remotePage);
	auto *setupGuideLayout = new QVBoxLayout(setupGuideBox);
	auto *guideLabel = new QLabel(
		QStringLiteral("1. Copy your OBS Browser Source Overlay URL from "
			       "streambeast.app/tools/obs-remote.\n"
			       "2. Paste the Overlay URL below.\n"
			       "3. Turn OBS Remote ON — no Browser Source or WebSocket password "
			       "needed."),
		setupGuideBox);
	guideLabel->setWordWrap(true);
	guideLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; }"));
	setupGuideLayout->addWidget(guideLabel);
	remoteLayout->addWidget(setupGuideBox);

	// Bridge status & toggle card
	auto *bridgeBox = new QGroupBox(QStringLiteral("OBS Remote Bridge"), remotePage);
	auto *bridgeLayout = new QVBoxLayout(bridgeBox);
	bridgeLayout->setSpacing(6);

	obsRemoteToggleBtn_ = new QPushButton(QStringLiteral("OBS Remote: OFF"), bridgeBox);
	obsRemoteToggleBtn_->setCheckable(true);
	obsRemoteToggleBtn_->setChecked(false);
	connect(obsRemoteToggleBtn_, &QPushButton::toggled, this, [this](bool checked) {
		if (updatingUi_)
			return;
		readConfigFromUi();
		config_.obsRemoteEnabled = checked;
		obsRemoteToggleBtn_->setText(checked ? QStringLiteral("OBS Remote: ON")
						     : QStringLiteral("OBS Remote: OFF"));
		saveGlobalConfigFile();
		reconfigureObsRemoteClient();
		if (obsReady_ && checked) {
			pushObsRemoteState(QDateTime::currentMSecsSinceEpoch());
		}
		updateObsRemoteStatusUi();
	});
	bridgeLayout->addWidget(obsRemoteToggleBtn_);

	obsRemoteStatusLabel_ = new QLabel(QStringLiteral("Status: Disabled"), bridgeBox);
	obsRemoteStatusLabel_->setWordWrap(true);
	bridgeLayout->addWidget(obsRemoteStatusLabel_);

	auto *testRow = new QHBoxLayout();
	testObsRemoteBtn_ = new QPushButton(QStringLiteral("Test Connection"), bridgeBox);
	obsRemoteTestLabel_ = new QLabel(QStringLiteral("Not tested"), bridgeBox);
	obsRemoteTestLabel_->setWordWrap(true);
	connect(testObsRemoteBtn_, &QPushButton::clicked, this,
		&SwitcherDock::onTestObsRemoteClicked);
	testRow->addWidget(testObsRemoteBtn_);
	testRow->addWidget(obsRemoteTestLabel_, 1);
	bridgeLayout->addLayout(testRow);
	remoteLayout->addWidget(bridgeBox);

	// Overlay URL card
	auto *credsBox = new QGroupBox(QStringLiteral("OBS Overlay URL"), remotePage);
	auto *credsForm = new QFormLayout(credsBox);
	credsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	credsForm->setSpacing(6);

	auto *tokenRow = new QHBoxLayout();
	obsRemoteTokenEdit_ = new QLineEdit(credsBox);
	obsRemoteTokenEdit_->setEchoMode(QLineEdit::Password);
	obsRemoteTokenEdit_->setPlaceholderText(
		QStringLiteral("https://streambeast.app/overlay/obs-remote?token=..."));
	obsRemoteShowTokenBtn_ = new QPushButton(QStringLiteral("Show"), credsBox);
	obsRemoteShowTokenBtn_->setCheckable(true);
	connect(obsRemoteShowTokenBtn_, &QPushButton::toggled, this, [this](bool shown) {
		obsRemoteTokenEdit_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		obsRemoteShowTokenBtn_->setText(shown ? QStringLiteral("Hide")
						      : QStringLiteral("Show"));
	});
	tokenRow->addWidget(obsRemoteTokenEdit_, 1);
	tokenRow->addWidget(obsRemoteShowTokenBtn_);
	credsForm->addRow(QStringLiteral("Overlay URL:"), tokenRow);

	remoteLayout->addWidget(credsBox);
	remoteLayout->addStretch(1);

	remoteScroll->setWidget(remotePage);
	tabs->addTab(remoteScroll, QStringLiteral("OBS Remote"));
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

	connect(clipSourceCombo_, &QComboBox::currentTextChanged, this,
		&SwitcherDock::onUiConfigChanged);
	connect(clipDirEdit_, &QLineEdit::editingFinished, this, &SwitcherDock::onUiConfigChanged);
	connect(clipShuffleCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
	connect(clipSubfoldersCheck_, &QCheckBox::toggled, this, &SwitcherDock::onUiConfigChanged);
	connect(clipOnlyFallbackCheck_, &QCheckBox::toggled, this,
		&SwitcherDock::onUiConfigChanged);

	connect(obsRemoteTokenEdit_, &QLineEdit::editingFinished, this,
		&SwitcherDock::onUiConfigChanged);
}

void SwitcherDock::onObsFinishedLoading()
{
	obsReady_ = true;
	obsExiting_ = false;
	refreshSceneAndSourceLists();
	if (!tickTimer_->isActive()) {
		tickTimer_->start();
	}
	reconfigureObsRemoteClient();
	onTick();
}

void SwitcherDock::onObsStateChanged()
{
	if (!obsReady_ || obsExiting_)
		return;
	if (obsRemoteClient_->isConfigured()) {
		pushObsRemoteState(QDateTime::currentMSecsSinceEpoch());
	}
}

void SwitcherDock::onObsExiting()
{
	obsExiting_ = true;
	obsReady_ = false;
	if (tickTimer_->isActive()) {
		tickTimer_->stop();
	}
	apiClient_->stop();
	obsRemoteClient_->stop();
}

void SwitcherDock::refreshSceneAndSourceLists()
{
	if (obsExiting_)
		return;

	updatingUi_ = true;
	{
		QSignalBlocker b1(mediaSourceCombo_);
		QSignalBlocker b2(clipSourceCombo_);
		QSignalBlocker b3(liveSceneCombo_);
		QSignalBlocker b4(fallbackSceneCombo_);
		QSignalBlocker b5(lowBitrateSceneCombo_);

		std::vector<SourceListItem> sources;
		obs_enum_sources(enumMediaSourcesProc, &sources);
		std::sort(sources.begin(), sources.end(),
			  [](const SourceListItem &a, const SourceListItem &b) {
				  return a.name < b.name;
			  });

		mediaSourceCombo_->clear();
		mediaSourceCombo_->addItem(QStringLiteral("(None)"), QString());

		clipSourceCombo_->clear();
		clipSourceCombo_->addItem(QStringLiteral("(None)"), QString());

		std::string firstStreamBeastSource;
		for (const auto &item : sources) {
			QString qName = QString::fromStdString(item.name);
			QString label = item.isStreamBeast
						? (qName + QStringLiteral(" [StreamBeast SRT]"))
						: qName;
			mediaSourceCombo_->addItem(label, qName);
			if (!item.isStreamBeast) {
				clipSourceCombo_->addItem(qName, qName);
			}
			if (item.isStreamBeast && firstStreamBeastSource.empty()) {
				firstStreamBeastSource = item.name;
			}
		}

		if (config_.mediaSourceName.empty() && !firstStreamBeastSource.empty()) {
			config_.mediaSourceName = firstStreamBeastSource;
		}

		auto selectSource = [](QComboBox *combo, const std::string &target) {
			int idx = combo->findData(QString::fromStdString(target));
			if (idx < 0 && !target.empty()) {
				QString qSaved = QString::fromStdString(target);
				combo->addItem(qSaved + QStringLiteral(" (Missing)"), qSaved);
				idx = combo->count() - 1;
			}
			combo->setCurrentIndex(std::max(0, idx));
		};

		selectSource(mediaSourceCombo_, config_.mediaSourceName);
		selectSource(clipSourceCombo_, config_.clipSourceName);

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

		QSignalBlocker bClipToggle(clipEnabledToggleBtn_);
		clipEnabledToggleBtn_->setChecked(config_.clipPlayerEnabled);
		clipEnabledToggleBtn_->setText(config_.clipPlayerEnabled
						       ? QStringLiteral("Random Clips: ON")
						       : QStringLiteral("Random Clips: OFF"));
		clipDirEdit_->setText(QString::fromStdString(config_.clipDirectory));
		clipShuffleCheck_->setChecked(config_.clipShuffleNoRepeat);
		clipSubfoldersCheck_->setChecked(config_.clipIncludeSubfolders);
		clipOnlyFallbackCheck_->setChecked(config_.clipOnlyOnFallbackScene);

		QSignalBlocker bRemoteToggle(obsRemoteToggleBtn_);
		obsRemoteToggleBtn_->setChecked(config_.obsRemoteEnabled);
		obsRemoteToggleBtn_->setText(config_.obsRemoteEnabled
						     ? QStringLiteral("OBS Remote: ON")
						     : QStringLiteral("OBS Remote: OFF"));
		if (!config_.obsRemoteToken.empty()) {
			obsRemoteTokenEdit_->setText(
				QStringLiteral("%1/overlay/obs-remote?token=%2")
					.arg(QString::fromStdString(config_.apiBaseUrl),
					     QString::fromStdString(config_.obsRemoteToken)));
		} else {
			obsRemoteTokenEdit_->clear();
		}
	}
	updatingUi_ = false;
	refreshSceneAndSourceLists();
	rescanClipDirectory();
	updateClipStatusUi();
	reconfigureObsRemoteClient();
	updateObsRemoteStatusUi();
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

	config_.clipPlayerEnabled = clipEnabledToggleBtn_->isChecked();
	config_.clipSourceName = clipSourceCombo_->currentData().toString().toStdString();
	config_.clipDirectory = clipDirEdit_->text().trimmed().toUtf8().toStdString();
	config_.clipShuffleNoRepeat = clipShuffleCheck_->isChecked();
	config_.clipIncludeSubfolders = clipSubfoldersCheck_->isChecked();
	config_.clipOnlyOnFallbackScene = clipOnlyFallbackCheck_->isChecked();

	config_.obsRemoteEnabled = obsRemoteToggleBtn_->isChecked();
	config_.obsRemoteToken =
		extractObsRemoteToken(obsRemoteTokenEdit_->text().toStdString());
}

void SwitcherDock::onUiConfigChanged()
{
	if (updatingUi_)
		return;
	readConfigFromUi();
	if (config_.clipDirectory != scannedClipDir_ ||
	    config_.clipIncludeSubfolders != scannedClipSubfolders_) {
		rescanClipDirectory();
	}
	engine_.reset();
	saveGlobalConfigFile();
	reconfigureObsRemoteClient();
	if (obsReady_) {
		onTick();
	} else {
		updateClipStatusUi();
		updateObsRemoteStatusUi();
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

	obs_data_set_bool(data, "clip_player_enabled", config_.clipPlayerEnabled);
	obs_data_set_string(data, "clip_source", config_.clipSourceName.c_str());
	obs_data_set_string(data, "clip_directory", config_.clipDirectory.c_str());
	obs_data_set_bool(data, "clip_shuffle_no_repeat", config_.clipShuffleNoRepeat);
	obs_data_set_bool(data, "clip_include_subfolders", config_.clipIncludeSubfolders);
	obs_data_set_bool(data, "clip_only_on_fallback", config_.clipOnlyOnFallbackScene);

	obs_data_set_bool(data, "obs_remote_enabled", config_.obsRemoteEnabled);
	obs_data_set_string(data, "obs_remote_token", config_.obsRemoteToken.c_str());
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

	if (obs_data_has_user_value(data, "clip_player_enabled"))
		config_.clipPlayerEnabled = obs_data_get_bool(data, "clip_player_enabled");
	if (obs_data_has_user_value(data, "clip_source"))
		config_.clipSourceName = getStr("clip_source");
	if (obs_data_has_user_value(data, "clip_directory"))
		config_.clipDirectory = getStr("clip_directory");
	if (obs_data_has_user_value(data, "clip_shuffle_no_repeat"))
		config_.clipShuffleNoRepeat = obs_data_get_bool(data, "clip_shuffle_no_repeat");
	if (obs_data_has_user_value(data, "clip_include_subfolders"))
		config_.clipIncludeSubfolders = obs_data_get_bool(data, "clip_include_subfolders");
	if (obs_data_has_user_value(data, "clip_only_on_fallback"))
		config_.clipOnlyOnFallbackScene = obs_data_get_bool(data, "clip_only_on_fallback");

	if (obs_data_has_user_value(data, "obs_remote_enabled"))
		config_.obsRemoteEnabled = obs_data_get_bool(data, "obs_remote_enabled");
	if (obs_data_has_user_value(data, "obs_remote_token"))
		config_.obsRemoteToken = extractObsRemoteToken(getStr("obs_remote_token"));
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
		clipSelector_.reset();
		scannedClipDir_.clear();
		wasClipTargetActive_ = false;
		lastClipLoadMs_ = -1;
		lastClipErrorMs_ = -1;
		clipConsecutiveErrors_ = 0;
		currentClipName_.clear();
		lastClipError_.clear();
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

	std::string activeScene = snap.currentSceneName;
	if (!decision.switchToSceneName.empty() &&
	    decision.switchToSceneName != snap.currentSceneName) {
		switchObsScene(decision.switchToSceneName);
		activeScene = decision.switchToSceneName;
	}

	if (decision.statusSummary != lastLogSummary_) {
		lastLogSummary_ = decision.statusSummary;
		obs_log(LOG_INFO, "Status [%s]: %s", decision.healthBadge.c_str(),
			decision.statusSummary.c_str());
	}

	updateClipPlayer(nowMs, activeScene);
	updateStatusUi(decision, localHealth, mediaFound, closeWhenInactive, effectiveKey);

	if (obsRemoteClient_->isConfigured() && (nowMs - lastRemotePushMs_) >= 2500) {
		pushObsRemoteState(nowMs);
	}
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

void SwitcherDock::rescanClipDirectory()
{
	scannedClipDir_ = config_.clipDirectory;
	scannedClipSubfolders_ = config_.clipIncludeSubfolders;

	std::vector<std::string> found;
	if (!config_.clipDirectory.empty()) {
		const QString dirPath = QString::fromStdString(config_.clipDirectory);
		const QDir dir(dirPath);
		if (dir.exists()) {
			const QDirIterator::IteratorFlags flags =
				config_.clipIncludeSubfolders ? QDirIterator::Subdirectories
							      : QDirIterator::NoIteratorFlags;
			QDirIterator it(dirPath, QDir::Files | QDir::Readable, flags);
			while (it.hasNext()) {
				const QString filePath = it.next();
				const std::string utf8Path =
					QFileInfo(filePath).absoluteFilePath().toUtf8().toStdString();
				if (isSupportedVideoExtension(utf8Path)) {
					found.push_back(utf8Path);
				}
			}
		}
	}

	clipSelector_.setClips(std::move(found));
}

bool SwitcherDock::playNextRandomClip(int64_t nowMs, bool forcePlay)
{
	if (obsExiting_)
		return false;

	if (config_.clipSourceName.empty()) {
		lastClipError_ = "Select or create a Clip Media Source first";
		return false;
	}
	if (config_.clipSourceName == config_.mediaSourceName) {
		lastClipError_ = "Clip Media Source cannot be the same as the SRT Media Source";
		return false;
	}
	if (config_.clipDirectory.empty()) {
		lastClipError_ = "Select a Clips Folder first";
		return false;
	}

	if (clipSelector_.clipCount() == 0) {
		rescanClipDirectory();
		if (clipSelector_.clipCount() == 0) {
			currentClipName_.clear();
			lastClipError_ = "No video clips found in folder";
			return false;
		}
	}

	obs_source_t *source = obs_get_source_by_name(config_.clipSourceName.c_str());
	if (!source) {
		lastClipError_ = "Clip Media Source not found";
		return false;
	}

	const std::string clipPath = clipSelector_.pickNext(config_.clipShuffleNoRepeat);
	if (clipPath.empty()) {
		obs_source_release(source);
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings) {
		settings = obs_data_create();
	}
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", clipPath.c_str());
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_source_update(source, settings);
	obs_data_release(settings);

	if (forcePlay) {
		obs_source_media_restart(source);
	}
	obs_source_release(source);

	currentClipName_ = QFileInfo(QString::fromStdString(clipPath)).fileName().toStdString();
	lastClipLoadMs_ = nowMs;
	lastClipError_.clear();
	obs_log(LOG_INFO, "Playing BRB clip: %s", currentClipName_.c_str());
	return true;
}

void SwitcherDock::stopClipMediaSource()
{
	if (config_.clipSourceName.empty() || obsExiting_ ||
	    config_.clipSourceName == config_.mediaSourceName) {
		return;
	}

	obs_source_t *source = obs_get_source_by_name(config_.clipSourceName.c_str());
	if (source) {
		obs_source_media_stop(source);
		obs_source_release(source);
	}
	lastClipLoadMs_ = -1;
	lastClipErrorMs_ = -1;
	clipConsecutiveErrors_ = 0;
	currentClipName_.clear();
}

void SwitcherDock::updateClipPlayer(int64_t nowMs, const std::string &currentSceneName)
{
	const bool onFallback = !config_.fallbackSceneName.empty() &&
				currentSceneName == config_.fallbackSceneName;

	if (!config_.clipPlayerEnabled || config_.clipSourceName.empty() ||
	    config_.clipDirectory.empty()) {
		if (wasClipTargetActive_) {
			stopClipMediaSource();
			wasClipTargetActive_ = false;
		}
		lastClipError_.clear();
		updateClipStatusUi();
		return;
	}

	if (config_.clipSourceName == config_.mediaSourceName) {
		lastClipError_ = "Clip Media Source cannot be the same as the SRT Media Source";
		wasClipTargetActive_ = false;
		updateClipStatusUi();
		return;
	}

	obs_source_t *clipSource = obs_get_source_by_name(config_.clipSourceName.c_str());
	if (!clipSource) {
		lastClipError_ = "Selected Clip Media Source not found";
		wasClipTargetActive_ = false;
		updateClipStatusUi();
		return;
	}

	const bool sourceActive = obs_source_active(clipSource);
	const bool shouldBeActive = config_.clipOnlyOnFallbackScene ? onFallback : sourceActive;

	if (!shouldBeActive) {
		if (wasClipTargetActive_) {
			obs_source_media_stop(clipSource);
			lastClipLoadMs_ = -1;
			lastClipErrorMs_ = -1;
			clipConsecutiveErrors_ = 0;
			currentClipName_.clear();
			wasClipTargetActive_ = false;
		}
		obs_source_release(clipSource);
		updateClipStatusUi();
		return;
	}

	if (!wasClipTargetActive_) {
		obs_source_release(clipSource);
		rescanClipDirectory();
		clipConsecutiveErrors_ = 0;
		lastClipError_.clear();
		if (playNextRandomClip(nowMs, true)) {
			wasClipTargetActive_ = true;
		}
		updateClipStatusUi();
		return;
	}

	const enum obs_media_state state = obs_source_media_get_state(clipSource);
	obs_source_release(clipSource);

	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		clipConsecutiveErrors_ = 0;
		lastClipError_.clear();
		break;

	case OBS_MEDIA_STATE_OPENING:
	case OBS_MEDIA_STATE_BUFFERING:
	case OBS_MEDIA_STATE_PAUSED:
		break;

	case OBS_MEDIA_STATE_ENDED:
		clipConsecutiveErrors_ = 0;
		playNextRandomClip(nowMs, true);
		break;

	case OBS_MEDIA_STATE_ERROR:
		++clipConsecutiveErrors_;
		if (clipSelector_.clipCount() > 0 &&
		    clipConsecutiveErrors_ <= static_cast<int>(clipSelector_.clipCount())) {
			if (lastClipErrorMs_ < 0 || (nowMs - lastClipErrorMs_) >= 1500) {
				lastClipErrorMs_ = nowMs;
				playNextRandomClip(nowMs, true);
			}
		} else {
			lastClipError_ = "Unable to play clips in selected folder";
		}
		break;

	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_NONE:
	default:
		if (lastClipLoadMs_ < 0 || (nowMs - lastClipLoadMs_) >= 1500) {
			playNextRandomClip(nowMs, true);
		}
		break;
	}

	updateClipStatusUi();
}

void SwitcherDock::updateClipStatusUi()
{
	if (!clipStatusLabel_ || !clipMonitorLabel_)
		return;

	const size_t total = clipSelector_.clipCount();
	if (!config_.clipPlayerEnabled) {
		clipStatusLabel_->setText(
			total > 0 ? QStringLiteral("Status: Disabled (%1 clips found)").arg(total)
				  : QStringLiteral("Status: Disabled"));
		clipMonitorLabel_->setVisible(false);
		return;
	}

	QString statusText;
	if (!lastClipError_.empty()) {
		statusText = QStringLiteral("Error: %1")
				     .arg(QString::fromStdString(lastClipError_));
	} else if (config_.clipSourceName.empty()) {
		statusText = QStringLiteral("Select or create a Clip Media Source");
	} else if (config_.clipDirectory.empty()) {
		statusText = QStringLiteral("Select a Clips Folder");
	} else if (total == 0) {
		statusText = QStringLiteral("No supported video clips found in folder");
	} else if (!currentClipName_.empty() && wasClipTargetActive_) {
		statusText = QStringLiteral("Playing: %1 (%2 clips)")
				     .arg(QString::fromStdString(currentClipName_))
				     .arg(total);
	} else {
		statusText = QStringLiteral("Ready: %1 clips queued (standby)").arg(total);
	}

	clipStatusLabel_->setText(QStringLiteral("Status: %1").arg(statusText));
	clipMonitorLabel_->setText(QStringLiteral("Random Clips: %1").arg(statusText));
	clipMonitorLabel_->setVisible(true);
}

void SwitcherDock::onBrowseClipDirClicked()
{
	const QString initialDir = clipDirEdit_->text().trimmed().isEmpty()
					   ? QDir::homePath()
					   : clipDirEdit_->text().trimmed();
	const QString chosen = QFileDialog::getExistingDirectory(
		this, QStringLiteral("Select Random Clips Folder"), initialDir);
	if (chosen.isEmpty())
		return;

	clipDirEdit_->setText(chosen);
	onUiConfigChanged();
}

void SwitcherDock::onCreateClipSourceClicked()
{
	if (obsExiting_)
		return;

	readConfigFromUi();
	const std::string targetScene = !config_.fallbackSceneName.empty()
						  ? config_.fallbackSceneName
						  : currentObsSceneName();
	if (targetScene.empty()) {
		lastClipError_ = "Select a Fallback / BRB scene first";
		updateClipStatusUi();
		return;
	}

	std::string candidateName = "StreamBeast Random Clips";
	for (int suffix = 2; suffix < 100; ++suffix) {
		obs_source_t *existing = obs_get_source_by_name(candidateName.c_str());
		if (!existing)
			break;
		obs_source_release(existing);
		candidateName = "StreamBeast Random Clips " + std::to_string(suffix);
	}

	obs_scene_t *scene = obs_get_scene_by_name(targetScene.c_str());
	if (!scene) {
		lastClipError_ = "Target Fallback scene not found";
		updateClipStatusUi();
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "close_when_inactive", true);
	obs_data_set_bool(settings, "restart_on_activate", false);

	obs_source_t *src =
		obs_source_create("ffmpeg_source", candidateName.c_str(), settings, nullptr);
	obs_data_release(settings);

	if (src) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		if (item) {
			struct obs_video_info ovi = {};
			if (obs_get_video_info(&ovi) && ovi.base_width > 0 &&
			    ovi.base_height > 0) {
				struct vec2 bounds = {static_cast<float>(ovi.base_width),
						      static_cast<float>(ovi.base_height)};
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
				obs_sceneitem_set_bounds(item, &bounds);
			}
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
		}
		obs_source_release(src);
		config_.clipSourceName = candidateName;
		config_.clipPlayerEnabled = true;
		lastClipError_.clear();
		obs_log(LOG_INFO, "Created BRB Clip Media Source '%s' in scene '%s'",
			candidateName.c_str(), targetScene.c_str());
	}
	obs_scene_release(scene);

	applyConfigToUi();
	saveGlobalConfigFile();
	if (obsReady_) {
		onTick();
	}
}

void SwitcherDock::onNextClipClicked()
{
	readConfigFromUi();
	rescanClipDirectory();
	if (playNextRandomClip(QDateTime::currentMSecsSinceEpoch(), true)) {
		wasClipTargetActive_ = true;
	}
	updateClipStatusUi();
}

void SwitcherDock::onRescanClipsClicked()
{
	readConfigFromUi();
	rescanClipDirectory();
	lastClipError_.clear();
	updateClipStatusUi();
}

void SwitcherDock::reconfigureObsRemoteClient()
{
	if (!obsReady_ || obsExiting_)
		return;

	obsRemoteClient_->configure(config_.obsRemoteEnabled,
				    QString::fromStdString(config_.apiBaseUrl),
				    QString::fromStdString(config_.obsRemoteToken));
}

void SwitcherDock::updateObsRemoteStatusUi()
{
	if (!obsRemoteStatusLabel_)
		return;

	if (!config_.obsRemoteEnabled) {
		obsRemoteStatusLabel_->setText(QStringLiteral("Status: Disabled"));
		return;
	}
	if (config_.obsRemoteToken.empty()) {
		obsRemoteStatusLabel_->setText(
			QStringLiteral("Status: Paste your OBS Overlay URL"));
		return;
	}
	obsRemoteStatusLabel_->setText(
		QStringLiteral("Status: %1").arg(QString::fromStdString(obsRemoteStatusText_)));
}

void SwitcherDock::pushObsRemoteState(int64_t nowMs)
{
	if (!obsReady_ || obsExiting_ || !obsRemoteClient_->isConfigured())
		return;

	lastRemotePushMs_ = nowMs;
	obsRemoteClient_->pushState(buildObsRemoteStateSnapshot(nowMs));
}

QJsonObject SwitcherDock::buildObsRemoteStateSnapshot(int64_t nowMs)
{
	const bool streamActive = obs_frontend_streaming_active();
	const bool recordActive = obs_frontend_recording_active();
	const bool replayActive = obs_frontend_replay_buffer_active();
	const std::string currentScene = currentObsSceneName();

	if (streamActive && remoteStreamStartMs_ <= 0) {
		remoteStreamStartMs_ = nowMs;
	} else if (!streamActive) {
		remoteStreamStartMs_ = 0;
		remoteBitrateKbps_ = 0;
		lastRemoteStreamBytes_ = 0;
		lastRemoteStreamSampleMs_ = 0;
	}

	QJsonArray scenesArr;
	char **sceneNames = obs_frontend_get_scene_names();
	if (sceneNames) {
		for (size_t i = 0; sceneNames[i] != nullptr; ++i) {
			scenesArr.append(QString::fromUtf8(sceneNames[i]));
		}
		bfree(sceneNames);
	}

	QJsonArray sceneItemsArr;
	if (!currentScene.empty()) {
		obs_scene_t *scene = obs_get_scene_by_name(currentScene.c_str());
		if (scene) {
			std::vector<QJsonObject> items;
			auto enumItems = [](obs_scene_t *, obs_sceneitem_t *item,
					    void *param) -> bool {
				auto *out = static_cast<std::vector<QJsonObject> *>(param);
				obs_source_t *src = obs_sceneitem_get_source(item);
				const char *name = src ? obs_source_get_name(src) : nullptr;
				if (!name || !*name)
					return true;

				QJsonObject obj;
				obj.insert(QStringLiteral("sceneItemId"),
					   static_cast<qint64>(obs_sceneitem_get_id(item)));
				obj.insert(QStringLiteral("sourceName"), QString::fromUtf8(name));
				obj.insert(QStringLiteral("sceneItemEnabled"),
					   obs_sceneitem_visible(item));
				const char *kind = obs_source_get_unversioned_id(src);
				obj.insert(QStringLiteral("inputKind"),
					   kind ? QString::fromUtf8(kind) : QString());
				out->push_back(std::move(obj));
				return true;
			};
			obs_scene_enum_items(scene, enumItems, &items);
			obs_scene_release(scene);

			for (auto it = items.rbegin(); it != items.rend(); ++it) {
				sceneItemsArr.append(*it);
			}
		}
	}

	struct AudioRow {
		QString name;
		bool muted = false;
		double mul = 1.0;
		double db = 0.0;
	};
	std::vector<AudioRow> audioRows;
	auto enumAudio = [](void *param, obs_source_t *source) -> bool {
		if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT)
			return true;
		if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
			return true;
		const char *name = obs_source_get_name(source);
		if (!name || !*name)
			return true;

		const float mul = obs_source_get_volume(source);
		const double db =
			(mul > 0.0f) ? (20.0 * std::log10(static_cast<double>(mul))) : -100.0;
		auto *out = static_cast<std::vector<AudioRow> *>(param);
		out->push_back(AudioRow{QString::fromUtf8(name), obs_source_muted(source),
					static_cast<double>(mul), db});
		return true;
	};
	obs_enum_sources(enumAudio, &audioRows);
	std::sort(audioRows.begin(), audioRows.end(),
		  [](const AudioRow &a, const AudioRow &b) {
			  return QString::localeAwareCompare(a.name, b.name) < 0;
		  });

	QJsonArray audioArr;
	for (const auto &row : audioRows) {
		QJsonObject obj;
		obj.insert(QStringLiteral("inputName"), row.name);
		obj.insert(QStringLiteral("inputMuted"), row.muted);
		obj.insert(QStringLiteral("inputVolumeMul"), row.mul);
		obj.insert(QStringLiteral("inputVolumeDb"), row.db);
		audioArr.append(obj);
	}

	uint64_t outputBytes = 0;
	int outputSkippedFrames = 0;
	int outputTotalFrames = 0;
	double outputCongestion = 0.0;
	int64_t outputDuration =
		(streamActive && remoteStreamStartMs_ > 0) ? (nowMs - remoteStreamStartMs_) : 0;

	if (streamActive) {
		obs_output_t *streamOut = obs_frontend_get_streaming_output();
		if (streamOut) {
			outputBytes = obs_output_get_total_bytes(streamOut);
			outputSkippedFrames = obs_output_get_frames_dropped(streamOut);
			outputTotalFrames = obs_output_get_total_frames(streamOut);
			outputCongestion = static_cast<double>(obs_output_get_congestion(streamOut));
			obs_output_release(streamOut);

			if (lastRemoteStreamSampleMs_ > 0 &&
			    outputBytes >= lastRemoteStreamBytes_) {
				const double dtSec =
					static_cast<double>(nowMs - lastRemoteStreamSampleMs_) /
					1000.0;
				if (dtSec > 0.2) {
					remoteBitrateKbps_ = static_cast<int>(std::lround(
						static_cast<double>(outputBytes -
								    lastRemoteStreamBytes_) *
						8.0 / dtSec / 1000.0));
				}
			}
			lastRemoteStreamBytes_ = outputBytes;
			lastRemoteStreamSampleMs_ = nowMs;
		}
	}

	QJsonObject statsObj;
	statsObj.insert(QStringLiteral("bitrateKbps"), remoteBitrateKbps_);
	statsObj.insert(QStringLiteral("outputSkippedFrames"), outputSkippedFrames);
	statsObj.insert(QStringLiteral("outputTotalFrames"), outputTotalFrames);
	statsObj.insert(QStringLiteral("renderSkippedFrames"),
			static_cast<int>(obs_get_lagged_frames()));
	statsObj.insert(QStringLiteral("renderTotalFrames"),
			static_cast<int>(obs_get_total_frames()));
	statsObj.insert(QStringLiteral("activeFps"), obs_get_active_fps());
	statsObj.insert(QStringLiteral("cpuUsage"),
			cpuUsageInfo_ ? os_cpu_usage_info_query(cpuUsageInfo_) : 0.0);
	statsObj.insert(QStringLiteral("memoryUsage"),
			static_cast<double>(os_get_proc_resident_size()) / (1024.0 * 1024.0));
	statsObj.insert(QStringLiteral("outputCongestion"), outputCongestion);
	statsObj.insert(QStringLiteral("outputDuration"), static_cast<qint64>(outputDuration));
	statsObj.insert(QStringLiteral("outputBytes"), static_cast<qint64>(outputBytes));

	const char *obsVer = obs_get_version_string();

	QJsonObject root;
	root.insert(QStringLiteral("obsConnected"), true);
	root.insert(QStringLiteral("obsVersion"),
		    obsVer ? QString::fromUtf8(obsVer) : QString());
	root.insert(QStringLiteral("streamActive"), streamActive);
	root.insert(QStringLiteral("recordActive"), recordActive);
	root.insert(QStringLiteral("replayBufferActive"), replayActive);
	root.insert(QStringLiteral("currentScene"), QString::fromStdString(currentScene));
	root.insert(QStringLiteral("scenes"), scenesArr);
	root.insert(QStringLiteral("sceneItems"), sceneItemsArr);
	root.insert(QStringLiteral("audioInputs"), audioArr);
	root.insert(QStringLiteral("stats"), statsObj);
	root.insert(QStringLiteral("lastError"), QJsonValue::Null);
	return root;
}

void SwitcherDock::onTestObsRemoteClicked()
{
	readConfigFromUi();
	if (config_.obsRemoteToken.empty()) {
		obsRemoteTestLabel_->setText(
			QStringLiteral("Paste your OBS Overlay URL first."));
		return;
	}

	testObsRemoteBtn_->setEnabled(false);
	obsRemoteTestLabel_->setText(QStringLiteral("Testing..."));
	obsRemoteClient_->testToken(QString::fromStdString(config_.apiBaseUrl),
				    QString::fromStdString(config_.obsRemoteToken));
}

void SwitcherDock::onObsRemoteConnectionChanged(bool connected, const QString &statusText)
{
	obsRemoteStatusText_ = statusText.toStdString();
	updateObsRemoteStatusUi();

	if (connected && obsReady_ && !obsExiting_) {
		pushObsRemoteState(QDateTime::currentMSecsSinceEpoch());
	}
}

void SwitcherDock::onObsRemoteTokenTested(bool ok, const QString &message)
{
	(void)ok;
	testObsRemoteBtn_->setEnabled(true);
	obsRemoteTestLabel_->setText(message);
}

void SwitcherDock::onObsRemoteCommandReceived(const QString &requestType,
					      const QJsonObject &requestData)
{
	if (!obsReady_ || obsExiting_)
		return;

	obs_log(LOG_INFO, "OBS Remote command: %s", requestType.toUtf8().constData());

	if (requestType == QStringLiteral("StartStream")) {
		if (!obs_frontend_streaming_active())
			obs_frontend_streaming_start();
	} else if (requestType == QStringLiteral("StopStream")) {
		if (obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
	} else if (requestType == QStringLiteral("StartRecord")) {
		if (!obs_frontend_recording_active())
			obs_frontend_recording_start();
	} else if (requestType == QStringLiteral("StopRecord")) {
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
	} else if (requestType == QStringLiteral("StartReplayBuffer")) {
		if (!obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_start();
	} else if (requestType == QStringLiteral("StopReplayBuffer")) {
		if (obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_stop();
	} else if (requestType == QStringLiteral("SaveReplayBuffer")) {
		if (obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_save();
	} else if (requestType == QStringLiteral("SetCurrentProgramScene")) {
		const std::string sceneName =
			requestData.value(QStringLiteral("sceneName")).toString().toStdString();
		switchObsScene(sceneName);
	} else if (requestType == QStringLiteral("SetSceneItemEnabled")) {
		const std::string sceneName =
			requestData.value(QStringLiteral("sceneName")).toString().toStdString();
		const int64_t itemId =
			requestData.value(QStringLiteral("sceneItemId")).toInteger(-1);
		const bool enabled =
			requestData.value(QStringLiteral("sceneItemEnabled")).toBool(true);
		const std::string targetScene =
			sceneName.empty() ? currentObsSceneName() : sceneName;
		if (!targetScene.empty() && itemId >= 0) {
			obs_scene_t *scene = obs_get_scene_by_name(targetScene.c_str());
			if (scene) {
				obs_sceneitem_t *item =
					obs_scene_find_sceneitem_by_id(scene, itemId);
				if (item) {
					obs_sceneitem_set_visible(item, enabled);
				}
				obs_scene_release(scene);
			}
		}
	} else if (requestType == QStringLiteral("SetInputMute")) {
		const std::string inputName =
			requestData.value(QStringLiteral("inputName")).toString().toStdString();
		const bool muted = requestData.value(QStringLiteral("inputMuted")).toBool(false);
		if (!inputName.empty()) {
			obs_source_t *src = obs_get_source_by_name(inputName.c_str());
			if (src) {
				obs_source_set_muted(src, muted);
				obs_source_release(src);
			}
		}
	} else if (requestType == QStringLiteral("SetInputVolume")) {
		const std::string inputName =
			requestData.value(QStringLiteral("inputName")).toString().toStdString();
		if (!inputName.empty()) {
			double mul = -1.0;
			if (requestData.contains(QStringLiteral("inputVolumeMul"))) {
				mul = requestData.value(QStringLiteral("inputVolumeMul"))
					      .toDouble(-1.0);
			} else if (requestData.contains(QStringLiteral("inputVolumeDb"))) {
				const double db =
					requestData.value(QStringLiteral("inputVolumeDb"))
						.toDouble(-100.0);
				mul = (db <= -99.0) ? 0.0 : std::pow(10.0, db / 20.0);
			}
			if (mul >= 0.0) {
				obs_source_t *src = obs_get_source_by_name(inputName.c_str());
				if (src) {
					obs_source_set_volume(
						src, static_cast<float>(std::clamp(mul, 0.0, 20.0)));
					obs_source_release(src);
				}
			}
		}
	}

	// Push refreshed OBS state back shortly after executing the command
	QTimer::singleShot(120, this, [this]() {
		if (obsReady_ && !obsExiting_ && obsRemoteClient_->isConfigured()) {
			pushObsRemoteState(QDateTime::currentMSecsSinceEpoch());
		}
	});
}

} // namespace streambeast
