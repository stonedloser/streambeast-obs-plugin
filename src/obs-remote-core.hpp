/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <string>
#include <vector>

namespace streambeast {

struct SseEvent {
	std::string event = "message";
	std::string data;
};

/**
 * Extracts an OBS Remote token from an overlay URL
 * (e.g. "https://streambeast.app/overlay/obs-remote?token=<token>")
 * or returns a raw token if provided directly.
 */
std::string extractObsRemoteToken(const std::string &input);

/**
 * Incremental Server-Sent Events (SSE) parser for chunked HTTP responses.
 */
class SseEventParser {
public:
	std::vector<SseEvent> feed(const std::string &chunk);
	void reset();

private:
	void processLine(const std::string &line, std::vector<SseEvent> &out);

	std::string buffer_;
	std::string currentEvent_ = "message";
	std::string currentData_;
	bool hasData_ = false;
};

} // namespace streambeast
