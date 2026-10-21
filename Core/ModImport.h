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

// Importing mods the player downloaded and unpacked, ported from Yakumo's "Import mod...".
//
// The mod folders found in a chosen folder are copied into the mods folder (PSP/MODS in the memory
// stick). Nothing is deleted: a mod folder of the same name that is already there moves to
// MODS/.backup first.
//
// A mod is a folder with a mod.ini, or a folder (or its "files" folder) that holds files named by
// the id of the file they replace: four hex digits, with a trailing P for a patch (0601, 0601P), as in
// the mhp3reload mod loader's files folder.

#include <string>
#include <vector>

#include "Common/File/Path.h"

namespace ModImport {

struct Candidate {
	Path folder;         // Where it is now.
	std::string id;      // Its folder name, and so its name in the mods folder.
	std::string name;    // From the mod.ini, or the folder name.
	std::string author;  // From the mod.ini, may be empty.
	bool replaces = false;  // A mod of that name is installed already.
};

struct Check {
	Path chosen;
	std::vector<Candidate> mods;
	std::string problem;  // Why nothing can be imported. Empty when something can.
};

struct Result {
	std::vector<std::string> imported;  // Ids.
	std::vector<Path> backups;          // Where replaced mods went.
	std::string error;                  // The first failure. The rest were still tried.
};

// The folder the mods live in.
Path GetModsFolder();

// The mods in `chosen`: the folder itself when it is a mod, otherwise the mod folders in it, or one
// level further down (an unpacked archive often wraps its mods in a folder of its own).
Check CheckImport(const Path &chosen, const Path &modsFolder);

// Copies the candidates into `modsFolder`. Blocking; mods are small.
Result Import(const Check &check, const Path &modsFolder);

// Whether a file name is a mod file named by its file id: "0601" or "0601P".
bool IsIdFileName(const std::string &name);

// Pulls Name and Author out of the [MOD INFO] section of a mod.ini.
void ParseModInfo(const std::string &iniText, std::string *name, std::string *author);

}  // namespace ModImport
