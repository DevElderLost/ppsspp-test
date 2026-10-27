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

// The mods folder, what each mod in it changes, which ones the player has turned on and in what
// order, and what that adds up to: for each game file, the replacement that wins and the patches
// that apply. Then the bytes a game file has once the enabled mods are applied (FileOverlay).
//
// A game says how a mod folder describes itself and how its files are numbered (ModFormat);
// the folder format of the one game this supports, Monster Hunter Portable 3rd HD, is at the end.
//
// Left out of the Yakumo version: armour and weapon models that the player assigns to a piece of
// equipment (a picker is needed), and code mods. Those mods are listed, and cannot be turned on.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/File/Path.h"
#include "Core/Mods/CodeMods.h"

namespace Mods {

// A file of the game's own, as the game's mods address it: an entry of DATA.BIN.
using FileId = std::uint32_t;

std::string Lower(std::string_view text);
bool IEquals(std::string_view a, std::string_view b);
std::string Trim(std::string_view text);
std::vector<std::string> SplitList(std::string_view value, char separator = ';');

// An .ini file as mod.ini and mods.ini use it: [Section], key=value, ; and # comments, values
// that may be in double quotes, names that are not case sensitive.
class IniFile {
public:
	using Entries = std::vector<std::pair<std::string, std::string>>;
	struct Section {
		std::string name;
		Entries entries;
	};

	static IniFile Parse(std::string_view text);
	static std::optional<IniFile> Load(const Path &path);

	bool HasSection(std::string_view name) const;
	const std::string *Find(std::string_view section, std::string_view key) const;  // the last one wins
	std::string Get(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
	std::vector<std::string> Sections() const;

private:
	const Section *FindSection(std::string_view name) const;
	std::vector<Section> sections_;
};

// One thing a mod does to one game file.
struct FileChange {
	enum class Kind { Replace, Patch };
	Kind kind{Kind::Replace};
	FileId file{};
	Path source;  // the mod's own file
};

struct Mod {
	std::string id;  // the folder's name; the key its choices are saved under
	Path folder;
	std::string name;
	std::string author;
	std::string type;         // as shown: "Files", "Patch", "Pack", ...
	std::string description;  // may hold line breaks
	std::string version;      // what the mod says it was made for, as shown
	std::vector<FileChange> changes;
	std::vector<std::string> members;  // a pack: the mods it turns on and off
	std::vector<std::string> depends;  // mods it needs, turned on with it
	std::vector<std::string> notes;    // parts that do nothing here, and why
	std::shared_ptr<const p3rd::CodeMod> code;  // a code mod: what it writes to the game's memory at start
	std::string unusable;              // why it cannot be turned on; empty when it can
};

// What a game supplies: how a folder describes a mod, and how files are named.
class ModFormat {
public:
	virtual ~ModFormat() = default;
	// The mod in `folder`, or nothing when the folder is not one.
	virtual std::optional<Mod> Read(const Path &folder) const = 0;
	// "0601": how the game's mods spell a file.
	virtual std::string FileName(FileId file) const = 0;
	virtual std::optional<FileId> ParseFile(const std::string &text) const = 0;
};

// The player's choices for one mod.
struct ModChoice {
	bool enabled{};
	int rank{};  // higher wins where two mods change the same file
};

// What the enabled mods add up to.
struct Resolution {
	struct Source {
		std::string mod;  // Mod::id
		Path path;
		bool operator==(const Source &o) const { return mod == o.mod && path == o.path; }
	};
	struct Conflict {
		FileId file{};
		std::string winner;                   // the replacement used; empty when only patches
		std::vector<std::string> overridden;  // replacements that lost, highest first
		std::vector<std::string> patched_by;  // patches applied to it, in order
	};
	std::map<FileId, Source> replacements;          // the winning replacement per file
	std::map<FileId, std::vector<Source>> patches;  // applied in this order: the last wins
	std::vector<Conflict> conflicts;
	// Code mods that are on, in the order they are installed (the lowest priority first, so that the highest
	// one's writes come last and win).
	std::vector<std::string> code;

	bool empty() const { return replacements.empty() && patches.empty(); }
	// The same code mods in the same order.
	bool SameCode(const Resolution &other) const { return code == other.code; }
	// Same files from the same sources; conflicts are not compared.
	bool SameFiles(const Resolution &other) const {
		return replacements == other.replacements && patches == other.patches;
	}
};

class ModLibrary {
public:
	explicit ModLibrary(const ModFormat &format) : format_(&format) {}

	// Reads every folder in `folder` (not recursively). A folder that is not a mod is skipped.
	// Choices of mods that are gone are kept, not shown.
	void Scan(const Path &folder);
	// The saved choices. A missing file means no mod is on.
	void LoadChoices(const Path &file);
	bool SaveChoices(const Path &file, std::string &error) const;

	// Highest priority first.
	const std::vector<Mod> &List() const { return mods_; }
	const Mod *Find(const std::string &id) const;
	ModChoice Choice(const std::string &id) const;
	bool Enabled(const std::string &id) const { return Choice(id).enabled; }

	// On also turns on the mods it depends on; a pack turns its members on and off with it.
	// A mod that cannot be used stays off.
	void SetEnabled(const std::string &id, bool on);
	// +1 raises the mod's priority by one place, -1 lowers it.
	void Move(const std::string &id, int delta);

	// The switch for all mods at once.
	bool Master() const { return master_; }
	void SetMaster(bool on) { master_ = on; }

	Resolution Resolve() const;
	const ModFormat &Format() const { return *format_; }

private:
	void Sort();

	const ModFormat *format_;
	std::vector<Mod> mods_;
	std::map<std::string, ModChoice> choices_;
	bool master_{true};
};

// A write a patch makes to guest memory outside the file itself, done once the game has finished
// loading the file.
struct MemoryWrite {
	std::uint32_t address{};
	std::vector<std::uint8_t> bytes;
};

struct PatchOutcome {
	std::size_t applied{};                // parts of the patch applied to the file's bytes
	std::vector<MemoryWrite> after_load;  // parts to write to memory after the load
	std::vector<std::string> problems;    // parts skipped, and why
};

struct FileContent {
	std::vector<std::uint8_t> bytes;
	bool replaced{};
	std::size_t patches{};  // patch files applied
	std::vector<MemoryWrite> after_load;
	std::vector<std::string> problems;
};

// The bytes a game file has once the enabled mods are applied: a replacement's, or the game's
// own, with the patches applied in order.
class FileOverlay {
public:
	struct Game {
		// The game's own bytes of a file.
		std::function<std::vector<std::uint8_t>(FileId)> original;
		// Its size, without reading it.
		std::function<std::uint64_t(FileId)> original_size;
		// Applies one patch file to a file's bytes.
		std::function<PatchOutcome(FileId, std::vector<std::uint8_t> &, const Path &)> patch;
	};

	explicit FileOverlay(Game game) : game_(std::move(game)) {}

	// Uses `resolution` from now on; contents built for the previous one are dropped.
	void Set(Resolution resolution);
	const Resolution &GetResolution() const { return resolution_; }

	// Whether the mods change this file at all.
	bool Touches(FileId file) const { return resolution_.replacements.count(file) || resolution_.patches.count(file); }
	// The size the file has with the mods, without building it: a replacement's size, or the
	// original's (patches never resize a file).
	std::optional<std::uint64_t> Size(FileId file) const;
	// Every file the mods change, with its size.
	std::map<FileId, std::uint64_t> Sizes() const;

	// Built on first use and kept. Null when the mods do not change the file, or a replacement
	// cannot be read (the game then gets its own file).
	std::shared_ptr<const FileContent> Content(FileId file);

private:
	Game game_;
	Resolution resolution_;
	std::map<FileId, std::shared_ptr<const FileContent>> cache_;
};

}  // namespace Mods

// The mod folders this game's community already uses, so that mods made for mhp3reload and its mod
// manager work unchanged:
//
// - A mod is a folder with a mod.ini whose [MOD INFO] section names it, its Type, its Files, and for
//   file and patch mods the Target file ids.
// - A file id is a DATA.BIN entry index in four upper-case hex digits.
// - A folder without a mod.ini that holds files named by file id (0601) and patches named by file
//   id and P (0601P), as mhp3reload's own files folder does, is taken as one mod.
namespace Mods::p3rd {

class ModFolderFormat final : public ModFormat {
public:
	// `entries`: how many files DATA.BIN has; targets beyond it are refused. 0 when not known yet.
	explicit ModFolderFormat(std::uint32_t entries = 0u) : entries_(entries) {}
	void SetEntries(std::uint32_t entries) { entries_ = entries; }

	std::optional<Mod> Read(const Path &folder) const override;
	std::string FileName(FileId file) const override;
	std::optional<FileId> ParseFile(const std::string &text) const override;

private:
	std::uint32_t entries_;
};

// Applies one patch file to a file's bytes. A patch is a list of blocks, each a guest address, a
// length and that many bytes, ended by the address FFFFFFFF.
//
// - A block for an address inside a code overlay's image (the overlay's header gives where it
//   loads) changes those bytes of the file, which the game copies to that address unchanged.
// - A block for other guest memory is written there once the overlay has loaded (after_load).
// - A block whose address is smaller than the file is taken as an offset into the file, for files
//   that load to no fixed address.
// - A block marked to run as it loads (the top bit of its length) is code, which is skipped and
//   reported.
PatchOutcome ApplyPatch(std::vector<std::uint8_t> &bytes, const Path &patch);

// A code overlay's header at the start of a file: where it loads, and its image size (header, code
// and data). Nothing when the file is not one.
struct OverlayImage {
	std::uint32_t load{};
	std::uint32_t code_end{};  // guest address after the code
	std::uint32_t size{};
};
std::optional<OverlayImage> GetOverlayImage(const std::vector<std::uint8_t> &bytes);

}  // namespace Mods::p3rd
