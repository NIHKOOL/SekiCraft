// SekiCraft game-side link: the shared memory as the game (Sekiro, or tools/fakegame) sees it.
//
// The game side creates the mapping and produces SkyState, the collision ring and the input ring;
// it consumes McState, the render ring and the event ring. Header-only (C++20, Windows).
#pragma once

#include "sekicraft_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>

namespace sekicraft
{
	class GameLink
	{
	public:
		enum class OpenResult { kCreated, kTookOver, kOtherGameLive, kFailed };

		GameLink() = default;
		GameLink(const GameLink&) = delete;
		GameLink& operator=(const GameLink&) = delete;
		~GameLink() { close(); }

		// Creates the mapping, or takes over one Minecraft kept open after an earlier game side
		// exited (ring positions are kept, so Minecraft's tails stay valid).
		OpenResult open()
		{
			const std::uint64_t bytes = proto::kMappingBytes;
			mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				DWORD(bytes >> 32), DWORD(bytes & 0xFFFFFFFF), proto::kMappingName);
			if (!mapping_)
				return OpenResult::kFailed;
			const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
			base_ = static_cast<std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
			if (!base_) {
				close();
				return OpenResult::kFailed;
			}
			auto* hdr = header();
			if (existed && hdr->magic == proto::kMagic && GetTickCount64() - gameBeat().load() < 3000 && hdr->skyrimPid != GetCurrentProcessId()) {
				otherPid_ = hdr->skyrimPid;
				close();
				return OpenResult::kOtherGameLive;
			}
			if (existed) {
				prevTeleportSeq_ = at<proto::SkyState>(proto::kOffSkyState)->teleportSeq;
				prevEpoch_ = at<proto::SkyState>(proto::kOffSkyState)->collisionEpoch;
			}
			// Overlay triple buffer: a new game process starts it over (Minecraft restarts its side
			// when it sees our pid change). A reload inside the same process keeps going; our front
			// slot lives in OverlayCtl::pad so it survives the reload.
			auto* overlay = at<proto::OverlayCtl>(proto::kOffOverlayCtl);
			if (!existed || hdr->skyrimPid != GetCurrentProcessId()) {
				std::atomic_ref<std::uint32_t>(overlay->state).store(0, std::memory_order_release);
				overlay->pad = 2;
			}
			hdr->magic = proto::kMagic;
			hdr->version = proto::kVersion;
			hdr->skyrimPid = GetCurrentProcessId();
			heartbeat();
			return existed ? OpenResult::kTookOver : OpenResult::kCreated;
		}

		void close()
		{
			if (base_)
				UnmapViewOfFile(base_);
			if (mapping_)
				CloseHandle(mapping_);
			base_ = nullptr;
			mapping_ = nullptr;
		}

		bool isOpen() const { return base_ != nullptr; }
		std::uint32_t otherGamePid() const { return otherPid_; }

		// Values left by a previous game side; continue from them so Minecraft sees changes.
		std::uint32_t previousTeleportSeq() const { return prevTeleportSeq_; }
		std::uint32_t previousCollisionEpoch() const { return prevEpoch_; }

		// ---- header ---------------------------------------------------------------------------

		void heartbeat() { gameBeat().store(GetTickCount64(), std::memory_order_release); }

		std::uint32_t minecraftPid() const { return header()->mcPid; }

		bool minecraftAlive(std::uint64_t timeoutMs = 3000) const
		{
			const auto beat = std::atomic_ref<std::uint64_t>(header()->mcHeartbeatMs).load(std::memory_order_acquire);
			return header()->mcPid != 0 && GetTickCount64() - beat < timeoutMs;
		}

		// ---- SkyState (we write, seqlock) -----------------------------------------------------
		// Write once per game frame: Minecraft paces its own frames on the sequence advancing.

		void writeSkyState(const proto::SkyState& st)
		{
			auto* dst = at<proto::SkyState>(proto::kOffSkyState);
			auto seq = std::atomic_ref<std::uint32_t>(dst->seq);
			const std::uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1, std::memory_order_relaxed);  // odd: writing
			std::atomic_thread_fence(std::memory_order_release);
			std::memcpy(reinterpret_cast<char*>(dst) + 4, reinterpret_cast<const char*>(&st) + 4, sizeof(st) - 4);
			seq.store(s + 2, std::memory_order_release);
		}

		// ---- McState (we read, seqlock) -------------------------------------------------------

		bool readMcState(proto::McState& out) const
		{
			auto* src = at<proto::McState>(proto::kOffMcState);
			auto seq = std::atomic_ref<std::uint32_t>(src->seq);
			for (int tries = 0; tries < 100; ++tries) {
				const std::uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 & 1)
					continue;
				std::memcpy(&out, src, sizeof(out));
				std::atomic_thread_fence(std::memory_order_acquire);
				if (seq.load(std::memory_order_relaxed) == s0)
					return true;
			}
			return false;
		}

		// ---- collision ring (we produce) ------------------------------------------------------

		void writeCollision(std::uint32_t type, const void* payload, std::uint32_t bytes)
		{
			const std::uint64_t ring = proto::kOffCollisionRing;
			const std::uint64_t cap = proto::kColRingDataBytes;
			const std::uint64_t total = (8 + std::uint64_t(bytes) + 7) & ~7ull;
			auto headRef = u64(ring + proto::kColRingHeadOff);
			std::uint64_t head = headRef.load(std::memory_order_relaxed);

			// Wait for room (Minecraft drains on its own thread).
			while (head + total + 8 - u64(ring + proto::kColRingTailOff).load(std::memory_order_acquire) > cap)
				Sleep(1);

			std::uint64_t pos = head % cap;
			if (pos + total > cap) {  // doesn't fit before the end: pad to the start
				auto* pad = at<proto::ColMsgHeader>(ring + proto::kColRingDataOff + pos);
				pad->type = proto::kColPad;
				pad->payloadBytes = 0;
				head += cap - pos;
				pos = 0;
			}
			auto* hdr = at<proto::ColMsgHeader>(ring + proto::kColRingDataOff + pos);
			hdr->type = type;
			hdr->payloadBytes = bytes;
			std::memcpy(hdr + 1, payload, bytes);
			headRef.store(head + total, std::memory_order_release);
		}

		// ---- input ring (we produce) ----------------------------------------------------------

		void sendInput(std::uint16_t type, std::uint16_t code, std::int32_t a, std::int32_t b = 0, std::int32_t c = 0)
		{
			const std::uint64_t ring = proto::kOffInputRing;
			auto headRef = u64(ring + proto::kInputRingHeadOff);
			const std::uint64_t head = headRef.load(std::memory_order_relaxed);
			auto* ev = at<proto::InputEvent>(ring + proto::kInputRingDataOff + (head & (proto::kInputRingEntries - 1)) * sizeof(proto::InputEvent));
			*ev = { type, code, a, b, c };
			headRef.store(head + 1, std::memory_order_release);
		}

		// ---- render ring (we consume) ---------------------------------------------------------
		// Calls fn(type, payload, payloadBytes) per message; returns bytes consumed.

		std::uint64_t drainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn = {})
		{
			const std::uint64_t ring = proto::kOffRenderRing;
			const std::uint64_t cap = proto::kRenRingDataBytes;
			const std::uint64_t head = u64(ring + proto::kRenRingHeadOff).load(std::memory_order_acquire);
			auto tailRef = u64(ring + proto::kRenRingTailOff);
			const std::uint64_t start = tailRef.load(std::memory_order_relaxed);
			std::uint64_t tail = start;
			while (fn && tail < head) {
				const std::uint64_t pos = tail % cap;
				const auto* msg = at<proto::ColMsgHeader>(ring + proto::kRenRingDataOff + pos);  // same {type, bytes} framing
				if (msg->type == proto::kRenPad) {
					tail += cap - pos;
					continue;
				}
				fn(msg->type, reinterpret_cast<const std::uint8_t*>(msg + 1), msg->payloadBytes);
				tail += (8 + std::uint64_t(msg->payloadBytes) + 7) & ~7ull;
			}
			tailRef.store(head, std::memory_order_release);
			return head - start;
		}

		// ---- overlay (Minecraft's hand + HUD + screens; triple buffer, we read) ----------------
		// Minecraft renders into its back slot and swaps it into the middle with the dirty bit set;
		// we swap our front slot for the middle one when it's dirty.

		bool acquireOverlayFrame()
		{
			auto* ctl = at<proto::OverlayCtl>(proto::kOffOverlayCtl);
			auto state = std::atomic_ref<std::uint32_t>(ctl->state);
			if (!(state.load(std::memory_order_acquire) & proto::kOverlayDirty))
				return false;
			const std::uint32_t old = state.exchange(ctl->pad & 3, std::memory_order_acq_rel);
			ctl->pad = old & 3;
			return true;
		}

		const proto::OverlaySlotHdr* overlayHeader() const
		{
			const std::uint32_t front = at<proto::OverlayCtl>(proto::kOffOverlayCtl)->pad & 3;
			return at<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * front);
		}

		const std::uint8_t* overlayPixels() const
		{
			const std::uint32_t front = at<proto::OverlayCtl>(proto::kOffOverlayCtl)->pad & 3;
			return at<std::uint8_t>(proto::kOffOverlayPixels + proto::kOverlaySlotBytes * front);
		}

		// ---- event ring (we consume) ----------------------------------------------------------

		template <class Fn>
		std::uint32_t drainEvents(Fn&& fn)
		{
			const std::uint64_t ring = proto::kOffEventRing;
			const std::uint64_t head = u64(ring + proto::kEventRingHeadOff).load(std::memory_order_acquire);
			auto tailRef = u64(ring + proto::kEventRingTailOff);
			std::uint64_t tail = tailRef.load(std::memory_order_relaxed);
			std::uint32_t n = 0;
			for (; tail < head; ++tail, ++n)
				fn(*at<proto::McEvent>(ring + proto::kEventRingDataOff + (tail & (proto::kEventRingEntries - 1)) * sizeof(proto::McEvent)));
			tailRef.store(tail, std::memory_order_release);
			return n;
		}

	private:
		template <class T>
		T* at(std::uint64_t off) const
		{
			return reinterpret_cast<T*>(base_ + off);
		}

		std::atomic_ref<std::uint64_t> u64(std::uint64_t off) const { return std::atomic_ref<std::uint64_t>(*at<std::uint64_t>(off)); }
		proto::Header* header() const { return at<proto::Header>(proto::kOffHeader); }
		std::atomic_ref<std::uint64_t> gameBeat() const { return std::atomic_ref<std::uint64_t>(header()->skyrimHeartbeatMs); }

		HANDLE        mapping_ = nullptr;
		std::uint8_t* base_ = nullptr;
		std::uint32_t otherPid_ = 0;
		std::uint32_t prevTeleportSeq_ = 0;
		std::uint32_t prevEpoch_ = 0;
	};
}
