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
#include <atomic>
#include <memory>
#include <mutex>
#include <set>

#include "Common/Log.h"
#include "Core/Mods/ModEngine.h"

namespace Mods {

using p3rd::ArchiveView;
using p3rd::Directory;
using p3rd::Layout;

bool IsSupportedGameId(const std::string &discId) {
	return discId == "NPJB40001";
}

std::optional<Directory> ReadDirectory(const ArchiveView::RawRead &raw, std::uint64_t bytes) {
	// The directory is small: its first word says how many blocks it takes.
	std::vector<std::uint8_t> head(p3rd::kBlock);
	head.resize(raw(0u, head));
	std::vector<std::uint8_t> first(head);
	p3rd::decrypt(first, 0u, 0u);
	const std::uint32_t blocks = first.size() >= 4u
		? static_cast<std::uint32_t>(first[0] | first[1] << 8u | first[2] << 16u | first[3] << 24u)
		: 0u;
	if (blocks == 0u || blocks >= 512u)
		return std::nullopt;
	std::vector<std::uint8_t> encrypted(static_cast<size_t>(blocks) * p3rd::kBlock);
	encrypted.resize(raw(0u, encrypted));
	return Directory::parse(encrypted, bytes);
}

struct Manager::State {
	mutable std::mutex lock;

	Path modsFolder;
	p3rd::ModFolderFormat format{0u};
	std::unique_ptr<ModLibrary> library;
	bool loaded = false;

	// With a game.
	const void *owner = nullptr;
	bool attached = false;
	ArchiveView::RawRead raw;
	std::uint64_t bytes = 0;
	std::optional<Directory> directory;
	std::unique_ptr<ArchiveView> view;
	std::unique_ptr<FileOverlay> overlay;
	std::shared_ptr<const Layout> layout;  // null: the disc's own archive
	Resolution active;
	bool serving = false;
	bool restartPending = false;
	bool switchedLive = false;
	std::optional<Resolution> waiting;  // a switch waiting for a file's load to end
	std::optional<FileId> partial;      // a file the game has begun but not finished reading
	std::set<FileId> loadedCode;        // overlays read in full with memory writes due
	std::atomic<bool> pendingCode{false};  // loadedCode is not empty; checked without the lock
	std::set<FileId> reported;          // files whose problems were logged
	std::vector<std::string> problems;
};

Manager::Manager() : s_(new State()) {
	s_->library.reset(new ModLibrary(s_->format));
}

Manager &Manager::Instance() {
	static Manager instance;
	return instance;
}

// ---- Helpers on the locked state ---------------------------------------------------------------

typedef Manager::State State;

static std::string ModNameLocked(State &s, const std::string &id) {
	if (const Mod *mod = s.library->Find(id))
		return mod->name;
	return id;
}

static std::map<FileId, std::uint64_t> SizesFor(State &s, const Resolution &resolution) {
	FileOverlay sizing(FileOverlay::Game{{}, [&s](FileId file) { return s.directory->size(file); }, {}});
	sizing.Set(resolution);
	return sizing.Sizes();
}

static std::shared_ptr<const Layout> LayoutFor(State &s, const Resolution &resolution) {
	if (resolution.empty() || !s.directory)
		return nullptr;
	return std::make_shared<const Layout>(Layout::build(*s.directory, SizesFor(s, resolution)));
}

static void LogResolution(State &s, const Resolution &resolution, const Layout *layout) {
	if (resolution.empty()) {
		INFO_LOG(Log::System, "[mods] no mod changes the game's files");
		return;
	}
	INFO_LOG(Log::System, "[mods] %d file(s) replaced, %d patched", (int)resolution.replacements.size(), (int)resolution.patches.size());
	if (layout != nullptr && layout->moved(*s.directory)) {
		INFO_LOG(Log::System, "[mods] DATA.BIN grows from %d to %d bytes", (int)s.directory->archive_bytes(), (int)layout->directory.archive_bytes());
	}
	for (const auto &entry : resolution.replacements) {
		INFO_LOG(Log::System, "[mods]   %s <- %s (%s)", s.format.FileName(entry.first).c_str(), entry.second.path.ToVisualString().c_str(), ModNameLocked(s, entry.second.mod).c_str());
	}
	for (const auto &entry : resolution.patches) {
		for (const Resolution::Source &source : entry.second)
			INFO_LOG(Log::System, "[mods]   %s patched by %s (%s)", s.format.FileName(entry.first).c_str(), source.path.ToVisualString().c_str(), ModNameLocked(s, source.mod).c_str());
	}
}

static void ActivateNow(State &s, const Resolution &resolution) {
	s.waiting.reset();
	if (!s.overlay || !s.view)
		return;
	s.overlay->Set(resolution);
	s.layout = LayoutFor(s, resolution);
	s.view->set(s.layout, s.overlay.get());
	s.serving = s.layout != nullptr;
	s.reported.clear();
	s.loadedCode.clear();
	s.pendingCode.store(false, std::memory_order_relaxed);
	s.active = resolution;
	LogResolution(s, resolution, s.layout.get());
}

static bool CanSwitch(State &s, const Resolution &next) {
	if (!s.directory)
		return false;
	const std::shared_ptr<const Layout> wanted = LayoutFor(s, next);
	const Layout disc = Layout::build(*s.directory, {});
	const Layout &now = s.layout ? *s.layout : disc;
	return (wanted ? *wanted : disc).same_as(now);
}

// Whether switching to `next` would change a file the game is halfway through reading. Streamed
// music is read a piece at a time all along, so only the files the switch changes count.
static bool SwitchWaits(State &s, const Resolution &next) {
	if (!s.serving || !s.partial)
		return false;
	const FileId file = *s.partial;
	return s.overlay->Touches(file) || next.replacements.count(file) || next.patches.count(file);
}

static void Activate(State &s, const Resolution &next) {
	if (s.overlay && s.overlay->GetResolution().SameFiles(next))
		return;
	// A file the game is halfway through reading keeps its bytes to the end.
	if (SwitchWaits(s, next)) {
		s.waiting = next;
		return;
	}
	s.switchedLive = true;
	ActivateNow(s, next);
}

static void NoteProblems(State &s, FileId file) {
	if (s.reported.count(file))
		return;
	s.reported.insert(file);
	const std::shared_ptr<const FileContent> content = s.overlay->Content(file);
	if (!content)
		return;
	for (const std::string &problem : content->problems) {
		const std::string line = s.format.FileName(file) + ": " + problem;
		WARN_LOG(Log::System, "[mods] %s", line.c_str());
		s.problems.push_back(line);
	}
}

// ---- With a game ---------------------------------------------------------------------------------

bool Manager::AttachDisc(const void *owner, ArchiveView::RawRead raw, std::uint64_t bytes, const Path &modsFolder) {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	s.owner = owner;
	s.attached = false;
	s.view.reset();
	s.overlay.reset();
	s.layout.reset();
	s.directory.reset();
	s.serving = false;
	s.restartPending = false;
	s.switchedLive = false;
	s.waiting.reset();
	s.partial.reset();
	s.loadedCode.clear();
	s.reported.clear();
	s.problems.clear();
	s.active = Resolution();
	s.raw = std::move(raw);
	s.bytes = bytes;

	s.directory = ReadDirectory(s.raw, s.bytes);
	if (!s.directory) {
		ERROR_LOG(Log::System, "[mods] DATA.BIN's directory could not be read; mods are off");
		return false;
	}
	s.format.SetEntries(static_cast<std::uint32_t>(s.directory->entries()));
	// The view and the overlay read the state's own members, which outlive them.
	State *self = &s;
	s.view.reset(new ArchiveView(*s.directory, [self](std::uint64_t offset, Span<std::uint8_t> out) -> std::size_t {
		if (!self->raw || offset >= self->bytes)
			return 0u;
		const auto count = static_cast<size_t>(std::min<std::uint64_t>(out.size(), self->bytes - offset));
		return self->raw(offset, out.first(count));
	}));
	s.overlay.reset(new FileOverlay(FileOverlay::Game{
		[self](FileId file) { return self->view->original(file); },
		[self](FileId file) { return self->directory->size(file); },
		[](FileId, std::vector<std::uint8_t> &data, const Path &patch) { return p3rd::ApplyPatch(data, patch); }}));
	s.attached = true;
	s.modsFolder = modsFolder;

	s.library->LoadChoices(modsFolder / "mods.ini");
	s.library->Scan(modsFolder);
	s.loaded = true;
	ActivateNow(s, s.library->Resolve());
	s.switchedLive = false;
	return true;
}

void Manager::DetachDisc(const void *owner) {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	if (s.owner != owner)
		return;
	s.owner = nullptr;
	s.attached = false;
	s.serving = false;
	s.view.reset();
	s.overlay.reset();
	s.layout.reset();
	s.directory.reset();
	s.raw = nullptr;
	s.active = Resolution();
	s.waiting.reset();
	s.partial.reset();
	s.loadedCode.clear();
}

bool Manager::Attached() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->attached;
}

bool Manager::Serving() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->serving;
}

std::uint64_t Manager::DataBinSize() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	if (s.view && s.serving)
		return s.view->size();
	return s.bytes;
}

std::size_t Manager::Read(std::uint64_t offset, std::uint8_t *out, std::size_t size) {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	if (!s.attached || !s.view)
		return 0u;
	Span<std::uint8_t> span(out, size);
	if (!s.serving) {
		if (s.waiting && !SwitchWaits(s, *s.waiting))
			ActivateNow(s, *s.waiting);
		if (!s.serving)
			return s.raw && offset < s.bytes ? s.view->read(offset, span, nullptr) : 0u;
	} else if (s.waiting && !SwitchWaits(s, *s.waiting)) {
		ActivateNow(s, *s.waiting);
	}

	std::vector<FileId> touched;
	const std::size_t count = s.view->read(offset, span, &touched);
	if (count == 0u || !s.layout)
		return count;

	// Which file the read ended in, and whether the game has all of it now.
	const Directory &d = s.layout->directory;
	const std::uint64_t end = offset + count;
	s.partial.reset();
	const std::int64_t last = d.entry_at(static_cast<std::uint32_t>((end - 1u) / p3rd::kBlock));
	if (last >= 0) {
		const auto file = static_cast<FileId>(last);
		const std::uint64_t fileEnd = static_cast<std::uint64_t>(d.blocks[file]) * p3rd::kBlock + d.size(file);
		if (end < fileEnd) {
			s.partial = file;
		} else if (s.overlay->Touches(file)) {
			const auto content = s.overlay->Content(file);
			if (content && !content->after_load.empty()) {
				s.loadedCode.insert(file);
				s.pendingCode.store(true, std::memory_order_relaxed);
			}
		}
	}
	FileId previous = ~FileId{};
	for (const FileId file : touched) {
		if (file == previous)
			continue;
		previous = file;
		NoteProblems(s, file);
	}
	return count;
}

void Manager::OnCodeLoaded() {
	// The game flushes its instruction cache often; this costs one load when nothing is due.
	if (!s_->pendingCode.load(std::memory_order_relaxed))
		return;
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	if (s.loadedCode.empty() || !s.overlay) {
		s.pendingCode.store(false, std::memory_order_relaxed);
		return;
	}
	for (const FileId file : s.loadedCode) {
		const std::shared_ptr<const FileContent> content = s.overlay->Content(file);
		if (!content)
			continue;
		const std::optional<p3rd::OverlayImage> image = p3rd::GetOverlayImage(content->bytes);
		if (!image)
			continue;
		// Only if that overlay is the one in its slot now.
		if (!GuestMemoryMatches(image->load, content->bytes.data(), 8u))
			continue;
		for (const MemoryWrite &write : content->after_load) {
			GuestMemoryWrite(write.address, write.bytes.data(), write.bytes.size());
			INFO_LOG(Log::System, "[mods] after loading %s: wrote %d bytes at 0x%08x", s.format.FileName(file).c_str(), (int)write.bytes.size(), write.address);
		}
	}
	s.loadedCode.clear();
	s.pendingCode.store(false, std::memory_order_relaxed);
}

// ---- For the screen ------------------------------------------------------------------------------

void Manager::Reload(const Path &modsFolder) {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	s.modsFolder = modsFolder;
	s.library->LoadChoices(modsFolder / "mods.ini");
	s.library->Scan(modsFolder);
	s.loaded = true;
}

std::vector<Mod> Manager::ModList() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->library->List();
}

ModChoice Manager::Choice(const std::string &id) const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->library->Choice(id);
}

bool Manager::Master() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->library->Master();
}

void Manager::SetMaster(bool on) {
	std::lock_guard<std::mutex> guard(s_->lock);
	s_->library->SetMaster(on);
}

void Manager::SetEnabled(const std::string &id, bool on) {
	std::lock_guard<std::mutex> guard(s_->lock);
	s_->library->SetEnabled(id, on);
}

void Manager::Move(const std::string &id, int delta) {
	std::lock_guard<std::mutex> guard(s_->lock);
	s_->library->Move(id, delta);
}

void Manager::Commit() {
	std::lock_guard<std::mutex> guard(s_->lock);
	State &s = *s_;
	std::string error;
	if (!s.library->SaveChoices(s.modsFolder / "mods.ini", error))
		ERROR_LOG(Log::System, "[mods] %s", error.c_str());
	if (!s.attached)
		return;
	const Resolution wanted = s.library->Resolve();
	if (wanted.SameFiles(s.active)) {
		s.active = wanted;  // the conflicts may read differently
		s.restartPending = false;
		return;
	}
	if (CanSwitch(s, wanted)) {
		s.restartPending = false;
		Activate(s, wanted);
	} else {
		s.restartPending = true;
	}
}

std::vector<Resolution::Conflict> Manager::Conflicts() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->library->Resolve().conflicts;
}

std::string Manager::ModName(const std::string &id) const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return ModNameLocked(*s_, id);
}

std::string Manager::PendingNote() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	const State &s = *s_;
	if (s.restartPending)
		return "Restart the game to apply: a file changes size in a way the running game cannot take.";
	if (s.waiting)
		return "Applies once the game finishes loading the file it is reading.";
	if (s.switchedLive)
		return "Applied: each file changes the next time the game loads it, usually at the next area or menu. Files it loads only once at start need a restart.";
	return std::string();
}

std::vector<std::string> Manager::Problems() const {
	std::lock_guard<std::mutex> guard(s_->lock);
	return s_->problems;
}

}  // namespace Mods
