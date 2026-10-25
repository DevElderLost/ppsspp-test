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

#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/Mods/ModEngine.h"

namespace Mods {

bool GuestMemoryMatches(std::uint32_t address, const std::uint8_t *expected, std::size_t size) {
	if (!Memory::IsValidRange(address, (u32)size))
		return false;
	const u8 *at = Memory::GetPointerUnchecked(address);
	for (size_t i = 0; i < size; ++i) {
		if (at[i] != expected[i])
			return false;
	}
	return true;
}

bool GuestMemoryWrite(std::uint32_t address, const std::uint8_t *data, std::size_t size) {
	if (size == 0 || !Memory::IsValidRange(address, (u32)size))
		return false;
	Memory::MemcpyUnchecked(address, data, (u32)size);
	// Patched code must not run from a stale translation.
	if (currentMIPS)
		currentMIPS->InvalidateICacheRangeDeferred(address, (int)size);
	return true;
}

}  // namespace Mods
