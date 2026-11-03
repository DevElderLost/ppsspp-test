// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <atomic>
#include <string>

#include "Common/Data/Text/I18n.h"
#include "Common/File/Path.h"
#include "Common/System/OSD.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/Promise.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Core/Mods/DataBinDump.h"
#include "UI/DeveloperSettingsPages.h"
#include "UI/FirmwareScreen.h"
#include "UI/MiscScreens.h"
#include "UI/RemoteISOScreen.h"
#include "UI/RetroAchievementScreens.h"
#include "UI/SavedataScreen.h"
#include "UI/SettingsPage.h"
#include "UI/Settings/MainSettingsPages.h"
#include "UI/SystemInfoScreen.h"

// Takes DATA.BIN apart in the background: a long job, many gigabytes of reads and writes.
static std::atomic<bool> g_dataBinDumpRunning{false};

static void StartDataBinDump(const Path &gamePath) {
	if (g_dataBinDumpRunning.exchange(true)) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, "DATA.BIN is already being extracted");
		return;
	}
	g_threadManager.EnqueueTask(new IndependentTask(TaskType::IO_BLOCKING, TaskPriority::LOW, [gamePath]() {
		Mods::DumpOptions options;
		options.progress = [](float progress) {
			g_OSD.SetProgressBar("databin_dump", "Extracting DATA.BIN", 0.0f, 100.0f, progress * 100.0f, 0.0f);
		};
		Mods::DumpStats stats;
		const Path destination = Mods::DefaultDumpFolder();
		g_OSD.SetProgressBar("databin_dump", "Extracting DATA.BIN", 0.0f, 100.0f, 0.0f, 0.0f);
		const bool ok = Mods::DumpDisc(gamePath, destination, options, &stats);
		g_OSD.RemoveProgressBar("databin_dump", ok, 0.5f);
		if (ok) {
			g_OSD.Show(OSDType::MESSAGE_SUCCESS, "DATA.BIN extracted",
				std::to_string(stats.written) + " files in " + destination.ToVisualString(), 10.0f);
		} else {
			g_OSD.Show(OSDType::MESSAGE_ERROR, "Could not extract DATA.BIN", stats.error, 10.0f);
		}
		g_dataBinDumpRunning = false;
	}));
}

void BuildToolsSettings(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();

	page->Header("Tools");

	const bool showRetroAchievements = System_GetPropertyInt(SYSPROP_DEVICE_TYPE) != DEVICE_TYPE_VR;
	if (showRetroAchievements) {
		page->Choice({ I18NCat::SYSTEM, "RetroAchievements" }).OnClick([host](UI::EventParams &) {
			host->PageScreenManager()->push(new RetroAchievementsSettingsScreen(host->PageGamePath()));
		}).Setup([](View *view) {
			static_cast<Choice *>(view)->SetIconRight(ImageID("I_RETROACHIEVEMENTS_LOGO"));
		});
	}

	// These were moved here so use the wrong translation objects, to avoid having to change all inis... This isn't a sustainable situation :P
	page->Choice({ I18NCat::SAVEDATA, "Savedata Manager" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new SavedataScreen(host->PageGamePath()));
	});
	page->Choice({ I18NCat::DEVELOPER, "System Information" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new SystemInfoScreen(host->PageGamePath()));
	});
	page->Link({ I18NCat::SYSTEM, "Developer Tools" }, I18NCat::DEVELOPER, &BuildDeveloperToolsPage);
	page->Choice({ I18NCat::REMOTEISO, "Remote disc streaming" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new RemoteISOScreen(host->PageGamePath()));
	});
	page->Choice({ I18NCat::SYSTEM, "PSP Firmware" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new FirmwareScreen(host->PageGamePath()));
	});

	// Fork addition.
	page->Choice({ I18NCat::SYSTEM, "Extract DATA.BIN..." }).OnClick([host](UI::EventParams &) {
		auto sy = GetI18NCategory(I18NCat::SYSTEM);
		auto di = GetI18NCategory(I18NCat::DIALOG);
		const Path gamePath = host->PageGamePath();
		if (gamePath.empty()) {
			g_OSD.Show(OSDType::MESSAGE_ERROR, sy->T("Open the settings from the game"),
				sy->T("ExtractNeedsGame", "Choose the game first: DATA.BIN is taken from its disc"), 8.0f);
			return;
		}
		const std::string message = std::string(sy->T("ExtractDataBinPrompt",
			"Extract the files of DATA.BIN, decrypted, to")) + "\n" + Mods::DefaultDumpFolder().ToVisualString() +
			"\n" + std::string(sy->T("ExtractDataBinSpace", "It takes a few minutes and about 2 GB."));
		host->PageScreenManager()->push(new PromptScreen(gamePath, message, di->T("Extract"), di->T("Cancel"), [gamePath](bool yes) {
			if (yes)
				StartDataBinDump(gamePath);
		}));
	});
	page->Hint({ I18NCat::SYSTEM, "Extract DATA.BIN Tip", "Monster Hunter Portable 3rd HD: every file, named by its file id, ready to edit and put in a mod folder" });
}
