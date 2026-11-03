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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/FileSystem.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/Loaders.h"
#include "Core/Mods/DataBinDump.h"
#include "Core/Mods/ModEngine.h"

namespace Mods {

// Bytes straight off the disc.
static size_t ReadDiscBytes(BlockDevice *device, std::uint64_t position, std::uint8_t *out, size_t size) {
	size_t done = 0;
	std::uint8_t sector[2048];
	while (done < size) {
		const std::uint32_t secNum = (std::uint32_t)((position + done) / 2048);
		const size_t inSector = (size_t)((position + done) & 2047);
		if (inSector == 0 && size - done >= 2048) {
			const size_t sectors = (size - done) / 2048;
			device->ReadBlocks(secNum, (int)sectors, out + done);
			done += sectors * 2048;
			continue;
		}
		const size_t count = std::min<size_t>(size - done, 2048 - inSector);
		if (!device->ReadBlock(secNum, sector))
			memset(sector, 0, sizeof(sector));
		memcpy(out + done, sector + inSector, count);
		done += count;
	}
	return done;
}

bool DumpDisc(const Path &gamePath, const Path &destination, const DumpOptions &options, DumpStats *stats) {
	DumpStats local;
	DumpStats &st = stats != nullptr ? *stats : local;
	st = DumpStats();
	if (gamePath.empty()) {
		st.error = "No game is selected";
		return false;
	}

	std::unique_ptr<FileLoader> loader(ConstructFileLoader(gamePath));
	std::string blockError;
	std::shared_ptr<BlockDevice> device(ConstructBlockDevice(loader.get(), &blockError));
	if (!device) {
		st.error = "This is not a disc image";
		return false;
	}
	SequentialHandleAllocator handles;
	ISOFileSystem iso(&handles, device);

	// Which game it is, from its PARAM.SFO. Not through OpenFile on DATA.BIN: that is where the
	// running game's mods attach, and this must not touch them.
	std::string discId;
	{
		const PSPFileInfo info = iso.GetFileInfo("/PSP_GAME/PARAM.SFO");
		if (info.exists && info.size > 0 && info.size < 1024 * 1024) {
			const int handle = iso.OpenFile("/PSP_GAME/PARAM.SFO", FILEACCESS_READ);
			if (handle >= 0) {
				std::vector<std::uint8_t> sfo((size_t)info.size);
				const size_t got = iso.ReadFile(handle, sfo.data(), info.size);
				iso.CloseFile(handle);
				ParamSFOData paramSFO;
				if (got == sfo.size() && paramSFO.ReadSFO(sfo))
					discId = paramSFO.GetDiscID();
			}
		}
	}
	if (!IsSupportedGameId(discId)) {
		st.error = "Only Monster Hunter Portable 3rd HD (NPJB-40001) has a DATA.BIN this can take apart";
		return false;
	}
	const PSPFileInfo data = iso.GetFileInfo("/PSP_GAME/USRDIR/DATA.BIN");
	if (!data.exists || data.size <= 0) {
		st.error = "The disc has no DATA.BIN";
		return false;
	}
	const std::uint64_t start = (std::uint64_t)data.startSector * 2048ULL;
	const std::uint64_t bytes = (std::uint64_t)data.size;
	BlockDevice *raw = device.get();
	return DumpArchive([raw, start, bytes](std::uint64_t offset, Span<std::uint8_t> out) -> std::size_t {
		if (offset >= bytes)
			return 0u;
		const std::size_t count = (std::size_t)std::min<std::uint64_t>(out.size(), bytes - offset);
		return ReadDiscBytes(raw, start + offset, out.data(), count);
	}, bytes, destination, options, &st);
}

}  // namespace Mods
