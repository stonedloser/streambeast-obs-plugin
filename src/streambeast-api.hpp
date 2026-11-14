/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace streambeast {

struct ApiTelemetrySnapshot {
	bool reachable = false;
	bool isLive = false;
	int bitrateKbps = -1;
	int peakBitrateKbps = -1;
	QString quality;
	QString resolution;
	double fps = 0.0;
	QString videoCodec;
	QString audioCodec;
	int64_t updatedAtMs = 0;
	int httpStatus = 0;
	QString errorMessage;
};

class StreamBeastApiClient : public QObject {
	Q_OBJECT

public:
	explicit StreamBeastApiClient(QObject *parent = nullptr);
	~StreamBeastApiClient() override;

	void configure(bool enabled, const QString &baseUrl, const QString &streamKey,
		       int pollIntervalMs);
	void pollNow(bool isManualTest = false);
	void stop();

	const ApiTelemetrySnapshot &latestSnapshot() const { return snapshot_; }
	bool isConfigured() const { return enabled_ && !streamKey_.isEmpty(); }
	bool isFresh(int64_t nowMs, int maxAgeMs) const;

signals:
	void telemetryUpdated(const ApiTelemetrySnapshot &snapshot, bool fromManualTest);

private slots:
	void onPollTimer();

private:
	void handleReply(QNetworkReply *reply, bool isManualTest);

	QNetworkAccessManager *nam_ = nullptr;
	QTimer *pollTimer_ = nullptr;
	QNetworkReply *inFlightReply_ = nullptr;

	bool enabled_ = false;
	QString baseUrl_ = QStringLiteral("https://streambeast.app");
	QString streamKey_;
	int pollIntervalMs_ = 2500;

	ApiTelemetrySnapshot snapshot_;
};

} // namespace streambeast
