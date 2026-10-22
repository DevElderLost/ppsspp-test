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

// Planar silhouette shadows, ported from the Yakumo (MHP3rd) host renderer.
//
// The game has no entity IDs in its draw calls, so this is a heuristic:
//  * Skinned (weighted), opaque, depth-writing 3D draws are shadow casters.
//  * Unskinned, depth-writing 3D draws are receiver candidates (terrain).
// Casters are flattened along a fixed direction onto the ground found below them, rasterized
// into a small coverage mask (union of all triangles, four samples per cell, 3x3 filtered),
// and the mask is clipped against the real receiver triangles. The result is a list of
// triangles that lie exactly on the visible terrain, which the GPU then draws with alpha blending.
//
// This file has no dependency on the rest of PPSSPP so that it can be unit tested on its own.
// The world space is assumed to be Y-up, like the games it was written for.

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

struct PlanarShadowOptions {
	bool enabled = false;
	float opacity = 0.25f;       // 0 to 0.6
	float directionX = 0.45f;    // Projection along world X per unit of height.
	float directionZ = 0.30f;    // Projection along world Z per unit of height.
	float floorOffset = 0.0f;
	unsigned resolution = 96;    // Mask size in cells: 32 to 192.
	// Distance LOD: 0 keeps every caster, 1 to 3 drop casters that cover less than 6%, 3% or 1.5%
	// of the screen height.
	unsigned lod = 2;
	// Skip the game's own blob shadows (decided by the caller, which knows the GE state).
	bool hideOriginal = true;
};

// Same memory layout as a PSP vertex with GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT and nothing else.
struct PlanarShadowVertex {
	uint32_t color;
	float x, y, z;
};

class PlanarShadows {
public:
	using Point = std::array<float, 3>;
	using Triangle = std::array<Point, 3>;

	// The matrices are in the PSP layout: view and world are 4x3 (12 floats), projection is 4x4.
	struct Scene {
		uint32_t target = 0;
		float view[12]{};
		float proj[16]{};
		uint32_t depthFunc = 0;    // As the PSP GE_CMD_ZTEST register, taken from the caster draw.
		// Viewport, depth range, offset, scissor and region registers of the caster draw (opaque to this
		// class, the caller fills and replays them). The 2D draw that triggers the shadows can have its own.
		static constexpr unsigned kRegs = 16;
		uint32_t regs[kRegs]{};
		bool operator==(const Scene &o) const;
	};

	PlanarShadows() {}

	void SetOptions(const PlanarShadowOptions &o) { options_ = o; }
	const PlanarShadowOptions &Options() const { return options_; }

	// Forget everything that was collected.
	void Reset();
	// Forget what was collected for one render target. Called when that target is cleared.
	void DropTarget(uint32_t target);

	// Collects world space triangles of one draw. world is the 4x3 world matrix of the draw, which
	// only serves to group draws of the same actor; the triangles must already be transformed by it.
	void AddCaster(const Scene &scene, const float world[12], const std::vector<Triangle> &triangles);
	void AddReceiver(const Scene &scene, const std::vector<Triangle> &triangles);

	bool HasCasters(uint32_t target) const;
	// True while a draw of that kind can still be collected. Lets the caller skip decoding when a
	// per frame limit was reached.
	bool WantsCasters(uint32_t target) const { return options_.enabled && options_.opacity > 0.0f && !finished_.count(target) && triangles_ < kTriangleLimit; }
	bool WantsReceivers(uint32_t target) const { return options_.enabled && !finished_.count(target) && receiverTriangles_ < kReceiverLimit; }
	// Render targets that have casters waiting to be drawn.
	std::vector<uint32_t> PendingTargets() const;
	bool WantsMoreFor(uint32_t target) const { return !finished_.count(target); }

	// Turns everything collected for the target into shadow triangles, and clears it.
	// Returns false (and leaves out empty) when there is nothing to draw.
	bool Build(uint32_t target, std::vector<PlanarShadowVertex> &out, Scene *sceneOut);

	// Statistics of the previous Build, for logging.
	struct Stats {
		size_t casters = 0;
		size_t lodCulled = 0;
		size_t emitted = 0;
		size_t conformed = 0;
		size_t skipped = 0;
		size_t work = 0;
	};
	const Stats &GetStats() const { return stats_; }

private:
	struct Grid {
		bool built = false;
		size_t count = 0;
		float x0 = 0, z0 = 0, inverse = 0;
		unsigned width = 0, height = 0;
		std::vector<std::vector<uint32_t>> cells;
		std::vector<uint32_t> large;
		std::vector<uint32_t> seen;
		uint32_t stamp = 0;
	};
	struct Caster {
		Scene scene;
		std::array<float, 12> world{};
		std::vector<Triangle> triangles;
		Grid grid;  // receivers only
	};
	struct Pick {
		const Caster *caster = nullptr;
		float distance = 0.0f;
		unsigned resolution = 0;
		float fade = 1.0f;
	};
	struct ClippedPolygon;

	static bool SameScene(const Scene &a, const Scene &b) { return a == b; }
	static ClippedPolygon ClipRectangle(const Triangle &t, float x0, float z0, float x1, float z1);
	static void BuildGrid(Caster &receiver);
	template <class Visit>
	void NearReceivers(const Scene &scene, float x0, float z0, float x1, float z1, Visit visit);
	static float LodCullFraction(unsigned lod);
	bool LodSelect(const Caster &caster, Pick &pick) const;
	void Rasterize(const Caster &caster, unsigned n, float fade, std::vector<PlanarShadowVertex> &out);

	PlanarShadowOptions options_;
	std::vector<Caster> casters_;
	std::vector<Caster> receivers_;
	std::set<uint32_t> finished_;
	std::vector<uint32_t> candidates_;
	size_t receiverTriangles_ = 0;
	size_t triangles_ = 0;
	Stats stats_;

	static constexpr size_t kTriangleLimit = 40000;
	static constexpr size_t kReceiverLimit = 60000;
	static constexpr size_t kMaxCasters = 48;
	static constexpr size_t kMaxReceiverSets = 16;
	static constexpr size_t kMaxShadowVertices = 120000;
	static constexpr size_t kPixelWorkLimit = 4000000;
	static constexpr unsigned kGridCells = 48;
	static constexpr size_t kGridLargeCells = 64;
};
