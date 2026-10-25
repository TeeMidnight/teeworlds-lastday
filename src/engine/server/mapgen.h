/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_MAPGEN_H
#define ENGINE_SERVER_MAPGEN_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/tl/string.h>
#include <base/tl/threading.h>

class CJsonWriter;
class IEngine;
class IStorage;
struct _json_value;
typedef struct _json_value json_value;
struct CStructEntrance;

// Maps a deterministic 32-bit hash onto [0,1). Probabilities in the floor
// configs are fractions of this range (0.25 = 25%), which gives content far
// finer control than whole percentages.
inline float HashToUnitFloat(unsigned Hash)
{
	// 24 bits are exactly representable in a float, so the result is always
	// strictly below 1.0 and never rounds up to a guaranteed success.
	return (float) (Hash & 0xffffffu) / 16777216.0f;
}

// The server-side metadata of a generated map: entrance regions, resource
// placements and world geometry. It is written next to the map as
// "<GENERATED_MAPS_DIR>/<map>.json" instead of being embedded as map items, so
// the map clients download contains only what they render. When a map is kept
// only in a pack the same document is built in memory.
void BuildGeneratedWorldMeta(const char *pEntrances, int EntrancesSize,
	const char *pResources, int ResourcesSize,
	const char *pWorld, int WorldSize,
	array<char> &Out);
void GeneratedWorldMetaPath(const char *pMapName, char *pBuf, int Size);
bool SaveGeneratedWorldMeta(IStorage *pStorage, const char *pMapName,
	const char *pEntrances, int EntrancesSize,
	const char *pResources, int ResourcesSize,
	const char *pWorld, int WorldSize);
bool LoadGeneratedWorldMeta(IStorage *pStorage, const char *pMapName, array<char> &OutJson);

// The kind of space a floor represents. The three kinds are generated and
// traversed in fundamentally different ways, so this is a first class concept
// rather than a set of optional config fields.
enum EWorldKind
{
	// A normal euclidean world: the terrain is procedurally generated from
	// noise (heightmap surface, caves, ore veins) and the exits lead out of
	// the generated surface.
	WORLDKIND_EUCLID = 0,

	// The hub world (e.g. Connector). A hand authored static map that links
	// the generated floors together. It is never generated: the server loads
	// it directly from maps/.
	WORLDKIND_HUB,

	// A non euclidean world: a hand authored static map whose exits do not
	// correspond to their entrances, so walking back does not return you where
	// you came from. Euclidean surface entrances reach these worlds with a
	// small probability (see CSurfaceEntranceConfig::m_NonEuclidProba).
	WORLDKIND_NONEUCLID,
};

const char *WorldKindName(EWorldKind Kind);
EWorldKind WorldKindFromString(const char *pName, bool *pValid);

// ---------------------------------------------------------------------------
// CInstruction: the parsed configuration of one floor
// ---------------------------------------------------------------------------
struct CInstruction
{
	// -----------------------------------------------------------------
	// procedural (WORLDKIND_EUCLID) generation settings
	// -----------------------------------------------------------------

	// the noise parameters of a surface/cave/ore field
	struct CNoiseConfig
	{
		int m_SeedOffset; // keeps the fields of one floor independent
		int m_Octaves;
		float m_Frequency;
		float m_Lacunarity;
		float m_Persistence;
		float m_Amplitude; // 0 = derive from the octave count
	};

	struct CCaveConfig
	{
		bool m_Enabled;
		CNoiseConfig m_Noise;
		float m_Threshold; // noise above this carves air
		int m_MinDepth; // never carve closer than this to the surface
		int m_SurfaceMargin; // keep this many tiles of rock above a cave
	};

	struct COreConfig
	{
		char m_aResId[32]; // resource id spawned at the vein
		char m_aDisplay[32];
		char m_aTile[64]; // tileset drawn where the vein is
		CNoiseConfig m_Noise;
		float m_Threshold; // noise above this becomes a vein
		int m_MinDepth; // veins only below this depth
		int m_Probes; // resource entities rolled per vein tile
		float m_Proba; // 0..1 chance that a vein tile spawns resources
		int m_HardnessMin;
		int m_HardnessMax;
		int m_RespawnTimeMin;
		int m_RespawnTimeMax;

		// Surface nodes: a few resources stand in the open air on the ground
		// line so a player can see and reach them. m_SurfaceProba is the 0..1
		// chance for one eligible column; 0 disables the pass. The deep veins
		// above are always generated.
		float m_SurfaceProba;
	};

	// A surface opening: a visible cave mouth cut into the terrain that carries
	// the player to another floor.
	//
	// It is deliberately built from real tiles rather than being an invisible
	// coordinate box, because an unseen trigger teleports the player with no
	// warning. The opening is framed with unhookable stone so it reads as a
	// structure, and only the lit mouth is an entrance.
	struct CSurfaceEntranceConfig
	{
		bool m_Enabled;
		int m_Count; // how many surface openings to carve

		// the entrance is one of the exits listed in the floor's "entrances";
		// the first Count of them are used, in order
		int m_MinSeparation; // keep openings at least this many tiles apart
		int m_Width; // inner width of the cave mouth, in tiles
		int m_Height; // inner height of the cave mouth, in tiles
		int m_Clearance; // leave this many tiles of rock between the mouth and the roof
		int m_Frame; // thickness of the unhookable frame around the mouth
		int m_Margin; // keep openings this far from the map edges

		// Very small probability (0..1) that one of these openings leads into
		// a random non euclidean world instead of its configured euclidean
		// target. This is rolled per opening and per source instance, so an
		// opening is either a normal exit or a non euclidean one for good.
		float m_NonEuclidProba;
	};

	// -----------------------------------------------------------------
	// background
	//
	// The backgrounds of the official maps are not tiling images: the sky is a
	// single huge quad with no texture at all, painted with a flat colour, and
	// the clouds / mountains / sun are individual sprites placed as quads over
	// it at their own parallax. A texture like bg_cloud1 or jungle_background is
	// not a seamless sky, so stretching one across the layer smears it; that is
	// why this block describes a sky colour plus a list of decorative quads.
	// -----------------------------------------------------------------

	// One decorative pass of quads: clouds, mountains, a sun, a fog band.
	// Several of these make up a layer.
	struct CQuadPassConfig
	{
		char m_aImage[64]; // image in mapres/, "" means a plain colour quad
		int m_Count; // how many quads to scatter
		int m_ScaleX; // size of one quad, in tiles
		int m_ScaleY;
		int m_RepeatX; // how many times the texture tiles across the quad
		int m_RepeatY;
		// Vertical placement in tiles.
		//
		// AnchorY is a *screen* offset from where the ground line appears, not a
		// world height: the renderer places a group at quad - camera *
		// parallax/100, so a low parallax group is anchored to the screen and a
		// world height would push it thousands of pixels off. Negative is above
		// the ground (clouds, mountains), positive below. SpreadY scatters the
		// copies around that anchor.
		int m_AnchorY;
		int m_SpreadY; // vertical scatter range, in tiles
		int m_ColorTop[4]; // tint at the top edge
		int m_ColorBottom[4]; // tint at the bottom edge (gradient)
		int m_SeedOffset;

		// Scatter the copies across the group-space band the camera can reach
		// instead of across the whole world width. For a low-parallax layer that
		// band is much narrower than the world and starts left of x = 0, so the
		// copies overlap heavily and no cut quad edge lands inside the view.
		bool m_Band;
	};

	// What a layer is for. Groups are drawn in file order, so the role decides
	// where a layer lands: a foreground layer must come after the game layer or
	// it would be painted underneath the terrain.
	enum ELayerRole
	{
		LAYERROLE_BACKGROUND = 0, // behind the terrain (sky, mountains, clouds)
		LAYERROLE_FOREGROUND, // in front of the terrain (fog, overhanging leaves)
	};

	// A generated layer. Terrain layers carry tiles, decoration layers carry
	// quads, and both are placed at their own parallax so they move at their own
	// speed like the official maps.
	struct CLayerConfig
	{
		char m_aName[32];
		ELayerRole m_Role;
		int m_ParallaxX;
		int m_ParallaxY;
		int m_OffsetX; // group offset in tiles
		int m_OffsetY;

		// a terrain layer is filled from the generated world instead of from
		// quads; exactly one layer has this set
		bool m_IsTerrain;

		// Whether this decoration layer drifts sideways (clouds, fog). A
		// drifting layer is emitted at -world, 0 and +world so a full width
		// shift maps it onto itself, which is what hides the loop.
		bool m_Drift;

		// Whether a static layer is emitted at -world and +world as well. The
		// copies continue the layer past the map's left and right edges, which
		// a cylindrical world needs: without them the leftmost quads have no
		// neighbour and their cut edge is visible near x = 0.
		bool m_Wrap;

		bool m_Enabled;
		array<CQuadPassConfig> m_Quads;
	};

	// The day/night cycle the generator bakes into a euclidean floor.
	//
	// The whole cycle is emitted as synchronized map envelopes, so the client
	// animates it without knowing anything about LastDay: the sky cross fades
	// between the day and the night palette while a sun and a moon traverse the
	// screen once per cycle. One palette and one pair of celestial bodies are
	// generated per floor, so a snowy biome keeps a cold night and a desert a
	// warm one. Disabled leaves the old static sky.
	struct CDayNightConfig
	{
		bool m_Enabled;
		int m_DayLengthSeconds; // one full cycle; 1200 = 20 minutes
		// The midnight palette. Left unset (negative channel) each channel is
		// derived from the day palette, darkened and shifted towards blue, so the
		// configured day colour still drives the night look.
		int m_NightSkyColor[4];
		int m_NightHorizonColor[4];
		// The warm band drawn at sunrise and sunset.
		int m_DawnColor[4];
		char m_aMoonImage[64];
		int m_SunSize;  // sprite edge in screen pixels
		int m_MoonSize;
	};

	// The sideways drift of the cloud layers. A drifting layer is emitted three
	// times: once at its own position and once shifted by a full world width in
	// each direction. The three copies make the layer exactly periodic over the
	// width, so shifting it by one width and wrapping the envelope back to zero
	// leaves the picture unchanged: the loop seam cannot be seen from anywhere.
	struct CCloudsConfig
	{
		bool m_DriftEnabled;
		int m_DriftSeconds; // time for the clouds to cross one full world width
		int m_Direction;    // +1 drifts right, -1 drifts left
	};

	struct CGeneration
	{
		bool m_Enabled;
		int m_Width;
		int m_Height;
		int m_SurfaceLevel; // tile y of the average ground line
		float m_SurfaceAmplitude; // how far the heightmap swings up and down
		CNoiseConfig m_Surface;
		int m_EdgeWall; // thickness of the sealed top and bottom border

		// Columns kept out of the cylindrical wrap at each end of the x axis.
		//
		// The automapper samples across the seam, and where the far side is empty
		// it stamps an outer corner tile onto the border. These columns clamp to
		// the map instead, so they read their own edge and come out plain. Two is
		// what a 3x3 rule needs for a stable neighbourhood; 0 restores the plain
		// edge to edge wrap. The seam is levelled separately, by m_WrapBlend.
		int m_WrapMargin;

		// How far the ground eases back from the seam to its natural shape.
		//
		// Clamping the rules still leaves a corner wherever the real terrain
		// slopes beside the border. The outer m_WrapMargin columns are pinned to a
		// shared seam height and the ground eases back over this many columns, so
		// the seam is flat with no step. A wide window costs terrain variation; a
		// narrow one leaves a ledge. 0 disables it.
		int m_WrapBlend;

		// the tileset image the terrain layer draws from, e.g. "grass_main".
		// It has to match the tileset the automap rule set below was written
		// for, otherwise the rule indices point at the wrong tiles.
		char m_aTileset[64];
		char m_aMainRuleSet[32]; // automap rule set for the terrain layer ("grass", "winter", ...)
		char m_aCaveRuleSet[32]; // optional rule set for the carved cave interiors

		// The vegetation doodads: a decorative tile layer drawn in front of the
		// terrain. m_aDoodadsTileset names the doodads sheet in mapres/ and
		// m_aDoodadsRuleset the "doodads" set in automap/*.json that scatters it.
		// Both default to the terrain's own set ("grass_main" -> "grass_doodads"),
		// and an empty value disables the layer.
		char m_aDoodadsTileset[64];
		char m_aDoodadsRuleset[32];

		// The tileset and rule set of the entrance frames. Configured separately
		// from the terrain because "unhookable" is not a look the rock sheet
		// carries. Empty reuses the terrain tileset.
		char m_aUnhookableTileset[64];
		char m_aUnhookableRuleSet[32];

		// The sky is a quad at parallax 0 with a vertical gradient, like the
		// official maps: m_SkyColor at the top fading to m_SkyHorizonColor. Sized
		// to the screen, since parallax 0 already pins it to the camera.
		int m_SkyColor[4];
		int m_SkyHorizonColor[4];

		// The sun (or moon) drawn over the sky. Empty disables it.
		char m_aSunImage[64];

		// The generated day/night cycle (euclidean floors only).
		CDayNightConfig m_DayNight;

		// The drifting cloud layers (euclidean floors only).
		CCloudsConfig m_Clouds;

		// The ring that links the instances (seeds 1..m_Size) of one euclidean
		// floor into a single closed world. Disabled by default: a floor without
		// a "ring" block keeps the old self-wrapping cylinder. Enabled makes every
		// instance a window of one shared noise field, so neighbouring instances
		// overlap for m_SharedMargin tiles of identical terrain and can be walked
		// into from either side.
		struct CRingConfig
		{
			bool m_Enabled;
			int m_Size;         // how many instances form the ring (1024)
			int m_SharedMargin; // tiles an instance shares with each neighbour (75)
			int m_BaseSeed;     // the single noise seed of the whole ring
		};
		CRingConfig m_Ring;

		// The anchor of the generated background scenery: the highest ground
		// (smallest tile y) anywhere on the ring. All instances of a ring have
		// to agree on it or the clouds and mountains jump vertically when a
		// player crosses into the neighbour. Cached because every instance
		// would otherwise scan the whole ring again; -1 while unknown.
		mutable int m_RingBackgroundGroundY;

		// every layer to emit, in the order they are written out
		array<CLayerConfig> m_Layers;

		CCaveConfig m_Caves;
		array<COreConfig> m_Ores;
		CSurfaceEntranceConfig m_SurfaceEntrances;
	};

	// -----------------------------------------------------------------
	// structure pasting (used by the non euclidean worlds)
	// -----------------------------------------------------------------
	struct CStruct
	{
		char m_aBaseMap[64];
		float m_GenerateProba; // 0..1 chance to paste this struct
		bool m_PasteAir; // whether empty struct tiles overwrite the base (default true)
		array<char> m_EntrancesJson; // serialized entrances json of this struct (optional)
		array<char> m_ResourcesJson; // serialized resources json of this struct (optional)
	};

	char m_aFloorName[64]; // unique floor/map name (the file name)
	EWorldKind m_Kind;
	char m_aBaseMap[64]; // the base map file to load (the "base" field)

	// Whether a train may pick this euclidean floor as a destination. The
	// travel menu lists exactly the floors that set this. The static kinds are
	// reached through their own entrances, so the flag only applies to euclid.
	bool m_TrainEnabled;
	CGeneration m_Generation;
	array<CStruct> m_Structs;
	array<char> m_DefaultEntrances;
	array<char> m_ResourcesJson; // serialized resources json of this base (optional)
};

// hash key of an (floor, seed) instance pair for CMapGen's known set
struct SInstanceKeyHash
{
	static unsigned hash(long long Key) { return (unsigned) (Key ^ (Key >> 32)); }
	static bool equal(long long A, long long B) { return A == B; }
};

class CMapGen
{
	// one append-only pack per floor ("mappacks/<mod>/<version>/<floor>.mappack"),
	// opened lazily and only touched while m_GenMutex is held
	struct SFloorPack;

public:
	// One generated instance handed over in memory, without a cache file.
	// m_pMap is a complete v4 datafile, m_pJson the world metadata sidecar.
	// Both are mem_alloc'd and belong to the caller.
	struct SMapInstance
	{
		void *m_pMap;
		int m_MapSize;
		void *m_pJson;
		int m_JsonSize;
	};
	static void FreeInstance(SMapInstance *pInstance);

private:

	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	class IStorage *Storage() { return m_pStorage; };
	class IConsole *Console() { return m_pConsole; };

	// keyed by str_quickhash of the floor name
	hash_table<unsigned, CInstruction *, 4> m_lInstructions;

	// Sorted by floor name after loading, so menu order and the random
	// non euclidean pick do not depend on directory iteration order.
	array<CInstruction *> m_lTrainFloors;
	array<CInstruction *> m_lNonEuclidFloors;
	void RebuildFloorLists();

	// -----------------------------------------------------------------
	// background train chunks
	//
	// Generating a floor takes long enough to be visible as a hitch, so the
	// train does not build a map on the tick a player clicks it. The engine
	// thread pool drains this queue and prepares the chunk for a destination's
	// next trip while the players are still in their current world. A prepared
	// ring chunk also queues its two neighbours: the game layer brings them in
	// on the first tick after the switch, and generating them there would stall
	// the whole server. m_GenLock guards the queue and the per-destination
	// state; m_GenMutex serializes the generator itself, because it reads shared
	// caches and writes a shared temporary file.
	// -----------------------------------------------------------------
	struct STrainChunk
	{
		int m_ReadySeed;   // fully generated, reserved for the next click
		int m_PendingSeed; // being generated; 0 when idle
	};

	struct CGenJob
	{
		CGenJob *m_pNext;
		int m_TrainIndex;
		char m_aFloorName[64];
		int m_Seed;
	};

	// one submitted engine job and the bookkeeping that keeps it alive until
	// the pool reports it done (see mapgen.cpp)
	struct CWorkerJob;

	class IEngine *m_pEngine;
	lock m_GenLock;
	lock m_GenMutex;

	// map packs: every generated instance is appended to its floor's pack, and
	// a missing instance is restored from the pack instead of being generated
	// again. The readers cache the pack index, so a lookup does not read the
	// map data. Both are only touched under m_GenMutex.
	bool m_UseMapPacks;
	// the server keeps no cache file: a generated instance is packed and the
	// file the generator wrote is removed again
	bool m_DeleteGeneratedFiles;
	array<SFloorPack *> m_lPacks;
	// Instances known to exist without asking the disk or opening a pack again:
	// filled when a pack is opened or appended to. Guarded by m_GenLock so the
	// per tick train code can consult it without waiting for a generation.
	hash_table<long long, bool, 16, SInstanceKeyHash> m_lKnownInstances;
	static long long InstanceKey(const char *pFloorName, int Seed);
	void RememberInstance(const char *pFloorName, int Seed);
	void RememberPackInstances(SFloorPack *pPack);
	// caller holds m_GenLock
	bool KnownInstance(const char *pFloorName, int Seed) const;
	// caller holds m_GenLock: available on disk or in the floor's pack
	bool InstanceAvailable(const char *pFloorName, int Seed) const;
	// caller holds m_GenMutex: open the pack and seed the known set once
	bool EnsurePackOpen(SFloorPack *pPack);
	SFloorPack *PackFor(const char *pFloorName);
	bool PackContains(const char *pFloorName, int Seed);
	bool ReadFromPack(const char *pFloorName, int Seed, SMapInstance *pOut);
	bool ExtractFromPack(const char *pFloorName, int Seed);
	// append bytes that are already in memory
	void AppendInstanceToPack(const char *pFloorName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize);
	// append an instance that exists as files (migration / tooling)
	void AppendToPack(const char *pFloorName, int Seed);
	// write the instance as the usual "<name>.map" + ".json" cache files
	void WriteInstanceFiles(const char *pFloorName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize);
	void ClosePacks();
	CGenJob *m_pGenHead;
	CGenJob *m_pGenTail;
	CGenJob *m_pGenRunning;
	CWorkerJob *m_pWorkerJobs;
	bool m_GenStarted;
	bool m_GenShutdown;
	bool m_GenWorkerActive;
	array<STrainChunk> m_lTrainChunks;

	static int GenWorker(void *pUser);
	int FindTrainFloor(const char *pFloorName) const;
	int RollTrainSeed(int Index);
	bool GeneratedMapExists(const char *pFloorName, int Seed) const;
	void QueueChunkLocked(int Index, int Seed);
	void QueuePrefetchLocked(const char *pFloorName, int Seed);
	void QueueRingNeighboursLocked(int Index, int Seed);
	bool RingNeighboursReadyLocked(int Index, int Seed);
	void PromoteReadyLocked(int Index);
	void ScheduleWorkerLocked();
	void ReapWorkerJobsLocked();
	void StopGeneration();

	// the synchronous core, called by RequestNewMap, EnsureNewMap and the
	// worker. The caller holds m_GenMutex.
	bool GenerateNow(const char *pFloorName, int Seed);

	// Resolves the actual destination of one carved euclidean entrance. It
	// usually returns pConfigured unchanged and rolls m_NonEuclidProba to
	// occasionally send the player into a non euclidean world instead.
	void ResolveEntranceTarget(const CInstruction *pSource, int Seed, int EntranceIndex, const char *pConfigured, char *pOut, int OutSize) const;

	static void FreeInstruction(CInstruction *&pInstruction, void *pUser);

	// IStorage::ListDirectory callback: loads every "<floorname>.json" file
	// found in the "maps/worlds" directory
	static int ListWorldsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);

	// parse a single floor config file and register its CInstruction
	void LoadFloor(const char *pFloorName, const char *pFilePath);

	// serializes the entrance array, appending "_<seed>" to the entrance
	// targets that are themselves generated worlds (configured in
	// worlds.json); static maps like the main world Connector keep their
	// plain name. The entrances carried by the struct maps themselves are
	// appended as well.
	void WriteEntrancesWithSeed(CJsonWriter *pWriter, const json_value &rEntrances, int Seed, const array<CStructEntrance> &lStructEntrances);

	// one generated instance, entirely in memory
	struct SGeneratedMap
	{
		array<char> m_Map;  // complete v4 datafile
		array<char> m_Meta; // world metadata sidecar json
	};

	// build a euclidean floor from noise (see worldgen.h)
	bool GenerateEuclidean(const CInstruction *pInstruction, int Seed, SGeneratedMap *pOut);

	// paste the structs of a non euclidean floor onto its base map
	bool GenerateNoneuclid(const CInstruction *pInstruction, int Seed, SGeneratedMap *pOut);

public:
	CMapGen(class IStorage *pStorage, class IConsole *pConsole);
	~CMapGen();

	// Generates a new map for the given floor (the floor name, e.g. the map
	// name prefix "<FloorName>_<Seed>"). The floor's instruction carries the
	// actual base map file to load and the world kind, which selects the
	// generation strategy. The result is written to
	// "<GENERATED_MAPS_DIR>/<FloorName>_<Seed>.map". Returns true on success.
	bool RequestNewMap(const char *pFloorName, int Seed);

	// Generates a map only when it is not already on disk, and waits for an
	// in-flight background generation of the same map instead of building it a
	// second time. Called by the server when a transition actually loads a map.
	// With map packs enabled the floor's pack is consulted first: a packed
	// instance is extracted instead of generated, and a disk map that predates
	// the pack is appended once so the pack converges on the whole floor.
	bool EnsureNewMap(const char *pFloorName, int Seed);

	// Returns one instance as bytes, without requiring a cache file: from the
	// floor's pack when it is there, otherwise after generating it. A fresh
	// generation is packed immediately and its cache file is removed again, so
	// the pack stays the only durable copy. The server loads maps this way.
	bool AcquireMap(const char *pFloorName, int Seed, SMapInstance *pOut);

	// Whether generated instances are kept in a per floor append-only pack.
	// Defaults to on; the server maps sv_mappack onto it.
	void SetUseMapPacks(bool Use) { m_UseMapPacks = Use; }
	bool UseMapPacks() const { return m_UseMapPacks; }
	// Remove a freshly generated instance from the cache after it was packed.
	// Only meaningful with map packs on, and only for a server that reads the
	// pack instead of the files.
	void SetDeleteGeneratedFiles(bool Delete) { m_DeleteGeneratedFiles = Delete; }

	// The pack file of a floor (for tooling and tests).
	void PackPath(const char *pFloorName, char *pBuf, int Size) const;

	// Starts using the engine's thread pool and queues one random chunk per
	// train destination, so the first travel to a world normally finds its map
	// already generated. Without an engine no job is submitted; only a chunk
	// that already exists on disk can be taken.
	void StartPreGeneration(class IEngine *pEngine);

	// Reaps finished pool jobs and schedules the queue when work is waiting.
	// Called from the main thread once per server tick.
	void UpdateBackgroundGeneration();

	// Consumes the pre-generated chunk for a train destination and immediately
	// queues the chunk for its following trip. Returns false when no chunk is
	// ready yet, when a ring chunk's two neighbours are still being generated
	// (the caller should let the player try again), or when the floor is not a
	// train destination.
	bool TakeTrainChunk(const char *pFloorName, char *pOutMapName, int OutSize);

	// the kind of a floor, or false when the floor is unknown. Used by the
	// server to decide whether a map should be generated at all.
	bool GetFloorKind(const char *pFloorName, EWorldKind *pKind) const;

	// The train destinations, in floor-name order. The game menu lists exactly
	// these and asks for the instance count to pick a random chunk on arrival.
	int NumTrainDestinations() const { return m_lTrainFloors.size(); }
	const char *TrainDestinationName(int Index) const;
	int TrainDestinationRingSize(int Index) const;
};

#endif // ENGINE_SERVER_MAPGEN_H
