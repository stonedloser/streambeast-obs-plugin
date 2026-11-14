/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "streambeast-api.hpp"
#include "switcher-core.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <algorithm>

namespace streambeast {

StreamBeastApiClient::StreamBeastApiClient(QObject *parent)
	: QObject(parent),
	  nam_(new QNetworkAccessManager(this)),
	  pollTimer_(new QTimer(this))
{
	pollTimer_->setSingleShot(false);
	connect(pollTimer_, &QTimer::timeout, this, &StreamBeastApiClient::onPollTimer);
}

StreamBeastApiClient::~StreamBeastApiClient()
{
	stop();
}

void StreamBeastApiClient::configure(bool enabled, const QString &baseUrl, const QString &streamKey,
				     int pollIntervalMs)
{
	const QString normalizedBase =
		QString::fromStdString(normalizeApiBaseUrl(baseUrl.toStdString()));
	const QString trimmedKey = streamKey.trimmed();
	const int clampedInterval = std::clamp(pollIntervalMs, 1000, 30000);

	const bool changed = (enabled_ != enabled) || (baseUrl_ != normalizedBase) ||
			     (streamKey_ != trimmedKey) || (pollIntervalMs_ != clampedInterval);

	enabled_ = enabled;
	baseUrl_ = normalizedBase;
	streamKey_ = trimmedKey;
	pollIntervalMs_ = clampedInterval;

	if (!isConfigured()) {
		stop();
		snapshot_ = ApiTelemetrySnapshot{};
		return;
	}

	if (changed || !pollTimer_->isActive()) {
		pollTimer_->start(pollIntervalMs_);
		pollNow(false);
	}
}

void StreamBeastApiClient::stop()
{
	if (pollTimer_->isActive()) {
		pollTimer_->stop();
	}
	if (inFlightReply_) {
		inFlightReply_->abort();
		inFlightReply_->deleteLater();
		inFlightReply_ = nullptr;
	}
}

bool StreamBeastApiClient::isFresh(int64_t nowMs, int maxAgeMs) const
{
	if (!snapshot_.reachable || snapshot_.updatedAtMs <= 0)
		return false;
	return (nowMs - snapshot_.updatedAtMs) <= maxAgeMs;
}

void StreamBeastApiClient::onPollTimer()
{
	pollNow(false);
}

void StreamBeastApiClient::pollNow(bool isManualTest)
{
	if (baseUrl_.isEmpty() || streamKey_.isEmpty()) {
		if (isManualTest) {
			ApiTelemetrySnapshot errSnap;
			errSnap.reachable = false;
			errSnap.errorMessage = QStringLiteral("No StreamBeast key configured.");
			emit telemetryUpdated(errSnap, true);
		}
		return;
	}

	if (!enabled_ && !isManualTest) {
		return;
	}

	if (inFlightReply_) {
		if (!isManualTest)
			return;
		inFlightReply_->abort();
		inFlightReply_->deleteLater();
		inFlightReply_ = nullptr;
	}

	const QUrl url(baseUrl_ + QStringLiteral("/api/srt/plugin-status"));
	if (!url.isValid() || url.scheme().isEmpty()) {
		if (isManualTest) {
			ApiTelemetrySnapshot errSnap;
			errSnap.reachable = false;
			errSnap.errorMessage = QStringLiteral("Invalid API URL.");
			emit telemetryUpdated(errSnap, true);
		}
		return;
	}

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("StreamBeast-OBS-Switcher/1.0.0"));
	req.setTransferTimeout(3000);

	QJsonObject body;
	body.insert(QStringLiteral("streamKey"), streamKey_);
	const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

	QNetworkReply *reply = nam_->post(req, payload);
	inFlightReply_ = reply;

	connect(reply, &QNetworkReply::finished, this,
		[this, reply, isManualTest]() { handleReply(reply, isManualTest); });
}

void StreamBeastApiClient::handleReply(QNetworkReply *reply, bool isManualTest)
{
	if (inFlightReply_ == reply) {
		inFlightReply_ = nullptr;
	}

	const int status =
		reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

	if (reply->error() != QNetworkReply::NoError) {
		// Do not wipe last known live state on brief transient abort, but mark unreachable
		snapshot_.reachable = false;
		snapshot_.httpStatus = status;
		snapshot_.errorMessage = reply->errorString();
		reply->deleteLater();
		emit telemetryUpdated(snapshot_, isManualTest);
		return;
	}

	const QByteArray raw = reply->readAll();
	reply->deleteLater();

	QJsonParseError parseErr{};
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
	if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
		snapshot_.reachable = false;
		snapshot_.httpStatus = status;
		snapshot_.errorMessage = QStringLiteral("Invalid JSON response");
		emit telemetryUpdated(snapshot_, isManualTest);
		return;
	}

	const QJsonObject obj = doc.object();
	ApiTelemetrySnapshot next;
	next.reachable = true;
	next.httpStatus = status;
	next.isLive = obj.value(QStringLiteral("isLive")).toBool(false);
	next.updatedAtMs = QDateTime::currentMSecsSinceEpoch();

	if (obj.contains(QStringLiteral("telemetry")) &&
	    obj.value(QStringLiteral("telemetry")).isObject()) {
		const QJsonObject tel = obj.value(QStringLiteral("telemetry")).toObject();
		next.bitrateKbps = tel.value(QStringLiteral("bitrateKbps")).toInt(-1);
		next.peakBitrateKbps = tel.value(QStringLiteral("peakBitrateKbps")).toInt(-1);
		next.quality = tel.value(QStringLiteral("quality")).toString();
		next.resolution = tel.value(QStringLiteral("resolution")).toString();
		next.fps = tel.value(QStringLiteral("fps")).toDouble(0.0);
		next.videoCodec = tel.value(QStringLiteral("videoCodec")).toString();
		next.audioCodec = tel.value(QStringLiteral("audioCodec")).toString();
	}

	snapshot_ = next;
	emit telemetryUpdated(snapshot_, isManualTest);
}

} // namespace streambeast
