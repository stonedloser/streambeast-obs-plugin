/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "switcher-core.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace streambeast {

namespace {

std::string trimWhitespace(const std::string &s)
{
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
		++start;
	}
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
		--end;
	}
	return s.substr(start, end - start);
}

std::string toLowerAscii(const std::string &s)
{
	std::string out = s;
	for (char &ch : out) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return out;
}

std::string urlDecodeBasic(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size()) {
			auto hexVal = [](char c) -> int {
				if (c >= '0' && c <= '9')
					return c - '0';
				if (c >= 'a' && c <= 'f')
					return 10 + (c - 'a');
				if (c >= 'A' && c <= 'F')
					return 10 + (c - 'A');
				return -1;
			};
			int hi = hexVal(s[i + 1]);
			int lo = hexVal(s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(s[i]);
	}
	return out;
}

bool isValidRawKeyToken(const std::string &token)
{
	if (token.size() < 8 || token.size() > 128)
		return false;
	for (char ch : token) {
		unsigned char uch = static_cast<unsigned char>(ch);
		if (!std::isalnum(uch) && ch != '_' && ch != '-')
			return false;
	}
	return true;
}

std::string formatRemainingSec(int64_t remainingMs)
{
	if (remainingMs < 0)
		remainingMs = 0;
	double sec = static_cast<double>(remainingMs) / 1000.0;
	std::ostringstream oss;
	oss.setf(std::ios::fixed);
	oss.precision(1);
	oss << sec << "s";
	return oss.str();
}

} // namespace

std::string normalizeApiBaseUrl(const std::string &rawUrl)
{
	std::string trimmed = trimWhitespace(rawUrl);
	while (!trimmed.empty() && trimmed.back() == '/') {
		trimmed.pop_back();
	}
	if (trimmed.empty()) {
		return "https://streambeast.app";
	}
	return trimmed;
}

bool isStreamBeastSrtUrl(const std::string &input)
{
	std::string lower = toLowerAscii(trimWhitespace(input));
	if (lower.rfind("srt://", 0) != 0)
		return false;
	if (lower.find("streambeast.app") != std::string::npos)
		return true;
	return lower.find("streamid=read:") != std::string::npos ||
	       lower.find("streamid=read%3a") != std::string::npos;
}

std::string extractStreamBeastKey(const std::string &input)
{
	std::string trimmed = trimWhitespace(input);
	if (trimmed.empty())
		return {};

	// Direct bare key (e.g. pasted by user into override field)
	if (trimmed.find("://") == std::string::npos && trimmed.find('=') == std::string::npos &&
	    trimmed.find('/') == std::string::npos && trimmed.find(':') == std::string::npos) {
		return isValidRawKeyToken(trimmed) ? trimmed : std::string{};
	}

	// Handle "read:<key>" or "publish:<key>" shorthand pasted directly
	if (trimmed.find("://") == std::string::npos && trimmed.find('?') == std::string::npos) {
		std::string lower = toLowerAscii(trimmed);
		if (lower.rfind("read:", 0) == 0 || lower.rfind("publish:", 0) == 0) {
			size_t colon = trimmed.find(':');
			std::string key = trimmed.substr(colon + 1);
			return isValidRawKeyToken(key) ? key : std::string{};
		}
	}

	// Parse query parameters from URL
	size_t qPos = trimmed.find('?');
	if (qPos == std::string::npos)
		return {};

	std::string query = trimmed.substr(qPos + 1);
	// Strip optional fragment
	size_t hashPos = query.find('#');
	if (hashPos != std::string::npos) {
		query = query.substr(0, hashPos);
	}

	std::istringstream iss(query);
	std::string param;
	while (std::getline(iss, param, '&')) {
		size_t eqPos = param.find('=');
		if (eqPos == std::string::npos)
			continue;
		std::string name = toLowerAscii(param.substr(0, eqPos));
		if (name != "streamid")
			continue;

		std::string val = urlDecodeBasic(param.substr(eqPos + 1));
		std::string lowerVal = toLowerAscii(val);
		if (lowerVal.rfind("read:", 0) == 0) {
			val = val.substr(5);
		} else if (lowerVal.rfind("publish:", 0) == 0) {
			val = val.substr(8);
		}
		// Extract the final path segment if a namespace prefix is present
		size_t slashPos = val.rfind('/');
		if (slashPos != std::string::npos) {
			val = val.substr(slashPos + 1);
		}
		val = trimWhitespace(val);
		return isValidRawKeyToken(val) ? val : std::string{};
	}

	return {};
}

void SrtSwitcherEngine::reset()
{
	resetTimers();
	lastRestartAttemptMs_ = -1;
}

void SrtSwitcherEngine::resetTimers()
{
	downSinceMs_ = -1;
	healthySinceMs_ = -1;
	lowBitrateSinceMs_ = -1;
	lowBitrateRecoverSinceMs_ = -1;
}

bool SrtSwitcherEngine::isManagedScene(const std::string &sceneName, const SwitcherConfig &cfg) const
{
	if (sceneName.empty())
		return false;
	if (sceneName == cfg.liveSceneName)
		return true;
	if (sceneName == cfg.fallbackSceneName)
		return true;
	if (!cfg.lowBitrateSceneName.empty() && sceneName == cfg.lowBitrateSceneName)
		return true;
	return false;
}

SrtSwitcherEngine::EffectiveFeed
SrtSwitcherEngine::classifyFeed(const SignalSnapshot &snap, const SwitcherConfig &cfg,
				bool &outNeedsRestart) const
{
	outNeedsRestart = false;

	const bool apiFresh = snap.apiConfigured && snap.apiReachable;
	if (!snap.mediaSourceFound && !apiFresh) {
		return EffectiveFeed::Unconfigured;
	}

	const bool onFallback = !cfg.fallbackSceneName.empty() &&
				snap.currentSceneName == cfg.fallbackSceneName;
	const bool onLowBitrate = !cfg.lowBitrateSceneName.empty() &&
				  snap.currentSceneName == cfg.lowBitrateSceneName;
	const bool hasLowBitrateScene = !cfg.lowBitrateSceneName.empty() && cfg.lowBitrateKbps > 0;

	auto isLowBitrateFromApi = [&]() -> bool {
		if (!hasLowBitrateScene || !apiFresh || !snap.apiIsLive)
			return false;
		if (snap.apiQuality == "stalled")
			return true;
		if (onLowBitrate) {
			// Hysteresis band: stay degraded until bitrate exceeds threshold + hysteresis
			if (snap.apiBitrateKbps > 0 &&
			    snap.apiBitrateKbps <
				    (cfg.lowBitrateKbps + cfg.lowBitrateRecoverHysteresisKbps)) {
				return true;
			}
			return false;
		}
		return snap.apiBitrateKbps > 0 && snap.apiBitrateKbps < cfg.lowBitrateKbps;
	};

	// 1. Server API says publisher is offline -> feed is down immediately
	if (apiFresh && !snap.apiIsLive) {
		if (snap.mediaSourceFound &&
		    (snap.localMediaHealth == MediaHealth::Error ||
		     snap.localMediaHealth == MediaHealth::Stalled)) {
			outNeedsRestart = true;
		}
		return EffectiveFeed::Down;
	}

	// 2. Server API confirms publisher IS live
	if (apiFresh && snap.apiIsLive) {
		// Special case: OBS Media Source closed while sitting on Fallback scene
		// (e.g., user has "Close file when inactive" enabled on the Media Source)
		if (onFallback && snap.mediaSourceFound &&
		    (snap.localMediaHealth == MediaHealth::Offline ||
		     snap.localMediaHealth == MediaHealth::Error ||
		     snap.localMediaHealth == MediaHealth::Opening)) {
			if (snap.apiQuality == "stalled") {
				return EffectiveFeed::Down;
			}
			outNeedsRestart = (snap.localMediaHealth != MediaHealth::Opening);
			return isLowBitrateFromApi() ? EffectiveFeed::LowBitrate
						     : EffectiveFeed::Healthy;
		}

		// If Media Source is actively stuck/stalled/errored while on Live/LowBitrate
		if (snap.mediaSourceFound &&
		    (snap.localMediaHealth == MediaHealth::Stalled ||
		     snap.localMediaHealth == MediaHealth::Error ||
		     snap.localMediaHealth == MediaHealth::Offline)) {
			outNeedsRestart = true;
			return EffectiveFeed::Down;
		}

		if (snap.mediaSourceFound && snap.localMediaHealth == MediaHealth::Opening) {
			// Still opening/buffering: treat as down for drop-timer purposes
			return EffectiveFeed::Down;
		}

		return isLowBitrateFromApi() ? EffectiveFeed::LowBitrate : EffectiveFeed::Healthy;
	}

	// 3. Pure local OBS Media Source mode (API disabled or unreachable)
	switch (snap.localMediaHealth) {
	case MediaHealth::Playing:
		return EffectiveFeed::Healthy;
	case MediaHealth::Opening:
		return EffectiveFeed::Down;
	case MediaHealth::Offline:
	case MediaHealth::Stalled:
	case MediaHealth::Error:
		outNeedsRestart = true;
		return EffectiveFeed::Down;
	case MediaHealth::Unknown:
	default:
		return EffectiveFeed::Unconfigured;
	}
}

void SrtSwitcherEngine::maybeRequestRestart(const SignalSnapshot &snap, const SwitcherConfig &cfg,
					      SwitcherDecision &decision)
{
	if (!cfg.autoRestartMediaSource || !snap.mediaSourceFound)
		return;
	if (lastRestartAttemptMs_ < 0 ||
	    (snap.nowMs - lastRestartAttemptMs_) >= cfg.mediaRestartCooldownMs) {
		decision.requestMediaRestart = true;
		lastRestartAttemptMs_ = snap.nowMs;
	}
}

SwitcherDecision SrtSwitcherEngine::evaluateReturnFromFallback(
	const SignalSnapshot &snap, const SwitcherConfig &cfg, TargetSceneRole targetRole,
	const std::string &targetScene, bool needsRestart, const std::string &targetBadge,
	const std::string &recoveredSummary, const std::string &returningPrefix,
	const std::string &manualSummary)
{
	SwitcherDecision decision;
	if (!cfg.autoReturnToLive) {
		healthySinceMs_ = -1;
		decision.desiredRole = TargetSceneRole::Fallback;
		decision.healthBadge = targetBadge;
		decision.statusSummary = manualSummary;
		return decision;
	}

	if (healthySinceMs_ < 0) {
		healthySinceMs_ = snap.nowMs;
	}
	const int64_t elapsed = snap.nowMs - healthySinceMs_;
	if (elapsed >= cfg.recoverDelayMs) {
		decision.desiredRole = targetRole;
		decision.switchToSceneName = targetScene;
		if (needsRestart) {
			maybeRequestRestart(snap, cfg, decision);
		}
		decision.healthBadge = targetBadge;
		decision.statusSummary = recoveredSummary;
	} else {
		decision.desiredRole = TargetSceneRole::Fallback;
		decision.healthBadge = "RECOVERING";
		decision.statusSummary =
			returningPrefix + formatRemainingSec(cfg.recoverDelayMs - elapsed);
	}
	return decision;
}

SwitcherDecision SrtSwitcherEngine::evaluateDown(const SignalSnapshot &snap,
						 const SwitcherConfig &cfg, bool onFallback,
						 bool onLowBitrate, bool needsRestart)
{
	SwitcherDecision decision;
	healthySinceMs_ = -1;
	lowBitrateSinceMs_ = -1;
	lowBitrateRecoverSinceMs_ = -1;

	if (needsRestart) {
		maybeRequestRestart(snap, cfg, decision);
	}

	if (onFallback) {
		downSinceMs_ = -1;
		decision.desiredRole = TargetSceneRole::Fallback;
		decision.healthBadge = "OFFLINE";
		decision.statusSummary = "SRT offline — Fallback scene active";
		return decision;
	}

	if (downSinceMs_ < 0) {
		downSinceMs_ = snap.nowMs;
	}
	const int64_t elapsed = snap.nowMs - downSinceMs_;
	if (elapsed >= cfg.dropDelayMs) {
		decision.desiredRole = TargetSceneRole::Fallback;
		decision.switchToSceneName = cfg.fallbackSceneName;
		decision.healthBadge = "OFFLINE";
		decision.statusSummary = "SRT dropped — switching to Fallback";
	} else {
		decision.desiredRole =
			onLowBitrate ? TargetSceneRole::LowBitrate : TargetSceneRole::Live;
		decision.healthBadge = "DROPPING";
		decision.statusSummary = "SRT signal lost — switching in " +
					 formatRemainingSec(cfg.dropDelayMs - elapsed);
	}
	return decision;
}

SwitcherDecision SrtSwitcherEngine::evaluateLowBitrate(const SignalSnapshot &snap,
						       const SwitcherConfig &cfg, bool onFallback,
						       bool onLowBitrate, bool needsRestart)
{
	downSinceMs_ = -1;
	lowBitrateRecoverSinceMs_ = -1;

	if (onFallback) {
		return evaluateReturnFromFallback(snap, cfg, TargetSceneRole::LowBitrate,
						  cfg.lowBitrateSceneName, needsRestart, "DEGRADED",
						  "SRT recovered (low bitrate) — switching",
						  "SRT live (low bitrate) — returning in ",
						  "SRT live (low bitrate) — manual return");
	}

	SwitcherDecision decision;
	healthySinceMs_ = -1;
	if (onLowBitrate) {
		lowBitrateSinceMs_ = -1;
		decision.desiredRole = TargetSceneRole::LowBitrate;
		decision.healthBadge = "DEGRADED";
		if (snap.apiBitrateKbps > 0) {
			decision.statusSummary = "Low bitrate (" +
						 std::to_string(snap.apiBitrateKbps) + " kbps)";
		} else {
			decision.statusSummary = "Low bitrate / stalled signal";
		}
		return decision;
	}

	if (lowBitrateSinceMs_ < 0) {
		lowBitrateSinceMs_ = snap.nowMs;
	}
	const int64_t elapsed = snap.nowMs - lowBitrateSinceMs_;
	if (elapsed >= cfg.lowBitrateDelayMs) {
		decision.desiredRole = TargetSceneRole::LowBitrate;
		decision.switchToSceneName = cfg.lowBitrateSceneName;
		decision.healthBadge = "DEGRADED";
		decision.statusSummary = "Low bitrate — switching to warning scene";
	} else {
		decision.desiredRole = TargetSceneRole::Live;
		decision.healthBadge = "DEGRADED";
		decision.statusSummary = "Low bitrate — warning scene in " +
					 formatRemainingSec(cfg.lowBitrateDelayMs - elapsed);
	}
	return decision;
}

SwitcherDecision SrtSwitcherEngine::evaluateHealthy(const SignalSnapshot &snap,
						    const SwitcherConfig &cfg, bool onFallback,
						    bool onLowBitrate, bool onLive,
						    bool needsRestart)
{
	downSinceMs_ = -1;
	lowBitrateSinceMs_ = -1;

	if (onFallback) {
		lowBitrateRecoverSinceMs_ = -1;
		return evaluateReturnFromFallback(snap, cfg, TargetSceneRole::Live,
						  cfg.liveSceneName, needsRestart, "LIVE",
						  "SRT recovered — switching to Live",
						  "SRT recovered — returning in ",
						  "SRT is back live (manual return)");
	}

	SwitcherDecision decision;
	healthySinceMs_ = -1;

	if (onLowBitrate) {
		if (lowBitrateRecoverSinceMs_ < 0) {
			lowBitrateRecoverSinceMs_ = snap.nowMs;
		}
		const int64_t elapsed = snap.nowMs - lowBitrateRecoverSinceMs_;
		if (elapsed >= cfg.recoverDelayMs) {
			decision.desiredRole = TargetSceneRole::Live;
			decision.switchToSceneName = cfg.liveSceneName;
			decision.healthBadge = "LIVE";
			decision.statusSummary = "Bitrate recovered — switching to Live";
		} else {
			decision.desiredRole = TargetSceneRole::LowBitrate;
			decision.healthBadge = "RECOVERING";
			decision.statusSummary = "Bitrate recovering — Live in " +
						 formatRemainingSec(cfg.recoverDelayMs - elapsed);
		}
		return decision;
	}

	lowBitrateRecoverSinceMs_ = -1;
	if (!onLive && cfg.autoReturnToLive) {
		decision.desiredRole = TargetSceneRole::Live;
		decision.switchToSceneName = cfg.liveSceneName;
		decision.healthBadge = "LIVE";
		decision.statusSummary = "SRT healthy — switching to Live";
		return decision;
	}

	decision.desiredRole = TargetSceneRole::Live;
	decision.healthBadge = "LIVE";
	if (snap.apiConfigured && snap.apiReachable && snap.apiBitrateKbps > 0) {
		decision.statusSummary =
			"SRT Live (" + std::to_string(snap.apiBitrateKbps) + " kbps)";
	} else {
		decision.statusSummary = "SRT Live";
	}
	return decision;
}

SwitcherDecision SrtSwitcherEngine::evaluate(const SignalSnapshot &snap, const SwitcherConfig &cfg)
{
	SwitcherDecision decision;

	if (!cfg.enabled) {
		resetTimers();
		decision.healthBadge = "PAUSED";
		decision.statusSummary = "Auto-switching paused";
		return decision;
	}

	if (cfg.liveSceneName.empty() || cfg.fallbackSceneName.empty()) {
		resetTimers();
		decision.healthBadge = "SETUP";
		decision.statusSummary = "Select Live and Fallback scenes";
		return decision;
	}

	if (cfg.onlyWhenStreamingOrRecording && !snap.isStreamingOrRecording) {
		resetTimers();
		decision.healthBadge = "STANDBY";
		decision.statusSummary = "Waiting for stream or recording";
		return decision;
	}

	if (cfg.onlySwitchManagedScenes && !isManagedScene(snap.currentSceneName, cfg)) {
		resetTimers();
		decision.healthBadge = "STANDBY";
		decision.statusSummary = "Manual scene active (not managed)";
		return decision;
	}

	bool needsRestart = false;
	const EffectiveFeed feed = classifyFeed(snap, cfg, needsRestart);

	if (feed == EffectiveFeed::Unconfigured) {
		resetTimers();
		decision.healthBadge = "SETUP";
		decision.statusSummary = "Select a StreamBeast SRT Media Source or Stream Key";
		return decision;
	}

	const bool onFallback = snap.currentSceneName == cfg.fallbackSceneName;
	const bool onLowBitrate = !cfg.lowBitrateSceneName.empty() &&
				  snap.currentSceneName == cfg.lowBitrateSceneName;
	const bool onLive = snap.currentSceneName == cfg.liveSceneName;

	switch (feed) {
	case EffectiveFeed::Down:
		return evaluateDown(snap, cfg, onFallback, onLowBitrate, needsRestart);
	case EffectiveFeed::LowBitrate:
		return evaluateLowBitrate(snap, cfg, onFallback, onLowBitrate, needsRestart);
	case EffectiveFeed::Healthy:
		return evaluateHealthy(snap, cfg, onFallback, onLowBitrate, onLive, needsRestart);
	case EffectiveFeed::Unconfigured:
	default:
		return decision;
	}
}

} // namespace streambeast
