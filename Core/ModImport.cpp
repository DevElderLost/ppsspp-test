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

#include <algorithm>
#include <cctype>
#include <ctime>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Core/ModImport.h"
#include "Core/System.h"

namespace ModImport {

static std::string Lower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return out;
}

static std::string Trim(std::string_view text) {
	size_t a = 0, b = text.size();
	while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n'))
		a++;
	while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n'))
		b--;
	return std::string(text.substr(a, b - a));
}

Path GetModsFolder() {
	return GetSysDirectory(DIRECTORY_PSP) / "MODS";
}

bool IsIdFileName(const std::string &name) {
	std::string stem = name;
	if (stem.size() == 5 && (stem.back() == 'P' || stem.back() == 'p'))
		stem.pop_back();
	if (stem.size() != 4)
		return false;
	return std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

void ParseModInfo(const std::string &iniText, std::string *name, std::string *author) {
	name->clear();
	author->clear();
	size_t pos = 0;
	// A UTF-8 byte order mark.
	if (iniText.size() >= 3 && (unsigned char)iniText[0] == 0xEF && (unsigned char)iniText[1] == 0xBB && (unsigned char)iniText[2] == 0xBF)
		pos = 3;
	bool inInfo = false;
	while (pos < iniText.size()) {
		size_t end = iniText.find('\n', pos);
		if (end == std::string::npos)
			end = iniText.size();
		const std::string line = Trim(std::string_view(iniText).substr(pos, end - pos));
		pos = end + 1;
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[') {
			const size_t close = line.find(']');
			inInfo = close != std::string::npos && Lower(Trim(line.substr(1, close - 1))) == "mod info";
			continue;
		}
		if (!inInfo)
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Lower(Trim(line.substr(0, eq)));
		std::string value = Trim(line.substr(eq + 1));
		// A value may be in double quotes; a quote left open runs to the end of the line.
		if (!value.empty() && value[0] == '"') {
			value.erase(0, 1);
			if (!value.empty() && value.back() == '"')
				value.pop_back();
		}
		if (key == "name")
			*name = value;
		else if (key == "author")
			*author = value;
	}
}

static std::vector<File::FileInfo> List(const Path &folder) {
	std::vector<File::FileInfo> files;
	File::GetFilesInDir(folder, &files);
	std::sort(files.begin(), files.end(), [](const File::FileInfo &a, const File::FileInfo &b) { return a.name < b.name; });
	return files;
}

static std::vector<File::FileInfo> Subfolders(const Path &folder) {
	std::vector<File::FileInfo> result;
	for (const File::FileInfo &info : List(folder)) {
		if (info.isDirectory && !info.name.empty() && info.name[0] != '.')
			result.push_back(info);
	}
	return result;
}

// Whether the folder is a mod, and what it says about itself.
static bool ReadMod(const Path &folder, Candidate *out) {
	const std::vector<File::FileInfo> files = List(folder);
	for (const File::FileInfo &info : files) {
		if (!info.isDirectory && Lower(info.name) == "mod.ini") {
			std::string text;
			File::ReadBinaryFileToString(info.fullName, &text);
			ParseModInfo(text, &out->name, &out->author);
			if (out->name.empty())
				out->name = folder.GetFilename();
			return true;
		}
	}
	// No mod.ini: files named by the file id they replace, in the folder or in its files folder.
	bool found = false;
	std::vector<File::FileInfo> candidates = files;
	for (const File::FileInfo &info : files) {
		if (info.isDirectory && Lower(info.name) == "files") {
			for (const File::FileInfo &inner : List(info.fullName))
				candidates.push_back(inner);
		}
	}
	for (const File::FileInfo &info : candidates) {
		if (!info.isDirectory && IsIdFileName(info.name)) {
			found = true;
			break;
		}
	}
	if (found) {
		out->name = folder.GetFilename();
		out->author.clear();
	}
	return found;
}

Check CheckImport(const Path &chosen, const Path &modsFolder) {
	Check check;
	check.chosen = chosen;
	if (!File::IsDirectory(chosen)) {
		check.problem = "This is not a folder.";
		return check;
	}
	if (chosen.StartsWith(modsFolder)) {
		check.problem = "This folder is in the mods folder already.";
		return check;
	}
	const auto add = [&](const Path &folder) {
		// A mod that is in the mods folder already is not imported onto itself.
		if (folder.StartsWith(modsFolder))
			return false;
		Candidate candidate;
		if (!ReadMod(folder, &candidate))
			return false;
		candidate.folder = folder;
		candidate.id = folder.GetFilename();
		// A name that is not a plain folder name cannot be a place in the mods folder.
		if (candidate.id.empty() || candidate.id[0] == '.' || candidate.id.find('/') != std::string::npos || candidate.id.find('\\') != std::string::npos)
			return false;
		candidate.replaces = File::Exists(modsFolder / candidate.id);
		check.mods.push_back(std::move(candidate));
		return true;
	};
	if (add(chosen))
		return check;
	for (const File::FileInfo &info : Subfolders(chosen))
		add(info.fullName);
	if (check.mods.empty()) {
		for (const File::FileInfo &info : Subfolders(chosen)) {
			for (const File::FileInfo &inner : Subfolders(info.fullName))
				add(inner.fullName);
		}
	}
	if (check.mods.empty()) {
		check.problem = "No mod here: a mod is a folder with a mod.ini, or with files named by the file id they replace. Unpack a downloaded archive first.";
	}
	return check;
}

static bool CopyRecursive(const Path &from, const Path &to) {
	if (!File::CreateFullPath(to))
		return false;
	bool ok = true;
	for (const File::FileInfo &info : List(from)) {
		if (info.isDirectory) {
			ok = CopyRecursive(info.fullName, to / info.name) && ok;
		} else {
			ok = File::Copy(info.fullName, to / info.name) && ok;
		}
	}
	return ok;
}

static std::string Timestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
#if defined(_WIN32)
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &tm);
	return text;
}

Result Import(const Check &check, const Path &modsFolder) {
	Result result;
	if (!File::CreateFullPath(modsFolder)) {
		result.error = "Cannot create " + modsFolder.ToString();
		return result;
	}
	const std::string stamp = Timestamp();
	for (const Candidate &candidate : check.mods) {
		const Path target = modsFolder / candidate.id;
		if (File::Exists(target)) {
			// Nothing is deleted: the installed mod is copied to the backup folder first, and only
			// removed from its place when that worked.
			const Path backup = modsFolder / ".backup" / (candidate.id + "-" + stamp);
			if (!CopyRecursive(target, backup)) {
				if (result.error.empty())
					result.error = "Cannot move the installed " + candidate.id + " aside";
				File::DeleteDirRecursively(backup);
				continue;
			}
			if (!File::DeleteDirRecursively(target)) {
				if (result.error.empty())
					result.error = "Cannot remove the installed " + candidate.id;
				continue;
			}
			result.backups.push_back(backup);
		}
		if (!CopyRecursive(candidate.folder, target)) {
			if (result.error.empty())
				result.error = "Cannot copy " + candidate.id;
			// No half a mod is left behind.
			File::DeleteDirRecursively(target);
			continue;
		}
		INFO_LOG(Log::System, "Imported mod %s", candidate.id.c_str());
		result.imported.push_back(candidate.id);
	}
	return result;
}

}  // namespace ModImport
