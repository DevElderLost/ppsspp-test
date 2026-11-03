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
#include <atomic>
#include <cmath>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "GPU/GPU.h"
#include "GPU/Common/FillScreen.h"
#include "GPU/Common/PresentationCommon.h"

namespace FillScreen {
namespace {

// Monster Hunter Portable 3rd HD Ver. (NPJB-40001), as in the Yakumo host's camera/game_aspect.cpp.
//
// The camera's initialisation (0x0882D5A4) copies four constants into the camera object: near 30, far 65000,
// aspect 480/272 and a vertical field of view of 0.8722 radians. The projection (0x0882CF0C) and the culling
// planes (0x0882BF1C) are built from those fields, and the camera's update (0x0882D150) builds them again only
// when the field of view differs from the one they were last built with, kept at +0x10.
constexpr u32 kAspectConstant = 0x08969F74u;
constexpr u32 kGameAspectBits = 0x3FE1E1E2u;  // 480/272
// The culling planes put the frustum's side corners at depth 1.5 times the far distance. That leaves the view
// uncovered once the aspect ratio passes about 3, so beyond that the depth factor shrinks to keep both inside.
constexpr u32 kCullDepthConstant = 0x08969ED4u;
constexpr u32 kGameCullDepthBits = 0xBFC00000u;  // -1.5
constexpr float kCullBudget = 4.0f;              // cot^2(0.8722 / 2) is 4.6: some margin kept
constexpr u32 kCameraPointer = 0x08A2F958u;
constexpr u32 kNear = 0x00u;
constexpr u32 kFar = 0x04u;
constexpr u32 kAspect = 0x08u;
constexpr u32 kFieldOfView = 0x0Cu;
constexpr u32 kBuiltFieldOfView = 0x10u;
constexpr u32 kCameraBytes = 0x11D0u;

struct CodeWord {
	u32 address;
	u32 word;
};

// Everything the aspect ratio relies on in the game's code, checked before anything is written.
constexpr CodeWord kSignature[] = {
	{ 0x0882D5D0u, 0xC4409F74u },  // lwc1 f0,-0x608c(v0): the camera's initialisation reads the aspect constant
	{ 0x0882D5D4u, 0xE4800008u },  // swc1 f0,0x8(a0): into the camera's aspect
	{ 0x0882D7A8u, 0xAC44F958u },  // sw a0,-0x6a8(v0): the camera's address, kept at 0x08A2F958
	{ 0x0882CF1Cu, 0xC48C000Cu },  // lwc1 f12,0xc(a0): the projection's field of view
	{ 0x0882CF20u, 0xC48D0008u },  // lwc1 f13,0x8(a0): the projection's aspect
	{ 0x0882CE88u, 0x46170DC3u },  // div.s f23,f1,f23: the projection's x scale is cot(fov/2) / aspect
	{ 0x0882D18Cu, 0xC4810010u },  // lwc1 f1,0x10(a0): the camera update reads the field of view last built
	{ 0x0882D190u, 0xC480000Cu },  // lwc1 f0,0xc(a0): and the current one
	{ 0x0882D194u, 0x46000832u },  // c.eq.s f1,f0: and builds the projection again when they differ
	{ 0x0882BF64u, 0xC4850008u },  // lwc1 f5,0x8(a0): the culling planes read the aspect
	{ 0x0882BF74u, 0xC4439ED4u },  // lwc1 f3,-0x612c(v0): and the depth factor constant
};

constexpr float kPspAspect = 480.0f / 272.0f;
// A movie is over this many flips after its last video draw.
constexpr int kMovieHoldFlips = 45;
// How often (in frames) the game's code is looked at again while it has not been recognised.
constexpr int kRecheckFrames = 60;

enum class Support {
	UNKNOWN,  // Not recognised (yet): another game or version, or the game is not in memory yet.
	READY,    // The game's code matches.
};

// Only touched by the emulation thread.
struct State {
	Support support = Support::UNKNOWN;
	// The port has written something since the game last had its own values.
	bool changed = false;
	int framesToCheck = 0;
	float applied = 0.0f;
};

State state;

std::atomic<bool> g_active{ false };
std::atomic<float> g_aspect{ kPspAspect };
std::atomic<bool> g_targetDisplayed{ false };
std::atomic<int> g_lastVideoFlip{ -1000000 };

bool Load32(u32 address, u32 *out) {
	if (!Memory::IsValid4AlignedAddress(address))
		return false;
	*out = Memory::ReadUnchecked_U32(address);
	return true;
}

bool LoadFloat(u32 address, float *out) {
	u32 bits;
	if (!Load32(address, &bits))
		return false;
	memcpy(out, &bits, sizeof(bits));
	return true;
}

void Store32(u32 address, u32 value) {
	if (Memory::IsValid4AlignedAddress(address))
		Memory::WriteUnchecked_U32(value, address);
}

void StoreFloat(u32 address, float value) {
	u32 bits;
	memcpy(&bits, &value, sizeof(bits));
	Store32(address, bits);
}

// Checks the game's code, and that the two constants are the game's (or ones this module wrote earlier, as a
// save state made with Fill screen on holds them).
bool CheckGame() {
	for (const CodeWord &expected : kSignature) {
		u32 found;
		if (!Load32(expected.address, &found) || found != expected.word)
			return false;
	}
	float aspect, depth;
	if (!LoadFloat(kAspectConstant, &aspect) || !LoadFloat(kCullDepthConstant, &depth))
		return false;
	return std::isfinite(aspect) && aspect >= 0.5f && aspect <= 5.0f && std::isfinite(depth) && depth <= -0.1f && depth >= -1.5f;
}

// The camera, when the pointer leads to one that looks like it.
u32 CameraAddress() {
	u32 address;
	if (!Load32(kCameraPointer, &address) || address == 0u || !Memory::IsValidRange(address, kCameraBytes))
		return 0u;
	float nearPlane, farPlane, aspect, fov;
	if (!LoadFloat(address + kNear, &nearPlane) || !LoadFloat(address + kFar, &farPlane) ||
		!LoadFloat(address + kAspect, &aspect) || !LoadFloat(address + kFieldOfView, &fov))
		return 0u;
	const bool plausible = std::isfinite(nearPlane) && std::isfinite(farPlane) && nearPlane > 0.0f && farPlane > nearPlane &&
		aspect > 0.25f && aspect < 8.0f && fov > 0.01f && fov < 3.1f;
	return plausible ? address : 0u;
}

bool MoviePlaying() {
	const int last = g_lastVideoFlip.load(std::memory_order_relaxed);
	const int now = gpuStats.totals.numFlips;
	return now >= last && now - last < kMovieHoldFlips;
}

// Writes the aspect ratio into the game, or puts the game's own back. Returns whether the view is widened,
// and then the aspect ratio it was given in *widenedAspect.
bool ApplyToGame(float aspect, bool wanted, float *widenedAspect) {
	const float game = kPspAspect;
	// Within a hair of the game's own shape is the game's own shape, written bit for bit as the game has it.
	const bool own = !wanted || !std::isfinite(aspect) || std::abs(aspect - game) < 0.002f;

	u32 aspectBits = 0, depthBits = 0;
	Load32(kAspectConstant, &aspectBits);
	Load32(kCullDepthConstant, &depthBits);
	// A save state can bring back values from another session.
	const bool modified = aspectBits != kGameAspectBits || depthBits != kGameCullDepthBits;
	if (own && !state.changed && !modified)
		return false;
	if (modified)
		state.changed = true;

	const float target = own ? game : std::clamp(aspect, 0.5f, 5.0f);
	float targetDepth = 0.0f;
	u32 targetAspectBits, targetDepthBits;
	if (own) {
		targetAspectBits = kGameAspectBits;
		targetDepthBits = kGameCullDepthBits;
	} else {
		targetDepth = -std::min(1.5f, kCullBudget / target);
		memcpy(&targetAspectBits, &target, sizeof(u32));
		memcpy(&targetDepthBits, &targetDepth, sizeof(u32));
	}
	if (aspectBits != targetAspectBits || depthBits != targetDepthBits) {
		Store32(kAspectConstant, targetAspectBits);
		Store32(kCullDepthConstant, targetDepthBits);
	}
	if (state.applied != target) {
		INFO_LOG(Log::G3D, "[fillscreen] the game's view is %f wide to 1 high", target);
		state.applied = target;
	}

	// The camera took its aspect from the constant when it was set up. Give it the new one and make the next
	// camera update build the projection and the culling planes again: it does when the field of view it last
	// built them with differs from the current one.
	u32 camera = CameraAddress();
	if (camera != 0u) {
		float cameraAspect, fov;
		if (LoadFloat(camera + kAspect, &cameraAspect) && LoadFloat(camera + kFieldOfView, &fov) && cameraAspect != target) {
			StoreFloat(camera + kAspect, target);
			StoreFloat(camera + kBuiltFieldOfView, -fov);
		}
		camera = CameraAddress();
	}
	// Back to the game's own values once the camera has them too. Until a camera turns up this keeps looking,
	// which costs one read a frame.
	u32 cameraBits = 0;
	if (camera != 0u)
		Load32(camera + kAspect, &cameraBits);
	state.changed = !own || camera == 0u || cameraBits != kGameAspectBits;
	*widenedAspect = target;
	return !own;
}

}  // namespace

void Reset() {
	state = State();
	g_active.store(false);
	g_aspect.store(kPspAspect);
	g_targetDisplayed.store(false);
	g_lastVideoFlip.store(-1000000);
}

void BeginFrame() {
	const bool requested = g_Config.bFillScreen && !g_Config.bSoftwareRendering;

	// Which game this is, and whether it has what the aspect ratio needs. While it is not known this is asked
	// now and then; once known, the game's code is looked at every frame (a few reads), so that nothing is ever
	// written into another game that was started since.
	if (state.support == Support::READY) {
		if (!CheckGame()) {
			state = State();
			g_active.store(false);
			return;
		}
	} else {
		if (!requested) {
			g_active.store(false);
			return;
		}
		if (state.framesToCheck > 0) {
			state.framesToCheck--;
			g_active.store(false);
			return;
		}
		state.framesToCheck = kRecheckFrames;
		if (!CheckGame()) {
			g_active.store(false);
			return;
		}
		state.support = Support::READY;
		INFO_LOG(Log::G3D, "[fillscreen] recognised Monster Hunter Portable 3rd HD Ver.");
	}

	const DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
	const float pixelW = (float)PSP_CoreParameter().pixelWidth;
	const float pixelH = (float)PSP_CoreParameter().pixelHeight;
	float aspect = kPspAspect;
	bool shapeUsable = pixelW > 0.0f && pixelH > 0.0f;
	if (shapeUsable) {
		const FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, pixelW, pixelH);
		shapeUsable = frame.w > 0.0f && frame.h > 0.0f;
		if (shapeUsable)
			aspect = frame.w / frame.h;
	}
	// The stretching branch of the display layout only follows the display when the layout is landscape on a
	// landscape display; anything else keeps the PSP's shape, and so does the game.
	const bool landscape = g_display.GetDeviceOrientation() == DeviceOrientation::Landscape && !config.InternalRotationIsPortrait();
	const bool wanted = requested && shapeUsable && landscape && !config.bEnableCardboardVR && !MoviePlaying();

	float widenedAspect = kPspAspect;
	const bool widened = ApplyToGame(aspect, wanted, &widenedAspect);
	if (widened)
		g_aspect.store(widenedAspect);
	g_active.store(widened);
}

bool Active() {
	return g_active.load(std::memory_order_relaxed);
}

void SetTargetDisplayed(bool displayed) {
	g_targetDisplayed.store(displayed, std::memory_order_relaxed);
}

void NoteVideoDraw() {
	g_lastVideoFlip.store(gpuStats.totals.numFlips, std::memory_order_relaxed);
}

bool GetInterfaceFit(float *fitX, float *fitY) {
	if (!g_active.load(std::memory_order_relaxed) || !g_targetDisplayed.load(std::memory_order_relaxed))
		return false;
	// The screen is spread over a picture of this shape: each PSP pixel is wider (or taller) than square.
	const float x = g_aspect.load(std::memory_order_relaxed) / kPspAspect;
	if (!(x > 0.0f))
		return false;
	// Shrink the interface by the larger of the two factors, so it is as large as square pixels allow.
	*fitX = x > 1.0f ? 1.0f / x : 1.0f;
	*fitY = x < 1.0f ? x : 1.0f;
	return *fitX < 0.999f || *fitY < 0.999f;
}

}  // namespace FillScreen
