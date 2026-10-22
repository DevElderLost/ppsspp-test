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
#include <cmath>
#include <cstring>
#include <limits>

#include "GPU/Common/PlanarShadows.h"

typedef PlanarShadows::Point Point;
typedef PlanarShadows::Triangle Triangle;

bool PlanarShadows::Scene::operator==(const Scene &o) const {
	return target == o.target && memcmp(view, o.view, sizeof(view)) == 0 && memcmp(proj, o.proj, sizeof(proj)) == 0;
}

// Sutherland-Hodgman clipping in world X/Z. Y interpolates along the receiver triangle, so the
// emitted polygon lies on the actual surface.
struct PlanarShadows::ClippedPolygon {
	std::array<Point, 8> points{};  // A triangle clipped by four half-planes has at most seven vertices.
	size_t count = 0;
	bool empty() const { return count == 0; }
	size_t size() const { return count; }
	const Point &back() const { return points[count - 1]; }
	const Point *begin() const { return points.data(); }
	const Point *end() const { return points.data() + count; }
	const Point &operator[](size_t i) const { return points[i]; }
	void push_back(const Point &p) { points[count++] = p; }
};

static inline unsigned Popcount4(unsigned v) {
	static const unsigned char table[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
	return table[v & 15];
}

static inline float Edge(const Point &a, const Point &b, float x, float z) {
	return (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]);
}

static bool HeightAt(const Triangle &t, float x, float z, float &height) {
	const float area = Edge(t[0], t[1], t[2][0], t[2][2]);
	if (std::abs(area) < 1e-8f)
		return false;
	const float a = Edge(t[1], t[2], x, z) / area;
	const float b = Edge(t[2], t[0], x, z) / area;
	const float c = 1 - a - b;
	if (a < -1e-5f || b < -1e-5f || c < -1e-5f)
		return false;
	height = a * t[0][1] + b * t[1][1] + c * t[2][1];
	return std::isfinite(height);
}

static unsigned CellOf(float offset, float inverse, unsigned count) {
	const float cell = std::floor(offset * inverse);
	if (!(cell > 0.0f))
		return 0;
	if (cell >= static_cast<float>(count - 1))
		return count - 1;
	return static_cast<unsigned>(cell);
}

PlanarShadows::ClippedPolygon PlanarShadows::ClipRectangle(const Triangle &t, float x0, float z0, float x1, float z1) {
	ClippedPolygon polygon;
	for (const auto &p : t)
		polygon.push_back(p);
	const auto cut = [&](unsigned axis, float bound, bool greater) {
		ClippedPolygon next;
		if (polygon.empty())
			return;
		Point a = polygon.back();
		float da = greater ? a[axis] - bound : bound - a[axis];
		for (const Point &b : polygon) {
			const float db = greater ? b[axis] - bound : bound - b[axis];
			if ((da >= 0) != (db >= 0)) {
				const float ratio = da / (da - db);
				Point hit{};
				for (unsigned k = 0; k < 3; ++k)
					hit[k] = a[k] + ratio * (b[k] - a[k]);
				hit[axis] = bound;
				next.push_back(hit);
			}
			if (db >= 0)
				next.push_back(b);
			a = b;
			da = db;
		}
		polygon = next;
	};
	cut(0, x0, true);
	cut(0, x1, false);
	cut(2, z0, true);
	cut(2, z1, false);
	return polygon;
}

void PlanarShadows::Reset() {
	casters_.clear();
	receivers_.clear();
	finished_.clear();
	receiverTriangles_ = 0;
	triangles_ = 0;
	stats_ = Stats();
}

void PlanarShadows::DropTarget(uint32_t target) {
	const auto onTarget = [&](const Caster &c) { return c.scene.target == target; };
	casters_.erase(std::remove_if(casters_.begin(), casters_.end(), onTarget), casters_.end());
	receivers_.erase(std::remove_if(receivers_.begin(), receivers_.end(), onTarget), receivers_.end());
	finished_.erase(target);
	// The counters are per frame limits, recount what is left.
	triangles_ = 0;
	for (const Caster &c : casters_)
		triangles_ += c.triangles.size();
	receiverTriangles_ = 0;
	for (const Caster &c : receivers_)
		receiverTriangles_ += c.triangles.size();
}

std::vector<uint32_t> PlanarShadows::PendingTargets() const {
	std::set<uint32_t> targets;
	for (const Caster &c : casters_) {
		if (!c.triangles.empty())
			targets.insert(c.scene.target);
	}
	return std::vector<uint32_t>(targets.begin(), targets.end());
}

bool PlanarShadows::HasCasters(uint32_t target) const {
	for (const Caster &c : casters_) {
		if (c.scene.target == target && !c.triangles.empty())
			return true;
	}
	return false;
}

void PlanarShadows::AddReceiver(const Scene &scene, const std::vector<Triangle> &triangles) {
	if (!options_.enabled || finished_.count(scene.target))
		return;
	auto it = std::find_if(receivers_.begin(), receivers_.end(), [&](const Caster &r) { return SameScene(r.scene, scene); });
	if (it == receivers_.end()) {
		if (receivers_.size() >= kMaxReceiverSets)
			return;
		Caster r;
		r.scene = scene;
		receivers_.push_back(std::move(r));
		it = receivers_.end() - 1;
	}
	for (const Triangle &t : triangles) {
		if (receiverTriangles_ >= kReceiverLimit)
			return;
		// Only surfaces that face mostly up can receive a shadow.
		Point a{}, b{}, normal{};
		for (unsigned k = 0; k < 3; ++k) {
			a[k] = t[1][k] - t[0][k];
			b[k] = t[2][k] - t[0][k];
		}
		normal = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (std::isfinite(length) && length > 1e-8f && std::abs(normal[1]) > 0.35f * length) {
			it->triangles.push_back(t);
			++receiverTriangles_;
		}
	}
}

void PlanarShadows::AddCaster(const Scene &scene, const float world[12], const std::vector<Triangle> &triangles) {
	if (!options_.enabled || options_.opacity <= 0.0f || finished_.count(scene.target))
		return;
	std::array<float, 12> key;
	memcpy(key.data(), world, sizeof(float) * 12);
	auto it = std::find_if(casters_.begin(), casters_.end(), [&](const Caster &c) { return c.world == key && SameScene(c.scene, scene); });
	if (it == casters_.end()) {
		if (casters_.size() >= kMaxCasters) {
			++stats_.skipped;
			return;
		}
		Caster c;
		c.world = key;
		c.scene = scene;
		casters_.push_back(std::move(c));
		it = casters_.end() - 1;
	}
	for (const Triangle &t : triangles) {
		if (triangles_ >= kTriangleLimit) {
			++stats_.skipped;
			return;
		}
		it->triangles.push_back(t);
		++triangles_;
	}
}

void PlanarShadows::BuildGrid(Caster &receiver) {
	Grid &g = receiver.grid;
	g = Grid{};
	g.built = true;
	g.count = receiver.triangles.size();
	if (receiver.triangles.empty())
		return;
	float minX = std::numeric_limits<float>::max(), minZ = minX;
	float maxX = -minX, maxZ = -minX;
	for (const Triangle &t : receiver.triangles) {
		for (const Point &p : t) {
			minX = std::min(minX, p[0]);
			maxX = std::max(maxX, p[0]);
			minZ = std::min(minZ, p[2]);
			maxZ = std::max(maxZ, p[2]);
		}
	}
	const float span = std::max({ maxX - minX, maxZ - minZ, 1e-3f });
	g.x0 = minX;
	g.z0 = minZ;
	g.inverse = static_cast<float>(kGridCells) / span;
	g.width = std::min(kGridCells, static_cast<unsigned>((maxX - minX) * g.inverse) + 1u);
	g.height = std::min(kGridCells, static_cast<unsigned>((maxZ - minZ) * g.inverse) + 1u);
	g.cells.assign(static_cast<size_t>(g.width) * g.height, {});
	g.seen.assign(receiver.triangles.size(), 0u);
	for (size_t i = 0; i < receiver.triangles.size(); ++i) {
		const Triangle &t = receiver.triangles[i];
		const unsigned cx0 = CellOf(std::min({ t[0][0], t[1][0], t[2][0] }) - g.x0, g.inverse, g.width);
		const unsigned cx1 = CellOf(std::max({ t[0][0], t[1][0], t[2][0] }) - g.x0, g.inverse, g.width);
		const unsigned cz0 = CellOf(std::min({ t[0][2], t[1][2], t[2][2] }) - g.z0, g.inverse, g.height);
		const unsigned cz1 = CellOf(std::max({ t[0][2], t[1][2], t[2][2] }) - g.z0, g.inverse, g.height);
		if (static_cast<size_t>(cx1 - cx0 + 1) * (cz1 - cz0 + 1) > kGridLargeCells) {
			g.large.push_back(static_cast<uint32_t>(i));
			continue;
		}
		for (unsigned cz = cz0; cz <= cz1; ++cz) {
			for (unsigned cx = cx0; cx <= cx1; ++cx)
				g.cells[static_cast<size_t>(cz) * g.width + cx].push_back(static_cast<uint32_t>(i));
		}
	}
}

// Visits the receiver triangles whose X/Z cells overlap the rectangle, each once and in their
// original order. The visitor returns false to stop.
template <class Visit>
void PlanarShadows::NearReceivers(const Scene &scene, float x0, float z0, float x1, float z1, Visit visit) {
	for (Caster &receiver : receivers_) {
		if (!SameScene(receiver.scene, scene))
			continue;
		if (!receiver.grid.built || receiver.grid.count != receiver.triangles.size())
			BuildGrid(receiver);
		Grid &g = receiver.grid;
		if (g.cells.empty())
			continue;
		if (++g.stamp == 0u) {
			std::fill(g.seen.begin(), g.seen.end(), 0u);
			g.stamp = 1u;
		}
		candidates_.clear();
		const auto add = [&](uint32_t i) {
			if (g.seen[i] == g.stamp)
				return;
			g.seen[i] = g.stamp;
			candidates_.push_back(i);
		};
		for (uint32_t i : g.large)
			add(i);
		const unsigned cx0 = CellOf(x0 - g.x0, g.inverse, g.width), cx1 = CellOf(x1 - g.x0, g.inverse, g.width);
		const unsigned cz0 = CellOf(z0 - g.z0, g.inverse, g.height), cz1 = CellOf(z1 - g.z0, g.inverse, g.height);
		for (unsigned cz = cz0; cz <= cz1; ++cz) {
			for (unsigned cx = cx0; cx <= cx1; ++cx) {
				for (uint32_t i : g.cells[static_cast<size_t>(cz) * g.width + cx])
					add(i);
			}
		}
		std::sort(candidates_.begin(), candidates_.end());
		for (uint32_t i : candidates_) {
			if (!visit(receiver.triangles[i]))
				return;
		}
	}
}

// Share of the screen height a caster covers below which its shadow is dropped.
float PlanarShadows::LodCullFraction(unsigned lod) {
	switch (lod) {
	case 1: return 0.06f;
	case 2: return 0.03f;
	case 3: return 0.015f;
	default: return 0.0f;
	}
}

// Decides from the caster's size on screen whether it gets a silhouette, how fine its mask is and
// how strongly it fades out near the limit.
bool PlanarShadows::LodSelect(const Caster &caster, Pick &pick) const {
	pick.resolution = options_.resolution;
	if (options_.lod == 0)
		return true;
	Point low{ 1e30f, 1e30f, 1e30f }, high{ -1e30f, -1e30f, -1e30f };
	for (const Triangle &t : caster.triangles) {
		for (const Point &p : t) {
			for (unsigned k = 0; k < 3; ++k) {
				low[k] = std::min(low[k], p[k]);
				high[k] = std::max(high[k], p[k]);
			}
		}
	}
	const float height = high[1] - low[1];
	if (!(height > 1e-3f))
		return true;
	const Point centre{ (low[0] + high[0]) * 0.5f, (low[1] + high[1]) * 0.5f, (low[2] + high[2]) * 0.5f };
	const float *view = caster.scene.view;
	float squared = 0.0f;
	for (unsigned k = 0; k < 3; ++k) {
		// 4x3 matrix: the translation row is at 9..11.
		const float c = view[k] * centre[0] + view[3 + k] * centre[1] + view[6 + k] * centre[2] + view[9 + k];
		squared += c * c;
	}
	const float distance = std::sqrt(squared);
	if (!std::isfinite(distance) || distance < 1e-3f)
		return true;
	// proj[5] is cot(fov/2) for a perspective matrix: the height covers height / distance * cot / 2
	// of the screen.
	const float cot = std::abs(caster.scene.proj[5]);
	const float scale = std::isfinite(cot) && cot > 0.1f && cot < 50.0f ? cot : 2.4f;
	const float screen = height / distance * scale * 0.5f;
	const float cull = LodCullFraction(options_.lod);
	if (screen <= cull)
		return false;
	pick.distance = distance;
	pick.fade = std::clamp((screen - cull) / cull, 0.0f, 1.0f);
	unsigned resolution = options_.resolution;
	if (screen < 0.07f)
		resolution /= 4u;
	else if (screen < 0.15f)
		resolution /= 2u;
	pick.resolution = std::max(32u, (resolution / 8u) * 8u);
	return true;
}

void PlanarShadows::Rasterize(const Caster &caster, unsigned n, float fade, std::vector<PlanarShadowVertex> &out) {
	const float opacity = options_.opacity * fade;
	if (std::lround(opacity * 255.0f) <= 0)
		return;
	float floorY = std::numeric_limits<float>::max();
	float top = -floorY;
	for (const auto &t : caster.triangles) {
		for (const auto &p : t) {
			floorY = std::min(floorY, p[1]);
			top = std::max(top, p[1]);
		}
	}
	if (!(top > floorY + 0.001f))
		return;
	const float height = top - floorY;
	// Ground under the actor stabilizes the projection reference instead of following a moving toe
	// every frame. No collision data is read.
	float centreX = 0, centreZ = 0;
	size_t points = 0;
	for (const auto &t : caster.triangles) {
		for (const auto &p : t) {
			centreX += p[0];
			centreZ += p[2];
			++points;
		}
	}
	centreX /= static_cast<float>(points);
	centreZ /= static_cast<float>(points);
	float ground = -std::numeric_limits<float>::infinity();
	NearReceivers(caster.scene, centreX, centreZ, centreX, centreZ, [&](const Triangle &t) {
		float y{};
		if (HeightAt(t, centreX, centreZ, y) && y <= floorY + height * 0.35f && y >= floorY - height * 2 && y > ground)
			ground = y;
		return true;
	});
	if (std::isfinite(ground))
		floorY = ground;
	floorY += options_.floorOffset;
	const float bias = std::max(0.02f, height * 0.002f);
	std::vector<Triangle> projected = caster.triangles;
	float x0 = std::numeric_limits<float>::max(), z0 = x0;
	float x1 = -x0, z1 = -x0;
	for (auto &t : projected) {
		for (auto &p : t) {
			const float h = p[1] - floorY;
			p = { p[0] + options_.directionX * h, floorY + bias, p[2] + options_.directionZ * h };
			x0 = std::min(x0, p[0]);
			x1 = std::max(x1, p[0]);
			z0 = std::min(z0, p[2]);
			z1 = std::max(z1, p[2]);
		}
	}
	if (!(x1 > x0 + 0.001f && z1 > z0 + 0.001f))
		return;
	// Square world-aligned cells: small pose changes no longer resize and stretch every texel as an
	// animated bounding box changes. Quantized scale can still step when an actor crosses a
	// power-of-two extent.
	const float span = std::max(x1 - x0, z1 - z0) / static_cast<float>(n - 8);
	const float dx = std::exp2(std::ceil(std::log2(std::max(span, 1.0f / 1024.0f))));
	const float dz = dx;
	x0 = (std::floor(x0 / dx) - 2) * dx;
	z0 = (std::floor(z0 / dz) - 2) * dz;

	// Bin nearby receivers by mask row to bound clipping work.
	std::vector<std::vector<const Triangle *>> rows(n);
	NearReceivers(caster.scene, x0, z0, x0 + n * dx, z0 + n * dz, [&](const Triangle &t) {
		const float lowY = std::min({ t[0][1], t[1][1], t[2][1] });
		const float highY = std::max({ t[0][1], t[1][1], t[2][1] });
		const float left = std::min({ t[0][0], t[1][0], t[2][0] });
		const float right = std::max({ t[0][0], t[1][0], t[2][0] });
		const float low = std::min({ t[0][2], t[1][2], t[2][2] });
		const float high = std::max({ t[0][2], t[1][2], t[2][2] });
		if (lowY > floorY + height * 0.5f || highY < floorY - height * 2 || right < x0 || left > x0 + n * dx || high < z0 || low > z0 + n * dz)
			return true;
		const int first = int(std::clamp(std::floor((low - z0) / dz), 0.0f, float(n - 1)));
		const int last = int(std::clamp(std::floor((high - z0) / dz), 0.0f, float(n - 1)));
		for (int row = first; row <= last; ++row)
			rows[row].push_back(&t);
		return true;
	});

	std::vector<unsigned char> mask(n * n, 0);
	for (auto &t : projected) {
		for (auto &p : t) {
			p[0] = (p[0] - x0) / dx;
			p[2] = (p[2] - z0) / dz;
		}
		if (std::abs(Edge(t[0], t[1], t[2][0], t[2][2])) < 1e-7f)
			continue;
		const int left = std::clamp(static_cast<int>(std::floor(std::min({ t[0][0], t[1][0], t[2][0] }))), 0, int(n) - 1);
		const int right = std::clamp(static_cast<int>(std::ceil(std::max({ t[0][0], t[1][0], t[2][0] }))), 0, int(n) - 1);
		const int low = std::clamp(static_cast<int>(std::floor(std::min({ t[0][2], t[1][2], t[2][2] }))), 0, int(n) - 1);
		const int high = std::clamp(static_cast<int>(std::ceil(std::max({ t[0][2], t[1][2], t[2][2] }))), 0, int(n) - 1);
		stats_.work += static_cast<size_t>(right - left + 1) * static_cast<size_t>(high - low + 1);
		if (stats_.work > kPixelWorkLimit) {
			++stats_.skipped;
			return;
		}
		for (int z = low; z <= high; ++z) {
			for (int x = left; x <= right; ++x) {
				// Union is monotonic: samples covered by an earlier triangle cannot change.
				unsigned char &cell = mask[z * n + x];
				if (cell == 15u)
					continue;
				// Four coverage bits union across triangles before filtering. This gives partial
				// coverage without a hard one-sample toggle.
				for (unsigned sample = 0; sample < 4; ++sample) {
					if (cell & (1u << sample))
						continue;
					const float sx = x + ((sample & 1) ? 0.75f : 0.25f);
					const float sz = z + ((sample & 2) ? 0.75f : 0.25f);
					const float a = Edge(t[0], t[1], sx, sz), b = Edge(t[1], t[2], sx, sz), c = Edge(t[2], t[0], sx, sz);
					if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
						cell |= 1u << sample;
				}
			}
		}
	}
	std::vector<unsigned char> coverage(n * n, 0);
	for (unsigned z = 1; z + 1 < n; ++z) {
		for (unsigned x = 1; x + 1 < n; ++x) {
			unsigned sum = 0;
			for (int j = -1; j <= 1; ++j) {
				for (int i = -1; i <= 1; ++i)
					sum += Popcount4(mask[(int(z) + j) * n + int(x) + i]);
			}
			coverage[z * n + x] = static_cast<unsigned char>(sum);
		}
	}
	// Clip each constant-alpha mask run against actual receiver triangles. Do not bridge holes or
	// use a floating fallback when terrain is absent.
	for (unsigned z = 0; z < n; ++z) {
		for (unsigned x = 0; x < n;) {
			const unsigned strength = coverage[z * n + x];
			if (!strength) {
				++x;
				continue;
			}
			const unsigned start = x++;
			while (x < n && coverage[z * n + x] == strength)
				++x;
			const uint32_t alpha = static_cast<uint32_t>(std::lround(opacity * 255.0f * strength / 36.0f));
			if (!alpha)
				continue;
			const float left = x0 + start * dx, right = x0 + x * dx, low = z0 + z * dz, high = low + dz;
			for (const Triangle *t : rows[z]) {
				if (++stats_.work > kPixelWorkLimit) {
					++stats_.skipped;
					return;
				}
				if (std::max({ (*t)[0][0], (*t)[1][0], (*t)[2][0] }) < left || std::min({ (*t)[0][0], (*t)[1][0], (*t)[2][0] }) > right)
					continue;
				ClippedPolygon polygon = ClipRectangle(*t, left, low, right, high);
				if (polygon.size() < 3)
					continue;
				const auto vertex = [&](const Point &p) {
					PlanarShadowVertex v;
					v.color = alpha << 24u;  // Black, with the alpha as the shadow strength.
					v.x = p[0];
					v.y = p[1] + bias + options_.floorOffset;
					v.z = p[2];
					return v;
				};
				for (size_t k = 1; k + 1 < polygon.size(); ++k) {
					if (std::abs(Edge(polygon[0], polygon[k], polygon[k + 1][0], polygon[k + 1][2])) < 1e-9f)
						continue;
					// Bound extra vertex memory independently from input size.
					if (out.size() + 3 > kMaxShadowVertices) {
						++stats_.skipped;
						return;
					}
					out.push_back(vertex(polygon[0]));
					out.push_back(vertex(polygon[k]));
					out.push_back(vertex(polygon[k + 1]));
					++stats_.conformed;
				}
			}
		}
	}
}

bool PlanarShadows::Build(uint32_t target, std::vector<PlanarShadowVertex> &out, Scene *sceneOut) {
	out.clear();
	// 2D scenery may precede the first actor. Keep receiver geometry until a scene with actual
	// candidates is flushed or the target is cleared.
	if (!HasCasters(target))
		return false;
	stats_ = Stats();
	std::vector<Pick> picks;
	Scene scene;
	bool haveScene = false;
	for (const Caster &caster : casters_) {
		if (caster.scene.target != target || caster.triangles.empty())
			continue;
		++stats_.casters;
		Pick pick;
		pick.caster = &caster;
		if (!LodSelect(caster, pick)) {
			++stats_.lodCulled;
			continue;
		}
		if (!haveScene) {
			scene = caster.scene;
			haveScene = true;
		}
		picks.push_back(pick);
	}
	// With the limit on, the nearest casters get the shadow slots first.
	if (options_.lod != 0)
		std::stable_sort(picks.begin(), picks.end(), [](const Pick &a, const Pick &b) { return a.distance < b.distance; });
	for (const Pick &pick : picks) {
		const size_t before = out.size();
		Rasterize(*pick.caster, pick.resolution, pick.fade, out);
		if (out.size() != before)
			++stats_.emitted;
	}
	DropTarget(target);
	finished_.insert(target);
	if (out.empty() || !haveScene)
		return false;
	if (sceneOut)
		*sceneOut = scene;
	return true;
}
