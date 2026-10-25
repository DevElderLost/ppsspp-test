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

// Code mods for Monster Hunter Portable 3rd: the mod files of the mhp3reload mod loader
// (Kurogami2134, GPLv3), installed by PPSSPP itself instead of by a loader running inside the game.
//
// The loader is a piece of PSP code that the game's EBOOT is patched to call early in its start. It reads the
// mod files and, for each block in them:
//
//   Patch block (0)  writes its bytes at a guest address.
//   Main block  (1)  copies its bytes to the loader's load area, one after the other, and records the
//                    4 character mod id and where it went in a table the game's code can read.
//                    A main block whose id is in the table already is skipped.
//   Hook block  (2)  writes a j or jal to (the main block's address + an offset) at a guest address,
//                    followed by some nops. It belongs to the last main block read in that file.
//   Init block  (3)  copies code to the load area and calls it.
//   End block (-1)   ends the file.
//
// This does the same writes, without running the loader: the mod files are read here and the result is
// written to guest memory when the game starts, before it runs. What it does not do is call code at load
// time: a main or patch block marked "run as it loads", and init blocks, make a mod unusable here.
// The file format is described in the loader's Readme.md and modfile.bt.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Mods::p3rd {

struct CodeBlock {
	enum class Kind { Patch, Main, Hook };
	Kind kind{Kind::Patch};
	std::uint32_t address{};            // Patch: where it goes. Hook: the instruction to replace.
	std::array<char, 4> id{};           // Main: the mod id.
	std::vector<std::uint8_t> bytes;    // Patch and Main: the content.
	std::int16_t offset{};              // Hook: from the main block's address (signed, as the loader reads it).
	std::uint8_t op{};                  // Hook: 8 for j, 0xC for jal.
	std::uint8_t nops{};                // Hook: nops written after the jump.
};

struct CodeMod {
	std::vector<CodeBlock> blocks;
};

// Reads a mod file in the loader's format. Nothing when it cannot be used here, with the reason in *why.
std::optional<CodeMod> ParseCodeMod(const std::vector<std::uint8_t> &file, std::string *why);

// Where the loader keeps things in the game's memory, which the mods are built for.
struct CodeLayout {
	std::uint32_t entryTable{};  // pairs of (mod id, address), ended by a zero id
	std::uint32_t loadStart{};   // where main blocks go
	std::uint32_t loadEnd{};     // the end of the free memory
	std::uint32_t maxEntries{};
	std::uint32_t ramStart{};
	std::uint32_t ramEnd{};
};

// The layout of the HD version (NPJB-40001): the loader's MOD_ENTRY_ADD 0x083B5600, 256 entries, the load
// area after them, 289.75 KB of free memory in all.
CodeLayout HdCodeLayout(std::uint32_t ramBytes);

struct CodeWrite {
	std::uint32_t address{};
	std::vector<std::uint8_t> bytes;
};

struct CodePlan {
	std::vector<CodeWrite> writes;                                  // in the order the loader makes them
	std::vector<std::pair<std::string, std::uint32_t>> loaded;      // mod id -> where its main block went
	std::vector<std::string> problems;                              // what was left out, and why
	std::uint32_t loadedBytes{};
};

struct CodeInput {
	std::string name;  // for messages
	const CodeMod *mod{};
};

// What the loader would write for these mod files, loaded in this order (a later one's writes win).
CodePlan PlanCodeMods(const std::vector<CodeInput> &mods, const CodeLayout &layout);

}  // namespace Mods::p3rd
