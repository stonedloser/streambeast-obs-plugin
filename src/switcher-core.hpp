/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <cstdint>
#include <string>

namespace streambeast {

enum class MediaHealth {
	Unknown,
	Offline,
	Opening,
	Playing,
	Stalled,
	Error,
};

enum class TargetSceneRole {
	None,
	Live,
	Fallback,
	LowBitrate,
};

struct SwitcherConfig {
	bool enabled = true;
	bool onlyWhenStreamingOrRecording = false;
	bool onlySwitchManagedScenes = true;
	bool autoReturnToLive = true;
	bool autoRestartMediaSource = true;
	bool useStreamBeastApi = true;

	std::string apiBaseUrl = "https://streambeast.app";
	std::string streamKeyOverride;

	std::string mediaSourceName;
	std::string liveSceneName;
	std::string fallbackSceneName;
	std::string lowBitrateSceneName;

	int dropDelayMs = 4000;
	int recoverDelayMs = 2500;
	int mediaStallTimeoutMs = 3500;
	int lowBitrateKbps = 800;
	int lowBitrateDelayMs = 5000;
	int lowBitrateRecoverHysteresisKbps = 250;
	int apiPollIntervalMs = 2500;
	int mediaRestartCooldownMs = 6000;
};

struct SignalSnapshot {
	int64_t nowMs = 0;
	bool isStreamingOrRecording = false;
	std::string currentSceneName;

	bool mediaSourceFound = false;
	MediaHealth localMediaHealth = MediaHealth::Unknown;

	bool apiConfigured = false;
	bool apiReachable = false;
	bool apiIsLive = false;
	int apiBitrateKbps = -1;
	std::string apiQuality;
};

struct SwitcherDecision {
	TargetSceneRole desiredRole = TargetSceneRole::None;
	std::string switchToSceneName;
	bool requestMediaRestart = false;
	std::string healthBadge = "STANDBY";
	std::string statusSummary;
};

/**
 * Extracts a StreamBeast SRT bridge key from an SRT URL (e.g.
 * "srt://srt.streambeast.app:8890?streamid=read:<key>") or returns a raw key
 * if the input already looks like a bare StreamBeast key.
 */
std::string extractStreamBeastKey(const std::string &input);

/**
 * Returns true if the given input URL looks like a StreamBeast SRT bridge URL.
 */
bool isStreamBeastSrtUrl(const std::string &input);

/**
 * Normalizes the base URL (strips trailing slashes).
 */
std::string normalizeApiBaseUrl(const std::string &rawUrl);

/**
 * Stateful hybrid SRT health evaluator and scene-switching state machine.
 */
class SrtSwitcherEngine {
public:
	void reset();
	SwitcherDecision evaluate(const SignalSnapshot &snap, const SwitcherConfig &cfg);

private:
	enum class EffectiveFeed {
		Unconfigured,
		Down,
		LowBitrate,
		Healthy,
	};

	void resetTimers();
	bool isManagedScene(const std::string &sceneName, const SwitcherConfig &cfg) const;
	EffectiveFeed classifyFeed(const SignalSnapshot &snap, const SwitcherConfig &cfg,
				   bool &outNeedsRestart) const;
	void maybeRequestRestart(const SignalSnapshot &snap, const SwitcherConfig &cfg,
				 SwitcherDecision &decision);
	SwitcherDecision evaluateReturnFromFallback(const SignalSnapshot &snap,
						    const SwitcherConfig &cfg,
						    TargetSceneRole targetRole,
						    const std::string &targetScene,
						    bool needsRestart,
						    const std::string &targetBadge,
						    const std::string &recoveredSummary,
						    const std::string &returningPrefix,
						    const std::string &manualSummary);
	SwitcherDecision evaluateDown(const SignalSnapshot &snap, const SwitcherConfig &cfg,
				      bool onFallback, bool onLowBitrate, bool needsRestart);
	SwitcherDecision evaluateLowBitrate(const SignalSnapshot &snap, const SwitcherConfig &cfg,
					    bool onFallback, bool onLowBitrate, bool needsRestart);
	SwitcherDecision evaluateHealthy(const SignalSnapshot &snap, const SwitcherConfig &cfg,
					 bool onFallback, bool onLowBitrate, bool onLive,
					 bool needsRestart);

	int64_t downSinceMs_ = -1;
	int64_t healthySinceMs_ = -1;
	int64_t lowBitrateSinceMs_ = -1;
	int64_t lowBitrateRecoverSinceMs_ = -1;
	int64_t lastRestartAttemptMs_ = -1;
};

} // namespace streambeast
