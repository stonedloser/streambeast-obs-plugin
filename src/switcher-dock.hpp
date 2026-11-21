/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "clip-player.hpp"
#include "obs-remote-client.hpp"
#include "streambeast-api.hpp"
#include "switcher-core.hpp"

#include <QWidget>
#include <obs.h>
#include <cstdint>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
class QVBoxLayout;
struct os_cpu_usage_info;

namespace streambeast {

class SwitcherDock : public QWidget {
	Q_OBJECT

public:
	explicit SwitcherDock(QWidget *parent = nullptr);
	~SwitcherDock() override;

	void onObsFinishedLoading();
	void onObsExiting();
	void onObsStateChanged();
	void refreshSceneAndSourceLists();

	void saveConfig(obs_data_t *saveData, bool saving);

private slots:
	void onTick();
	void onTelemetryUpdated(const ApiTelemetrySnapshot &snapshot, bool fromManualTest);
	void onUiConfigChanged();
	void onTestApiClicked();
	void onRestartSourceClicked();
	void onManualLiveClicked();
	void onManualFallbackClicked();
	void onBrowseClipDirClicked();
	void onCreateClipSourceClicked();
	void onNextClipClicked();
	void onRescanClipsClicked();

	void onTestObsRemoteClicked();
	void onObsRemoteConnectionChanged(bool connected, const QString &statusText);
	void onObsRemoteTokenTested(bool ok, const QString &message);
	void onObsRemoteCommandReceived(const QString &requestType,
					const QJsonObject &requestData);

private:
	void buildUi();
	void buildAutoSceneTab(QTabWidget *tabs);
	void buildAutoSceneSettings(QWidget *parentPage, QVBoxLayout *parentLayout);
	void buildClipsTab(QTabWidget *tabs);
	void buildObsRemoteTab(QTabWidget *tabs);
	void connectUiSignals();
	void applyConfigToUi();
	void readConfigFromUi();
	void saveGlobalConfigFile() const;
	void loadGlobalConfigFile();
	void applyObsDataToConfig(obs_data_t *data);
	void writeConfigToObsData(obs_data_t *data) const;
	int maxApiAgeMs() const;

	MediaHealth sampleLocalMediaHealth(int64_t nowMs, bool &outFound,
					   bool &outCloseWhenInactive,
					   std::string &outExtractedKey);
	std::string currentObsSceneName() const;
	bool isObsStreamingOrRecording() const;
	void switchObsScene(const std::string &sceneName);
	void restartObsMediaSource();
	void updateStatusUi(const SwitcherDecision &decision, MediaHealth localHealth,
			    bool mediaFound, bool closeWhenInactive,
			    const std::string &effectiveKey);
	void reconfigureApiClient(const std::string &effectiveKey);

	void rescanClipDirectory();
	bool playNextRandomClip(int64_t nowMs, bool forcePlay);
	void stopClipMediaSource();
	void updateClipPlayer(int64_t nowMs, const std::string &currentSceneName);
	QString formatClipStatusText() const;
	void updateClipStatusUi();

	void reconfigureObsRemoteClient();
	void pushObsRemoteState(int64_t nowMs);
	QJsonObject buildObsRemoteStateSnapshot(int64_t nowMs);
	void updateObsRemoteStatusUi();

	SwitcherConfig config_;
	SrtSwitcherEngine engine_;
	SwitcherDecision lastDecision_;
	RandomClipSelector clipSelector_;
	StreamBeastApiClient *apiClient_ = nullptr;
	ObsRemoteClient *obsRemoteClient_ = nullptr;
	QTimer *tickTimer_ = nullptr;
	struct os_cpu_usage_info *cpuUsageInfo_ = nullptr;

	bool obsReady_ = false;
	bool obsExiting_ = false;
	bool updatingUi_ = false;

	// Local media playback progress tracking for stall detection
	int64_t lastMediaTime_ = -1;
	int64_t lastMediaAdvanceMs_ = -1;
	std::string lastLogSummary_;
	std::string lastAutoDetectedKey_;

	// BRB random clip player state
	bool wasClipTargetActive_ = false;
	int64_t lastClipLoadMs_ = -1;
	int64_t lastClipErrorMs_ = -1;
	int clipConsecutiveErrors_ = 0;
	std::string scannedClipDir_;
	bool scannedClipSubfolders_ = false;
	std::string currentClipName_;
	std::string lastClipError_;

	// OBS Remote state
	std::string obsRemoteStatusText_ = "Disabled";
	int64_t lastRemotePushMs_ = 0;
	uint64_t lastRemoteStreamBytes_ = 0;
	int64_t lastRemoteStreamSampleMs_ = 0;
	int remoteBitrateKbps_ = 0;
	int64_t remoteStreamStartMs_ = 0;

	// Monitor tab widgets
	QLabel *badgeLabel_ = nullptr;
	QLabel *summaryLabel_ = nullptr;
	QLabel *telemetryLabel_ = nullptr;
	QLabel *localStateLabel_ = nullptr;
	QLabel *clipMonitorLabel_ = nullptr;
	QLabel *tipLabel_ = nullptr;
	QPushButton *enableToggleBtn_ = nullptr;

	QComboBox *mediaSourceCombo_ = nullptr;
	QComboBox *liveSceneCombo_ = nullptr;
	QComboBox *fallbackSceneCombo_ = nullptr;
	QComboBox *lowBitrateSceneCombo_ = nullptr;

	// Random Clips tab widgets
	QPushButton *clipEnabledToggleBtn_ = nullptr;
	QLabel *clipStatusLabel_ = nullptr;
	QComboBox *clipSourceCombo_ = nullptr;
	QPushButton *createClipSourceBtn_ = nullptr;
	QLineEdit *clipDirEdit_ = nullptr;
	QPushButton *browseClipDirBtn_ = nullptr;
	QCheckBox *clipShuffleCheck_ = nullptr;
	QCheckBox *clipSubfoldersCheck_ = nullptr;
	QCheckBox *clipOnlyFallbackCheck_ = nullptr;
	QPushButton *nextClipBtn_ = nullptr;
	QPushButton *rescanClipsBtn_ = nullptr;

	// Settings tab widgets
	QDoubleSpinBox *dropDelaySpin_ = nullptr;
	QDoubleSpinBox *recoverDelaySpin_ = nullptr;
	QDoubleSpinBox *stallTimeoutSpin_ = nullptr;
	QDoubleSpinBox *restartCooldownSpin_ = nullptr;

	QSpinBox *lowBitrateThresholdSpin_ = nullptr;
	QDoubleSpinBox *lowBitrateDelaySpin_ = nullptr;
	QSpinBox *lowBitrateHysteresisSpin_ = nullptr;

	QCheckBox *useApiCheck_ = nullptr;
	QLineEdit *apiUrlEdit_ = nullptr;
	QLineEdit *streamKeyEdit_ = nullptr;
	QPushButton *showKeyBtn_ = nullptr;
	QPushButton *testApiBtn_ = nullptr;
	QLabel *apiStatusLabel_ = nullptr;

	QCheckBox *onlyStreamingCheck_ = nullptr;
	QCheckBox *onlyManagedScenesCheck_ = nullptr;
	QCheckBox *autoReturnCheck_ = nullptr;
	QCheckBox *autoRestartCheck_ = nullptr;

	// OBS Remote tab widgets
	QPushButton *obsRemoteToggleBtn_ = nullptr;
	QLabel *obsRemoteStatusLabel_ = nullptr;
	QLineEdit *obsRemoteTokenEdit_ = nullptr;
	QPushButton *obsRemoteShowTokenBtn_ = nullptr;
	QPushButton *testObsRemoteBtn_ = nullptr;
	QLabel *obsRemoteTestLabel_ = nullptr;
};

} // namespace streambeast
