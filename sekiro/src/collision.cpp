#include "collision.h"

#include "clip.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace sekicraft::collision
{
	namespace
	{
		namespace proto = sekicraft::proto;

		constexpr float  kSpacing = 0.5f;                  // column grid (blocks): 2x2 columns per block
		constexpr int    kRegion = 8;                      // blocks per region edge (SkyCollision.REGION_SIZE)
		constexpr int    kColsPerRegion = 16;              // kRegion / kSpacing
		constexpr int    kSpan = kColsPerRegion + 2;       // a job's columns: the region plus one on each side
		constexpr int    kMaxHits = 24;                    // surfaces kept per column
		constexpr float  kMatchDy = 0.9f;                  // neighbouring surfaces this close in height are one surface
		constexpr int    kRadius = 2;                      // region columns around the player (5x5)
		constexpr int    kBelow = 3, kAbove = 2;           // regions below / above the player's
		constexpr float  kWallHeights[] = { 0.3f, 0.9f, 1.5f };  // above each floor: shins, waist, head
		constexpr float  kWallTopDefault = 1.9f;           // wall height when the far side has no surface above the hit
		constexpr float  kSlab = 0.25f;                    // voxel slab under each surface (blocks)
		constexpr double kBudgetMs = 2.0;                  // ray-cast time per game frame
		constexpr float  kTeleport = 48.0f;                // a jump this far starts a new collision epoch

		int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

		struct Column
		{
			std::array<float, kMaxHits> ys{};  // MC y of each surface, highest first
			int n = 0;
			std::uint32_t buried = 0;           // bit s: surface s is inside closed geometry (see scanColumn)

			// Index of the surface closest to y within kMatchDy, or -1.
			int match(float y) const
			{
				int best = -1;
				float bestD = kMatchDy;
				for (int i = 0; i < n; ++i) {
					const float d = std::abs(ys[i] - y);
					if (d <= bestD) {
						bestD = d;
						best = i;
					}
				}
				return best;
			}

			// Lowest surface at or above y, or NAN.
			float lowestAbove(float y) const
			{
				float r = NAN;
				for (int i = 0; i < n; ++i)
					if (ys[i] >= y)
						r = ys[i];  // descending: the last one at or above y is the lowest
				return r;
			}
		};

		struct Band
		{
			int   id = 0;
			int   ryLo = 0, ryHi = 0;
			float yLow() const { return float(ryLo * kRegion); }
			float yHigh() const { return float((ryHi + 1) * kRegion); }
		};

		struct Job
		{
			int  rx = 0, rz = 0;
			Band band;
			bool rescan = false;  // cast again even where this generation already has a column
		};

		struct Done
		{
			Job                        job;
			std::vector<Column>        cols;  // kSpan x kSpan, [dk * kSpan + di]
			std::vector<proto::ColTri> walls;
		};

		// ---- shared between the worker and the game thread (g_mutex) ----
		std::mutex             g_mutex;
		std::deque<Job>        g_queue;
		std::vector<Done>      g_done;
		int                    g_generation = 0;  // bumps on reset / new epoch: in-flight work is dropped
		// Right after a load Sekiro is still streaming in the area's collision: rays find nothing, and
		// an empty scan would be sent as "no ground here". A new epoch waits until a ray straight down
		// from the player hits the ground they stand on (or kGroundWaitMs passes, e.g. mid-air).
		game::Vec3             g_groundProbe{};       // Sekiro coords
		int                    g_groundOkGeneration = -1;
		std::atomic<long long> g_casts{ 0 }, g_tickUs{ 0 }, g_ticks{ 0 };

		// ---- game thread only ----
		struct Progress
		{
			bool active = false;
			int  generation = -1;
			Job  job;
			int  phase = 0;  // 0: columns, 1: walls
			int  index = 0;
			Done out;
		};
		Progress                              g_work;
		int                                   g_probeGeneration = -1;
		unsigned long long                    g_probeSince = 0, g_probeLast = 0;
		int                                   g_cacheBand = -1;
		int                                   g_cacheGeneration = -1;
		std::unordered_map<long long, Column> g_cache;

		// ---- worker only ----
		struct Area
		{
			bool                     active = false;
			Band                     band;
			int                      cx = 0, cz = 0;  // centre region column
			game::Vec3               last{};
			std::set<std::pair<int, int>> queued;     // region columns queued or done for this band
			int                      regionsSent = 0;
		};
		Area g_area;

		// Region columns whose scan found nothing at all: probably not streamed in yet. Scanned again
		// a few times, a few seconds apart. Worker only.
		struct Retry
		{
			unsigned long long due = 0;
			int                tries = 0;
		};
		std::map<std::pair<int, int>, Retry> g_retry;
		constexpr int                       kEmptyRetries = 5;
		constexpr unsigned long long        kEmptyRetryMs = 3000;
		constexpr unsigned long long        kGroundWaitMs = 15000;

		bool isEmpty(const Done& d)
		{
			if (!d.walls.empty())
				return false;
			for (const Column& c : d.cols)
				if (c.n)
					return false;
			return true;
		}

		// Dug blocks, as Minecraft reports them per 16^3 section (kRenDug): Sekiro's geometry in them
		// is gone. Worker only.
		struct DugSection
		{
			std::array<std::uint64_t, 64> bits{};  // bit x + 16z + 256y
		};
		std::unordered_map<long long, DugSection> g_dug;
		std::set<std::pair<int, int>>             g_dugChanged;  // region columns to send again
		unsigned                                  g_dugGeneration = 0;
		// The last finished scan of each region column, so a dig re-sends without casting rays again.
		std::map<std::pair<int, int>, Done>       g_doneCache;

		long long sectionKey(int sx, int sy, int sz)
		{
			return (long long(sx & 0x3FFFFF) << 42) | (long long(sy & 0xFFFFF) << 22) | long long(sz & 0x3FFFFF);
		}

		bool isDug(int x, int y, int z)
		{
			auto it = g_dug.find(sectionKey(floorDiv(x, 16), floorDiv(y, 16), floorDiv(z, 16)));
			if (it == g_dug.end())
				return false;
			const int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
			return (it->second.bits[bit >> 6] >> (bit & 63)) & 1;
		}

		long long colKey(int i, int k) { return (long long(i) << 32) ^ (long long(k) & 0xFFFFFFFFll); }
		float colX(int i) { return (float(i) + 0.5f) * kSpacing; }
		float colZ(int k) { return (float(k) + 0.5f) * kSpacing; }

		long long nowQpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double qpcToMs(long long ticks)
		{
			static const double freq = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return double(f.QuadPart);
			}();
			return double(ticks) * 1000.0 / freq;
		}

		// A ray in Minecraft coordinates (the engine works in Sekiro's; z flips).
		bool cast(game::Vec3 startMc, game::Vec3 deltaMc, game::RayHit& hitMc)
		{
			++g_casts;
			game::RayHit h{};
			if (!game::castRay(game::fromMc(startMc), { deltaMc.x, deltaMc.y, -deltaMc.z }, h))
				return false;
			hitMc.pos = game::toMc(h.pos);
			hitMc.normal = { h.normal.x, h.normal.y, -h.normal.z };
			hitMc.fraction = h.fraction;
			return true;
		}

		// How many surfaces a segment passes through (two-sided: restart a hair past each hit).
		constexpr float kParityReach = 24.0f;
		int crossings(game::Vec3 start, game::Vec3 delta)
		{
			const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			const game::Vec3 dir{ delta.x / len, delta.y / len, delta.z / len };
			float travelled = 0;
			int n = 0;
			for (; n < 16 && travelled < len - 0.05f;) {
				const float left = len - travelled;
				game::RayHit h{};
				if (!cast(start, { dir.x * left, dir.y * left, dir.z * left }, h))
					break;
				const float step = h.fraction * left + 0.02f;
				travelled += step;
				start = { start.x + dir.x * step, start.y + dir.y * step, start.z + dir.z * step };
				++n;
			}
			return n;
		}

		// Every surface in column (i, k) inside the band, top down. Surfaces are two-sided, so the
		// ray just restarts a hair below each hit.
		Column scanColumn(int i, int k, const Band& band)
		{
			Column c;
			const float x = colX(i), z = colZ(k);
			const float bottom = band.yLow() - 1.0f;
			float y = band.yHigh();
			for (int guard = 0; guard < 48 && y > bottom && c.n < kMaxHits; ++guard) {
				game::RayHit h{};
				if (!cast({ x, y, z }, { 0, bottom - y, 0 }, h))
					break;
				if (!(h.pos.y < y + 0.001f) || h.pos.y < bottom - 0.01f)
					break;  // not a sane hit below the start
				c.ys[c.n++] = h.pos.y;
				y = h.pos.y - 0.02f;
			}
			// Which surfaces are inside closed geometry (ground running on under a cliff, a castle
			// wall's footing)? Surfaces are two-sided, so ask by parity: a long ray from just above
			// the surface crosses a closed shape's faces an odd number of times when it starts inside
			// it. Odd both ways (east, north): buried. Buried surfaces aren't ground and grow no walls
			// (walls seen from inside rock would face the wrong way: digging would raise blocks in
			// the open air in front of the cliff).
			for (int s = 0; s < c.n; ++s) {
				const float above = s > 0 ? c.ys[s - 1] : INFINITY;
				const float py = std::min(c.ys[s] + 0.5f, (c.ys[s] + above) * 0.5f);
				if ((crossings({ x, py, z }, { kParityReach, 0, 0 }) & 1) && (crossings({ x, py, z }, { 0, 0, -kParityReach }) & 1))
					c.buried |= 1u << s;
			}
			return c;
		}

		void addQuad(std::vector<proto::ColTri>& out, game::Vec3 p0, game::Vec3 p1, game::Vec3 p2, game::Vec3 p3)
		{
			out.push_back({ { p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z }, 0 });
			out.push_back({ { p0.x, p0.y, p0.z, p2.x, p2.y, p2.z, p3.x, p3.y, p3.z }, 0 });
		}

		// Walls seen from column `from` toward its neighbour `to` (one grid step along x or z): from
		// each surface of `from` that something could stand on, horizontal rays toward `to` at shin,
		// waist and head height. A hit is a wall, from that floor up to `to`'s next surface above
		// (or a default height), never above `from`'s own ceiling.
		void testWalls(const Column& from, int fi, int fk, const Column& to, int ti, int tk, std::vector<proto::ColTri>& out)
		{
			const bool  alongX = fi != ti;
			const float fx = colX(fi), fz = colZ(fk), tx = colX(ti), tz = colZ(tk);
			for (int s = 0; s < from.n; ++s) {
				if ((from.buried >> s) & 1)
					continue;  // inside rock: what it would see are the rock's faces from behind
				const float floorY = from.ys[s];
				const float ceiling = s > 0 ? from.ys[s - 1] : INFINITY;
				// Where the wall is at each height: (height, position along the ray's axis).
				std::array<std::pair<float, float>, 32> profile{};
				int samples = 0;
				float top = -INFINITY;
				for (float dh : kWallHeights) {
					const float h = floorY + dh;
					if (h > ceiling - 0.05f)
						break;
					game::RayHit hit{};
					if (!cast({ fx, h, fz }, { tx - fx, 0, tz - fz }, hit))
						continue;
					const float above = to.lowestAbove(h - 0.05f);
					top = std::max(top, std::min(std::isnan(above) ? floorY + kWallTopDefault : above, ceiling));
					profile[samples++] = { h, alongX ? hit.pos.x : hit.pos.z };
				}
				if (!samples || top <= floorY + 0.05f)
					continue;
				// A tall wall: follow it up every 0.5 (castle walls and cliffs lean back), reaching
				// further than the next column as it recedes. Stops where it ends or turns away.
				const float dirX = (tx - fx) / kSpacing, dirZ = (tz - fz) / kSpacing;
				for (float h = floorY + 2.0f; h < std::min({ top, ceiling, floorY + 12.0f }) - 0.05f && samples < int(profile.size()); h += 0.5f) {
					game::RayHit hit{};
					if (!cast({ fx, h, fz }, { dirX * 2.0f, 0, dirZ * 2.0f }, hit)) {
						top = h - 0.25f;
						break;
					}
					profile[samples++] = { h, alongX ? hit.pos.x : hit.pos.z };
				}
				// Rock: diggable stone, facing back toward `from` (the open side the rays came from).
				// Slanted pieces between samples; the first and last stretch to the floor and the top.
				const std::size_t first = out.size();
				auto piece = [&](float y0, float a0, float y1, float a1) {
					if (y1 <= y0 + 0.01f)
						return;
					if (alongX) {
						const float z0 = float(fk) * kSpacing, z1 = z0 + kSpacing;
						addQuad(out, { a0, y0, z0 }, { a1, y1, z0 }, { a1, y1, z1 }, { a0, y0, z1 });
					} else {
						const float x0 = float(fi) * kSpacing, x1 = x0 + kSpacing;
						addQuad(out, { x0, y0, a0 }, { x0, y1, a1 }, { x1, y1, a1 }, { x1, y0, a0 });
					}
				};
				piece(floorY, profile[0].second, profile[0].first, profile[0].second);
				for (int i = 0; i + 1 < samples; ++i)
					piece(profile[i].first, profile[i].second, profile[i + 1].first, profile[i + 1].second);
				piece(profile[samples - 1].first, profile[samples - 1].second, top, profile[samples - 1].second);
				const float openX = fx - tx, openZ = fz - tz;
				for (std::size_t i = first; i < out.size(); ++i) {
					float* v = out[i].v;
					const float nx = (v[4] - v[1]) * (v[8] - v[2]) - (v[5] - v[2]) * (v[7] - v[1]);
					const float nz = (v[3] - v[0]) * (v[7] - v[1]) - (v[4] - v[1]) * (v[6] - v[0]);
					if (nx * openX + nz * openZ < 0) {
						for (int k = 0; k < 3; ++k)
							std::swap(v[3 + k], v[6 + k]);
					}
					out[i].flags = proto::kTriDiggable | (std::uint32_t(proto::kDigStone) << proto::kTriMaterialShift);
				}
			}
		}

		// ---- worker: geometry from a finished job ----

		// Floor and ceiling triangles: surfaces of the four columns around each grid cell that line
		// up in height become two triangles (or one, where a corner has no matching surface).
		void buildSurfaces(const Done& d, std::vector<proto::ColTri>& out)
		{
			const int i0 = d.job.rx * kColsPerRegion - 1, k0 = d.job.rz * kColsPerRegion - 1;
			auto point = [&](int di, int dk, float y) { return game::Vec3{ colX(i0 + di), y, colZ(k0 + dk) }; };
			for (int dk = 0; dk + 1 < kSpan; ++dk) {
				for (int di = 0; di + 1 < kSpan; ++di) {
					const Column* corner[4] = { &d.cols[dk * kSpan + di], &d.cols[dk * kSpan + di + 1], &d.cols[(dk + 1) * kSpan + di],
						&d.cols[(dk + 1) * kSpan + di + 1] };  // a (di,dk), b (di+1,dk), c (di,dk+1), d (di+1,dk+1)
					const int cdi[4] = { di, di + 1, di, di + 1 }, cdk[4] = { dk, dk, dk + 1, dk + 1 };
					std::set<std::array<int, 4>> seen;
					for (int seedCorner = 0; seedCorner < 4; ++seedCorner) {
						for (int s = 0; s < corner[seedCorner]->n; ++s) {
							const float y = corner[seedCorner]->ys[s];
							std::array<int, 4> m{};
							for (int c = 0; c < 4; ++c)
								m[c] = corner[c]->match(y);
							if (!seen.insert(m).second)
								continue;
							auto p = [&](int c) { return point(cdi[c], cdk[c], corner[c]->ys[m[c]]); };
							// Ground (a surface with room above it, so solid below): diggable land, wound to
							// face up (out of the solid side). Anything else (ceilings, thin layers): not.
							const int seedAbove = s > 0 ? s - 1 : -1;
							const bool ground = !((corner[seedCorner]->buried >> s) & 1) && (seedAbove < 0 || corner[seedCorner]->ys[seedAbove] - y > 1.0f);
							auto tri = [&](int c0, int c1, int c2) {
								game::Vec3 a = p(c0), b = p(c1), e = p(c2);
								const float nx = (b.y - a.y) * (e.z - a.z) - (b.z - a.z) * (e.y - a.y);
								const float ny = (b.z - a.z) * (e.x - a.x) - (b.x - a.x) * (e.z - a.z);
								const float nz = (b.x - a.x) * (e.y - a.y) - (b.y - a.y) * (e.x - a.x);
								std::uint32_t flags = 0;
								if (ground) {
									if (ny < 0)
										std::swap(b, e);
									const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
									const bool flat = len > 0 && std::abs(ny) / len > 0.7f;
									flags = proto::kTriDiggable | proto::kTriTerrain |
									        (std::uint32_t(flat ? proto::kDigGrass : proto::kDigDirt) << proto::kTriMaterialShift);
								}
								out.push_back({ { a.x, a.y, a.z, b.x, b.y, b.z, e.x, e.y, e.z }, flags });
							};
							const bool A = m[0] >= 0, B = m[1] >= 0, C = m[2] >= 0, D = m[3] >= 0;
							if (A && B && D)
								tri(0, 3, 1);
							if (A && D && C)
								tri(0, 2, 3);
							if (!A && B && C && D)
								tri(1, 2, 3);
							if (!D && A && B && C)
								tri(0, 2, 1);
						}
					}
				}
			}
		}

		bool overlapsY(const proto::ColTri& t, float lo, float hi)
		{
			const float mn = std::min({ t.v[1], t.v[4], t.v[7] }), mx = std::max({ t.v[1], t.v[4], t.v[7] });
			return mx >= lo - 0.01f && mn <= hi + 0.01f;
		}

		void sendJob(GameLink& link, const Done& d, std::uint32_t epoch)
		{
			std::vector<proto::ColTri> built = d.walls;
			buildSurfaces(d, built);

			// Dug blocks: Sekiro's geometry in them is gone. A diggable triangle touching one is cut
			// (what's outside every dug block stays), and its original goes along as a ghost, so
			// Minecraft still knows what's solid behind it.
			std::vector<proto::ColTri> tris;
			tris.reserve(built.size());
			std::vector<Clip::Cube> cubes;
			std::vector<Clip::Poly> pieces;
			for (const proto::ColTri& t : built) {
				cubes.clear();
				if ((t.flags & proto::kTriDiggable) && !g_dug.empty()) {
					const int x0 = int(std::floor(std::min({ t.v[0], t.v[3], t.v[6] }))), x1 = int(std::floor(std::max({ t.v[0], t.v[3], t.v[6] })));
					const int y0 = int(std::floor(std::min({ t.v[1], t.v[4], t.v[7] }) - 0.001f)), y1 = int(std::floor(std::max({ t.v[1], t.v[4], t.v[7] })));
					const int z0 = int(std::floor(std::min({ t.v[2], t.v[5], t.v[8] }))), z1 = int(std::floor(std::max({ t.v[2], t.v[5], t.v[8] })));
					for (int x = x0; x <= x1; ++x)
						for (int y = y0; y <= y1; ++y)
							for (int z = z0; z <= z1; ++z)
								if (isDug(x, y, z))
									cubes.push_back({ x, y, z });
				}
				if (cubes.empty()) {
					tris.push_back(t);
					continue;
				}
				tris.push_back({ { t.v[0], t.v[1], t.v[2], t.v[3], t.v[4], t.v[5], t.v[6], t.v[7], t.v[8] }, t.flags | proto::kTriGhost });
				pieces.clear();
				Clip::Subtract(Clip::FromTriangle(t.v, t.v + 3, t.v + 6), cubes, pieces);
				for (const auto& piece : pieces)
					for (std::size_t v = 1; v + 1 < piece.size(); ++v)
						tris.push_back({ { piece[0].p[0], piece[0].p[1], piece[0].p[2], piece[v].p[0], piece[v].p[1], piece[v].p[2], piece[v + 1].p[0],
										   piece[v + 1].p[1], piece[v + 1].p[2] },
							t.flags });
			}

			const int x0 = d.job.rx * kRegion, z0 = d.job.rz * kRegion;
			std::string buf;
			for (int ry = d.job.band.ryLo; ry <= d.job.band.ryHi; ++ry) {
				const int y0 = ry * kRegion;

				// Triangles touching this region's height range.
				proto::ColRegion tr{ x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, 0 };
				buf.assign(reinterpret_cast<const char*>(&tr), sizeof(tr));
				for (const auto& t : tris) {
					if (overlapsY(t, float(y0), float(y0 + kRegion))) {
						buf.append(reinterpret_cast<const char*>(&t), sizeof(t));
						++tr.count;
					}
				}
				std::memcpy(buf.data(), &tr, sizeof(tr));
				link.writeCollision(proto::kColTris, buf.data(), std::uint32_t(buf.size()));

				// Voxels: a thin slab under every surface of the region's own 16x16 columns.
				std::map<std::tuple<int, int, int>, proto::ColBlock> blocks;
				for (int dk = 1; dk <= kColsPerRegion; ++dk) {
					for (int di = 1; di <= kColsPerRegion; ++di) {
						const Column& c = d.cols[dk * kSpan + di];
						const int i = d.job.rx * kColsPerRegion + di - 1, k = d.job.rz * kColsPerRegion + dk - 1;
						const int bx = floorDiv(i, 2), bz = floorDiv(k, 2);
						const int sx = (i - 2 * bx) * 4, sz = (k - 2 * bz) * 4;  // sub-voxel offset inside the block
						std::uint64_t rowMask = 0;
						for (int zz = 0; zz < 4; ++zz)
							for (int xx = 0; xx < 4; ++xx)
								rowMask |= 1ull << ((sz + zz) * 8 + sx + xx);
						for (int s = 0; s < c.n; ++s) {
							const int l0 = int(std::floor((c.ys[s] - kSlab) * 8.0f)), l1 = int(std::floor(c.ys[s] * 8.0f - 1e-3f));
							for (int l = l0; l <= l1; ++l) {
								const int by = floorDiv(l, 8);
								if (by < y0 || by >= y0 + kRegion || isDug(bx, by, bz))
									continue;
								auto& b = blocks[{ bx, by, bz }];
								b.x = bx;
								b.y = by;
								b.z = bz;
								b.bits[l - by * 8] |= rowMask;
							}
						}
					}
				}
				proto::ColRegion vr{ x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, 0 };
				buf.assign(reinterpret_cast<const char*>(&vr), sizeof(vr));
				for (const auto& [key, b] : blocks) {
					buf.append(reinterpret_cast<const char*>(&b), sizeof(b));
					++vr.count;
				}
				std::memcpy(buf.data(), &vr, sizeof(vr));
				link.writeCollision(proto::kColRegion, buf.data(), std::uint32_t(buf.size()));
				++g_area.regionsSent;
			}
		}

		void queueAround(int cx, int cz)
		{
			std::vector<std::pair<int, int>> want;
			for (int dz = -kRadius; dz <= kRadius; ++dz)
				for (int dx = -kRadius; dx <= kRadius; ++dx)
					if (!g_area.queued.count({ cx + dx, cz + dz }))
						want.push_back({ cx + dx, cz + dz });
			std::sort(want.begin(), want.end(), [&](auto a, auto b) {
				return std::hypot(a.first - cx, a.second - cz) < std::hypot(b.first - cx, b.second - cz);
			});
			std::lock_guard lock(g_mutex);
			for (auto [rx, rz] : want) {
				g_area.queued.insert({ rx, rz });
				g_queue.push_back({ rx, rz, g_area.band });
			}
			// Nearest first, also among jobs queued earlier.
			std::stable_sort(g_queue.begin(), g_queue.end(), [&](const Job& a, const Job& b) {
				return std::hypot(a.rx - cx, a.rz - cz) < std::hypot(b.rx - cx, b.rz - cz);
			});
		}
	}

	void tickGameThread()
	{
		if (!game::castRayReady())
			return;
		const long long start = nowQpc();
		{
			std::lock_guard lock(g_mutex);
			if (!g_work.active || g_work.generation != g_generation) {
				g_work = {};
				if (g_queue.empty())
					return;
				if (g_groundOkGeneration != g_generation) {
					const unsigned long long now = GetTickCount64();
					if (g_probeGeneration != g_generation) {
						g_probeGeneration = g_generation;
						g_probeSince = now;
						g_probeLast = 0;
					}
					if (now - g_probeLast < 100)
						return;
					g_probeLast = now;
					game::RayHit hit;
					const game::Vec3 from{ g_groundProbe.x, g_groundProbe.y + 1.5f, g_groundProbe.z };
					const bool ground = game::castRay(from, { 0.0f, -4.0f, 0.0f }, hit);
					++g_casts;
					if (!ground && now - g_probeSince < kGroundWaitMs)
						return;
					g_groundOkGeneration = g_generation;
					logf(ground ? "collision: ground under the player after %.1f s; scanning" : "collision: no ground under the player after %.1f s; scanning anyway",
						(now - g_probeSince) / 1000.0);
				}
				g_work.active = true;
				g_work.generation = g_generation;
				g_work.job = g_queue.front();
				g_queue.pop_front();
				g_work.out.job = g_work.job;
				g_work.out.cols.assign(kSpan * kSpan, Column{});
			}
		}
		if (g_cacheBand != g_work.job.band.id || g_cacheGeneration != g_work.generation) {
			g_cache.clear();
			g_cacheBand = g_work.job.band.id;
			g_cacheGeneration = g_work.generation;
		}

		const Job& job = g_work.job;
		const int i0 = job.rx * kColsPerRegion - 1, k0 = job.rz * kColsPerRegion - 1;
		const int pairsPerAxis = (kSpan - 1) * kColsPerRegion;  // 17 boundaries x 16 rows

		while (qpcToMs(nowQpc() - start) < kBudgetMs) {
			if (g_work.phase == 0) {
				if (g_work.index >= kSpan * kSpan) {
					g_work.phase = 1;
					g_work.index = 0;
					continue;
				}
				const int di = g_work.index % kSpan, dk = g_work.index / kSpan;
				const long long key = colKey(i0 + di, k0 + dk);
				auto it = g_cache.find(key);
				if (it == g_cache.end())
					it = g_cache.emplace(key, scanColumn(i0 + di, k0 + dk, job.band)).first;
				else if (job.rescan)
					it->second = scanColumn(i0 + di, k0 + dk, job.band);
				g_work.out.cols[g_work.index] = it->second;
				++g_work.index;
				continue;
			}
			if (g_work.index >= 2 * pairsPerAxis) {
				std::lock_guard lock(g_mutex);
				if (g_work.generation == g_generation)
					g_done.push_back(std::move(g_work.out));
				g_work = {};
				break;
			}
			// Boundaries along x (between di and di+1, rows dk 1..16), then along z; both directions.
			const bool alongX = g_work.index < pairsPerAxis;
			const int  p = alongX ? g_work.index : g_work.index - pairsPerAxis;
			const int  step = p % (kSpan - 1), row = 1 + p / (kSpan - 1);
			const int  di = alongX ? step : row, dk = alongX ? row : step;
			const int  dj = alongX ? di + 1 : di, dl = alongX ? dk : dk + 1;
			const Column& a = g_work.out.cols[dk * kSpan + di];
			const Column& b = g_work.out.cols[dl * kSpan + dj];
			testWalls(a, i0 + di, k0 + dk, b, i0 + dj, k0 + dl, g_work.out.walls);
			testWalls(b, i0 + dj, k0 + dl, a, i0 + di, k0 + dk, g_work.out.walls);
			++g_work.index;
		}
		g_tickUs += (long long)(qpcToMs(nowQpc() - start) * 1000.0);
		++g_ticks;
	}

	void update(GameLink& link, bool inGame, game::Vec3 p, std::uint32_t& epoch)
	{
		if (!game::castRayReady())
			return;
		if (!inGame) {
			if (g_area.active)
				reset();
			return;
		}
		const int rx = floorDiv(int(std::floor(p.x)), kRegion), rz = floorDiv(int(std::floor(p.z)), kRegion);
		const int ry = floorDiv(int(std::floor(p.y)), kRegion);

		const bool teleported = g_area.active &&
			std::abs(p.x - g_area.last.x) + std::abs(p.y - g_area.last.y) + std::abs(p.z - g_area.last.z) > kTeleport;
		if (!g_area.active || teleported) {
			// New epoch: Minecraft drops everything it has and we start over around the player.
			{
				std::lock_guard lock(g_mutex);
				++g_generation;
				g_queue.clear();
				g_done.clear();
			}
			const int nextBand = g_area.band.id + 1;
			g_doneCache.clear();
			g_retry.clear();
			{
				std::lock_guard lock(g_mutex);
				g_groundProbe = game::fromMc(p);
			}
			g_area = {};
			g_area.active = true;
			g_area.band = { nextBand, ry - kBelow, ry + kAbove };
			++epoch;
			link.writeCollision(proto::kColClear, &epoch, sizeof(epoch));
			g_area.cx = rx;
			g_area.cz = rz;
			queueAround(rx, rz);
			logf("collision: epoch %u around region (%d, %d, %d)%s", epoch, rx, ry, rz, teleported ? " (teleport)" : "");
		} else if (ry < g_area.band.ryLo + 2 || ry > g_area.band.ryHi - 1) {
			// Climbed or fell out of the band's comfortable middle: rescan at the new height.
			{
				std::lock_guard lock(g_mutex);
				g_queue.clear();
			}
			g_area.band = { g_area.band.id + 1, ry - kBelow, ry + kAbove };
			g_area.queued.clear();
			g_area.cx = rx;
			g_area.cz = rz;
			queueAround(rx, rz);
			logf("collision: new height band around region y %d", ry);
		} else if (rx != g_area.cx || rz != g_area.cz) {
			g_area.cx = rx;
			g_area.cz = rz;
			queueAround(rx, rz);
		}
		g_area.last = p;

		std::vector<Done> done;
		{
			std::lock_guard lock(g_mutex);
			done.swap(g_done);
		}
		const unsigned long long now = GetTickCount64();
		for (Done& d : done) {
			if (d.job.band.id != g_area.band.id)
				continue;
			const std::pair<int, int> key{ d.job.rx, d.job.rz };
			if (isEmpty(d)) {
				Retry& r = g_retry[key];
				if (r.tries < kEmptyRetries) {
					++r.tries;
					r.due = now + kEmptyRetryMs;
				}
			} else if (auto it = g_retry.find(key); it != g_retry.end()) {
				if (it->second.tries > 0)
					logf("collision: region column (%d, %d) has ground now (scan %d)", key.first, key.second, it->second.tries + 1);
				g_retry.erase(it);
			}
			sendJob(link, d, epoch);
			g_doneCache[{ d.job.rx, d.job.rz }] = std::move(d);
		}
		// Empty region columns near the player: scan again (Sekiro may have streamed them in since).
		{
			std::vector<Job> again;
			for (auto& [key, r] : g_retry) {
				if (r.due == 0 || now < r.due)
					continue;
				r.due = 0;  // set again if this scan is empty too
				if (std::abs(key.first - g_area.cx) <= kRadius && std::abs(key.second - g_area.cz) <= kRadius)
					again.push_back({ key.first, key.second, g_area.band, true });
			}
			if (!again.empty()) {
				std::lock_guard lock(g_mutex);
				for (const Job& j : again)
					g_queue.push_back(j);
			}
		}

		// Digs since last time: send those regions again from their cached scans.
		for (const auto& key : g_dugChanged) {
			auto it = g_doneCache.find(key);
			if (it != g_doneCache.end() && it->second.job.band.id == g_area.band.id)
				sendJob(link, it->second, epoch);
		}
		g_dugChanged.clear();
	}

	void onDug(const std::uint8_t* p, std::uint32_t bytes)
	{
		if (bytes < sizeof(proto::RenDug))
			return;
		proto::RenDug h;
		std::memcpy(&h, p, sizeof(h));
		const long long key = sectionKey(h.sx, h.sy, h.sz);
		if (h.count == 0 || bytes < sizeof(h) + 512) {
			g_dug.erase(key);
		} else {
			DugSection& d = g_dug[key];
			std::memcpy(d.bits.data(), p + sizeof(h), 512);
		}
		// The region columns this section overlaps (2x2 of them: sections are 16, regions 8).
		for (int rx = h.sx * 2; rx <= h.sx * 2 + 1; ++rx)
			for (int rz = h.sz * 2; rz <= h.sz * 2 + 1; ++rz)
				g_dugChanged.insert({ rx, rz });
		++g_dugGeneration;
		static int logged = 0;
		if (logged++ < 20)
			logf("collision: Minecraft dug %u blocks in section (%d, %d, %d)", h.count, h.sx, h.sy, h.sz);
	}

	bool dugAt(int x, int y, int z)
	{
		return isDug(x, y, z);
	}

	unsigned dugGeneration()
	{
		return g_dugGeneration;
	}

	void dugWindow(int ox, int oy, int oz, int n, std::vector<std::uint8_t>& out)
	{
		out.assign(std::size_t(n) * n * n, 0);
		for (const auto& [key, d] : g_dug) {
			// Decode the section from its key (see sectionKey).
			const auto sext = [](long long v, int bits) { return int((v << (64 - bits)) >> (64 - bits)); };
			const int sx = sext(key >> 42, 22), sy = sext((key >> 22) & 0xFFFFF, 20), sz = sext(key & 0x3FFFFF, 22);
			if (sx * 16 + 16 <= ox || sx * 16 >= ox + n || sy * 16 + 16 <= oy || sy * 16 >= oy + n || sz * 16 + 16 <= oz || sz * 16 >= oz + n)
				continue;
			for (int bit = 0; bit < 4096; ++bit) {
				if (!((d.bits[bit >> 6] >> (bit & 63)) & 1))
					continue;
				const int x = sx * 16 + (bit & 15) - ox, z = sz * 16 + ((bit >> 4) & 15) - oz, y = sy * 16 + (bit >> 8) - oy;
				if (x >= 0 && y >= 0 && z >= 0 && x < n && y < n && z < n)
					out[std::size_t(x) + std::size_t(n) * (std::size_t(y) + std::size_t(n) * std::size_t(z))] = 1;
			}
		}
	}

	void clearDug()
	{
		++g_dugGeneration;
		g_dug.clear();
		for (const auto& [key, d] : g_doneCache)
			g_dugChanged.insert(key);
	}

	void reset()
	{
		std::lock_guard lock(g_mutex);
		++g_generation;
		g_queue.clear();
		g_done.clear();
		const int band = g_area.band.id;
		g_area = {};
		g_area.band.id = band;
	}

	std::string stats()
	{
		const long long casts = g_casts.exchange(0), us = g_tickUs.exchange(0), ticks = g_ticks.exchange(0);
		std::size_t queued;
		{
			std::lock_guard lock(g_mutex);
			queued = g_queue.size();
		}
		char buf[200];
		std::snprintf(buf, sizeof(buf), "collision: %lld casts, %.1f ms ray time over %lld ticks (%.2f ms/tick), %zu region columns queued, %d regions sent",
			casts, us / 1000.0, ticks, ticks ? us / 1000.0 / double(ticks) : 0.0, queued, g_area.regionsSent);
		return buf;
	}
}
