#include "collision.h"

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
				const float floorY = from.ys[s];
				const float ceiling = s > 0 ? from.ys[s - 1] : INFINITY;
				float top = -INFINITY, wallAt = 0;
				int hits = 0;
				for (float dh : kWallHeights) {
					const float h = floorY + dh;
					if (h > ceiling - 0.05f)
						break;
					game::RayHit hit{};
					if (!cast({ fx, h, fz }, { tx - fx, 0, tz - fz }, hit))
						continue;
					const float above = to.lowestAbove(h - 0.05f);
					top = std::max(top, std::min(std::isnan(above) ? floorY + kWallTopDefault : above, ceiling));
					wallAt += alongX ? hit.pos.x : hit.pos.z;
					++hits;
				}
				if (!hits || top <= floorY + 0.05f)
					continue;
				wallAt /= float(hits);
				if (alongX) {
					const float z0 = float(fk) * kSpacing, z1 = z0 + kSpacing;
					addQuad(out, { wallAt, floorY, z0 }, { wallAt, top, z0 }, { wallAt, top, z1 }, { wallAt, floorY, z1 });
				} else {
					const float x0 = float(fi) * kSpacing, x1 = x0 + kSpacing;
					addQuad(out, { x0, floorY, wallAt }, { x0, top, wallAt }, { x1, top, wallAt }, { x1, floorY, wallAt });
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
							auto tri = [&](int c0, int c1, int c2) {
								const game::Vec3 a = p(c0), b = p(c1), e = p(c2);
								out.push_back({ { a.x, a.y, a.z, b.x, b.y, b.z, e.x, e.y, e.z }, 0 });
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
			std::vector<proto::ColTri> tris = d.walls;
			buildSurfaces(d, tris);

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
								if (by < y0 || by >= y0 + kRegion)
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
		for (const Done& d : done)
			if (d.job.band.id == g_area.band.id)
				sendJob(link, d, epoch);
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
