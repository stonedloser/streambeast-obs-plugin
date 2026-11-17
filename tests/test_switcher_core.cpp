/*
StreamBeast SRT Switcher - Core Unit Tests
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "switcher-core.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace streambeast;

static void test_url_and_key_extraction()
{
	assert(isStreamBeastSrtUrl(
		"srt://srt.streambeast.app:8890?streamid=read:AbCdEf1234567890"));
	assert(isStreamBeastSrtUrl("SRT://srt.streambeast.app:8890?streamid=read:TestKey1234"));
	assert(isStreamBeastSrtUrl("srt://example.com:8890?streamid=read:TestKey1234"));
	assert(!isStreamBeastSrtUrl("rtmp://live.twitch.tv/app/live_12345"));
	assert(!isStreamBeastSrtUrl("C:/Videos/sample.mp4"));

	assert(extractStreamBeastKey(
		       "srt://srt.streambeast.app:8890?streamid=read:AbCdEf1234567890") ==
	       "AbCdEf1234567890");
	assert(extractStreamBeastKey(
		       "srt://srt.streambeast.app:8890?latency=200000&streamid=read%3AMyKey987654&mode=caller") ==
	       "MyKey987654");
	assert(extractStreamBeastKey(
		       "srt://srt.streambeast.app:8890?streamid=publish:PubKey123456") ==
	       "PubKey123456");
	assert(extractStreamBeastKey(
		       "srt://srt.streambeast.app:8890?streamid=read:bridge/NestedKey123") ==
	       "NestedKey123");
	assert(extractStreamBeastKey("BareStreamKey123456") == "BareStreamKey123456");
	assert(extractStreamBeastKey("read:BareStreamKey123456") == "BareStreamKey123456");
	assert(extractStreamBeastKey("rtmp://live.twitch.tv/app/live_12345").empty());
	assert(extractStreamBeastKey("short").empty());

	assert(normalizeApiBaseUrl("https://streambeast.app/") == "https://streambeast.app");
	assert(normalizeApiBaseUrl("  ") == "https://streambeast.app");
}

static SwitcherConfig defaultTestConfig()
{
	SwitcherConfig cfg;
	cfg.enabled = true;
	cfg.onlyWhenStreamingOrRecording = false;
	cfg.onlySwitchManagedScenes = true;
	cfg.autoReturnToLive = true;
	cfg.autoRestartMediaSource = true;
	cfg.useStreamBeastApi = true;
	cfg.mediaSourceName = "Phone Camera";
	cfg.liveSceneName = "IRL Live";
	cfg.fallbackSceneName = "BRB Offline";
	cfg.lowBitrateSceneName = "Low Signal";
	cfg.dropDelayMs = 4000;
	cfg.recoverDelayMs = 2500;
	cfg.lowBitrateKbps = 800;
	cfg.lowBitrateDelayMs = 5000;
	cfg.lowBitrateRecoverHysteresisKbps = 250;
	cfg.mediaRestartCooldownMs = 6000;
	return cfg;
}

static void test_drop_debounce_and_recovery()
{
	SrtSwitcherEngine engine;
	SwitcherConfig cfg = defaultTestConfig();

	SignalSnapshot snap;
	snap.currentSceneName = "IRL Live";
	snap.mediaSourceFound = true;
	snap.localMediaHealth = MediaHealth::Playing;
	snap.apiConfigured = true;
	snap.apiReachable = true;
	snap.apiIsLive = true;
	snap.apiBitrateKbps = 3500;
	snap.apiQuality = "excellent";

	// Healthy feed stays on Live
	snap.nowMs = 1000;
	auto d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "LIVE");

	// Signal drops -> should debounce for dropDelayMs (4000ms)
	snap.apiIsLive = false;
	snap.localMediaHealth = MediaHealth::Stalled;
	snap.nowMs = 2000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "DROPPING");
	assert(d.requestMediaRestart); // First restart attempt allowed immediately

	snap.nowMs = 5500; // 3500ms elapsed (< 4000ms)
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(!d.requestMediaRestart); // Cooldown 6000ms active

	snap.nowMs = 6000; // 4000ms elapsed -> switch to Fallback!
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName == "BRB Offline");
	assert(d.healthBadge == "OFFLINE");

	// Now on Fallback scene with "Close file when inactive" (localMediaHealth == Offline)
	// StreamBeast API says phone is live again -> should recover + request restart!
	snap.currentSceneName = "BRB Offline";
	snap.localMediaHealth = MediaHealth::Offline;
	snap.apiIsLive = true;
	snap.apiBitrateKbps = 3200;
	snap.apiQuality = "excellent";
	snap.nowMs = 9000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "RECOVERING");

	snap.nowMs = 11500; // 2500ms elapsed -> switch back to Live + restart media source!
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName == "IRL Live");
	assert(d.requestMediaRestart);
	assert(d.healthBadge == "LIVE");
}

static void test_low_bitrate_and_hysteresis()
{
	SrtSwitcherEngine engine;
	SwitcherConfig cfg = defaultTestConfig();

	SignalSnapshot snap;
	snap.currentSceneName = "IRL Live";
	snap.mediaSourceFound = true;
	snap.localMediaHealth = MediaHealth::Playing;
	snap.apiConfigured = true;
	snap.apiReachable = true;
	snap.apiIsLive = true;
	snap.apiBitrateKbps = 500; // < 800 kbps
	snap.apiQuality = "fair";

	snap.nowMs = 1000;
	auto d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "DEGRADED");

	snap.nowMs = 6000; // 5000ms elapsed -> switch to Low Signal
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName == "Low Signal");

	// Bitrate rises to 900 kbps (inside hysteresis band 800 + 250 = 1050) -> stays Low Signal
	snap.currentSceneName = "Low Signal";
	snap.apiBitrateKbps = 900;
	snap.nowMs = 10000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "DEGRADED");

	// Bitrate rises to 1200 kbps (>= 1050) -> recovers after recoverDelayMs (2500ms)
	snap.apiBitrateKbps = 1200;
	snap.apiQuality = "good";
	snap.nowMs = 11000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "RECOVERING");

	snap.nowMs = 13500;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName == "IRL Live");
	assert(d.healthBadge == "LIVE");
}

static void test_safety_guards_and_local_only_mode()
{
	SrtSwitcherEngine engine;
	SwitcherConfig cfg = defaultTestConfig();
	cfg.useStreamBeastApi = false;
	cfg.onlyWhenStreamingOrRecording = true;

	SignalSnapshot snap;
	snap.currentSceneName = "IRL Live";
	snap.mediaSourceFound = true;
	snap.localMediaHealth = MediaHealth::Error;
	snap.isStreamingOrRecording = false;
	snap.nowMs = 1000;

	// Not streaming/recording -> standby
	auto d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "STANDBY");

	// Start streaming, but user manually switched to "Intro" (unmanaged scene)
	snap.isStreamingOrRecording = true;
	snap.currentSceneName = "Intro";
	snap.nowMs = 2000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "STANDBY");

	// Switch to managed scene -> local-only error triggers fallback after dropDelayMs
	snap.currentSceneName = "IRL Live";
	snap.nowMs = 3000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());

	snap.nowMs = 7000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName == "BRB Offline");

	// Manual return mode: autoReturnToLive = false stays on Fallback even when Playing
	cfg.autoReturnToLive = false;
	snap.currentSceneName = "BRB Offline";
	snap.localMediaHealth = MediaHealth::Playing;
	snap.nowMs = 15000;
	d = engine.evaluate(snap, cfg);
	assert(d.switchToSceneName.empty());
	assert(d.healthBadge == "LIVE");
}

int main()
{
	test_url_and_key_extraction();
	test_drop_debounce_and_recovery();
	test_low_bitrate_and_hysteresis();
	test_safety_guards_and_local_only_mode();
	std::cout << "All StreamBeast SRT Switcher core tests passed!\n";
	return 0;
}
