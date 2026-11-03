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
#include <cstdio>
#include <cstring>

#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/Mods/DataBinDump.h"
#include "Core/Mods/ModEngine.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"

namespace Mods {

Path DefaultDumpFolder() {
	return GetSysDirectory(DIRECTORY_DUMP) / "DATA.BIN";
}

bool DumpArchive(const p3rd::ArchiveView::RawRead &raw, std::uint64_t archiveBytes, const Path &destination,
	const DumpOptions &options, DumpStats *stats) {
	DumpStats local;
	DumpStats &st = stats != nullptr ? *stats : local;
	st = DumpStats();

	const std::optional<p3rd::Directory> directory = ReadDirectory(raw, archiveBytes);
	if (!directory) {
		st.error = "DATA.BIN's directory could not be read";
		return false;
	}
	st.entries = directory->entries();
	if (!File::CreateFullPath(destination)) {
		st.error = "Cannot create " + destination.ToString();
		return false;
	}

	std::uint64_t total = 0;
	for (std::uint32_t i = 0; i < directory->entries(); ++i)
		total += directory->size(i);
	std::uint64_t done = 0;

	p3rd::ArchiveView view(*directory, raw);
	std::string listing =
		"# The files of DATA.BIN, decrypted as the game reads them. A file is named by its id: the\n"
		"# four hex digits are the entry number, which is what a mod's Target says. To replace one, put\n"
		"# a changed copy with the same name in a mod folder (and Type=File, Version=HD in its mod.ini).\n"
		"# id  first block  size in bytes\n";
	for (std::uint32_t i = 0; i < directory->entries(); ++i) {
		if (options.cancel != nullptr && options.cancel->load()) {
			st.cancelled = true;
			return false;
		}
		char name[16];
		snprintf(name, sizeof(name), "%04X", i);
		const std::uint64_t size = directory->size(i);
		char line[96];
		snprintf(line, sizeof(line), "%s %u %llu\n", name, directory->blocks[i], (unsigned long long)size);
		listing += line;
		if (size > 0u) {
			const std::vector<std::uint8_t> data = view.original(i);
			if (data.size() != size) {
				st.error = std::string("The disc could not be read for ") + name;
				return false;
			}
			if (!File::WriteDataToFile(false, data.data(), data.size(), destination / name)) {
				st.error = std::string("Cannot write ") + (destination / name).ToString();
				return false;
			}
			++st.written;
			st.bytes += size;
		}
		done += size;
		if (options.progress)
			options.progress(total > 0u ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f);
	}
	if (!File::WriteDataToFile(true, listing.data(), listing.size(), destination / "entries.txt")) {
		st.error = "Cannot write entries.txt";
		return false;
	}
	return true;
}

}  // namespace Mods
