// Engine.h - every address, offset and vtable slot this plugin uses.
//
// Skyrim Legendary Edition 1.9.32.0.8 only. Each entry carries the evidence it
// was read from, so anyone can re-check it in a disassembler instead of
// trusting the number. Nothing here is guessed from the Special Edition build:
// where the two editions differ the difference is written down.
//
// Addresses are absolute for an image based at 0x00400000, which is where the
// executable is loaded. The plugin refuses to run on any other build.

#pragma once

#include "skse/GameTypes.h"
#include "skse/GameReferences.h"
#include "skse/GameInput.h"
#include "skse/GameMenus.h"
#include "skse/NiNodes.h"
#include "skse/NiTypes.h"

namespace Engine
{
	// ------------------------------------------------- reading the game safely
	//
	// See the long note in Engine.cpp. Nothing in this plugin walks a pointer
	// chain into the game by hand any more. A tester's crash log caught the
	// engine's OWN GetCharController faulting on a middle-process pointer that
	// was not even aligned to four, moments after a save finished loading,
	// because this mod had asked for the controller while the game was still
	// rebuilding it.

	// Can this process read `length` bytes there? Non-null, aligned, committed.
	bool Readable(const void * address, size_t length);

	// The pointer stored at object + offset, or null if either end is unreadable.
	void * Link(const void * object, UInt32 offset);

	// Forgets what it knows about which memory is committed. Called when the game
	// has had a chance to unmap something.
	void ForgetMemoryLayout();

	struct Chain
	{
		void * process;			// actor + 0x88
		void * middle;			// process + 0x04
		void * controller;		// middle + 0x15C
	};

	// Every link of the actor -> controller chain, or false and nothing.
	bool ActorChain(void * actor, Chain & out);

	// Is this actor far enough along to be asked anything? 3D, and a whole chain.
	bool ActorIsBuilt(void * actor);

	// ---------------------------------------------------------------- units

	// The physics world runs in metres, the game world in units. Every vector
	// that crosses that border is scaled. The engine keeps the factor in three
	// places; this is the one the vanilla ray cast uses.
	inline float HavokScale()				{ return *(float *)0x01B11570; }
	inline float HavokScaleInverse()		{ const float s = HavokScale(); return s != 0.0f ? 1.0f / s : 69.99125f; }

	// ------------------------------------------------------------ singletons

	inline PlayerCharacter * Player()		{ return *(PlayerCharacter **)0x01310588; }	// SKSE LE g_thePlayer
	inline void * PlayerControls()			{ return *(void **)0x012E7454; }
	inline void * ControlMap()				{ return *(void **)0x012E7458; }			// "InputManager" in the SKSE headers
	inline void * PlayerCamera()			{ return *(void **)0x012E7288; }
	inline InputStringHolder * UserEvents()	{ return *(InputStringHolder **)0x01B2E388; }
	inline MenuManager * Menus()			{ return *(MenuManager **)0x012E3548; }
	inline UIManager * UI()					{ return *(UIManager **)0x012E35E4; }
	inline void * ScaleformLoader()			{ return *(void **)0x01B2E9B0; }

	inline bool  IsGodMode()				{ return *(bool *)0x01B398C5; }				// set by tgm, 0x0053C9D0
	inline float GlobalTimeMultiplier()		{ return *(float *)0x012C2410; }			// set by SGTM, 0x0052E1C0
	inline float HUDOpacity()				{ return *(float *)0x012B6304; }			// fHUDOpacity, read by the HUD at 0x008A9C33

	// --------------------------------------------------------- actor layout

	// TESObjectREFR: rot 0x28, pos 0x34, parentCell 0x40 (SKSE LE headers).
	// Actor: process 0x88, ActorState 0x64, ActorValueOwner 0x60.
	inline NiPoint3 & Position(TESObjectREFR * ref)	{ return *(NiPoint3 *)((UInt8 *)ref + 0x34); }
	inline NiPoint3 & Rotation(TESObjectREFR * ref)	{ return *(NiPoint3 *)((UInt8 *)ref + 0x28); }
	inline TESObjectCELL * ParentCell(TESObjectREFR * ref) { return *(TESObjectCELL **)((UInt8 *)ref + 0x40); }

	// ActorState's two flag words. The bit layout is the same as Special
	// Edition's; the knock bits were confirmed against the camera code at
	// 0x00840BE0, which tests (flags04 >> 25) & 7.
	inline UInt32 & ActorFlags1(Actor * actor)		{ return *(UInt32 *)((UInt8 *)actor + 0x68); }
	inline UInt32 & ActorFlags2(Actor * actor)		{ return *(UInt32 *)((UInt8 *)actor + 0x6C); }

	enum
	{
		kFlag1_MovingBack		= 1 << 0,
		kFlag1_MovingForward	= 1 << 1,
		kFlag1_MovingRight		= 1 << 2,
		kFlag1_MovingLeft		= 1 << 3,
		kFlag1_Walking			= 1 << 6,
		kFlag1_Running			= 1 << 7,
		kFlag1_Sprinting		= 1 << 8,
		kFlag1_Sneaking			= 1 << 9,
		kFlag1_Swimming			= 1 << 10,

		kFlag2_Staggered		= 1 << 13,
	};

	inline UInt32 SitSleepState(Actor * actor)	{ return (ActorFlags1(actor) >> 14) & 0x0F; }
	inline UInt32 LifeState(Actor * actor)		{ return (ActorFlags1(actor) >> 21) & 0x0F; }
	inline UInt32 KnockState(Actor * actor)		{ return (ActorFlags1(actor) >> 25) & 0x07; }
	inline UInt32 AttackState(Actor * actor)	{ return (ActorFlags1(actor) >> 28) & 0x0F; }
	inline UInt32 WeaponState(Actor * actor)	{ return (ActorFlags2(actor) >> 5) & 0x07; }

	enum { kWeaponState_Drawn = 3 };
	enum { kLifeState_Alive = 0, kLifeState_Dying = 1, kLifeState_Dead = 2 };

	// IMovementState, the vtable of the ActorState subobject at actor + 0x64.
	// Slot 5 returns the movement speed the engine itself uses for the
	// "IsMoving" condition (0x005048C0 compares it against 5.0).
	// Virtual, on a subobject of the actor, and it reaches the character
	// controller inside - so it answers nothing for an actor the game is still
	// putting together.
	inline float MovementSpeed(Actor * actor)
	{
		Chain chain;
		if(!ActorChain(actor, chain)) return 0.0f;

		void * state = (UInt8 *)actor + 0x64;
		void ** vtable = *(void ***)state;
		if(!Readable(vtable, 6 * sizeof(void *))) return 0.0f;

		float (__thiscall * fn)(void *) = (float (__thiscall *)(void *))vtable[5];
		return fn(state);
	}

	// ActorValueOwner at actor + 0x60. Slot 1 reads a value, slot 6 applies a
	// modifier - Papyrus DamageActorValue (0x008DD370) calls it with
	// (kDamage = 2, actorValue, -amount).
	enum { kActorValue_Health = 0x18, kActorValue_Stamina = 0x1A };

	inline float GetActorValue(Actor * actor, UInt32 av)
	{
		if(!Readable(actor, 0x64)) return 0.0f;
		void * owner = (UInt8 *)actor + 0x60;
		void ** vtable = *(void ***)owner;
		if(!Readable(vtable, 2 * sizeof(void *))) return 0.0f;

		float (__thiscall * fn)(void *, UInt32) = (float (__thiscall *)(void *, UInt32))vtable[1];
		return fn(owner, av);
	}

	inline void DamageActorValue(Actor * actor, UInt32 av, float amount)
	{
		if(!Readable(actor, 0x64)) return;
		void * owner = (UInt8 *)actor + 0x60;
		void ** vtable = *(void ***)owner;
		if(!Readable(vtable, 7 * sizeof(void *))) return;

		void (__thiscall * fn)(void *, UInt32, UInt32, float) =
			(void (__thiscall *)(void *, UInt32, UInt32, float))vtable[6];
		fn(owner, 2 /* damage modifier */, av, -amount);
	}

	// AIProcess::UpdateRegenDelay, read out of the bow-zoom stamina path at
	// 0x0074F5F7.
	inline void UpdateRegenDelay(Actor * actor, UInt32 av, float delay)
	{
		void * process = Link(actor, 0x88);
		if(!process) return;
		typedef void (__thiscall * fn_t)(void *, UInt32, float);
		((fn_t)0x006FCC90)(process, av, delay);
	}

	// HUDMenu's "the bar flashes because you have none left", 0x00897540.
	inline void FlashMeter(UInt32 av)
	{
		typedef void (__cdecl * fn_t)(UInt32);
		((fn_t)0x00897540)(av);
	}

	// The player is in the air when the character controller's Havok state is
	// hkpCharacterStateType::kInAir. 0x006AB2F0 is the engine's own answer.
	//
	// And the engine's own answer walks the player's process chain with a single
	// null check, which is what killed a tester's game moments after a load: the
	// chain was half rebuilt and the middle link read as an odd number. The
	// engine is in no position to doubt that pointer; this mod is, so the chain
	// is checked here before the question is asked.
	inline bool PlayerIsInMidair()
	{
		// 0x006AB2F0 is a member function: the engine loads the actor into ECX
		// and calls it (see 0x006AF9E7, 0x006C2F1C, 0x00739985). It is NOT a
		// free function - calling it as one leaves ECX as whatever the compiler
		// last put there, and the very next thing it does is read through it.
		void * player = Player();
		Chain chain;
		if(!ActorChain(player, chain)) return false;

		typedef bool (__thiscall * fn_t)(void * actor);
		return ((fn_t)0x006AB2F0)(player);
	}

	// ------------------------------------------------- character controller

	// Offsets confirmed in the constructor at 0x00D1E850 and in the users
	// listed beside each field.
	namespace Controller
	{
		enum
		{
			kForwardVec			= 0x040,	// hkVector4, points backwards
			kSurfaceInfo		= 0x150,	// supportedState is the first byte
			kContext			= 0x190,	// hkpCharacterContext
			kCurrentState		= 0x1A0,	// context + 0x10, read by 0x00D42B80
			kFlags				= 0x1B8,
			kWaterHeight		= 0x1D8,
			kFallStartHeight	= 0x1E0,
			kFallTime			= 0x1E4,
			kSpeedPct			= 0x204,
		};

		enum
		{
			kFlag_Support					= 1 << 8,
			kFlag_PotentialSupportManifold	= 1 << 9,
			kFlag_NoSim						= 1 << 17,
		};

		enum { kState_OnGround = 0, kState_Jumping = 1, kState_InAir = 2, kState_Climbing = 3, kState_Flying = 4, kState_Swimming = 5 };
		enum { kSupport_Unsupported = 0, kSupport_Sliding = 1, kSupport_Supported = 2 };
	}

	inline void * CharController(Actor * actor)
	{
		Chain chain;
		return ActorChain(actor, chain) ? chain.controller : NULL;
	}

	inline UInt32 & ControllerFlags(void * ctrl)		{ return *(UInt32 *)((UInt8 *)ctrl + Controller::kFlags); }
	inline UInt32 ControllerState(void * ctrl)			{ return *(UInt32 *)((UInt8 *)ctrl + Controller::kCurrentState); }
	inline UInt8  ControllerSupport(void * ctrl)		{ return *(UInt8 *)((UInt8 *)ctrl + Controller::kSurfaceInfo); }
	inline float & ControllerFallTime(void * ctrl)		{ return *(float *)((UInt8 *)ctrl + Controller::kFallTime); }
	inline float & ControllerFallStart(void * ctrl)		{ return *(float *)((UInt8 *)ctrl + Controller::kFallStartHeight); }
	inline float ControllerSpeedPct(void * ctrl)		{ return *(float *)((UInt8 *)ctrl + Controller::kSpeedPct); }
	inline float ControllerWaterHeight(void * ctrl)		{ return *(float *)((UInt8 *)ctrl + Controller::kWaterHeight); }

	// The controller's virtual calls. Slot indices match Special Edition's;
	// the implementations read at 0x00D64FE0 and friends confirm the shapes.
	inline void ControllerGetPosition(void * ctrl, float * outVec4, bool applyCenterOffset)
	{
		typedef void (__thiscall * fn_t)(void *, float *, bool);
		(*(fn_t **)ctrl)[2](ctrl, outVec4, applyCenterOffset);
	}

	inline void ControllerSetPosition(void * ctrl, const float * vec4, bool applyCenterOffset, bool forceWarp)
	{
		typedef void (__thiscall * fn_t)(void *, const float *, bool, bool);
		(*(fn_t **)ctrl)[3](ctrl, vec4, applyCenterOffset, forceWarp);
	}

	inline void ControllerGetLinearVelocity(void * ctrl, float * outVec4)
	{
		typedef void (__thiscall * fn_t)(void *, float *);
		(*(fn_t **)ctrl)[6](ctrl, outVec4);
	}

	inline void ControllerSetLinearVelocity(void * ctrl, const float * vec4)
	{
		typedef void (__thiscall * fn_t)(void *, const float *);
		(*(fn_t **)ctrl)[7](ctrl, vec4);
	}

	inline UInt32 ControllerCollisionFilter(void * ctrl)
	{
		UInt32 out = 0;
		typedef UInt32 * (__thiscall * fn_t)(void *, UInt32 *);
		(*(fn_t **)ctrl)[8](ctrl, &out);
		return out;
	}

	// ------------------------------------------------------ animation graph

	// The holder is a subobject of every reference at + 0x20. Papyrus'
	// GetAnimationVariableBool (0x00903390) calls slot 0x12 on it.
	inline void * GraphHolder(TESObjectREFR * ref)	{ return (UInt8 *)ref + 0x20; }

	// The holder's table, checked as far as the slot being called.
	//
	// The holder is a subobject of the reference, so a reference the game is
	// building or tearing down has one too - with whatever table pointer the
	// memory happened to hold. Every call below goes through this.
	inline void ** HolderVtable(TESObjectREFR * ref, UInt32 slot)
	{
		if(!Readable(ref, 0x24)) return NULL;
		void ** vtable = *(void ***)((UInt8 *)ref + 0x20);
		return Readable(vtable, (slot + 1) * sizeof(void *)) ? vtable : NULL;
	}

	inline bool GetGraphVariableBool(TESObjectREFR * ref, const BSFixedString & name, bool & out)
	{
		void ** vtable = HolderVtable(ref, 0x12);
		if(!vtable) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, bool &);
		return ((fn_t)vtable[0x12])(GraphHolder(ref), name, out);
	}

	inline bool GetGraphVariableInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 & out)
	{
		void ** vtable = HolderVtable(ref, 0x11);
		if(!vtable) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, SInt32 &);
		return ((fn_t)vtable[0x11])(GraphHolder(ref), name, out);
	}

	inline bool GetGraphVariableFloat(TESObjectREFR * ref, const BSFixedString & name, float & out)
	{
		void ** vtable = HolderVtable(ref, 0x10);
		if(!vtable) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, float &);
		return ((fn_t)vtable[0x10])(GraphHolder(ref), name, out);
	}

	inline bool SetGraphVariableBool(TESObjectREFR * ref, const BSFixedString & name, bool value)
	{
		if(!Readable(ref, 0x24)) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, bool);
		return ((fn_t)0x00650C60)(GraphHolder(ref), name, value);
	}

	inline bool SetGraphVariableInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 value)
	{
		if(!Readable(ref, 0x24)) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, SInt32);
		return ((fn_t)0x00650C80)(GraphHolder(ref), name, value);
	}

	inline bool SetGraphVariableFloat(TESObjectREFR * ref, const BSFixedString & name, float value)
	{
		if(!Readable(ref, 0x24)) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &, float);
		return ((fn_t)0x00650CA0)(GraphHolder(ref), name, value);
	}

	inline bool NotifyAnimationGraph(TESObjectREFR * ref, const BSFixedString & event)
	{
		void ** vtable = HolderVtable(ref, 0x01);
		if(!vtable) return false;
		typedef bool (__thiscall * fn_t)(void *, const BSFixedString &);
		return ((fn_t)vtable[0x01])(GraphHolder(ref), event);
	}

	inline bool GetAnimationGraphManager(TESObjectREFR * ref, void *& outManagerPtr)
	{
		void ** vtable = HolderVtable(ref, 0x02);
		if(!vtable) return false;
		typedef bool (__thiscall * fn_t)(void *, void **);
		return ((fn_t)vtable[0x02])(GraphHolder(ref), &outManagerPtr);
	}

	// BSAnimationGraphManager, laid out by the constructor at 0x00BA7B60 and
	// walked by Update at 0x00BA7C90.
	namespace GraphManager
	{
		enum
		{
			kBoundChannels	= 0x08,		// data, capacity, size
			kGraphs			= 0x20,		// small array: capacity/flag, data, size
			kActiveGraph	= 0x5C,
		};
	}

	inline void * ActiveGraph(void * manager)
	{
		if(!manager) return NULL;
		const UInt32 capacity	= *(UInt32 *)((UInt8 *)manager + GraphManager::kGraphs + 0x00);
		const UInt32 size		= *(UInt32 *)((UInt8 *)manager + GraphManager::kGraphs + 0x08);
		if(!size) return NULL;
		void ** data = (capacity & 0x80000000)
					 ? (void **)((UInt8 *)manager + GraphManager::kGraphs + 0x04)
					 : *(void ***)((UInt8 *)manager + GraphManager::kGraphs + 0x04);
		UInt32 index = *(UInt32 *)((UInt8 *)manager + GraphManager::kActiveGraph);
		if(index >= size) index = 0;
		return data[index];
	}

	// ------------------------------------------------------------- controls

	// ControlMap::ToggleControls, 0x00A67A30. The user-event flags are the same
	// values as Special Edition's.
	enum
	{
		kControl_Movement	= 1 << 0,
		kControl_Looking	= 1 << 1,
		kControl_Activate	= 1 << 2,
		kControl_Menu		= 1 << 3,
		kControl_Console	= 1 << 4,
		kControl_POVSwitch	= 1 << 5,
		kControl_Fighting	= 1 << 6,
		kControl_Sneaking	= 1 << 7,
		kControl_MainFour	= 1 << 8,
		kControl_WheelZoom	= 1 << 9,
		kControl_Jumping	= 1 << 10,
		kControl_VATS		= 1 << 11,
	};

	inline UInt32 EnabledControls()
	{
		void * map = ControlMap();
		return map ? *(UInt32 *)((UInt8 *)map + 0x90) : 0xFFFFFFFF;
	}

	inline void ToggleControls(UInt32 flags, bool enable)
	{
		void * map = ControlMap();
		if(!map) return;
		typedef void (__thiscall * fn_t)(void *, UInt32, bool, bool);
		((fn_t)0x00A67A30)(map, flags, enable, true);
	}

	// Whether this plugin is the one holding jumping and the main four shut. The
	// main four are the tween menu and the inventory / magic / map / skills keys,
	// on a gamepad as on a keyboard: a stop event that never arrives (a ragdoll, a
	// reset, a load, a slide the behaviour refused) used to leave them shut for the
	// rest of the session. Every place that shuts them goes through here, and the
	// tick gives them back the moment nothing of ours is running.
	extern bool ActionControlsHeld;

	inline void HoldActionControls(bool hold)
	{
		ToggleControls(kControl_Jumping, !hold);
		ToggleControls(kControl_MainFour, !hold);
		ActionControlsHeld = hold;
	}

	inline void ReleaseActionControls()
	{
		if(ActionControlsHeld) HoldActionControls(false);
	}

	// The control map's enabled flags travel in the save. A save made while an
	// older build had the main four shut - or one of this build's own holds that
	// never got its end event - loads with them still shut and with nobody
	// holding them, so ReleaseActionControls has nothing to give back. The
	// signature is specific: jumping and/or the main four off while movement,
	// looking, fighting and the menu are all on. A scripted lock-out (the
	// opening cart, a cutscene) turns off movement and the rest with them, and
	// is left alone. Returns true if it opened anything.
	inline bool RepairStuckControls(UInt32 & before)
	{
		before = EnabledControls();
		const UInt32 core = kControl_Movement | kControl_Looking | kControl_Fighting | kControl_Menu;
		if((before & core) != core) return false;

		const UInt32 ours = kControl_Jumping | kControl_MainFour;
		if((before & ours) == ours) return false;

		ToggleControls(ours & ~before, true);
		return true;
	}

	// PlayerControls' movement data starts at + 0x14; autoMove is data + 0x24
	// (AutoMoveHandler at 0x00770FD0 toggles it).
	inline bool AutoMove()
	{
		void * controls = PlayerControls();
		return controls ? *(bool *)((UInt8 *)controls + 0x38) : false;
	}

	// --------------------------------------------------------------- camera

	// PlayerCamera is a TESCamera: current state at + 0x20, and every state
	// carries its id at + 0x0C.
	inline void * CameraState()
	{
		void * camera = PlayerCamera();
		return camera ? *(void **)((UInt8 *)camera + 0x20) : NULL;
	}

	enum { kCameraState_FirstPerson = 0, kCameraState_ThirdPerson = 9 };

	inline UInt32 CameraStateID(void * state)	{ return state ? *(UInt32 *)((UInt8 *)state + 0x0C) : 0xFFFFFFFF; }

	namespace ThirdPerson
	{
		enum
		{
			kTargetZoomOffset	= 0x54,
			kCurrentZoomOffset	= 0x58,
			kTargetYaw			= 0x5C,
			kCurrentYaw			= 0x60,
			kFreeRotation		= 0xAC,		// NiPoint2
			kStateNotActive		= 0xB5,
		};
	}

	namespace FirstPerson
	{
		enum { kSittingRotation = 0x20 };	// added to the camera yaw by 0x00839F60
	}

	// --------------------------------------------------------------- memory

	// The game's own heap. Anything the engine will later free itself, or that
	// holds an engine object, has to come from here.
	inline void * HeapAllocate(UInt32 size, UInt32 alignment, bool aligned)
	{
		typedef void * (__thiscall * fn_t)(void *, UInt32, UInt32, bool);
		return ((fn_t)0x00A48D60)((void *)0x01B418B0, size, alignment, aligned);
	}

	inline void HeapFree(void * mem, bool aligned)
	{
		typedef void (__thiscall * fn_t)(void *, void *, bool);
		((fn_t)0x00A487B0)((void *)0x01B418B0, mem, aligned);
	}

	// ------------------------------------------------------------- messages

	// The engine's message box, as Papyrus' Debug.MessageBox calls it
	// (0x008EE530 -> 0x0087AC60).
	void MessageBox(const char * text);

	// ----------------------------------------------------------- 3D / scale

	// TESObjectREFR::GetNiRootNode(firstPerson) is vtable slot 0x6F in the
	// SKSE headers; the third person root is what carries the actor's scale.
	inline NiNode * Get3D(TESObjectREFR * ref, bool firstPerson)
	{
		// A virtual call, so the reference and the table it carries both have to
		// be there. A reference that is being torn down is the normal case here,
		// not the exceptional one.
		if(!Readable(ref, sizeof(void *))) return NULL;
		void ** vtable = *(void ***)ref;
		if(!Readable(vtable, (0x6F + 1) * sizeof(void *))) return NULL;

		typedef NiNode * (__thiscall * fn_t)(void *, UInt32);
		return ((fn_t)vtable[0x6F])(ref, firstPerson ? 1 : 0);
	}
}
