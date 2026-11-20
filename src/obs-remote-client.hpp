/*
StreamBeast IRL
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "obs-remote-core.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace streambeast {

class ObsRemoteClient : public QObject {
	Q_OBJECT

public:
	explicit ObsRemoteClient(QObject *parent = nullptr);
	~ObsRemoteClient() override;

	void configure(bool enabled, const QString &baseUrl, const QString &token);
	void stop();

	void pushState(const QJsonObject &stateObj);
	void testToken(const QString &baseUrl, const QString &token);

	bool isConfigured() const { return enabled_ && !token_.isEmpty(); }
	bool isConnected() const { return sseConnected_; }

signals:
	void connectionChanged(bool connected, const QString &statusText);
	void commandReceived(const QString &requestType, const QJsonObject &requestData);
	void configReloadRequested();
	void tokenTested(bool ok, const QString &message);

private slots:
	void connectSse();

private:
	void disconnectSse();
	void abortStateReply();
	void abortTestReply();
	void handleSseReadyRead();
	void handleSseFinished();

	QNetworkAccessManager *nam_ = nullptr;
	QTimer *reconnectTimer_ = nullptr;

	QNetworkReply *sseReply_ = nullptr;
	QNetworkReply *stateReply_ = nullptr;
	QNetworkReply *testReply_ = nullptr;

	SseEventParser sseParser_;

	bool enabled_ = false;
	bool sseConnected_ = false;
	bool hasPendingState_ = false;
	QJsonObject pendingState_;

	QString baseUrl_ = QStringLiteral("https://streambeast.app");
	QString token_;
};

} // namespace streambeast
