/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace streambeast {

/**
 * Returns true if the filename or path has a supported video clip extension.
 */
bool isSupportedVideoExtension(const std::string &pathOrFilename);

/**
 * Stateful random clip selector with optional shuffle-without-repeat (shuffle bag)
 * and back-to-back repeat avoidance.
 */
class RandomClipSelector {
public:
	RandomClipSelector();
	explicit RandomClipSelector(uint64_t seed);

	void setClips(std::vector<std::string> clips);
	void reset();

	std::string pickNext(bool shuffleNoRepeat);

	size_t clipCount() const { return clips_.size(); }
	size_t remainingInBag() const { return bag_.size(); }
	const std::vector<std::string> &clips() const { return clips_; }
	const std::string &lastPlayed() const { return lastPlayed_; }

private:
	void refillBag();

	std::vector<std::string> clips_;
	std::vector<std::string> bag_;
	std::string lastPlayed_;
	std::mt19937_64 rng_;
};

} // namespace streambeast
