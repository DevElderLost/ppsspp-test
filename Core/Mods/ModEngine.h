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

// Ported from Yakumo (profiles/mhp3rd/host/mods), MIT License,
// Copyright (c) 2026 PSPRecomp contributors, Copyright (c) 2026 Yakumo contributors.
// The MIT notice is in Core/Mods/LICENSE-Yakumo.txt.

#pragma once

// Mods for Monster Hunter Portable 3rd HD (NPJB-40001): the mods folder (PSP/MODS in the memory
// stick) read in the community's format (ModLibrary.h), served to the game through its DATA.BIN
// reads (DataBin.h). The ISO file system asks Serving() and reads the disc as before when no mod
// changes anything, so with no mod on, not a byte the game reads is different.
//
// The choices (which mods are on, their order) are in PSP/MODS/mods.ini.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Core/Mods/DataBin.h"
#include "Core/Mods/ModLibrary.h"

namespace Mods {

// The disc id of the one game whose archive this knows.
bool IsSupportedGameId(const std::string &discId);

// DATA.BIN's directory, read through `raw` (bytes at an offset inside the archive as they are on the
// disc). `bytes` is the archive's size. Nothing when it does not parse as this game's archive.
std::optional<p3rd::Directory> ReadDirectory(const p3rd::ArchiveView::RawRead &raw, std::uint64_t bytes);

class Manager {
public:
	static Manager &Instance();

	// The game's disc is open: `raw` reads bytes at an offset inside DATA.BIN as they are on the
	// disc, `bytes` is its size there. Reads the mods folder and the choices, and starts serving
	// what they add up to. Returns whether the archive was understood.
	bool AttachDisc(const void *owner, p3rd::ArchiveView::RawRead raw, std::uint64_t bytes, const Path &modsFolder);
	// Only the file system that attached can detach, so that one that is gone does not stop another.
	void DetachDisc(const void *owner);
	bool Attached() const;

	// The game has been loaded into memory and has not run yet. Installs the code mods that are on: what they
	// write is written to guest memory now, as the mhp3reload loader would at the start of the game. Needs the
	// disc id (only the HD version is known) and the size of the guest RAM. Works before and without DATA.BIN.
	void StartGame(const Path &modsFolder, const std::string &discId, std::uint32_t ramBytes);
	void StopGame();

	// Whether the mods change what the game reads from DATA.BIN now. Cheap.
	bool Serving() const;
	// DATA.BIN's size as the game sees it: larger when a mod's file grew it.
	std::uint64_t DataBinSize() const;
	// Reads DATA.BIN as the game sees it, at an offset into the archive.
	std::size_t Read(std::uint64_t offset, std::uint8_t *out, std::size_t size);

	// The game flushed its instruction cache, which it does right after copying a code overlay into
	// place: the point where an overlay has finished loading. Writes the patches that go to memory
	// outside a loaded overlay.
	void OnCodeLoaded();

	// For the Mods screen. Works without a game too: then it only reads and writes the choices.
	void Reload(const Path &modsFolder);
	std::vector<Mod> ModList() const;
	ModChoice Choice(const std::string &id) const;
	bool Master() const;
	void SetMaster(bool on);
	void SetEnabled(const std::string &id, bool on);
	void Move(const std::string &id, int delta);
	// Saves the choices and, with a game running, applies them when the game can take it.
	void Commit();
	std::vector<Resolution::Conflict> Conflicts() const;
	std::string ModName(const std::string &id) const;

	// "Applies at the next load" or "Restart the game to apply": what the last change needs.
	std::string PendingNote() const;
	// Problems met while serving a mod's file this run, newest last.
	std::vector<std::string> Problems() const;

	// The state; defined in the .cpp.
	struct State;

private:
	Manager();
	State *s_;
};

// A short message for the player, shown on the screen while the game runs (the log level of most phones
// hides the engine's own messages, so what matters is also said here).
enum class NoticeKind { Info, Success, Error };
void ShowNotice(NoticeKind kind, const std::string &title, const std::string &text);

// Guest memory, which the engine writes after an overlay loads. Kept apart so that the rest of the
// engine can be tested without a running PSP.
bool GuestMemoryMatches(std::uint32_t address, const std::uint8_t *expected, std::size_t size);
// Returns whether it was written; an address outside the game's memory is not.
bool GuestMemoryWrite(std::uint32_t address, const std::uint8_t *data, std::size_t size);

}  // namespace Mods
