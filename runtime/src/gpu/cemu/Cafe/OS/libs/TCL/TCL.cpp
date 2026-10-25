#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"

#include "HW/Latte/Core/LattePM4.h"
#include "OS/libs/coreinit/coreinit_Time.h"

namespace TCL
{
	SysAllocator<coreinit::OSEvent> s_updateRetirementEvent;
	uint64 s_currentRetireMarker = 0;

	struct TCLStatePPC // mapped into PPC space
	{
		uint64be gpuRetireMarker; // written by GPU
	};

	SysAllocator<TCLStatePPC> s_tclStatePPC;

	// called from GPU for timestamp EOP event
	void TCLGPUNotifyNewRetirementTimestamp()
	{
		// gpuRetireMarker is updated via event eop command
		__OSLockScheduler();
		coreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());
		__OSUnlockScheduler();
	}

	void TCLPublish(int why);
	extern std::atomic<uint64> s_publishedMarker;

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut)
	{
		TCLPublish(2);  // polling for GPU progress
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			MEMPTR<uint32> b;
			// this is the timestamp of the last buffer that was retired by the GPU
			stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
			*timestampOut = retireTimestamp.load();
			return 0;
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLTimestamp(): Unsupported timestamp ID {}", (uint32)id);
			*timestampOut = 0;
			return 0;
		}
	}

	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout)
	{
		if (waitTs > s_publishedMarker.load(std::memory_order::acquire))
		{
			TCLPublish(3);  // waiting for work the GPU hasn't been given yet
		}
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			while ( true )
			{
				stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
				uint64 currentTimestamp = retireTimestamp.load();
				if (currentTimestamp >= waitTs)
					return 0;
				coreinit::OSWaitEvent(s_updateRetirementEvent.GetPtr());
			}
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLWaitTimestamp(): Unsupported timestamp ID {}", (uint32)id);
		}
		return 0;
	}

	static constexpr uint32 TCL_RING_BUFFER_SIZE = 4096; // in U32s

	std::atomic<uint32> tclRingBufferA[TCL_RING_BUFFER_SIZE];
	std::atomic<uint32> tclRingBufferA_readIndex{0};
	std::atomic<uint32> tclRingBufferA_writeIndex{0};

	// WF_GPU_LAG_MS (default 0: off) after it was submitted, or at once when the game waits on the GPU (timestamps,
	// draw done, flips, occlusion results, a full ring) or swaps. Executing commands the instant they arrive
	// let the GPU draw with uniform data the game was still writing for that frame: on Outset the palms'
	// depth pass got last frame's leaf alpha and the color pass this frame's, so leaves vanished.
	// WF_GPU_LAG_MS=0 turns it off; when commands are held and none arrive for 40 ms they are released too.
	std::atomic<uint64> s_written64{0};    // words written to the ring so far
	std::atomic<uint64> s_published64{0};  // words the GPU may read
	uint64 s_read64 = 0;                   // words the GPU has read (GPU thread)
	std::atomic<uint64> s_lastWriteNs{0};
	std::atomic<uint64> s_publishedMarker{0};  // retire marker of the last submission the GPU was given
	std::atomic<uint32> g_publishCounts[8];    // by reason, when something was held (WF_SWAPLOG)

	struct PendingSubmit { uint64 end; uint64 marker; uint64 timeNs; };
	static constexpr uint32 kPendingSize = 1024;
	PendingSubmit s_pending[kPendingSize];
	std::atomic<uint32> s_pendingHead{0}, s_pendingTail{0};  // written by the CPU / read by the GPU

	static uint64 now_ns()
	{
		return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::atomic<bool> g_holdWholeFrames{false};  // F10 (test): the GPU gets a frame only once the game has finished it

	static uint64 lag_ns()
	{
		static const uint64 lag = (uint64)((std::getenv("WF_GPU_LAG_MS") ? std::atof(std::getenv("WF_GPU_LAG_MS")) : 0.0) * 1000000.0);
		return g_holdWholeFrames.load(std::memory_order::relaxed) ? ~0ull / 2 : lag;
	}

	static void publish_to(uint64 end, uint64 marker)
	{
		uint64 cur = s_published64.load(std::memory_order::relaxed);
		while (end > cur && !s_published64.compare_exchange_weak(cur, end, std::memory_order::release)) {}
		uint64 m = s_publishedMarker.load(std::memory_order::relaxed);
		while (marker > m && !s_publishedMarker.compare_exchange_weak(m, marker, std::memory_order::release)) {}
	}

	void TCLPublish(int why)
	{
		uint64 marker = s_currentRetireMarker;  // read before the write count: never claims more than is published
		uint64 written = s_written64.load(std::memory_order::acquire);
		if (s_published64.load(std::memory_order::relaxed) < written)
			g_publishCounts[why & 7]++;
		publish_to(written, marker);
	}

	// GPU code calls this to grab the next command word
	bool TCLGPUReadRBWord(uint32& cmdWord)
	{
		if (s_read64 >= s_published64.load(std::memory_order::acquire))
		{
			// release what has waited long enough
			uint64 now = now_ns();
			uint32 tail = s_pendingTail.load(std::memory_order::relaxed);
			while (tail != s_pendingHead.load(std::memory_order::acquire) && s_pending[tail % kPendingSize].timeNs + lag_ns() <= now)
			{
				publish_to(s_pending[tail % kPendingSize].end, s_pending[tail % kPendingSize].marker);
				tail++;
			}
			s_pendingTail.store(tail, std::memory_order::release);
			if (s_read64 >= s_published64.load(std::memory_order::acquire))
			{
				uint64 last = s_lastWriteNs.load(std::memory_order::relaxed);
				if (s_published64.load(std::memory_order::relaxed) < s_written64.load(std::memory_order::acquire) && last && now - last > 40000000)
				{
					static uint32 s_logged = 0;
					if (s_logged++ < 5)
						cemuLog_log(LogType::Force, "GPU lag: released after 40 ms without new commands");
					TCLPublish(4);
				}
				return false;
			}
		}
		uint32 readIndex = (uint32)(s_read64 % TCL_RING_BUFFER_SIZE);
		cmdWord = tclRingBufferA[readIndex].load(std::memory_order::relaxed);
		s_read64++;
		tclRingBufferA_readIndex.store((uint32)(s_read64 % TCL_RING_BUFFER_SIZE), std::memory_order::release);
		return true;
	}

	void TCLWaitForRBSpace(uint32be numU32s)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);
		while (true)
		{
			uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::acquire);
			uint32 distance = (readIndex + TCL_RING_BUFFER_SIZE - writeIndex) & (TCL_RING_BUFFER_SIZE - 1);
			if (writeIndex == readIndex) // buffer completely empty
				distance = TCL_RING_BUFFER_SIZE;
			if (distance >= numU32s + 1) // assume distance minus one, because we are never allowed to completely wrap around
				break;
			TCLPublish(5);
			coreinit::OSSleepTicks(coreinit::EspressoTime::ConvertNsToTimerTicks(500000)); // sleep 0.5ms
		}
	}

	// this function assumes that TCLWaitForRBSpace was called and that there is enough space
	void TCLWriteCmd(uint32be* cmd, uint32 cmdLen)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);
		const uint32 cmdLenTotal = cmdLen;

		while (cmdLen > 0)
		{
			tclRingBufferA[writeIndex].store(*cmd, std::memory_order::relaxed);
			writeIndex++;
			writeIndex &= (TCL_RING_BUFFER_SIZE - 1);
			cmd++;
			cmdLen--;
		}

		tclRingBufferA_writeIndex.store(writeIndex, std::memory_order::release);
		uint64 written = s_written64.fetch_add(cmdLenTotal, std::memory_order::acq_rel) + cmdLenTotal;
		uint32 head = s_pendingHead.load(std::memory_order::relaxed);
		if (lag_ns() == 0 || head - s_pendingTail.load(std::memory_order::acquire) >= kPendingSize)
		{
			TCLPublish(6);
			return;
		}
		uint64 now = now_ns();
		s_pending[head % kPendingSize] = {written, s_currentRetireMarker, now};
		s_pendingHead.store(head + 1, std::memory_order::release);
		s_lastWriteNs.store(now, std::memory_order::relaxed);
	}

	#define EVENT_TYPE_TS		5

	void TCLSubmitRetireMarker(bool triggerEventInterrupt)
	{
		s_currentRetireMarker++;
		uint32be cmd[6];
		cmd[0] = pm4HeaderType3(IT_EVENT_WRITE_EOP, 5);
		cmd[1] = (4 | (EVENT_TYPE_TS << 8)); // event type (bits 8-15) and event index (bits 0-7).
		cmd[2] = MEMPTR<void>(&s_tclStatePPC->gpuRetireMarker).GetMPTR(); // address lower 32bits + data sel bits
		cmd[3] = 0x40000000; // select 64bit write, lower 16 bits are the upper bits of the address
		if (triggerEventInterrupt)
			cmd[3] |= 0x2000000; // trigger interrupt after value has been written
		cmd[4] = (uint32)s_currentRetireMarker; // data lower 32 bits
		cmd[5] = (uint32)(s_currentRetireMarker>>32); // data higher 32 bits
		TCLWriteCmd(cmd, 6);
	}

	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut)
	{
		TCLSubmissionFlag flags = *controlFlags;
		cemu_assert_debug(timestampValueOut); // handle case where this is null

		// make sure there is enough space to submit all commands at one
		uint32 totalCommandLength = cmdLen;
		totalCommandLength += 6; // space needed for TCLSubmitRetireMarker

		TCLWaitForRBSpace(totalCommandLength);

		// submit command buffer
		TCLWriteCmd(cmd, cmdLen);

		// create new marker timestamp and tell GPU to write it to our variable after its done processing the command
		if ((HAS_FLAG(flags, TCLSubmissionFlag::USE_RETIRED_MARKER)))
		{
			TCLSubmitRetireMarker(!HAS_FLAG(flags, TCLSubmissionFlag::NO_MARKER_INTERRUPT));
			*timestampValueOut = s_currentRetireMarker; // incremented before each submit
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "tcl";
		}

		void RPLMapped() override
		{
			cafeExportRegister("TCL", TCLSubmitToRing, LogType::Placeholder);
			cafeExportRegister("TCL", TCLTimestamp, LogType::Placeholder);
			cafeExportRegister("TCL", TCLWaitTimestamp, LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
				coreinit::OSInitEvent(s_updateRetirementEvent.GetPtr(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
			}
		}
	}s_COStclModule;

	COSModule* GetModule()
	{
		return &s_COStclModule;
	}
}
