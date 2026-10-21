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

// Taking DATA.BIN apart: every file in the archive, decrypted, in a folder. Each is named by its file
// id (four hex digits, 0601), which is also how a mod names the file it replaces, so a file changed
// here can go straight into a mod folder. entries.txt lists them all.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "Common/File/Path.h"
#include "Core/Mods/DataBin.h"

namespace Mods {

struct DumpOptions {
	std::function<void(float)> progress;     // 0 to 1, after each file
	const std::atomic<bool> *cancel = nullptr;  // set it to stop between two files
};

struct DumpStats {
	std::size_t entries = 0;   // in the archive
	std::size_t written = 0;   // files written, the empty ones are only listed
	std::uint64_t bytes = 0;   // written
	bool cancelled = false;
	std::string error;         // empty unless it failed
};

// Where it goes unless told otherwise: the dump folder of the memory stick, in DATA.BIN.
Path DefaultDumpFolder();

// From an archive read through `raw`, which gives bytes at an offset inside it as they are on the disc.
bool DumpArchive(const p3rd::ArchiveView::RawRead &raw, std::uint64_t archiveBytes, const Path &destination,
	const DumpOptions &options, DumpStats *stats);

// From a game's disc image. Opens it on its own, so the running game is not in the way, and checks it is
// the game this knows. The game's own files are written, whatever mods are on.
bool DumpDisc(const Path &gamePath, const Path &destination, const DumpOptions &options, DumpStats *stats);

}  // namespace Mods
