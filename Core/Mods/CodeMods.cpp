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

// Follows the behaviour of the mhp3reload mod loader (GPLv3), written from its format description and
// its source. No code of the loader is used. See Core/Mods/NOTICE-mhp3reload.txt.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Core/Mods/CodeMods.h"

namespace Mods::p3rd {

static std::uint32_t Read32(const std::vector<std::uint8_t> &data, size_t at) {
	return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1u]) << 8u |
		static_cast<std::uint32_t>(data[at + 2u]) << 16u | static_cast<std::uint32_t>(data[at + 3u]) << 24u;
}

static void Put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
	for (unsigned i = 0; i < 4u; ++i)
		out.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
}

static std::string Hex8(std::uint32_t value) {
	char text[16];
	snprintf(text, sizeof(text), "0x%08X", value);
	return text;
}

std::optional<CodeMod> ParseCodeMod(const std::vector<std::uint8_t> &file, std::string *why) {
	const auto fail = [why](std::string reason) -> std::optional<CodeMod> {
		if (why != nullptr)
			*why = std::move(reason);
		return std::nullopt;
	};
	// The format version: the four characters "0.01". The loader skips a file that has another.
	if (file.size() < 4u || std::memcmp(file.data(), "0.01", 4u) != 0)
		return fail("It is not a mod file this knows: the format version is not 0.01.");

	CodeMod mod;
	size_t at = 4u;
	bool haveMain = false;
	while (at + 4u <= file.size()) {
		const std::uint32_t typeWord = Read32(file, at);
		at += 4u;
		// The loader looks at the low half of the word, as a signed number: below zero ends the file, and so
		// does a type it does not know.
		const auto type = static_cast<std::int16_t>(typeWord & 0xFFFFu);
		if (type == 0) {
			if (at + 8u > file.size())
				return fail("A patch block is cut off.");
			CodeBlock block;
			block.kind = CodeBlock::Kind::Patch;
			block.address = Read32(file, at);
			const std::uint32_t lengthWord = Read32(file, at + 4u);
			at += 8u;
			if ((lengthWord & 0x80000000u) != 0u)
				return fail("A patch block runs its own code as it loads, which is not supported here yet.");
			const std::uint32_t length = lengthWord & 0x7FFFFFFFu;
			if (length > file.size() - at)
				return fail("A patch block is longer than the file.");
			block.bytes.assign(file.begin() + static_cast<std::ptrdiff_t>(at), file.begin() + static_cast<std::ptrdiff_t>(at + length));
			at += length;
			mod.blocks.push_back(std::move(block));
		} else if (type == 1) {
			if (at + 8u > file.size())
				return fail("A main block is cut off.");
			CodeBlock block;
			block.kind = CodeBlock::Kind::Main;
			const std::uint32_t lengthWord = Read32(file, at);
			std::memcpy(block.id.data(), file.data() + at + 4u, 4u);
			at += 8u;
			if ((lengthWord & 0x80000000u) != 0u)
				return fail("A main block runs its own code as it loads, which is not supported here yet.");
			const std::uint32_t length = lengthWord & 0x7FFFFFFFu;
			if (length > file.size() - at)
				return fail("A main block is longer than the file.");
			block.bytes.assign(file.begin() + static_cast<std::ptrdiff_t>(at), file.begin() + static_cast<std::ptrdiff_t>(at + length));
			at += length;
			haveMain = true;
			mod.blocks.push_back(std::move(block));
		} else if (type == 2) {
			if (at + 8u > file.size())
				return fail("A hook block is cut off.");
			CodeBlock block;
			block.kind = CodeBlock::Kind::Hook;
			block.address = Read32(file, at);
			block.offset = static_cast<std::int16_t>(static_cast<std::uint16_t>(file[at + 4u] | file[at + 5u] << 8u));
			block.op = file[at + 6u];
			block.nops = file[at + 7u];
			at += 8u;
			if (!haveMain)
				return fail("A hook block comes before any main block, so it has no code to jump to.");
			if (block.op != 0x08 && block.op != 0x0C)
				return fail("A hook block has an operation that is neither j nor jal.");
			mod.blocks.push_back(std::move(block));
		} else if (type == 3) {
			return fail("It has an init block, which runs its own code as it loads. That is not supported here yet.");
		} else {
			break;  // the end block, or a block type the loader does not know
		}
	}
	if (mod.blocks.empty())
		return fail("The mod file holds no blocks.");
	return mod;
}

CodeLayout HdCodeLayout(std::uint32_t ramBytes) {
	CodeLayout layout;
	layout.entryTable = 0x083B5600u;
	layout.maxEntries = 0x800u / 8u;  // "reserve 256 entries"
	layout.loadStart = layout.entryTable + 0x800u;
	layout.loadEnd = layout.entryTable + 0x48700u;  // "289.75kb free"
	layout.ramStart = 0x08000000u;
	layout.ramEnd = 0x08000000u + ramBytes;
	return layout;
}

CodePlan PlanCodeMods(const std::vector<CodeInput> &mods, const CodeLayout &layout) {
	CodePlan plan;
	std::vector<std::pair<std::array<char, 4>, std::uint32_t>> table;  // the entry table, as the loader builds it
	std::uint32_t loadAt = layout.loadStart;
	const auto inRam = [&layout](std::uint32_t address, std::uint64_t size) {
		return address >= layout.ramStart && static_cast<std::uint64_t>(address) + size <= layout.ramEnd;
	};

	for (const CodeInput &input : mods) {
		if (input.mod == nullptr)
			continue;
		// A hook belongs to the main block read last in its file.
		std::optional<std::uint32_t> current;
		for (const CodeBlock &block : input.mod->blocks) {
			switch (block.kind) {
			case CodeBlock::Kind::Main: {
				const auto found = std::find_if(table.begin(), table.end(), [&block](const auto &entry) { return entry.first == block.id; });
				if (found != table.end()) {
					current = found->second;  // already loaded: the mod shares that code
					break;
				}
				if (table.size() >= layout.maxEntries) {
					plan.problems.push_back(input.name + ": the table of loaded mods is full");
					current.reset();
					break;
				}
				if (loadAt > layout.loadEnd || block.bytes.size() > layout.loadEnd - loadAt) {
					plan.problems.push_back(input.name + ": there is no room left for its code");
					current.reset();
					break;
				}
				table.emplace_back(block.id, loadAt);
				plan.loaded.emplace_back(std::string(block.id.data(), 4u), loadAt);
				plan.writes.push_back({loadAt, block.bytes});
				current = loadAt;
				loadAt += static_cast<std::uint32_t>(block.bytes.size());
				plan.loadedBytes += static_cast<std::uint32_t>(block.bytes.size());
				break;
			}
			case CodeBlock::Kind::Patch:
				if (!inRam(block.address, block.bytes.size())) {
					plan.problems.push_back(input.name + ": a patch block for " + Hex8(block.address) + " is outside the game's memory");
					break;
				}
				plan.writes.push_back({block.address, block.bytes});
				break;
			case CodeBlock::Kind::Hook: {
				if (!current) {
					plan.problems.push_back(input.name + ": the hook for " + Hex8(block.address) + " has no code to jump to");
					break;
				}
				const std::uint64_t span = 4u + static_cast<std::uint64_t>(block.nops) * 4u;
				if (!inRam(block.address, span)) {
					plan.problems.push_back(input.name + ": a hook for " + Hex8(block.address) + " is outside the game's memory");
					break;
				}
				// As the loader does: the target is the main block's address plus the signed offset, shifted right by
				// two, with the operation in the top byte.
				const std::uint32_t target = *current + static_cast<std::uint32_t>(static_cast<std::int32_t>(block.offset));
				const std::uint32_t word = (target >> 2) | (static_cast<std::uint32_t>(block.op) << 24u);
				CodeWrite write;
				write.address = block.address;
				Put32(write.bytes, word);
				for (unsigned i = 0; i < block.nops; ++i)
					Put32(write.bytes, 0u);
				plan.writes.push_back(std::move(write));
				break;
			}
			}
		}
	}

	// The table the game's code can read to find a mod by its id: ids and addresses, ended by a zero id.
	if (!table.empty()) {
		CodeWrite entries;
		entries.address = layout.entryTable;
		for (const auto &entry : table) {
			entries.bytes.insert(entries.bytes.end(), entry.first.begin(), entry.first.end());
			Put32(entries.bytes, entry.second);
		}
		Put32(entries.bytes, 0u);
		Put32(entries.bytes, 0u);
		plan.writes.insert(plan.writes.begin(), std::move(entries));
	}
	return plan;
}

}  // namespace Mods::p3rd
