/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "obs-remote-core.hpp"

#include <cctype>
#include <sstream>

namespace streambeast {

namespace {

std::string trimWs(const std::string &s)
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

std::string toLower(const std::string &s)
{
	std::string out = s;
	for (char &ch : out) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return out;
}

std::string urlDecode(const std::string &s)
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

bool isValidObsRemoteToken(const std::string &token)
{
	if (token.size() < 16 || token.size() > 128)
		return false;
	for (char ch : token) {
		unsigned char uch = static_cast<unsigned char>(ch);
		if (!std::isalnum(uch) && ch != '_' && ch != '-')
			return false;
	}
	return true;
}

} // namespace

std::string extractObsRemoteToken(const std::string &input)
{
	std::string trimmed = trimWs(input);
	if (trimmed.empty())
		return {};

	// Direct bare token
	if (trimmed.find("://") == std::string::npos && trimmed.find('?') == std::string::npos &&
	    trimmed.find('=') == std::string::npos && trimmed.find('/') == std::string::npos) {
		return isValidObsRemoteToken(trimmed) ? trimmed : std::string{};
	}

	// Query parameter ?token=... or &token=...
	size_t qPos = trimmed.find('?');
	std::string query = (qPos != std::string::npos) ? trimmed.substr(qPos + 1) : trimmed;

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
		std::string name = toLower(trimWs(param.substr(0, eqPos)));
		if (name != "token")
			continue;
		std::string val = trimWs(urlDecode(param.substr(eqPos + 1)));
		if (isValidObsRemoteToken(val))
			return val;
	}

	return {};
}

void SseEventParser::reset()
{
	buffer_.clear();
	currentEvent_ = "message";
	currentData_.clear();
	hasData_ = false;
}

std::vector<SseEvent> SseEventParser::feed(const std::string &chunk)
{
	std::vector<SseEvent> events;
	buffer_ += chunk;

	size_t lineStart = 0;
	while (true) {
		size_t nlPos = buffer_.find('\n', lineStart);
		if (nlPos == std::string::npos)
			break;

		std::string line = buffer_.substr(lineStart, nlPos - lineStart);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		processLine(line, events);
		lineStart = nlPos + 1;
	}

	if (lineStart > 0) {
		buffer_.erase(0, lineStart);
	}
	return events;
}

void SseEventParser::processLine(const std::string &line, std::vector<SseEvent> &out)
{
	if (line.empty()) {
		if (hasData_) {
			SseEvent ev;
			ev.event = currentEvent_.empty() ? "message" : currentEvent_;
			ev.data = currentData_;
			out.push_back(std::move(ev));
		}
		currentEvent_ = "message";
		currentData_.clear();
		hasData_ = false;
		return;
	}

	// Comment / keepalive line (e.g. ": ping 12345")
	if (line[0] == ':')
		return;

	std::string field = line;
	std::string value;
	const size_t colonPos = line.find(':');
	if (colonPos != std::string::npos) {
		field = line.substr(0, colonPos);
		value = line.substr(colonPos + 1);
		if (!value.empty() && value[0] == ' ') {
			value.erase(0, 1);
		}
	}

	if (field == "event") {
		currentEvent_ = value;
	} else if (field == "data") {
		if (hasData_) {
			currentData_.push_back('\n');
		}
		currentData_ += value;
		hasData_ = true;
	}
}

} // namespace streambeast
