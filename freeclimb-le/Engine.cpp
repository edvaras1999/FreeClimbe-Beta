#include "Engine.h"
#include "Log.h"

#include "skse/GameReferences.h"

#include <windows.h>

// The SDK declares this as a pointer to the engine's own lookup and defines it
// in GameReferences.cpp - a file that drags in half the reference layer for
// three functions this plugin never calls. The one symbol the menu code needs
// is defined here instead, at the address the SDK itself uses.
const _LookupREFRByHandle LookupREFRByHandle = (_LookupREFRByHandle)0x004A9180;

namespace Engine
{
	bool ActionControlsHeld = false;

	// The engine's own message box, reached the way Papyrus' Debug.MessageBox
	// reaches it (0x008EE530 -> 0x0087AC60): text, no callback, no buttons of
	// its own beyond the default.
	void MessageBox(const char * text)
	{
		if(!text) return;

		typedef void (__cdecl * fn_t)(const char *, void *, UInt32, UInt32, UInt32, void *, UInt32);
		((fn_t)0x0087AC60)(text, NULL, 0, 4, 0x0A, (void *)0x01094A2C, 0);
	}

	// ------------------------------------------------------- reading the game
	//
	// WHY THERE IS A LAYER HERE AT ALL, AND WHY EVERY POINTER WALK GOES THROUGH
	// IT.
	//
	// The player's character controller is not a field, it is a chain:
	//
	//     actor + 0x88   -> AIProcess
	//     AIProcess + 4  -> MiddleProcess
	//     Middle + 0x15C -> the controller
	//
	// The engine's own accessor for it, AIProcess::GetCharController at
	// 0x00720880, is three instructions:
	//
	//     mov ecx,[ecx+4] / test ecx,ecx / je / mov eax,[ecx+0x15C]
	//
	// It checks the middle link for NULL and nothing else, because inside the
	// engine nothing else can happen: whatever touches that chain runs where the
	// engine knows the player is built. This mod does not. It ticks from the
	// animation graph, and the player's AIProcess is torn down and rebuilt
	// whenever the player's 3D is - loading a save, crossing a cell boundary,
	// coming back from the title screen. For a moment, [actor+0x88] still points
	// at a block that has been handed back to the heap, something else has taken
	// it, and the middle link reads as whatever that something else put there.
	//
	// A tester's crash log caught it exactly: ecx = 0x0DA4F3CE, which is not even
	// aligned to four, so it was never a pointer to anything - and the fault was
	// in the ENGINE's accessor, reached through an engine function this mod
	// called, right after PostLoadGame. Nothing in the plugin's own code was
	// wrong at the point it died; it had simply asked a question too early.
	//
	// So no chain is walked and no engine function is handed a pointer from one
	// until every link has been checked: non-null, aligned, and in memory this
	// process has actually committed. The answer when a link fails is "there is
	// no controller", which every caller already knows how to take.
	// The three cheap tests first, because they are the ones that catch what
	// actually turns up. The value from the crash log, 0x0DA4F3CE, fails the
	// alignment test in four instructions; no engine object has ever sat on an
	// odd address. Only what survives that is worth asking the kernel about.
	//
	// What this CANNOT do, said plainly: it cannot tell a live object from a
	// freed one. A heap block that has been handed back is still committed, so a
	// stale pointer that happens to stay aligned passes here. That is what the
	// guard in Parkour.cpp is for - this layer removes the common case, the guard
	// catches the rest, and between them a bad pointer costs a skipped tick
	// instead of the game.
	namespace
	{
		// Committed regions are large and few, and the same handful answer every
		// call, so the kernel is asked once per region rather than once per read.
		enum { kRegionCache = 8 };

		struct Region
		{
			UInt32	base;
			UInt32	end;
			bool	usable;
		};

		Region	s_regions[kRegionCache];
		UInt32	s_regionNext = 0;

		bool RegionAllows(UInt32 address, size_t length)
		{
			for(UInt32 i = 0; i < kRegionCache; ++i)
			{
				const Region & region = s_regions[i];
				if(!region.end) continue;
				if(address < region.base || address >= region.end) continue;
				return region.usable && (address + length <= region.end);
			}

			MEMORY_BASIC_INFORMATION info;
			if(!VirtualQuery((const void *)address, &info, sizeof(info)))
				return false;

			Region fresh;
			fresh.base		= (UInt32)info.BaseAddress;
			fresh.end		= (UInt32)info.BaseAddress + (UInt32)info.RegionSize;
			fresh.usable	= (info.State == MEM_COMMIT) &&
							  !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));

			// Written last, so a reader on another thread never sees half an entry.
			Region & slot = s_regions[s_regionNext % kRegionCache];
			++s_regionNext;
			slot.end	= 0;
			slot.base	= fresh.base;
			slot.usable	= fresh.usable;
			slot.end	= fresh.end;

			return fresh.usable && (address + length <= fresh.end);
		}
	}

	void ForgetMemoryLayout()
	{
		// Called whenever the game has had the chance to unmap something - a
		// load, a new game - so a region freed in the meantime is asked about
		// again rather than remembered as readable.
		for(UInt32 i = 0; i < kRegionCache; ++i)
			s_regions[i].end = 0;
	}

	bool Readable(const void * address, size_t length)
	{
		const UInt32 value = (UInt32)address;
		if(value < 0x00010000)		return false;	// the null page
		if(value & 3)				return false;	// no engine object is misaligned
		// Not 0x80000000: a large-address-aware game, which most Legendary
		// Editions are, keeps ordinary heap above it. The top 64 KB is the
		// only part of the address space no process can own.
		if(value >= 0xFFFF0000)		return false;
		if(value + length < value)	return false;	// wrapped

		return RegionAllows(value, length);
	}

	// The pointer at `object + offset`, or null if either end of that is not
	// something this process can read.
	void * Link(const void * object, UInt32 offset)
	{
		if(!Readable(object, offset + sizeof(void *))) return NULL;
		void * value = *(void * const *)((const UInt8 *)object + offset);
		return Readable(value, sizeof(void *)) ? value : NULL;
	}

	bool ActorChain(void * actor, Chain & out)
	{
		out.process = out.middle = out.controller = NULL;

		if(!Readable(actor, 0x8C)) return false;

		out.process = Link(actor, 0x88);
		if(!out.process) return false;

		out.middle = Link(out.process, 0x04);
		if(!out.middle) return false;

		// The controller has to be good for its whole span, because the plugin
		// reads flags and a state out of the far end of it and writes a position
		// back through it.
		if(!Readable(out.middle, Controller::kFlags + 4)) return false;
		out.controller = *(void **)((UInt8 *)out.middle + 0x15C);
		if(!Readable(out.controller, Controller::kSpeedPct + 4)) return false;

		return true;
	}

	// Is the player far enough along to be asked anything at all?
	//
	// This is the gate the whole mod hangs off. A player without 3D, without a
	// process chain or without a controller is a player the game is still
	// building, and every question put to one has the same answer: not yet.
	bool ActorIsBuilt(void * actor)
	{
		if(!actor) return false;
		if(!Readable(actor, 0x140)) return false;

		Chain chain;
		if(!ActorChain(actor, chain)) return false;

		return Get3D((TESObjectREFR *)actor, false) != NULL ||
			   Get3D((TESObjectREFR *)actor, true)  != NULL;
	}
}
