/*
StreamBeast SRT Switcher
Copyright (C) 2026 StreamBeast (https://streambeast.app)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "switcher-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QPointer>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "streambeast_srt_switcher_dock";
QPointer<streambeast::SwitcherDock> g_dock;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (!g_dock)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		g_dock->onObsFinishedLoading();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED:
		g_dock->refreshSceneAndSourceLists();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		g_dock->onObsExiting();
		break;
	default:
		break;
	}
}

void onSaveCallback(obs_data_t *save_data, bool saving, void *)
{
	if (g_dock) {
		g_dock->saveConfig(save_data, saving);
	}
}

void onToolsMenuClicked(void *)
{
	if (!g_dock)
		return;

	if (QWidget *parent = g_dock->parentWidget()) {
		parent->show();
		parent->raise();
		parent->activateWindow();
	} else {
		g_dock->show();
		g_dock->raise();
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);

	g_dock = new streambeast::SwitcherDock();
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("StreamBeast.DockTitle"),
					 g_dock)) {
		obs_log(LOG_ERROR, "Failed to register StreamBeast SRT Switcher dock");
		delete g_dock;
		g_dock = nullptr;
		return false;
	}

	obs_frontend_add_tools_menu_item(obs_module_text("StreamBeast.MenuTools"),
					 onToolsMenuClicked, nullptr);
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_frontend_add_save_callback(onSaveCallback, nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_frontend_remove_save_callback(onSaveCallback, nullptr);

	if (g_dock) {
		g_dock->onObsExiting();
		obs_frontend_remove_dock(kDockId);
		g_dock = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
