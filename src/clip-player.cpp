/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "clip-player.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace streambeast {

namespace {

constexpr std::array<const char *, 15> kSupportedVideoExtensions = {
	".mp4", ".m4v", ".mkv", ".mov", ".webm", ".flv", ".ts",  ".m2ts",
	".mts", ".avi", ".wmv", ".mpg", ".mpeg", ".ogv", ".3gp",
};

std::string toLowerExtension(const std::string &ext)
{
	std::string out = ext;
	for (char &ch : out) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return out;
}

} // namespace

bool isSupportedVideoExtension(const std::string &pathOrFilename)
{
	const size_t slashPos = pathOrFilename.find_last_of("/\\");
	const size_t dotPos = pathOrFilename.find_last_of('.');
	if (dotPos == std::string::npos)
		return false;
	if (slashPos != std::string::npos && dotPos < slashPos)
		return false;

	const std::string ext = toLowerExtension(pathOrFilename.substr(dotPos));
	for (const char *supported : kSupportedVideoExtensions) {
		if (ext == supported)
			return true;
	}
	return false;
}

RandomClipSelector::RandomClipSelector() : rng_(std::random_device{}()) {}

RandomClipSelector::RandomClipSelector(uint64_t seed) : rng_(seed) {}

void RandomClipSelector::setClips(std::vector<std::string> clips)
{
	std::sort(clips.begin(), clips.end());
	clips.erase(std::unique(clips.begin(), clips.end()), clips.end());

	if (clips == clips_)
		return;

	clips_ = std::move(clips);
	if (clips_.empty()) {
		bag_.clear();
		lastPlayed_.clear();
		return;
	}

	// Keep existing unplayed clips in the bag and mix in newly added clips
	std::unordered_set<std::string> validSet(clips_.begin(), clips_.end());
	std::unordered_set<std::string> inBag;
	std::vector<std::string> nextBag;
	nextBag.reserve(clips_.size());

	for (const auto &item : bag_) {
		if (validSet.count(item) > 0 && inBag.insert(item).second) {
			nextBag.push_back(item);
		}
	}

	// If the bag was never populated yet, or all entries were invalidated, refill fresh
	if (nextBag.empty()) {
		bag_.clear();
		refillBag();
		return;
	}

	// Append newly discovered clips (excluding the last played clip if already played this round)
	for (const auto &clip : clips_) {
		if (inBag.count(clip) == 0 && clip != lastPlayed_) {
			nextBag.push_back(clip);
		}
	}
	std::shuffle(nextBag.begin(), nextBag.end(), rng_);
	bag_ = std::move(nextBag);
}

void RandomClipSelector::reset()
{
	bag_.clear();
	lastPlayed_.clear();
}

void RandomClipSelector::refillBag()
{
	bag_ = clips_;
	if (bag_.size() <= 1)
		return;

	std::shuffle(bag_.begin(), bag_.end(), rng_);

	// Avoid playing the exact same clip back-to-back across reshuffle boundaries
	if (!lastPlayed_.empty() && bag_.back() == lastPlayed_) {
		std::swap(bag_.back(), bag_.front());
	}
}

std::string RandomClipSelector::pickNext(bool shuffleNoRepeat)
{
	if (clips_.empty()) {
		lastPlayed_.clear();
		return {};
	}

	if (clips_.size() == 1) {
		lastPlayed_ = clips_[0];
		return clips_[0];
	}

	if (shuffleNoRepeat) {
		if (bag_.empty()) {
			refillBag();
		}
		std::string chosen = std::move(bag_.back());
		bag_.pop_back();
		lastPlayed_ = chosen;
		return chosen;
	}

	std::uniform_int_distribution<size_t> dist(0, clips_.size() - 1);
	for (int attempt = 0; attempt < 8; ++attempt) {
		const std::string &candidate = clips_[dist(rng_)];
		if (candidate != lastPlayed_) {
			lastPlayed_ = candidate;
			return candidate;
		}
	}

	// Fallback deterministic step if RNG drew lastPlayed_ repeatedly
	for (const auto &candidate : clips_) {
		if (candidate != lastPlayed_) {
			lastPlayed_ = candidate;
			return candidate;
		}
	}
	return clips_[0];
}

} // namespace streambeast
