/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

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

namespace streambeast {

class SwitcherDock : public QWidget {
	Q_OBJECT

public:
	explicit SwitcherDock(QWidget *parent = nullptr);
	~SwitcherDock() override;

	void onObsFinishedLoading();
	void onObsExiting();
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

private:
	void buildUi();
	void buildMonitorTab(QTabWidget *tabs);
	void buildSettingsTab(QTabWidget *tabs);
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

	SwitcherConfig config_;
	SrtSwitcherEngine engine_;
	StreamBeastApiClient *apiClient_ = nullptr;
	QTimer *tickTimer_ = nullptr;

	bool obsReady_ = false;
	bool obsExiting_ = false;
	bool updatingUi_ = false;

	// Local media playback progress tracking for stall detection
	int64_t lastMediaTime_ = -1;
	int64_t lastMediaAdvanceMs_ = -1;
	std::string lastLogSummary_;
	std::string lastAutoDetectedKey_;

	// Monitor tab widgets
	QLabel *badgeLabel_ = nullptr;
	QLabel *summaryLabel_ = nullptr;
	QLabel *telemetryLabel_ = nullptr;
	QLabel *localStateLabel_ = nullptr;
	QLabel *tipLabel_ = nullptr;
	QPushButton *enableToggleBtn_ = nullptr;

	QComboBox *mediaSourceCombo_ = nullptr;
	QComboBox *liveSceneCombo_ = nullptr;
	QComboBox *fallbackSceneCombo_ = nullptr;
	QComboBox *lowBitrateSceneCombo_ = nullptr;

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
};

} // namespace streambeast
