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

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <set>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Core/Mods/ModLibrary.h"

namespace Mods {

std::string Lower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return out;
}

bool IEquals(std::string_view a, std::string_view b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
		return std::tolower(x) == std::tolower(y);
	});
}

std::string Trim(std::string_view text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return std::string();
	const auto last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1u));
}

std::vector<std::string> SplitList(std::string_view value, char separator) {
	std::vector<std::string> parts;
	size_t start = 0u;
	while (start <= value.size()) {
		const auto end = value.find(separator, start);
		std::string part = Trim(value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
		if (!part.empty())
			parts.push_back(std::move(part));
		if (end == std::string_view::npos)
			break;
		start = end + 1u;
	}
	return parts;
}

static bool StartsWith(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// `"text"` -> text; `"text` -> text; text -> text.
static std::string Unquote(std::string_view raw) {
	std::string value = Trim(raw);
	if (value.empty() || value.front() != '"')
		return value;
	const auto close = value.find('"', 1u);
	if (close == std::string::npos)
		return Trim(std::string_view(value).substr(1u));
	return value.substr(1u, close - 1u);
}

static bool ReadBytes(const Path &path, std::vector<std::uint8_t> *out) {
	std::string data;
	if (!File::ReadBinaryFileToString(path, &data))
		return false;
	out->assign(data.begin(), data.end());
	return true;
}

IniFile IniFile::Parse(std::string_view text) {
	IniFile file;
	if (StartsWith(text, "\xEF\xBB\xBF"))
		text.remove_prefix(3u);
	Section *current = nullptr;
	const auto openSection = [&file](std::string name) -> Section * {
		for (Section &existing : file.sections_) {
			if (IEquals(existing.name, name))
				return &existing;
		}
		file.sections_.push_back(Section{std::move(name), {}});
		return &file.sections_.back();
	};
	size_t start = 0u;
	while (start < text.size()) {
		auto end = text.find('\n', start);
		if (end == std::string_view::npos)
			end = text.size();
		const std::string line = Trim(text.substr(start, end - start));
		start = end + 1u;
		if (line.empty() || line.front() == ';' || line.front() == '#')
			continue;
		if (line.front() == '[') {
			const auto close = line.find(']');
			current = openSection(Trim(std::string_view(line).substr(1u, close == std::string::npos ? std::string::npos : close - 1u)));
			continue;
		}
		const auto equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		if (current == nullptr)
			current = openSection(std::string());
		current->entries.emplace_back(Trim(std::string_view(line).substr(0u, equals)), Unquote(std::string_view(line).substr(equals + 1u)));
	}
	return file;
}

std::optional<IniFile> IniFile::Load(const Path &path) {
	std::string text;
	if (!File::ReadBinaryFileToString(path, &text))
		return std::nullopt;
	return Parse(text);
}

const IniFile::Section *IniFile::FindSection(std::string_view name) const {
	for (const Section &s : sections_) {
		if (IEquals(s.name, name))
			return &s;
	}
	return nullptr;
}

bool IniFile::HasSection(std::string_view name) const {
	return FindSection(name) != nullptr;
}

const std::string *IniFile::Find(std::string_view sectionName, std::string_view key) const {
	const Section *s = FindSection(sectionName);
	if (s == nullptr)
		return nullptr;
	for (auto it = s->entries.rbegin(); it != s->entries.rend(); ++it) {
		if (IEquals(it->first, key))
			return &it->second;
	}
	return nullptr;
}

std::string IniFile::Get(std::string_view sectionName, std::string_view key, std::string_view fallback) const {
	const std::string *value = Find(sectionName, key);
	return value != nullptr ? *value : std::string(fallback);
}

std::vector<std::string> IniFile::Sections() const {
	std::vector<std::string> names;
	names.reserve(sections_.size());
	for (const Section &s : sections_)
		names.push_back(s.name);
	return names;
}

// ---- The library ----------------------------------------------------------------------------

static const char *const kGeneral = "general";
static const char *const kModPrefix = "mod ";

void ModLibrary::Scan(const Path &folder) {
	mods_.clear();
	std::vector<File::FileInfo> listing;
	File::GetFilesInDir(folder, &listing);
	std::vector<File::FileInfo> folders;
	for (const File::FileInfo &info : listing) {
		if (!info.isDirectory || info.name.empty() || info.name[0] == '.')  // .backup and hidden folders
			continue;
		folders.push_back(info);
	}
	std::sort(folders.begin(), folders.end(), [](const File::FileInfo &a, const File::FileInfo &b) { return a.name < b.name; });
	int top = 0;
	for (const auto &entry : choices_)
		top = std::max(top, entry.second.rank);
	for (const File::FileInfo &info : folders) {
		std::optional<Mod> mod = format_->Read(info.fullName);
		if (!mod)
			continue;
		mod->id = info.name;
		mod->folder = info.fullName;
		// A mod seen for the first time goes above the others: the one added last wins, as players expect.
		if (!choices_.count(mod->id))
			choices_[mod->id].rank = ++top;
		ModChoice &c = choices_[mod->id];
		if (!mod->unusable.empty())
			c.enabled = false;
		mods_.push_back(std::move(*mod));
	}
	Sort();
}

void ModLibrary::Sort() {
	std::stable_sort(mods_.begin(), mods_.end(), [this](const Mod &a, const Mod &b) {
		const int ra = choices_.at(a.id).rank;
		const int rb = choices_.at(b.id).rank;
		if (ra != rb)
			return ra > rb;
		return a.id < b.id;
	});
}

void ModLibrary::LoadChoices(const Path &file) {
	choices_.clear();
	master_ = true;
	const std::optional<IniFile> ini = IniFile::Load(file);
	if (!ini)
		return;
	master_ = ini->Get(kGeneral, "enabled", "1") != "0";
	for (const std::string &section : ini->Sections()) {
		if (!IEquals(std::string_view(section).substr(0, 4), kModPrefix))
			continue;
		const std::string id = section.substr(4);
		ModChoice c;
		c.enabled = ini->Get(section, "enabled") == "1";
		c.rank = atoi(ini->Get(section, "rank", "0").c_str());
		choices_[id] = c;
	}
}

bool ModLibrary::SaveChoices(const Path &file, std::string &error) const {
	std::string text =
		"# Written by the Mods screen: which mods are on, and their order (a higher rank wins where\n"
		"# two mods change the same file). The mods themselves are folders in the mods folder.\n";
	text += std::string("[") + kGeneral + "]\nenabled=" + (master_ ? "1" : "0") + "\n";
	for (const auto &entry : choices_) {
		text += "\n[" + std::string(kModPrefix) + entry.first + "]\nenabled=" + (entry.second.enabled ? "1" : "0") +
			"\nrank=" + std::to_string(entry.second.rank) + "\n";
	}
	if (file.CanNavigateUp())
		File::CreateFullPath(file.NavigateUp());
	if (!File::WriteStringToFile(true, text, file)) {
		error = "cannot write " + file.ToString();
		return false;
	}
	return true;
}

const Mod *ModLibrary::Find(const std::string &id) const {
	for (const Mod &mod : mods_) {
		if (mod.id == id)
			return &mod;
	}
	return nullptr;
}

ModChoice ModLibrary::Choice(const std::string &id) const {
	const auto found = choices_.find(id);
	return found != choices_.end() ? found->second : ModChoice{};
}

void ModLibrary::SetEnabled(const std::string &id, bool on) {
	std::set<std::string> visited;
	// Dependencies and pack members may name each other; each mod once.
	std::function<void(const std::string &, bool)> apply = [&](const std::string &target, bool value) {
		if (!visited.insert(target).second)
			return;
		const Mod *mod = Find(target);
		if (mod == nullptr)
			return;
		if (value && !mod->unusable.empty())
			return;
		choices_[target].enabled = value;
		for (const std::string &member : mod->members)
			apply(member, value);
		if (value) {
			for (const std::string &dependency : mod->depends)
				apply(dependency, true);
		}
	};
	apply(id, on);
}

void ModLibrary::Move(const std::string &id, int delta) {
	const auto it = std::find_if(mods_.begin(), mods_.end(), [&id](const Mod &m) { return m.id == id; });
	if (it == mods_.end())
		return;
	const auto index = static_cast<std::ptrdiff_t>(it - mods_.begin());
	// The list is highest first: raising a mod swaps it with the one above.
	const std::ptrdiff_t other = index - delta;
	if (other < 0 || other >= static_cast<std::ptrdiff_t>(mods_.size()))
		return;
	ModChoice &a = choices_[id];
	ModChoice &b = choices_[mods_[static_cast<size_t>(other)].id];
	if (a.rank == b.rank)
		a.rank += delta;
	else
		std::swap(a.rank, b.rank);
	Sort();
}

Resolution ModLibrary::Resolve() const {
	Resolution result;
	if (!master_)
		return result;
	struct Touch {
		std::vector<std::string> replaced;  // lowest priority first
		std::vector<std::string> patched;
	};
	std::map<FileId, Touch> touches;
	// Lowest priority first, so a later mod's replacement wins and its patch is applied after (over)
	// the earlier ones.
	for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
		const Mod &mod = *it;
		const ModChoice c = Choice(mod.id);
		if (!c.enabled || !mod.unusable.empty())
			continue;
		for (const FileChange &change : mod.changes) {
			Touch &touch = touches[change.file];
			if (change.kind == FileChange::Kind::Replace) {
				result.replacements[change.file] = {mod.id, change.source};
				touch.replaced.erase(std::remove(touch.replaced.begin(), touch.replaced.end(), mod.id), touch.replaced.end());
				touch.replaced.push_back(mod.id);
			} else {
				result.patches[change.file].push_back({mod.id, change.source});
				touch.patched.push_back(mod.id);
			}
		}
	}
	for (const auto &entry : touches) {
		const Touch &touch = entry.second;
		const bool several = touch.replaced.size() > 1u;
		bool foreignPatch = false;
		for (const std::string &patcher : touch.patched)
			foreignPatch = foreignPatch || (!touch.replaced.empty() && patcher != touch.replaced.back());
		if (!several && !foreignPatch)
			continue;
		Resolution::Conflict conflict;
		conflict.file = entry.first;
		if (!touch.replaced.empty())
			conflict.winner = touch.replaced.back();
		for (auto r = touch.replaced.rbegin(); r != touch.replaced.rend(); ++r) {
			if (*r != conflict.winner)
				conflict.overridden.push_back(*r);
		}
		conflict.patched_by = touch.patched;
		result.conflicts.push_back(std::move(conflict));
	}
	return result;
}

// ---- The overlay ----------------------------------------------------------------------------

void FileOverlay::Set(Resolution resolution) {
	resolution_ = std::move(resolution);
	cache_.clear();
}

std::optional<std::uint64_t> FileOverlay::Size(FileId file) const {
	const auto found = resolution_.replacements.find(file);
	if (found != resolution_.replacements.end()) {
		if (!File::Exists(found->second.path))
			return std::nullopt;
		return File::GetFileSize(found->second.path);
	}
	if (resolution_.patches.count(file))
		return game_.original_size(file);
	return std::nullopt;
}

std::map<FileId, std::uint64_t> FileOverlay::Sizes() const {
	std::map<FileId, std::uint64_t> result;
	for (const auto &entry : resolution_.replacements) {
		if (const auto bytes = Size(entry.first))
			result[entry.first] = *bytes;
	}
	for (const auto &entry : resolution_.patches) {
		if (!result.count(entry.first))
			result[entry.first] = game_.original_size(entry.first);
	}
	return result;
}

std::shared_ptr<const FileContent> FileOverlay::Content(FileId file) {
	const auto cached = cache_.find(file);
	if (cached != cache_.end())
		return cached->second;
	if (!Touches(file))
		return nullptr;
	auto content = std::make_shared<FileContent>();
	const auto found = resolution_.replacements.find(file);
	if (found != resolution_.replacements.end()) {
		if (ReadBytes(found->second.path, &content->bytes)) {
			content->replaced = true;
		} else {
			content->problems.push_back("cannot read " + found->second.path.ToString() + "; the game's file is used");
		}
	}
	if (!content->replaced)
		content->bytes = game_.original(file);
	const auto patches = resolution_.patches.find(file);
	if (patches != resolution_.patches.end()) {
		for (const Resolution::Source &source : patches->second) {
			PatchOutcome outcome = game_.patch(file, content->bytes, source.path);
			if (outcome.applied > 0u || !outcome.after_load.empty())
				++content->patches;
			for (MemoryWrite &write : outcome.after_load)
				content->after_load.push_back(std::move(write));
			for (std::string &problem : outcome.problems)
				content->problems.push_back(source.mod + ": " + std::move(problem));
		}
	}
	cache_[file] = content;
	return content;
}

}  // namespace Mods

// ---- The folder format of Monster Hunter Portable 3rd HD -------------------------------------------

namespace Mods::p3rd {

static const char *const kInfo = "MOD INFO";

// Guest RAM on the PSP; addresses given through a cached or uncached mirror are folded onto it first.
static const std::uint32_t kRamStart = 0x08000000u;
static const std::uint32_t kRamEnd = 0x0C000000u;
static const std::uint32_t kMirrorMask = 0x3FFFFFFFu;

static std::string Hex4(std::uint32_t value) {
	char text[16];
	snprintf(text, sizeof(text), "%04X", value);
	return text;
}

static std::string Hex8(std::uint32_t value) {
	char text[16];
	snprintf(text, sizeof(text), "0x%08X", value);
	return text;
}

// A file of the mod, found without regard to case (mods are made on Windows and played on other
// systems too). `relative` may use either slash.
static std::optional<Path> FindFile(const Path &folder, const std::string &relative) {
	Path at = folder;
	std::string rest = relative;
	std::replace(rest.begin(), rest.end(), '\\', '/');
	for (const std::string &part : SplitList(rest, '/')) {
		if (File::Exists(at / part)) {
			at = at / part;
			continue;
		}
		bool found = false;
		std::vector<File::FileInfo> listing;
		File::GetFilesInDir(at, &listing);
		for (const File::FileInfo &info : listing) {
			if (IEquals(info.name, part)) {
				at = info.fullName;
				found = true;
				break;
			}
		}
		if (!found)
			return std::nullopt;
	}
	if (!File::Exists(at) || File::IsDirectory(at))
		return std::nullopt;
	return at;
}

static std::uint32_t Load32(const std::vector<std::uint8_t> &bytes, size_t offset) {
	return static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u |
		static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u | static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u;
}

// "0601" -> 0x601; also without leading zeros, and with 0x.
static std::optional<FileId> ParseHex(std::string text) {
	text = Trim(text);
	if (text.size() > 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		text = text.substr(2);
	if (text.empty() || text.size() > 6u)
		return std::nullopt;
	FileId value{};
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		return std::nullopt;
	return value;
}

// A file named like mhp3reload's files folder has them: 0601 or 0601P.
struct IdFile {
	FileId file{};
	bool patch{};
};

static std::optional<IdFile> IdFileName(const std::string &name) {
	std::string stem = name;
	bool patch = false;
	if (stem.size() == 5u && (stem.back() == 'P' || stem.back() == 'p')) {
		patch = true;
		stem.pop_back();
	}
	if (stem.size() != 4u || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
		return std::nullopt;
	const auto file = ParseHex(stem);
	if (!file)
		return std::nullopt;
	return IdFile{*file, patch};
}

namespace {

class Reader {
public:
	Reader(const ModFolderFormat &format, std::uint32_t entries, const Path &folder, Mod &mod)
		: format_(format), entries_(entries), folder_(folder), mod_(mod) {}

	// Files and targets as the Version key selects them.
	void FileChanges(const IniFile &ini, const std::string &section, const std::string &version, FileChange::Kind kind) {
		std::string files = ini.Get(section, "Files");
		std::string targets = ini.Get(section, "Target");
		if (version == "BOTH") {
			if (const std::string *hd = ini.Find(section, "FilesHD"))
				files = *hd;
			if (const std::string *hd = ini.Find(section, "TargetHD"))
				targets = *hd;
		} else if (version != "HD") {
			Unusable("Made for the PSP version" + std::string(version.empty() ? " (it gives no Version, which means PSP)" : "") +
				": its file ids are not the HD version's.");
			return;
		}
		const std::vector<std::string> names = SplitList(files);
		const std::vector<std::string> ids = SplitList(targets);
		if (names.empty()) {
			Unusable("Its mod.ini lists no files.");
			return;
		}
		if (names.size() != ids.size()) {
			Unusable("Its mod.ini lists " + std::to_string(names.size()) + " files for " + std::to_string(ids.size()) + " targets.");
			return;
		}
		for (size_t i = 0; i < names.size(); ++i) {
			const std::optional<FileId> file = format_.ParseFile(ids[i]);
			if (!file) {
				Unusable("Target \"" + ids[i] + "\" is not a file id.");
				return;
			}
			if (entries_ != 0u && *file >= entries_) {
				Unusable("Target " + format_.FileName(*file) + " is not a file of this game.");
				return;
			}
			const std::optional<Path> source = FindFile(folder_, names[i]);
			if (!source) {
				Unusable("The file \"" + names[i] + "\" is missing from the folder.");
				return;
			}
			mod_.changes.push_back({kind, *file, *source});
		}
	}

	void Unusable(std::string why) {
		if (mod_.unusable.empty())
			mod_.unusable = std::move(why);
	}

private:
	const ModFolderFormat &format_;
	std::uint32_t entries_;
	const Path &folder_;
	Mod &mod_;
};

std::string VersionLabel(const std::string &version) {
	if (version == "HD")
		return "HD version";
	if (version == "BOTH")
		return "PSP and HD versions";
	return "PSP version";
}

// A folder without a mod.ini, holding files named by file id.
std::optional<Mod> ReadIdFiles(const ModFolderFormat &format, std::uint32_t entries, const Path &folder) {
	Mod mod;
	std::vector<Path> places{folder, folder / "files"};
	for (const Path &where : places) {
		std::vector<File::FileInfo> listing;
		File::GetFilesInDir(where, &listing);
		for (const File::FileInfo &info : listing) {
			if (info.isDirectory)
				continue;
			const auto id = IdFileName(info.name);
			if (!id || (entries != 0u && id->file >= entries))
				continue;
			mod.changes.push_back({id->patch ? FileChange::Kind::Patch : FileChange::Kind::Replace, id->file, info.fullName});
		}
	}
	if (mod.changes.empty())
		return std::nullopt;
	std::stable_sort(mod.changes.begin(), mod.changes.end(), [](const FileChange &a, const FileChange &b) { return a.file < b.file; });
	mod.name = folder.GetFilename();
	const bool patches = std::any_of(mod.changes.begin(), mod.changes.end(), [](const FileChange &c) { return c.kind == FileChange::Kind::Patch; });
	const bool files = std::any_of(mod.changes.begin(), mod.changes.end(), [](const FileChange &c) { return c.kind == FileChange::Kind::Replace; });
	mod.type = files && patches ? "Files and patches" : patches ? "Patch" : "Files";
	mod.version = "HD version (assumed)";
	mod.description = "No mod.ini: its files are named by the file id they replace (" + format.FileName(mod.changes.front().file) +
		"), as in mhp3reload's files folder. Taken to be made for the HD version.";
	return mod;
}

}  // namespace

std::string ModFolderFormat::FileName(FileId file) const {
	return Hex4(file);
}

std::optional<FileId> ModFolderFormat::ParseFile(const std::string &text) const {
	return ParseHex(text);
}

std::optional<Mod> ModFolderFormat::Read(const Path &folder) const {
	const std::optional<Path> iniPath = FindFile(folder, "mod.ini");
	if (!iniPath)
		return ReadIdFiles(*this, entries_, folder);
	const std::optional<IniFile> ini = IniFile::Load(*iniPath);
	if (!ini)
		return std::nullopt;

	Mod mod;
	mod.name = ini->Get(kInfo, "Name");
	if (mod.name.empty())
		mod.name = folder.GetFilename();
	mod.author = ini->Get(kInfo, "Author");
	std::string description = ini->Get(kInfo, "Description");
	std::replace(description.begin(), description.end(), '\\', '\n');
	mod.description = std::move(description);
	std::string version = Trim(ini->Get(kInfo, "Version"));
	std::transform(version.begin(), version.end(), version.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	mod.depends = SplitList(ini->Get(kInfo, "Depends"));
	if (ini->Find(kInfo, "Script") != nullptr)
		mod.notes.push_back("Its settings script is not run: scripts are for the PSP mod manager.");
	if (!ini->HasSection(kInfo))
		mod.unusable = "Its mod.ini has no [MOD INFO] section.";

	Reader reader(*this, entries_, folder, mod);
	const std::string type = Trim(ini->Get(kInfo, "Type"));
	mod.version = VersionLabel(version);
	if (IEquals(type, "File")) {
		mod.type = "Files";
		reader.FileChanges(*ini, kInfo, version, FileChange::Kind::Replace);
	} else if (IEquals(type, "Patch")) {
		mod.type = "Patch";
		reader.FileChanges(*ini, kInfo, version, FileChange::Kind::Patch);
	} else if (IEquals(type, "Code")) {
		mod.type = "Code";
		reader.Unusable("A code mod runs its own PSP code inside the game. That is not supported here yet.");
	} else if (IEquals(type, "Pack")) {
		mod.type = "Pack";
		mod.members = SplitList(ini->Get(kInfo, "ModList"));
		mod.version.clear();
		if (mod.members.empty())
			reader.Unusable("The pack lists no mods.");
	} else if (IEquals(type, "PseudoPack")) {
		mod.type = "Pack";
		for (const std::string &sub : SplitList(ini->Get(kInfo, "SubModList"))) {
			const std::string subType = Trim(ini->Get(sub, "Type"));
			Mod part;
			Reader subReader(*this, entries_, folder, part);
			if (IEquals(subType, "File"))
				subReader.FileChanges(*ini, sub, version, FileChange::Kind::Replace);
			else if (IEquals(subType, "Patch"))
				subReader.FileChanges(*ini, sub, version, FileChange::Kind::Patch);
			else
				part.unusable = "its type " + (subType.empty() ? std::string("is missing") : subType) + " cannot be part of a pack here";
			if (!part.unusable.empty()) {
				mod.notes.push_back("Part \"" + sub + "\" is left out: " + part.unusable);
				continue;
			}
			mod.changes.insert(mod.changes.end(), part.changes.begin(), part.changes.end());
		}
		if (mod.changes.empty())
			reader.Unusable("None of its parts can be used here.");
	} else if (type.size() > 5u && IEquals(std::string_view(type).substr(0, 5), "Equip")) {
		mod.type = "Equipment";
		mod.version.clear();
		reader.Unusable("An equipment model mod needs the piece of equipment to be chosen, which is not supported here yet.");
	} else {
		mod.type = type.empty() ? "Unknown" : type;
		reader.Unusable(type.empty() ? "Its mod.ini gives no Type." : "Unknown type " + type + ".");
	}
	return mod;
}

std::optional<OverlayImage> GetOverlayImage(const std::vector<std::uint8_t> &bytes) {
	const size_t kHeader = 64u;
	if (bytes.size() < kHeader || bytes[0] != 'M' || bytes[1] != 'W' || bytes[2] != 'o' || bytes[3] != '3')
		return std::nullopt;
	OverlayImage image;
	image.load = Load32(bytes, 8u);
	image.code_end = image.load + static_cast<std::uint32_t>(kHeader) + Load32(bytes, 12u);
	image.size = static_cast<std::uint32_t>(bytes.size());
	return image;
}

PatchOutcome ApplyPatch(std::vector<std::uint8_t> &bytes, const Path &patch) {
	PatchOutcome outcome;
	std::vector<std::uint8_t> data;
	if (!ReadBytes(patch, &data)) {
		outcome.problems.push_back("cannot read " + patch.GetFilename());
		return outcome;
	}
	const std::optional<OverlayImage> overlay = GetOverlayImage(bytes);
	size_t at = 0u;
	while (at + 8u <= data.size()) {
		const std::uint32_t address = Load32(data, at);
		const std::uint32_t lengthWord = Load32(data, at + 4u);
		if (address == 0xFFFFFFFFu)
			break;
		const std::uint32_t length = lengthWord & 0x7FFFFFFFu;
		if (at + 8u + length > data.size()) {
			outcome.problems.push_back("the block for " + Hex8(address) + " runs past the end of the patch");
			break;
		}
		const auto payload = data.begin() + static_cast<std::ptrdiff_t>(at + 8u);
		at += 8u + length;
		if ((lengthWord & 0x80000000u) != 0u) {
			outcome.problems.push_back("the block for " + Hex8(address) + " is code that runs as it loads, which is not supported");
			continue;
		}
		const std::uint32_t guest = address & kMirrorMask;
		const bool inRam = guest >= kRamStart && guest < kRamEnd;
		if (overlay && inRam && guest >= overlay->load && (std::uint64_t)(guest - overlay->load) + length <= overlay->size) {
			std::copy_n(payload, length, bytes.begin() + (guest - overlay->load));
			++outcome.applied;
		} else if (overlay && inRam) {
			outcome.after_load.push_back({guest, std::vector<std::uint8_t>(payload, payload + length)});
		} else if (!inRam && static_cast<std::uint64_t>(address) + length <= bytes.size()) {
			std::copy_n(payload, length, bytes.begin() + address);
			++outcome.applied;
		} else if (inRam) {
			outcome.problems.push_back("the block for " + Hex8(address) + " writes to memory, but this file does not load to a fixed address");
		} else {
			outcome.problems.push_back("the block for " + Hex8(address) + " is outside the file");
		}
	}
	return outcome;
}

}  // namespace Mods::p3rd
