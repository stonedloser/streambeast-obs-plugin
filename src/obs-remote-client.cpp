/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "obs-remote-client.hpp"
#include "switcher-core.hpp"

#include <plugin-support.h>

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace streambeast {

ObsRemoteClient::ObsRemoteClient(QObject *parent)
	: QObject(parent),
	  nam_(new QNetworkAccessManager(this)),
	  reconnectTimer_(new QTimer(this))
{
	reconnectTimer_->setSingleShot(true);
	reconnectTimer_->setInterval(3500);
	connect(reconnectTimer_, &QTimer::timeout, this, &ObsRemoteClient::connectSse);
}

ObsRemoteClient::~ObsRemoteClient()
{
	stop();
}

void ObsRemoteClient::configure(bool enabled, const QString &baseUrl, const QString &token)
{
	const QString normalizedBase =
		QString::fromStdString(normalizeApiBaseUrl(baseUrl.toStdString()));
	const QString cleanToken =
		QString::fromStdString(extractObsRemoteToken(token.toStdString()));

	const bool changed = (enabled_ != enabled) || (baseUrl_ != normalizedBase) ||
			     (token_ != cleanToken);

	enabled_ = enabled;
	baseUrl_ = normalizedBase;
	token_ = cleanToken;

	if (!isConfigured()) {
		stop();
		emit connectionChanged(false, enabled_ ? QStringLiteral("Waiting for token")
						       : QStringLiteral("Disabled"));
		return;
	}

	if (changed || (!sseReply_ && !reconnectTimer_->isActive())) {
		disconnectSse();
		connectSse();
	}
}

void ObsRemoteClient::stop()
{
	if (reconnectTimer_->isActive()) {
		reconnectTimer_->stop();
	}
	disconnectSse();
	abortStateReply();
	abortTestReply();
	hasPendingState_ = false;
	if (sseConnected_) {
		sseConnected_ = false;
	}
}

void ObsRemoteClient::disconnectSse()
{
	if (sseReply_) {
		sseReply_->disconnect(this);
		sseReply_->abort();
		sseReply_->deleteLater();
		sseReply_ = nullptr;
	}
	sseParser_.reset();
	sseConnected_ = false;
}

void ObsRemoteClient::abortStateReply()
{
	if (stateReply_) {
		stateReply_->disconnect(this);
		stateReply_->abort();
		stateReply_->deleteLater();
		stateReply_ = nullptr;
	}
}

void ObsRemoteClient::abortTestReply()
{
	if (testReply_) {
		testReply_->disconnect(this);
		testReply_->abort();
		testReply_->deleteLater();
		testReply_ = nullptr;
	}
}

void ObsRemoteClient::connectSse()
{
	if (!isConfigured())
		return;

	disconnectSse();

	QUrl url(baseUrl_ + QStringLiteral("/api/obs-remote/overlay/events"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("token"), token_);
	url.setQuery(query);

	if (!url.isValid() || url.scheme().isEmpty()) {
		emit connectionChanged(false, QStringLiteral("Invalid StreamBeast API URL"));
		return;
	}

	QNetworkRequest req(url);
	req.setRawHeader("Accept", "text/event-stream");
	req.setRawHeader("Cache-Control", "no-cache");
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("StreamBeast-IRL/%1")
			      .arg(QString::fromUtf8(PLUGIN_VERSION)));
	req.setTransferTimeout(0);

	sseReply_ = nam_->get(req);
	connect(sseReply_, &QNetworkReply::readyRead, this, &ObsRemoteClient::handleSseReadyRead);
	connect(sseReply_, &QNetworkReply::finished, this, &ObsRemoteClient::handleSseFinished);

	emit connectionChanged(false, QStringLiteral("Connecting to StreamBeast OBS Remote..."));
}

void ObsRemoteClient::handleSseReadyRead()
{
	if (!sseReply_)
		return;

	const QByteArray bytes = sseReply_->readAll();
	if (bytes.isEmpty())
		return;

	if (!sseConnected_) {
		sseConnected_ = true;
		emit connectionChanged(true, QStringLiteral("Connected to StreamBeast OBS Remote"));
	}

	const std::vector<SseEvent> events = sseParser_.feed(bytes.toStdString());
	for (const SseEvent &ev : events) {
		if (ev.event == "ready") {
			sseConnected_ = true;
			emit connectionChanged(
				true, QStringLiteral("Connected to StreamBeast OBS Remote"));
			continue;
		}

		if (ev.event == "obs_config") {
			emit configReloadRequested();
			continue;
		}

		if (ev.event == "obs_command") {
			QJsonParseError err{};
			const QJsonDocument doc =
				QJsonDocument::fromJson(QByteArray::fromStdString(ev.data), &err);
			if (err.error != QJsonParseError::NoError || !doc.isObject())
				continue;

			const QJsonObject obj = doc.object();
			const QString requestType = obj.value(QStringLiteral("requestType")).toString();
			const QJsonObject requestData =
				obj.value(QStringLiteral("requestData")).toObject();
			if (!requestType.isEmpty()) {
				emit commandReceived(requestType, requestData);
			}
		}
	}
}

void ObsRemoteClient::handleSseFinished()
{
	QNetworkReply *reply = sseReply_;
	sseReply_ = nullptr;
	sseConnected_ = false;
	sseParser_.reset();

	if (!reply)
		return;

	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QNetworkReply::NetworkError err = reply->error();
	const QString errStr = reply->errorString();
	reply->deleteLater();

	if (!isConfigured())
		return;

	if (status == 401 || status == 403) {
		emit connectionChanged(
			false,
			QStringLiteral("Invalid OBS Remote token (HTTP %1)").arg(status));
		return;
	}

	const QString reason =
		(err != QNetworkReply::NoError && !errStr.isEmpty())
			? errStr
			: QStringLiteral("Disconnected (HTTP %1)").arg(status);
	emit connectionChanged(false,
			       QStringLiteral("%1 — reconnecting...").arg(reason));
	reconnectTimer_->start();
}

void ObsRemoteClient::pushState(const QJsonObject &stateObj)
{
	if (!isConfigured())
		return;

	if (stateReply_) {
		hasPendingState_ = true;
		pendingState_ = stateObj;
		return;
	}

	const QUrl url(baseUrl_ + QStringLiteral("/api/obs-remote/overlay/state"));
	if (!url.isValid() || url.scheme().isEmpty())
		return;

	QJsonObject payloadObj = stateObj;
	payloadObj.insert(QStringLiteral("token"), token_);

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("StreamBeast-IRL/%1")
			      .arg(QString::fromUtf8(PLUGIN_VERSION)));
	req.setTransferTimeout(3000);

	const QByteArray payload = QJsonDocument(payloadObj).toJson(QJsonDocument::Compact);
	QNetworkReply *reply = nam_->post(req, payload);
	stateReply_ = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		if (stateReply_ == reply) {
			stateReply_ = nullptr;
		}
		reply->deleteLater();
		if (hasPendingState_ && isConfigured()) {
			QJsonObject next = pendingState_;
			hasPendingState_ = false;
			pendingState_ = QJsonObject();
			pushState(next);
		}
	});
}

void ObsRemoteClient::testToken(const QString &baseUrl, const QString &token)
{
	const QString normalizedBase =
		QString::fromStdString(normalizeApiBaseUrl(baseUrl.toStdString()));
	const QString cleanToken =
		QString::fromStdString(extractObsRemoteToken(token.toStdString()));

	if (cleanToken.isEmpty()) {
		emit tokenTested(
			false,
			QStringLiteral("Invalid or missing OBS Remote Overlay URL."));
		return;
	}

	abortTestReply();

	QUrl url(normalizedBase + QStringLiteral("/api/obs-remote/overlay/config"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("token"), cleanToken);
	url.setQuery(query);

	if (!url.isValid() || url.scheme().isEmpty()) {
		emit tokenTested(false, QStringLiteral("Invalid API URL."));
		return;
	}

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("StreamBeast-IRL/%1")
			      .arg(QString::fromUtf8(PLUGIN_VERSION)));
	req.setTransferTimeout(3000);

	QNetworkReply *reply = nam_->get(req);
	testReply_ = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		if (testReply_ == reply) {
			testReply_ = nullptr;
		}
		const int status =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError err = reply->error();
		const QString errStr = reply->errorString();
		reply->deleteLater();

		if (err == QNetworkReply::NoError && status == 200) {
			emit tokenTested(true, QStringLiteral("Token verified (HTTP 200)"));
		} else if (status == 401 || status == 403) {
			emit tokenTested(
				false,
				QStringLiteral("Invalid token (HTTP %1)").arg(status));
		} else {
			emit tokenTested(
				false,
				QStringLiteral("Failed: %1 (HTTP %2)")
					.arg(errStr.isEmpty() ? QStringLiteral("Network error")
							      : errStr)
					.arg(status));
		}
	});
}

} // namespace streambeast
