/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_WORLDGEN_H
#define ENGINE_SERVER_WORLDGEN_H

#include <base/tl/array.h>
#include <base/vmath.h>
#include <game/mapitems.h>

struct CInstruction;

// A container placed in a generated structure. m_ID is stable for the map
// instance and is the key of the container's world save entry; m_X/m_Y are map
// tile coordinates, like a resource placement.
struct SContainerSpawn
{
	int m_ID;
	char m_aKind[32];
	// loot table the structure assigned to this container; empty means the
	// container spawns empty
	char m_aLoot[32];
	int m_X;
	int m_Y;
};

// A creature placed by a generated structure. m_Group is the persistent boss
// encounter key: when the boss dies the whole group is skipped on reload.
struct SGeneratedMob
{
	int m_ID;
	int m_Group;
	char m_aMob[64];
	bool m_Boss;
	int m_X;
	int m_Y;
};

// The result of generating one euclidean world. Owns its tile buffers; the
// caller hands them to the map creator.
class CGeneratedWorld
{
public:
	// one tile layer of the generated map
	struct CLayer
	{
		char m_aName[32];
		int m_Width;
		int m_Height;
		int m_ParallaxX;
		int m_ParallaxY;
		int m_Flags; // TILESLAYERFLAG_* (the game layer sets GAME)
		bool m_IsGameLayer;
		const char *m_pImageName; // external image of this layer, may be 0
		CTile *m_pTiles;
	};

	// Layer indices in the order GenerateEuclideanWorld adds them. The generator
	// addresses these directly, so the meaning is fixed rather than looked up by
	// name.
	enum
	{
		// the terrain the player sees: decoration only, no collision meaning
		LAYER_TERRAIN = 0,
		// the collision layer, derived from the terrain once it is final
		LAYER_GAME = 1,
		// the entrance frames. Separate because "unhookable" is collision, not a
		// look: in the terrain layer it would make the decoration carry
		// collision meaning and draw the frame with the wrong tileset.
		LAYER_UNHOOKABLE = 2,
	};

	// a resource that should be spawned in the generated world
	struct CResource
	{
		char m_aResId[32];
		char m_aDisplay[32];
		int m_X;
		int m_Y;
		int m_Hardness;
		int m_RespawnTime;
	};

	// an exit region, in map tile coordinates; the axes marked with
	// INT_MIN / INT_MAX are unrestricted (same convention as the config)
	struct CEntrance
	{
		int m_StartX, m_StartY, m_EndX, m_EndY;
		char m_aTarget[64];
	};

	CGeneratedWorld();
	~CGeneratedWorld();

	// the surface height (tile y of the first solid tile) per column, kept so
	// the entrance carving and any later feature can reuse it
	array<int> m_Surface;

	// The ground line the decorative background is anchored to. On a ring this
	// is the highest ground of every instance, not just this one, so the
	// scenery stays at the same screen height across a seam.
	int m_BackgroundGroundY;
	array<CLayer> m_lLayers;
	array<CResource> m_lResources;
	array<SContainerSpawn> m_lContainers;
	array<SGeneratedMob> m_lMobs;
	array<CEntrance> m_lEntrances;

	CLayer *AddLayer(const char *pName, int Width, int Height, bool IsGameLayer);
};

// Builds the terrain of a euclidean floor from its noise settings, up to but not
// including the surface entrances.
//
// The caller runs the automapper after this and before BuildWorldEntrances,
// since the mapper resets every solid tile to its basetile. Returns 0 when the
// configuration is unusable.
CGeneratedWorld *GenerateEuclideanWorld(const CInstruction *pInstruction, int Seed);

// Cuts the visible cave mouths and registers their exits. Runs after the
// automapper, so the entrance structure keeps the tiles it sets. Also places the
// spawn points and derives the collision layer.
void BuildWorldEntrances(const CInstruction *pInstruction, int Seed, CGeneratedWorld *pWorld);

// Maps one instance (Seed) of a euclidean floor onto its floor's ring.
//
// Without a ring this is the classic self-wrapping cylinder: offset 0, period =
// width and the instance's own seed driving the noise. With a ring the instance
// is a window of the ring's single shared noise field: instance Seed starts at
// global x (Seed-1)*m_Stride and the field closes after m_Period tiles. Two
// neighbouring instances then read the very same value for the strip they
// share, which is what makes the terrain continue across the boundary.
struct CWorldRing
{
	bool m_Enabled;
	int m_OffsetX; // global x of this instance's local x = 0
	int m_Period; // circumference of the whole ring, in tiles
	int m_Stride; // distance between two neighbouring instances
	unsigned m_NoiseSeed;
};
CWorldRing WorldRing(const CInstruction *pInstruction, int Seed);

// The x (in world units) at which a character leaves a ring instance to one
// side. The shared margin is split 1/3 : 2/3, so a crossing has hysteresis:
// the outgoing trigger sits at 2/3 of the strip and the arrival lands back at
// 1/3, which stops a character that just crossed from crossing straight back.
// Direction is +1 for the right neighbour and -1 for the left.
inline float RingTriggerX(int WidthTiles, int SharedMargin, int Direction)
{
	if(Direction > 0)
		return ((float) (WidthTiles - SharedMargin) + (float) (2 * SharedMargin) / 3.0f) * 32.0f;
	return ((float) SharedMargin / 3.0f) * 32.0f;
}

// Where a crossing from LocalX (world units) lands in the neighbour: the same
// global column, re-based onto the neighbour's local axis. StrideTiles is the
// distance between two neighbouring instances.
inline float RingArrivalX(int StrideTiles, int Direction, float LocalX)
{
	return LocalX - (float) Direction * (float) StrideTiles * 32.0f;
}

// RingArrivalX with the source column clamped into the strip the two instances
// share. Only that strip has the same terrain on both sides: the crossing test
// runs after the move, so a fast crossing or a delayed switch can leave LocalX
// past the trigger, past the map edge (the collision then clamps to the border
// column), where mapping straight through would land the character on the
// neighbour's own terrain, which is often solid.
inline float RingArrivalXClamped(int StrideTiles, int WidthTiles, int SharedMargin, int Direction, float LocalX)
{
	float SourceX = LocalX;
	if(Direction > 0)
		SourceX = clamp(SourceX, (float) (WidthTiles - SharedMargin) * 32.0f, (float) WidthTiles * 32.0f - 1.0f);
	else
		SourceX = clamp(SourceX, 0.0f, (float) SharedMargin * 32.0f - 1.0f);
	return RingArrivalX(StrideTiles, Direction, SourceX);
}

// Whether a position (world units) is past one of the two transfer triggers,
// and which way it leaves the instance. 0 means it is still inside.
//
// The comparison is inclusive on both sides: a position exactly at the trigger
// has already crossed. The arrival lands back inside the other trigger, so the
// transfer cannot bounce straight back.
inline int RingCrossDirection(float X, float RightTriggerX, float LeftTriggerX)
{
	if(X >= RightTriggerX)
		return 1;
	if(X <= LeftTriggerX)
		return -1;
	return 0;
}

// The instance index one step in Direction (+1 right, -1 left) reaches from
// Seed, wrapping around the ring (instance Size is the last one).
inline int RingNeighbourSeed(int Seed, int Direction, int Size)
{
	if(Size < 1)
		return Seed;
	int Target = Seed + Direction;
	while(Target > Size)
		Target -= Size;
	while(Target < 1)
		Target += Size;
	return Target;
}

// The instance at the middle of a ring of Size instances. Seeds are 1..Size,
// so 1024 lands on 512 and an odd size lands on its exact centre. The train
// uses it to always arrive in the middle of a floor instead of a random chunk.
inline int RingCenterSeed(int Size)
{
	if(Size <= 1)
		return 1;
	return (Size + 1) / 2;
}

// The shortest horizontal shift (in tiles) that brings an entity of the
// instance at OffsetTo into the frame of the instance at OffsetFrom. Taking the
// short way round the ring is what makes the last instance line up with the
// first instead of being a whole circumference away.
inline int RingNeighbourShift(int OffsetFrom, int OffsetTo, int Period)
{
	if(Period <= 0)
		return OffsetTo - OffsetFrom;
	int D = OffsetTo - OffsetFrom;
	while(D > Period / 2)
		D -= Period;
	while(D < -(Period / 2))
		D += Period;
	return D;
}

// The index a generated solid cell is marked with before the automapper runs.
// Matches the shipped rule sets' "basetile", so the mapper sees a solid tile.
enum
{
	GENERATED_SOLID_TILE = 1,
	GENERATED_AIR_TILE = TILE_AIR,
};

#endif // ENGINE_SERVER_WORLDGEN_H
