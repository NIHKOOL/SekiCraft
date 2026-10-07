#include "combat.h"

#include "game.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sekicraft::combat
{
	namespace
	{
		namespace proto = sekicraft::proto;

		constexpr float     kRange = 48.0f;          // characters this close to Wolf get stand-ins (m)
		constexpr float     kHpPerMcDamage = 12.0f;  // a diamond sword (7) takes ~6 hits on a 476 HP soldier
		constexpr float     kPosturePerMcDamage = 8.0f;
		constexpr int       kEnemyTeam = 6;
		constexpr int       kHelperMaxHp = 9999;     // invisible helper objects have 9999 HP
		constexpr ULONGLONG kScanEveryMs = 1000;

		struct Hit
		{
			std::uint32_t handle;
			float         mcDamage;
			bool          critical;
		};

		// worker <-> game thread
		std::mutex                                    g_mutex;
		std::vector<Hit>                              g_hits;
		std::unordered_map<std::uint32_t, std::uintptr_t> g_byHandle;  // nearby characters by handle
		std::atomic<float>                            g_wolfLost{ 0 };     // HP Wolf lost, not yet sent to Minecraft
		std::atomic<std::uint32_t>                    g_wolfAttacker{ 0 }; // the enemy that most likely did it (handle)
		std::atomic<bool>                             g_minecraftDied{ false };
		std::atomic<int>                              g_wolfMaxHp{ 0 };
		std::atomic<int>                              g_hitsApplied{ 0 }, g_kills{ 0 }, g_postureBreaks{ 0 }, g_deathblows{ 0 };
		std::vector<DeathblowRequest>                 g_deathblowRequests;  // game thread -> worker (g_mutex)
		std::vector<DeathblowRequest>                 g_forced;             // worker -> game thread (g_mutex)

		// worker only
		std::vector<std::uintptr_t> g_known;
		ULONGLONG                   g_lastScan = 0;
		std::size_t                 g_actorsSent = 0;
		double                      g_scanMs = 0;

		// game thread only
		bool g_wolfGuarded = false;
		int  g_wolfLastHp = -1;
	}

	void update(GameLink& link, bool minecraftControls)
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastScan >= kScanEveryMs) {
			g_lastScan = now;
			LARGE_INTEGER t0, t1, f;
			QueryPerformanceCounter(&t0);
			game::scanCharacters(g_known);
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			g_scanMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
		}

		game::Character wolf{};
		const std::uintptr_t hero = game::player();
		if (!hero || !game::readCharacter(hero, wolf)) {
			link.writeActors(nullptr, 0);
			return;
		}

		// The actor table: nearby characters that can be fought (not Wolf, not helper objects).
		static proto::ActorRecord actors[proto::kMaxActors];
		std::uint32_t count = 0;
		std::unordered_map<std::uint32_t, std::uintptr_t> byHandle;
		for (std::uintptr_t c : g_known) {
			game::Character ch{};
			if (c == hero || !game::readCharacter(c, ch) || ch.maxHp <= 0 || ch.maxHp >= kHelperMaxHp)
				continue;
			const float dx = ch.pos.x - wolf.pos.x, dy = ch.pos.y - wolf.pos.y, dz = ch.pos.z - wolf.pos.z;
			if (dx * dx + dy * dy + dz * dz > kRange * kRange || count >= proto::kMaxActors)
				continue;
			const game::Vec3 m = game::toMc(ch.pos);
			proto::ActorRecord& a = actors[count++];
			a = {};
			a.formId = ch.handle;
			a.flags = (ch.team == kEnemyTeam ? proto::kActorHostile : 0u) | (ch.hp <= 0 ? proto::kActorDead : 0u);
			a.x = m.x;
			a.y = m.y;
			a.z = m.z;
			a.yaw = game::yawToMc(ch.theta);
			a.width = 0.8f;   // human-sized; bigger enemies later (from their character params)
			a.height = 1.9f;
			a.healthFrac = std::clamp(float(ch.hp) / float(ch.maxHp), 0.0f, 1.0f);
			a.level = 1;
			std::snprintf(a.name, sizeof(a.name), "c%04u", ch.characterId / 10000);
			byHandle[ch.handle] = c;
		}
		link.writeActors(actors, count);
		g_actorsSent = count;
		{
			std::lock_guard lock(g_mutex);
			g_byHandle.swap(byHandle);
		}

		// Hits on Wolf -> Minecraft damage. Minecraft divides what it's sent by 5 (SkyCraft's Skyrim
		// scale), so send lost * 100 / maxHp to get lost / maxHp of Minecraft's 20 health.
		const float lost = g_wolfLost.exchange(0.0f);
		const int maxHp = g_wolfMaxHp;
		if (lost > 0 && maxHp > 0 && minecraftControls) {
			const float sent = lost * 100.0f / float(maxHp);
			const std::uint32_t attacker = g_wolfAttacker.exchange(0);
			link.sendInput(proto::kInHurt, proto::kHurtMelee, int(sent * 100.0f), std::int32_t(attacker), 0);
			logf("combat: Sekiro hit Wolf for %.0f HP (%.0f%% of %d) -> %.1f Minecraft damage, attacker %08X", lost, lost * 100.0f / float(maxHp), maxHp,
				sent / 5.0f, attacker);
		}
	}

	void onEvent(const proto::McEvent& ev)
	{
		if (ev.type == proto::kEvHitActor) {
			std::lock_guard lock(g_mutex);
			g_hits.push_back({ ev.formId, ev.a, (ev.flags & proto::kHitCritical) != 0 });
		} else if (ev.type == proto::kEvPlayerDied) {
			logf("combat: the Minecraft player died");
			g_minecraftDied = true;
		}
	}

	void tickGameThread(bool minecraftControls)
	{
		// Minecraft's hits on Sekiro's characters.
		std::vector<Hit> hits;
		std::unordered_map<std::uint32_t, std::uintptr_t> byHandle;
		{
			std::lock_guard lock(g_mutex);
			hits.swap(g_hits);
			if (!hits.empty())
				byHandle = g_byHandle;
		}
		for (const Hit& h : hits) {
			auto it = byHandle.find(h.handle);
			game::Character c{};
			if (it == byHandle.end() || !game::readCharacter(it->second, c) || (c.hp <= 0 && game::lives(c) <= 0))
				continue;
			const int hpDamage = std::max(1, int(std::lround(h.mcDamage * kHpPerMcDamage)));
			const int postureDamage = std::max(1, int(std::lround(h.mcDamage * kPosturePerMcDamage)));
			const int hp = std::max(0, c.hp - hpDamage);
			const int posture = std::max(0, c.posture - postureDamage);
			const int left = game::lives(c);
			++g_hitsApplied;
			// Deathblow-ready: posture broken, or a boss with its HP gone (Sekiro keeps it at 1 while
			// lives are left). The hit asks for Sekiro's own deathblow instead of doing damage.
			if (c.posture <= 0 || (c.hp <= 1 && left >= 1)) {
				std::lock_guard lock(g_mutex);
				g_deathblowRequests.push_back({ h.handle, c.chr, left });
				logf("combat: %08X (c%04u) is ready for a deathblow (posture %d, HP %d, lives %d)", h.handle, c.characterId / 10000, c.posture, c.hp, left);
				continue;
			}
			if (hp <= 0 && left <= 1) {  // an ordinary enemy's HP gone: it dies
				game::setLives(c, 0);
				game::setHp(c, 0);
				++g_kills;
				logf("combat: Minecraft hit %08X (c%04u) for %.1f: killed", h.handle, c.characterId / 10000, h.mcDamage);
				continue;
			}
			game::setHp(c, std::max(hp, 1));
			game::setPosture(c, posture);  // at 0 Sekiro breaks its posture: it staggers, red dot
			if (posture == 0)
				++g_postureBreaks;
			logf("combat: Minecraft hit %08X (c%04u) for %.1f%s: HP %d -> %d of %d, posture %d -> %d of %d, lives %d", h.handle, c.characterId / 10000, h.mcDamage,
				h.critical ? " (critical)" : "", c.hp, std::max(hp, 1), c.maxHp, c.posture, posture, c.maxPosture, left);
		}

		// Deathblows Sekiro didn't perform: done in memory (one life off, or dead).
		std::vector<DeathblowRequest> forced;
		{
			std::lock_guard lock(g_mutex);
			forced.swap(g_forced);
		}
		for (const DeathblowRequest& r : forced) {
			game::Character c{};
			if (!game::readCharacter(r.chr, c))
				continue;
			const int left = game::lives(c);
			if (left > 1) {
				game::setLives(c, left - 1);
				game::setHp(c, c.maxHp);
				game::refillPosture(c);
			} else {
				game::setLives(c, 0);
				game::setHp(c, 0);
				++g_kills;
			}
			logf("combat: deathblow on %08X done in memory: %d lives left", r.handle, std::max(left - 1, 0));
		}

		// Wolf: unkillable while Minecraft drives; what he loses goes to Minecraft.
		game::Character wolf{};
		const std::uintptr_t hero = game::player();
		if (!hero || !game::readCharacter(hero, wolf))
			return;
		g_wolfMaxHp = wolf.maxHp;
		if (minecraftControls) {
			if (!g_wolfGuarded) {
				g_wolfGuarded = game::setNoDeath(wolf, true);
				g_wolfLastHp = wolf.hp;
			}
			if (g_wolfLastHp >= 0 && wolf.hp < g_wolfLastHp) {
				// Sekiro doesn't tell us who hit Wolf; the nearest enemy within 8 m is the best guess.
				// Minecraft needs an attacker to know the hit's direction (shields block by it).
				std::unordered_map<std::uint32_t, std::uintptr_t> nearby;
				{
					std::lock_guard lock(g_mutex);
					nearby = g_byHandle;
				}
				float best = 8.0f * 8.0f;
				std::uint32_t attacker = 0;
				for (const auto& [handle, chr] : nearby) {
					game::Character e{};
					if (!game::readCharacter(chr, e) || e.team != kEnemyTeam || e.hp <= 0)
						continue;
					const float dx = e.pos.x - wolf.pos.x, dy = e.pos.y - wolf.pos.y, dz = e.pos.z - wolf.pos.z;
					const float d2 = dx * dx + dy * dy + dz * dz;
					if (d2 < best) {
						best = d2;
						attacker = handle;
					}
				}
				g_wolfAttacker = attacker;
				g_wolfLost = g_wolfLost.load() + float(g_wolfLastHp - wolf.hp);
				game::setHp(wolf, wolf.maxHp);  // Minecraft's health is what counts
				wolf.hp = wolf.maxHp;
			}
			g_wolfLastHp = wolf.hp;
		} else if (g_wolfGuarded) {
			game::setNoDeath(wolf, false);
			g_wolfGuarded = false;
			g_wolfLastHp = -1;
		}
	}

	bool takeMinecraftDeath()
	{
		return g_minecraftDied.exchange(false);
	}

	bool takeDeathblowRequest(DeathblowRequest& out)
	{
		std::lock_guard lock(g_mutex);
		if (g_deathblowRequests.empty())
			return false;
		out = g_deathblowRequests.front();
		g_deathblowRequests.clear();  // one at a time; later clicks during the stagger are the same request
		return true;
	}

	bool deathblowDone(const DeathblowRequest& r)
	{
		game::Character c{};
		if (!game::readCharacter(r.chr, c))
			return true;  // gone
		if (c.hp <= 0 || game::lives(c) < r.livesBefore) {
			++g_deathblows;
			return true;
		}
		return false;
	}

	void forceDeathblow(const DeathblowRequest& r)
	{
		std::lock_guard lock(g_mutex);
		g_forced.push_back(r);
	}

	void reset()
	{
		std::lock_guard lock(g_mutex);
		g_hits.clear();
		g_byHandle.clear();
		g_deathblowRequests.clear();
		g_forced.clear();
		g_known.clear();
		g_lastScan = 0;
	}

	void shutdown()
	{
		game::Character wolf{};
		if (g_wolfGuarded && game::readCharacter(game::player(), wolf))
			game::setNoDeath(wolf, false);
		g_wolfGuarded = false;
	}

	std::string stats()
	{
		char buf[200];
		std::snprintf(buf, sizeof(buf), "combat: %zu characters known (scan %.1f ms), %zu stand-ins in Minecraft, %d hits applied, %d kills, %d posture breaks, %d Sekiro deathblows",
			g_known.size(), g_scanMs, g_actorsSent, g_hitsApplied.load(), g_kills.load(), g_postureBreaks.load(), g_deathblows.load());
		return buf;
	}
}
