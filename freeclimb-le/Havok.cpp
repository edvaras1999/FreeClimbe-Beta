#include "Havok.h"
#include "Engine.h"

#include "skse/GameReferences.h"
#include "skse/GameRTTI.h"

#include <windows.h>
#include <float.h>

namespace
{

	// Gives the slot a hit at `fraction` should be written to, or NULL when the
	// result already holds kMaxHits hits that are all nearer than this one.
	Havok::Hit * SlotFor(Havok::Result & result, float fraction)
	{
		if(result.hitCount < Havok::kMaxHits)
			return &result.hits[result.hitCount++];

		UInt32 farthest = 0;
		for(UInt32 i = 1; i < Havok::kMaxHits; ++i)
			if(result.hits[i].fraction > result.hits[farthest].fraction)
				farthest = i;

		if(fraction >= result.hits[farthest].fraction)
			return NULL;

		return &result.hits[farthest];
	}

	// ------------------------------------------------------------ addresses

	// bhkWorld's virtual table: PickObject is slot 0x2F and GetWorld1 slot
	// 0x23 (the vanilla ray cast at 0x006BF6E0 calls +0xBC, and PickObject
	// itself calls +0x8C to reach the hkpWorld).
	enum { kBhkWorld_GetWorld1 = 0x23, kBhkWorld_PickObject = 0x2F };

	// The bhkWorld of a cell: interior cells keep it in their ExtraHavok, the
	// exterior one lives in a global. 0x004C5430 answers for both.
	typedef void * (__fastcall * _CellBhkWorld)(TESObjectCELL * cell);
	const _CellBhkWorld CellBhkWorld = (_CellBhkWorld)0x004C5430;

	typedef void (__fastcall * _RayCollectorCtor)(void * self);
	const _RayCollectorCtor RayCollectorCtor = (_RayCollectorCtor)0x0045E010;
	const _RayCollectorCtor RayCollectorDtor = (_RayCollectorCtor)0x0045E140;

	// bhkCapsuleShape(this, vertexA, vertexB, radius) - takes game units and
	// scales into Havok itself, then builds the hkpCapsuleShape behind it.
	typedef void * (__thiscall * _CapsuleCtor)(void * self, const NiPoint3 * a, const NiPoint3 * b, float radius);
	const _CapsuleCtor CapsuleShapeCtor = (_CapsuleCtor)0x00D3F300;

	// hkpWorld::linearCast(collidable, input, castCollector, startCollector)
	typedef void (__thiscall * _LinearCast)(void * world, const void * collidable, const void * input,
											void * castCollector, void * startCollector);
	const _LinearCast LinearCast = (_LinearCast)0x00D4F190;

	// Finding out what a cast hit, in the two steps the engine itself takes.
	//
	// THIS IS WHAT CRASHED 1.0.7, so it is worth writing down in full.
	//
	// Special Edition asks TESHavokUtilities::FindCollidableRef for the
	// reference behind a collidable and gets one back. The function at
	// 0x004726B0 was taken for that one. It is not: it is the OTHER half of the
	// pair, FindCollidableObject, and what it returns is the scene object - in
	// its main path literally bhkNiCollisionObject::sceneObject, the pointer at
	// +0x08 of the collision object (read at 0x00D13C50, which 0x004726B0 tail
	// calls). An NiAVObject, not a reference.
	//
	// Reading ->baseForm at +0x24 of that scene object lands inside
	// NiAVObject::localTransform - on the second entry of the rotation matrix.
	// For anything standing square to the world that entry is 0.0, the null
	// check passes and nothing happens, which is why this ran at all. For
	// anything ROTATED it is a sine: a float, read as a pointer, and
	// dereferenced one byte later for the form type. The address 0xBF7D8C99 in
	// the crash report is 0xBF7D8C87 + 0x12, and 0xBF7D8C87 as a float is
	// -0.99042553 - a rotation matrix entry, exactly as predicted. Whether that
	// bogus address happens to be mapped depends on how much memory the load
	// order has committed, which is why it took some machines and not others.
	//
	// The engine never does this. Its own ray cast (0x006BF93C) calls 0x004726B0
	// and passes the result straight to 0x004D7C90 before using it as a
	// reference; 14 of the 20 call sites in the executable do the same. That
	// second function walks up the scene graph by parent (+0x18) until it finds
	// a node carrying a reference, reads the reference's baseForm at +0x24 and
	// its formType at +0x12 - the very offsets this plugin wants - and skips
	// furniture markers (type 0x2A) in favour of whatever owns them. It also
	// answers the player's own first person 3D with the player.
	//
	// Both are __cdecl with one stack argument (0x004726B0: mov ecx,[esp+4] then
	// ret; 0x004D7C90: mov esi,[esp+0xc] after two pushes, and its own recursive
	// call cleans the stack with add esp,4).
	typedef void * (__cdecl * _FindCollidableObject)(const void * collidable);
	const _FindCollidableObject FindCollidableObject = (_FindCollidableObject)0x004726B0;

	typedef TESObjectREFR * (__cdecl * _ReferenceFor3D)(const void * sceneObject);
	const _ReferenceFor3D ReferenceFor3D = (_ReferenceFor3D)0x004D7C90;

	// Where the executable is and how far it goes, so a pointer that should be
	// an engine object can be checked for carrying an engine vtable.
	UInt32 s_imageEnd = 0;	// namespace scope: a function-local static has a guarded initialiser (F4)

	UInt32 ImageEnd()
	{
		UInt32 & s_end = s_imageEnd;
		if(!s_end)
		{
			const UInt8 * base = (const UInt8 *)0x00400000;
			const UInt32 peOffset = *(const UInt32 *)(base + 0x3C);
			s_end = 0x00400000 + *(const UInt32 *)(base + peOffset + 0x18 + 0x38);	// SizeOfImage
		}
		return s_end;
	}

	// Could this be a pointer at all?
	bool LooksLikePointer(const void * object)
	{
		const UInt32 address = (UInt32)object;
		if(address < 0x00010000)	return false;	// the null page
		if(address & 3)				return false;	// no engine object is misaligned
		if(address >= 0xFFFF0000)	return false;	// reserved top of the address space (see Engine::Readable)
		return true;
	}

	// Does this look like an object the game made?
	//
	// Every reference and every form in the game carries a vtable that lives in
	// the executable itself. A rotation matrix entry read as a pointer does not,
	// which is what tells the two apart - and where such a value points at
	// nothing at all, reading the vtable faults and the guard below catches it.
	// Four compares against a crash.
	//
	// Only applied to what is actually dereferenced here, the reference and its
	// base form. The scene object in between is handed to the engine's own
	// lookup rather than read, and a scene graph can hold node types that belong
	// to another plugin - body physics does exactly that - whose vtables are in
	// that plugin, not in the game.
	bool LooksLikeEngineObject(const void * object)
	{
		if(!LooksLikePointer(object)) return false;
		const UInt32 vtable = *(const UInt32 *)object;
		return vtable >= 0x00400000 && vtable < ImageEnd();
	}

	// The whole lookup, in one place. It must only be called while the world's
	// read lock is held - the callers below see to that - because the collidable,
	// the scene object and the reference are only known to be alive while nothing
	// can take the physics world apart.
	//
	// Nothing leaves here but values: whether there is a reference at all, and the
	// type of its base form. Both are written last, after every check has passed,
	// so a half-validated answer cannot get out.
	//
	// No C++ object with a destructor may live in here: the compiler will not
	// mix structured exception handling with unwinding. The guard is the last
	// line of defence, not the plan - the plan is the two-step lookup above.
	void ResolveRef(const void * collidable, bool & outHasReference, UInt8 & outFormType)
	{
		outHasReference	= false;
		outFormType		= Havok::kFormType_Unknown;

		if(!collidable) return;

		__try
		{
			void * object = FindCollidableObject(collidable);
			if(!LooksLikePointer(object)) return;

			TESObjectREFR * ref = ReferenceFor3D(object);
			if(!LooksLikeEngineObject(ref)) return;

			// A vtable inside the executable only says the game made it. Every
			// reference class - references, actors, projectiles, hazards - sits
			// in one run of form types, and a culling process or a node does not.
			if(ref->formType < kFormType_Reference || ref->formType > kFormType_PHZD) return;

			TESForm * base = ref->baseForm;
			if(!LooksLikeEngineObject(base)) return;

			const UInt8 type = base->formType;

			outFormType		= type;
			outHasReference	= true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			outHasReference	= false;
			outFormType		= Havok::kFormType_Unknown;
		}
	}

	// ------------------------------------------------------------- world lock
	//
	// What the port was missing, and the one thing the Special Edition original
	// does on every single cast:
	//
	//     RE::BSReadLockGuard lock(bhkWorld->worldLock);
	//
	// around both its ray pick and its linear cast. The mod ticks from the
	// player's animation graph, which is not necessarily the thread the physics
	// world is being stepped on, and a cast walking a broad phase that is being
	// rebuilt under it reads whatever is half-written - a different outcome on
	// every machine, which is the shape of the reports.
	//
	// The engine's own bhkWorld::PickObject (0x00D104A0) takes exactly this
	// lock: 0x00432A40 constructs an eight byte guard { hkpWorld *, mode } over
	// the world, and 0x004319E0 releases it. Mode 0 is the read side
	// (0x00D0E2C0 -> 0x00A4AEE0, which bumps a reader count) and the whole
	// mechanism is a no-op while the flag at 0x012D19A0 is clear, so this costs
	// nothing on a game that is not running physics across threads.
	//
	// Nesting is safe, and the original nests it the same way: the writer side
	// (0x00A4AF50) only ever swaps a lock word of exactly zero and otherwise
	// spins, so it cannot hold a second read lock off while waiting for the
	// first, and a reader that already owns the lock just counts up.
	struct WorldLock
	{
		void *	hkWorld;
		int		mode;

		void Acquire(void * bhkWorld)
		{
			hkWorld	= NULL;
			mode	= 0;
			typedef void * (__thiscall * _Acquire)(WorldLock *, void *, int);
			((_Acquire)0x00432A40)(this, bhkWorld, 0 /* read */);
		}

		void Release()
		{
			typedef void (__fastcall * _Release)(WorldLock *);
			((_Release)0x004319E0)(this);
		}
	};

	// The Havok allocator the hit arrays grow from. The object lives at this
	// address and its free is vtable slot 4, thiscall(this, memory, bytes) -
	// read out of the collector destructor at 0x00584E8A.
	void * const kHavokAllocator = (void *)0x012D1C00;

	void HavokFree(void * memory, UInt32 bytes)
	{
		if(!memory) return;
		typedef void (__thiscall * _Free)(void *, void *, UInt32);
		_Free free = (_Free)((void **)(*(void ***)kHavokAllocator))[4];
		free(kHavokAllocator, memory, bytes);
	}

	// ---------------------------------------------------------------- types
	//
	// EVERY STRUCTURE BELOW IS ALIGNED TO SIXTEEN, AND THAT IS NOT DECORATION.
	//
	// This is what crashed the mod on loading a save and on starting a new game,
	// for a year's worth of versions, and it is the one difference between this
	// edition and Special Edition that cannot be read off a disassembly listing:
	// 64-bit code is handed a 16-byte aligned stack by the calling convention, so
	// the original never had to think about any of this. In 32-bit code a stack
	// local is aligned to four bytes and no more.
	//
	// Havok's vectors are SSE registers, and the engine moves them with MOVAPS,
	// which FAULTS OUTRIGHT on an address that is not a multiple of sixteen. The
	// instruction that killed it was
	//
	//     00DE014E   movaps xmm0, [edi]          ; edi = the cast input
	//
	// reached from hkpWorld::linearCast, called from ShapeCast below, called from
	// the animation graph tick. The crash log says the faulting address is
	// 0xFFFFFFFF, which is not an address at all: that is what Windows reports
	// for a misaligned SSE access, and it is the signature to recognise.
	//
	// Why it took some people and not others, and why it came and went between
	// versions: the address of a stack local is the stack pointer at that point
	// plus an offset the compiler chose, so whether it lands on a multiple of
	// sixteen depends on how deep the call was and on how much the rest of the
	// function happened to put on the stack. Change anything above it - another
	// local, a different thread doing the tick - and the alignment moves. The mod
	// was not working on some machines; it was getting away with it.
	//
	// So nothing handed to Havok is left to chance here, whether this edition's
	// engine is seen to use MOVAPS on it or not.

	// hkpAllRayHitTempCollector: the constructor above lays it out, each hit is
	// 0x60 bytes with the fraction at +0x10 and the collidable at +0x50.
	__declspec(align(16)) struct RayCollector
	{
		UInt8	raw[0x320];

		void *	Data() const	{ return *(void **)(raw + 0x10); }
		UInt32	Size() const	{ return *(UInt32 *)(raw + 0x14); }
		UInt8 *	At(UInt32 i) const	{ return (UInt8 *)Data() + i * 0x60; }
	};

	// hkpAllCdPointCollector. Built by hand rather than with the engine's
	// constructor at 0x005851B0, because that one also builds a phantom and a
	// shape for the engine's own sphere caster.
	__declspec(align(16)) struct PointCollector
	{
		// What the engine's own constructor writes: an early-out distance no
		// contact can beat, and room for sixteen contacts before the array
		// would have to reach for the heap.
		enum { kEarlyOut = 0x7F7FFFEE, kInplace = 16 };

		void *	vtbl;				// 0x00
		float	earlyOutDistance;	// 0x04
		UInt32	pad08[2];			// 0x08
		void *	data;				// 0x10
		UInt32	size;				// 0x14
		UInt32	capacity;			// 0x18
		UInt32	pad1C;				// 0x1C
		UInt8	storage[kInplace * 0x30];	// 0x20

		void Init()
		{
			vtbl				= (void *)0x010A7C74;
			*(UInt32 *)&earlyOutDistance = kEarlyOut;
			data				= storage;
			size				= 0;
			capacity			= 0x80000000 | kInplace;	// inplace storage, nothing owned
			pad08[0] = pad08[1] = 0;
			pad1C = 0;
		}

		void Release()
		{
			// The array only owns memory if it had to grow past the inplace
			// storage, which clears the flag - the same test the engine's own
			// destructor makes at 0x00584E70.
			if((SInt32)capacity >= 0)
			{
				HavokFree(data, (capacity & 0x3FFFFFFF) * 0x30);
				data = NULL;
				capacity = 0x80000000;
			}
			size = 0;
		}

		UInt8 * At(UInt32 i) const { return (UInt8 *)data + i * 0x30; }
	};

	// hkpCollidable. The offsets are the ones FindCollidableRef reads:
	// ownerOffset at +0x10, the broad phase type at +0x18 and the collision
	// filter at +0x1C.
	__declspec(align(16)) struct Collidable
	{
		const void *	shape;			// 0x00
		UInt32			shapeKey;		// 0x04
		const void *	motion;			// 0x08 - an hkTransform for a free-standing shape
		const void *	parent;			// 0x0C
		SInt8			ownerOffset;	// 0x10
		UInt8			forceCollideOntoPpu;
		UInt16			shapeSizeOnSpu;
		UInt32			broadPhaseId;	// 0x14
		UInt8			broadPhaseType;	// 0x18
		SInt8			broadPhaseOwnerOffset;
		UInt8			objectQualityType;
		UInt8			pad1B;
		UInt32			collisionFilterInfo;	// 0x1C
		UInt8			boundingVolumeData[0x38];
		float			allowedPenetrationDepth;
	};

	__declspec(align(16)) struct LinearCastInput
	{
		float	to[4];					// 0x00
		float	maxExtraPenetration;	// 0x10
		float	startPointTolerance;	// 0x14
	};

	// ------------------------------------------------------------- helpers

	NiPoint3 Normalised(const NiPoint3 & v)
	{
		const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		if(len <= 1e-6f) return NiPoint3(0.0f, 0.0f, 0.0f);
		return NiPoint3(v.x / len, v.y / len, v.z / len);
	}

	// The caster's own filter, with the layer swapped for the one we want to
	// collide as. Keeping the caster's system group is what stops every cast
	// from hitting the player.
	UInt32 CastFilter(Actor * caster, UInt32 layer)
	{
		UInt32 filter = 0;
		if(caster)
		{
			void * ctrl = Engine::CharController(caster);
			if(ctrl) filter = Engine::ControllerCollisionFilter(ctrl);
		}
		return (filter & ~0x7F) | (layer & 0x7F);
	}

	void * WorldOf(Actor * caster)
	{
		if(!caster) return NULL;
		TESObjectCELL * cell = Engine::ParentCell(caster);
		if(!cell) return NULL;
		return CellBhkWorld(cell);
	}

	// ------------------------------------------------------- shapes, once each
	//
	// Every sphere cast used to take a block off the game's heap, run the
	// bhkCapsuleShape constructor (which builds an hkpCapsuleShape behind it),
	// cast once, and hand both back through the deleting destructor. The Special
	// Edition original does the same - but a tick of the ledge search makes
	// twenty to forty sphere casts, so that is up to forty allocations and forty
	// shape constructions per frame, all of them for one of four radii, all of
	// them identical to the last frame's.
	//
	// A cast does not modify the shape it sweeps, and a shape that belongs to no
	// world has nothing to keep it in step with anything, so the four are built
	// once and kept. That is four small allocations for the life of the process
	// against forty per frame, and it takes the game's heap - which other
	// plugins replace, and which locks - out of the per-frame path entirely.
	enum { kShapeCacheSize = 8 };

	struct CachedShape
	{
		float	radius;
		void *	wrapper;	// bhkCapsuleShape
		void *	shape;		// the hkpCapsuleShape behind it
	};

	CachedShape s_shapes[kShapeCacheSize];
	UInt32		s_shapeCount = 0;

	// The hkpCapsuleShape for a sphere of this radius, built the first time it is
	// asked for.
	//
	// outWrapper comes back non-null only when the cache had no room, and then it
	// is the caller's to release after the cast - so a plugin that one day casts
	// with more than eight different radii gets slower, not leakier. Every radius
	// in this plugin is a constant, and there are three of them.
	void * SphereShape(float radius, void *& outWrapper)
	{
		outWrapper = NULL;

		for(UInt32 i = 0; i < s_shapeCount; ++i)
			if(s_shapes[i].radius == radius)
				return s_shapes[i].shape;

		void * wrapper = Engine::HeapAllocate(0x14, 0, false);
		if(!wrapper) return NULL;

		const NiPoint3 zero(0.0f, 0.0f, 0.0f);
		CapsuleShapeCtor(wrapper, &zero, &zero, radius);

		void * shape = *(void **)((UInt8 *)wrapper + 0x08);	// bhkRefObject::referencedObject
		if(!shape)
		{
			Engine::HeapFree(wrapper, false);
			return NULL;
		}

		if(s_shapeCount < kShapeCacheSize)
		{
			CachedShape & slot = s_shapes[s_shapeCount];
			slot.radius		= radius;
			slot.wrapper	= wrapper;
			slot.shape		= shape;
			++s_shapeCount;
		}
		else
		{
			outWrapper = wrapper;
		}

		return shape;
	}

	void SortHits(Havok::Result & result)
	{
		for(UInt32 i = 1; i < result.hitCount; ++i)
		{
			Havok::Hit key = result.hits[i];
			SInt32 j = (SInt32)i - 1;
			while(j >= 0 && result.hits[j].fraction > key.fraction)
			{
				result.hits[j + 1] = result.hits[j];
				--j;
			}
			result.hits[j + 1] = key;
		}
	}

	// The two casts, each inside the lock, each giving it back whatever happens -
	// and each reading EVERYTHING it needs out of what the cast returned before
	// it lets go.
	//
	// The collector holds pointers to collidables, and a collidable belongs to the
	// physics world. The read lock is what keeps the world from being taken apart,
	// so the moment it is released those pointers are only addresses. Reading the
	// layer and following a collidable to its reference after the release - which
	// is what the first versions here did, while the comments said otherwise - is
	// a use of memory that may have been freed and handed to something else, and
	// a stale pointer that now points at a live object of another type is the one
	// kind of mistake no null check can see.
	//
	// A fault anywhere under the cast would otherwise walk straight past the
	// release and leave the world read-locked for good - and the next thread to
	// want the write side spins on it forever. A crash is bad; a game that hangs
	// with no message at all is worse, and it is the one failure this mod must
	// never cause. __finally runs on the way out either way.
	//
	// Nothing in the locked functions sorts, classifies or logs: copy and leave.
	// No C++ object with a destructor may live in these two functions.
	void ReadRayHits(Havok::Result & result, const RayCollector & collector,
					 const NiPoint3 & start, const NiPoint3 & dir, float distance)
	{
		const UInt32 count = collector.Size();
		for(UInt32 i = 0; i < count; ++i)
		{
			const UInt8 * entry = collector.At(i);
			const float fraction = *(const float *)(entry + 0x10);
			const void * collidable = *(const void **)(entry + 0x50);
			if(!collidable) continue;

			Havok::Hit * slot = SlotFor(result, fraction);
			if(!slot) continue;

			Havok::Hit & hit = *slot;
			hit.fraction	= fraction;
			hit.distance	= distance * fraction;
			hit.position	= NiPoint3(start.x + dir.x * hit.distance, start.y + dir.y * hit.distance, start.z + dir.z * hit.distance);
			const float * n = (const float *)(entry + 0x00);
			hit.normal		= NiPoint3(n[0], n[1], n[2]);
			hit.layer		= *(const UInt32 *)((const UInt8 *)collidable + 0x1C) & 0x7F;
			ResolveRef(collidable, hit.hasReference, hit.formType);
		}
	}

	void ReadPickRoot(Havok::Result & result, const UInt8 * pickData)
	{
		const void * root = *(const void **)(pickData + 0x80);
		if(!root) return;

		result.layer = *(const UInt32 *)((const UInt8 *)root + 0x1C) & 0x7F;
		ResolveRef(root, result.hasReference, result.formType);
	}

	bool LockedPickAndRead(void * world, UInt8 * pickData, const RayCollector & collector,
						   Havok::Result & result, const NiPoint3 & start, const NiPoint3 & dir,
						   float distance)
	{
		bool picked = false;

		WorldLock lock;
		lock.Acquire(world);
		__try
		{
			typedef bool (__thiscall * _PickObject)(void *, void *);
			picked = (*(_PickObject **)world)[kBhkWorld_PickObject](world, pickData) &&
					 *(float *)(pickData + 0x40) < 1.0f;

			ReadRayHits(result, collector, start, dir, distance);
			if(picked) ReadPickRoot(result, pickData);
		}
		__finally
		{
			lock.Release();
		}

		return picked;
	}

	void ReadShapeHits(Havok::Result & result, const PointCollector & collector, float distance)
	{
		const UInt32 count = collector.size;
		for(UInt32 i = 0; i < count; ++i)
		{
			const UInt8 * entry = collector.At(i);
			const float * position	= (const float *)(entry + 0x00);
			const float * normal	= (const float *)(entry + 0x10);
			// The fourth component of the separating normal is how far along
			// the cast the contact happened.
			const float fraction	= normal[3];
			const void * collidableB = *(const void **)(entry + 0x28);
			if(!collidableB) continue;

			// A swept shape against a piece of architecture produces a contact
			// per face it touches, so this one overflows even more readily than
			// the ray does.
			Havok::Hit * slot = SlotFor(result, fraction);
			if(!slot) continue;

			Havok::Hit & hit = *slot;
			hit.fraction	= fraction;
			hit.distance	= distance * fraction;
			hit.position	= NiPoint3(position[0] * Engine::HavokScaleInverse(),
									   position[1] * Engine::HavokScaleInverse(),
									   position[2] * Engine::HavokScaleInverse());
			hit.normal		= NiPoint3(normal[0], normal[1], normal[2]);
			hit.layer		= *(const UInt32 *)((const UInt8 *)collidableB + 0x1C) & 0x7F;
			ResolveRef(collidableB, hit.hasReference, hit.formType);
		}
	}

	void LockedLinearCastAndRead(void * world, void * hkWorld, const void * collidable,
								 const void * input, PointCollector & collector,
								 Havok::Result & result, float distance)
	{
		WorldLock lock;
		lock.Acquire(world);
		__try
		{
			LinearCast(hkWorld, collidable, input, &collector, NULL);
			ReadShapeHits(result, collector, distance);
		}
		__finally
		{
			lock.Release();
		}
	}
}

namespace Havok
{
	Result RayCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, UInt32 layer, Actor * caster)
	{
		Result result;
		const NiPoint3 dir = Normalised(direction);

		result.start		= start;
		result.direction	= dir;
		result.distance		= distance;
		result.hitPosition	= NiPoint3(start.x + dir.x * distance, start.y + dir.y * distance, start.z + dir.z * distance);

		void * world = WorldOf(caster);
		if(!world || distance <= 0.0f) return result;

		const float scale = Engine::HavokScale();
		const float inv   = Engine::HavokScaleInverse();

		__declspec(align(16)) UInt8 pickData[0xC0];
		memset(pickData, 0, sizeof(pickData));

		__declspec(align(16)) RayCollector collector;
		RayCollectorCtor(&collector);

		float * from = (float *)(pickData + 0x00);
		float * to   = (float *)(pickData + 0x10);
		from[0] = start.x * scale;	from[1] = start.y * scale;	from[2] = start.z * scale;	from[3] = 0.0f;
		to[0] = result.hitPosition.x * scale;
		to[1] = result.hitPosition.y * scale;
		to[2] = result.hitPosition.z * scale;
		to[3] = 0.0f;

		*(UInt32 *)(pickData + 0x20) = 0;							// no shape collection filter
		*(UInt32 *)(pickData + 0x24) = CastFilter(caster, layer);

		// The output block starts out as "nothing hit", exactly as the vanilla
		// caller at 0x006BF6E0 primes it.
		*(float  *)(pickData + 0x40) = 1.0f;						// hit fraction
		*(UInt32 *)(pickData + 0x44) = 0xFFFFFFFF;					// extra info
		*(UInt32 *)(pickData + 0x48) = 0xFFFFFFFF;					// first shape key
		*(UInt32 *)(pickData + 0x50) = 0xFFFFFFFF;
		*(UInt32 *)(pickData + 0x70) = 0;							// shape key index
		*(UInt32 *)(pickData + 0x80) = 0;							// root collidable
		*(void  **)(pickData + 0xAC) = &collector;					// all hits, not just the nearest
		*(UInt8  *)(pickData + 0xB0) = 0;

		const bool picked = LockedPickAndRead(world, pickData, collector, result, start, dir, distance);

		SortHits(result);

		if(picked)
		{
			result.didHit		= true;
			result.distance		= distance * *(float *)(pickData + 0x40);
			result.hitPosition	= NiPoint3(start.x + dir.x * result.distance,
										   start.y + dir.y * result.distance,
										   start.z + dir.z * result.distance);
			const float * n = (const float *)(pickData + 0x30);
			result.normal		= NiPoint3(n[0], n[1], n[2]);
			// layer, reference and form type of the root were read under the lock
		}
		else if(result.hitCount)
		{
			// PickObject reports "no hit" when the nearest hit was filtered out
			// of its own output, but the collector still holds the list.
			result.didHit		= true;
			result.distance		= result.hits[0].distance;
			result.hitPosition	= result.hits[0].position;
			result.normal		= result.hits[0].normal;
			result.layer		= result.hits[0].layer;
			result.hasReference	= result.hits[0].hasReference;
			result.formType		= result.hits[0].formType;
		}

		(void)inv;
		RayCollectorDtor(&collector);
		return result;
	}

	Result ShapeCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, float radius, UInt32 layer, Actor * caster)
	{
		Result result;
		const NiPoint3 dir = Normalised(direction);

		result.start		= start;
		result.direction	= dir;
		result.distance		= distance;
		result.hitPosition	= NiPoint3(start.x + dir.x * distance, start.y + dir.y * distance, start.z + dir.z * distance);

		void * world = WorldOf(caster);
		if(!world || distance <= 0.0f || radius <= 0.0f) return result;

		// A capsule whose two vertices sit on top of each other is a sphere,
		// which is what every cast in this plugin wants: no orientation to get
		// wrong as the cast direction changes. Built once per radius and kept -
		// see SphereShape above.
		void * shapeWrapper = NULL;
		void * shape = SphereShape(radius, shapeWrapper);
		if(!shape) return result;

		const float scale = Engine::HavokScale();

		// hkTransform: three rotation columns then the translation.
		__declspec(align(16)) float transform[16];
		memset(transform, 0, sizeof(transform));
		transform[0] = 1.0f; transform[5] = 1.0f; transform[10] = 1.0f;
		transform[12] = start.x * scale;
		transform[13] = start.y * scale;
		transform[14] = start.z * scale;

		__declspec(align(16)) Collidable collidable;
		memset(&collidable, 0, sizeof(collidable));
		collidable.shape				= shape;
		collidable.shapeKey				= 0xFFFFFFFF;
		collidable.motion				= transform;
		collidable.parent				= NULL;
		collidable.ownerOffset			= 0;
		collidable.broadPhaseType		= 0;
		collidable.collisionFilterInfo	= CastFilter(caster, layer);
		collidable.allowedPenetrationDepth = 0.0f;

		__declspec(align(16)) LinearCastInput input;
		input.to[0] = result.hitPosition.x * scale;
		input.to[1] = result.hitPosition.y * scale;
		input.to[2] = result.hitPosition.z * scale;
		input.to[3] = 0.0f;
		input.maxExtraPenetration	= 0.01f;
		input.startPointTolerance	= 0.01f;

		__declspec(align(16)) PointCollector collector;
		collector.Init();

		typedef void * (__thiscall * _GetWorld1)(void *);
		void * hkWorld = (*(_GetWorld1 **)world)[kBhkWorld_GetWorld1](world);
		if(hkWorld)
			LockedLinearCastAndRead(world, hkWorld, &collidable, &input, collector, result, distance);

		SortHits(result);

		if(result.hitCount)
		{
			result.didHit		= true;
			result.distance		= result.hits[0].distance;
			result.hitPosition	= result.hits[0].position;
			result.normal		= result.hits[0].normal;
			result.layer		= result.hits[0].layer;
			result.hasReference	= result.hits[0].hasReference;
			result.formType		= result.hits[0].formType;
		}

		collector.Release();

		// Only ever set when the shape cache was full, which this plugin's three
		// radii cannot do: the wrapper's deleting destructor releases the
		// hkpCapsuleShape behind it and frees the wrapper from the heap it came
		// from.
		if(shapeWrapper)
		{
			typedef void (__thiscall * _DeleteThis)(void *, UInt8);
			(*(_DeleteThis **)shapeWrapper)[0](shapeWrapper, 1);
		}

		return result;
	}
}
