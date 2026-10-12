// Havok.h - ray casts and shape casts against the physics world.
//
// The parkour system is a physics query first and an animation system second:
// every ledge, vault and headroom test here is a cast into the Havok world.
//
// Special Edition builds an hkpCollidable by hand and hands it to
// hkpWorld::linearCast. This edition can do the same, but almost nothing about
// the surrounding types matches: the hit records are laid out differently (the
// collidable of the object you hit is at +0x28, not +0x24, which the engine's
// own collector-fill at 0x00D57960 settles), the capsule shape is built by a
// single constructor that scales to Havok itself, and the ray pick takes its
// all-hits collector in a different slot of the pick data.

#pragma once

#include "skse/NiTypes.h"

class Actor;
class TESObjectREFR;

namespace Havok
{
	// How many hits of one cast are kept. The mod this is ported from walks
	// every hit the collector returns, looking for the first one on a layer
	// worth climbing; a downward probe through a cluttered column - a city
	// street, under a tree, a room full of props - regularly returns more than
	// a handful, and the one that matters is not always among the first. An
	// earlier build here kept the first eight in collector order, which is why
	// the same fence would work in one spot and not in another.
	//
	// The collector itself holds sixteen in place before it grows, so this is
	// past what a probe of this length realistically produces, and the fill
	// below keeps the NEAREST hits rather than the first ones, so the cap can
	// never hide something closer.
	enum { kMaxHits = 32 };

	// What `formType` says when the hit could not be traced back to a form -
	// terrain, a collision object with no reference behind it, anything the
	// lookup refused. It excludes nothing, which is what a null reference used
	// to mean to the callers.
	enum { kFormType_Unknown = 0xFF };

	struct Hit
	{
		float			fraction;		// 0..1 along the cast
		float			distance;		// game units
		NiPoint3		position;
		NiPoint3		normal;
		UInt32			layer;

		// Whether a real reference owns what was hit: terrain and loose
		// collision have none, and neither does anything the lookup refused.
		//
		// This is a value, not a pointer, on purpose. The reference was found and
		// checked while the cast still held the physics world, which is the only
		// moment it is known to be alive, and nothing that points into the world
		// is allowed out of that window.
		bool			hasReference;
		UInt8			formType;		// its base form's type, or kFormType_Unknown
	};

	struct Result
	{
		bool			didHit;
		float			distance;		// to the nearest accepted hit, else the full length
		NiPoint3		start;
		NiPoint3		direction;
		NiPoint3		hitPosition;
		NiPoint3		normal;
		UInt32			layer;
		bool			hasReference;
		UInt8			formType;

		UInt32			hitCount;
		Hit				hits[kMaxHits];	// sorted, nearest first

		// Only the header is cleared, not the hit array.
		//
		// A tick of the ledge search makes dozens of casts, and each one used to
		// zero 1.1 kB of hits that nothing ever read: the fill writes every
		// field of a hit as it adds it, and every reader stops at hitCount.
		Result()
		{
			memset(this, 0, (size_t)((const UInt8 *)&hits[0] - (const UInt8 *)this));
			formType = kFormType_Unknown;
		}
	};

	// Collision layers, as the engine numbers them. Only the ones this plugin
	// names are listed; the full table is the same as Special Edition's.
	enum
	{
		kLayer_Unidentified	= 0,
		kLayer_Static		= 1,
		kLayer_AnimStatic	= 2,
		kLayer_Transparent	= 3,
		kLayer_Clutter		= 4,
		kLayer_Weapon		= 5,
		kLayer_Projectile	= 6,
		kLayer_Spell		= 7,
		kLayer_Biped		= 8,
		kLayer_Trees		= 9,
		kLayer_Props		= 10,
		kLayer_Water		= 11,
		kLayer_Trigger		= 12,
		kLayer_Terrain		= 13,
		kLayer_Trap			= 14,
		kLayer_NonCollidable= 15,
		kLayer_Ground		= 17,
		kLayer_DebrisLarge	= 20,
		kLayer_ActorZone	= 22,
		kLayer_CharController= 30,
		kLayer_DeadBip		= 32,
		kLayer_DoorDetection= 37,
	};

	// A ray from `start` along `direction` for `distance` game units, cast with
	// the actor's own collision filter so it never hits the caster. Every hit
	// is kept, nearest first.
	Result RayCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, UInt32 layer, Actor * caster);

	// The same, with a sphere of `radius` swept along the ray. Used wherever a
	// ray would slip through a gap the player cannot.
	Result ShapeCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, float radius, UInt32 layer, Actor * caster);
}
