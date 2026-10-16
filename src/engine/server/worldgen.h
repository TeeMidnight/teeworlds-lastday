/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_WORLDGEN_H
#define ENGINE_SERVER_WORLDGEN_H

#include <base/tl/array.h>
#include <base/vmath.h>

#include <game/mapitems.h>

struct CInstruction;

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
	array<CLayer> m_lLayers;
	array<CResource> m_lResources;
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
	int m_Period;  // circumference of the whole ring, in tiles
	int m_Stride;  // distance between two neighbouring instances
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

// The index a generated solid cell is marked with before the automapper runs.
// Matches the shipped rule sets' "basetile", so the mapper sees a solid tile.
enum
{
	GENERATED_SOLID_TILE = 1,
	GENERATED_AIR_TILE = TILE_AIR,
};

#endif // ENGINE_SERVER_WORLDGEN_H
