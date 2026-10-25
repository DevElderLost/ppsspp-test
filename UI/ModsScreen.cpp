// Copyright (c) 2026- PPSSPP Project.

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

#include "Common/Data/Text/I18n.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/UI/Notice.h"
#include "Common/UI/View.h"
#include "Core/ModImport.h"
#include "Core/Mods/ModEngine.h"
#include "Core/System.h"
#include "UI/MiscViews.h"
#include "UI/ModsScreen.h"

std::string_view ModsScreen::GetTitle() const {
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	return sy->T("Mods");
}

void ModsScreen::BeforeCreateViews() {
	Mods::Manager &mods = Mods::Manager::Instance();
	mods.Reload(ModImport::GetModsFolder());
	const std::vector<Mods::Mod> list = mods.ModList();
	ids_.clear();
	enabled_.reset(new bool[list.size() + 1]());
	for (size_t i = 0; i < list.size(); ++i) {
		ids_.push_back(list[i].id);
		enabled_[i] = mods.Choice(list[i].id).enabled;
	}
	master_ = mods.Master();
}

// Saves the choices, applies them to the running game, and shows what that needs.
void ModsScreen::Changed() {
	Mods::Manager &mods = Mods::Manager::Instance();
	mods.SetMaster(master_);
	mods.Commit();
	RecreateViews();
}

void ModsScreen::CreateSettingsViews(UI::ViewGroup *parent) {
	using namespace UI;
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	Mods::Manager &mods = Mods::Manager::Instance();

	parent->Add(new ItemHeader(sy->T("Mods")));
	CheckBox *master = parent->Add(new CheckBox(&master_, sy->T("Use mods")));
	master->OnClick.Add([this](UI::EventParams &) {
		Changed();
	});
	parent->Add(new Choice(sy->T("Scan the mods folder again")))->OnClick.Add([this](UI::EventParams &) {
		RecreateViews();
	});

	const std::string folder = ModImport::GetModsFolder().ToVisualString();
	parent->Add(new TextView(std::string(sy->T("Mods folder")) + ": " + folder, ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(8, 4, 8, 4))));

	if (!mods.Attached()) {
		parent->Add(new NoticeView(NoticeLevel::INFO, sy->T("No game with mods is running"),
			sy->T("ModsNoGame", "Mods are for Monster Hunter Portable 3rd HD (NPJB-40001). What you turn on here is used the next time it starts.")));
	}
	const std::string note = mods.PendingNote();
	if (!note.empty()) {
		parent->Add(new NoticeView(NoticeLevel::WARN, sy->T("Changes"), note));
	}
	const std::vector<std::string> problems = mods.Problems();
	if (!problems.empty()) {
		parent->Add(new ItemHeader(sy->T("Problems")));
		const size_t first = problems.size() > 6u ? problems.size() - 6u : 0u;
		for (size_t i = first; i < problems.size(); ++i)
			parent->Add(new TextView(problems[i], ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(8, 2, 8, 2))));
	}
}

void ModsScreen::CreateContentViews(UI::ViewGroup *parent) {
	using namespace UI;
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	Mods::Manager &mods = Mods::Manager::Instance();

	LinearLayout *column = parent->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	column->Add(new ItemHeader(sy->T("Installed mods")));
	const std::vector<Mods::Mod> list = mods.ModList();
	if (list.empty()) {
		column->Add(new NoticeView(NoticeLevel::INFO, sy->T("No mods installed"),
			sy->T("ModsNone", "Use Import mod... in the system settings to add a mod folder.")));
		return;
	}
	column->Add(new TextView(sy->T("ModsOrder", "Highest first. Where two mods change the same file, the higher one wins."), ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(8, 0, 8, 8))));

	for (size_t i = 0; i < list.size() && i < ids_.size(); ++i) {
		const Mods::Mod &mod = list[i];
		const std::string id = mod.id;
		LinearLayout *row = column->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		if (mod.unusable.empty()) {
			CheckBox *check = row->Add(new CheckBox(&enabled_[i], mod.name, "", new LinearLayoutParams(1.0f)));
			check->OnClick.Add([this, id, i](UI::EventParams &) {
				Mods::Manager::Instance().SetEnabled(id, enabled_[i]);
				Changed();
			});
		} else {
			// A mod that cannot be turned on has no check box, but still shows its name.
			row->Add(new TextView(mod.name, ALIGN_LEFT, false, new LinearLayoutParams(1.0f, UI::Margins(16, 8))));
		}
		row->Add(new Choice(sy->T("Up"), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)))->OnClick.Add([this, id](UI::EventParams &) {
			Mods::Manager::Instance().Move(id, +1);
			Changed();
		});
		row->Add(new Choice(sy->T("Down"), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)))->OnClick.Add([this, id](UI::EventParams &) {
			Mods::Manager::Instance().Move(id, -1);
			Changed();
		});

		std::string info = mod.type;
		if (!mod.version.empty())
			info += " - " + mod.version;
		if (!mod.author.empty())
			info += " - " + mod.author;
		column->Add(new TextView(info, ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(24, 0, 8, 0))));
		if (!mod.description.empty())
			column->Add(new TextView(mod.description, ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(24, 0, 8, 0))));
		if (!mod.unusable.empty())
			column->Add(new NoticeView(NoticeLevel::WARN, sy->T("Cannot be turned on"), mod.unusable));
		for (const std::string &note : mod.notes)
			column->Add(new TextView(note, ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(24, 0, 8, 0))));
		column->Add(new Spacer(8.0f));
	}

	const std::vector<Mods::Resolution::Conflict> conflicts = mods.Conflicts();
	if (!conflicts.empty()) {
		column->Add(new ItemHeader(sy->T("Conflicts")));
		for (const Mods::Resolution::Conflict &conflict : conflicts) {
			char file[16];
			snprintf(file, sizeof(file), "%04X", conflict.file);
			std::string text = std::string(file) + ": ";
			if (!conflict.winner.empty())
				text += mods.ModName(conflict.winner) + " wins";
			for (const std::string &lost : conflict.overridden)
				text += ", " + mods.ModName(lost) + " is overridden";
			for (const std::string &patcher : conflict.patched_by)
				text += ", patched by " + mods.ModName(patcher);
			column->Add(new TextView(text, ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, UI::Margins(8, 2, 8, 2))));
		}
	}
}
