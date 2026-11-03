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

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/ViewGroup.h"

#include "UI/SimpleDialogScreen.h"

// The mods in PSP/MODS: which are on, and their order, where a higher one wins a file two mods
// both change. See Core/Mods. Works with or without a game running; with Monster Hunter Portable 3rd
// HD running, changes apply to the running game when it can take them.
class ModsScreen : public UITwoPaneBaseDialogScreen {
public:
	explicit ModsScreen(const Path &gamePath)
		: UITwoPaneBaseDialogScreen(gamePath, TwoPaneFlags::SettingsCanScroll | TwoPaneFlags::ContentsCanScroll) {}

	const char *tag() const override { return "Mods"; }

protected:
	std::string_view GetTitle() const override;
	void BeforeCreateViews() override;
	void CreateSettingsViews(UI::ViewGroup *parent) override;
	void CreateContentViews(UI::ViewGroup *parent) override;

private:
	void Changed();

	std::vector<std::string> ids_;
	std::unique_ptr<bool[]> enabled_;
	bool master_ = true;
};
