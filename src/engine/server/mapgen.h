/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_MAPGEN_H
#define ENGINE_SERVER_MAPGEN_H

#include <base/tl/hashtable.h>
#include <base/tl/string.h>

class CJsonWriter;
struct _json_value;
typedef struct _json_value json_value;
struct CStructEntrance;

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
	// you came from. Currently disabled at the config level (no floor routes
	// into it), but the kind is parsed so the world can be re-enabled later
	// without touching the generator.
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
		int m_Proba;
		int m_HardnessMin;
		int m_HardnessMax;
		int m_RespawnTimeMin;
		int m_RespawnTimeMax;

		// Surface nodes: a few resources stand in the open air on the ground
		// line so a player can see and reach them. m_SurfaceProba is the chance
		// in percent for one eligible column; 0 disables the pass. The deep
		// veins above are always generated.
		int m_SurfaceProba;
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
		int m_GenerateProba;
		bool m_PasteAir; // whether empty struct tiles overwrite the base (default true)
		array<char> m_EntrancesJson; // serialized entrances json of this struct (optional)
		array<char> m_ResourcesJson; // serialized resources json of this struct (optional)
	};

	char m_aFloorName[64]; // unique floor/map name (the file name)
	EWorldKind m_Kind;
	char m_aBaseMap[64]; // the base map file to load (the "base" field)
	CGeneration m_Generation;
	array<CStruct> m_Structs;
	array<char> m_DefaultEntrances;
	array<char> m_ResourcesJson; // serialized resources json of this base (optional)
};

class CMapGen
{
	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	class IStorage *Storage() { return m_pStorage; };
	class IConsole *Console() { return m_pConsole; };

	// keyed by str_quickhash of the floor name
	hash_table<unsigned, CInstruction *, 4> m_lInstructions;

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

	// build a euclidean floor from noise (see worldgen.h)
	bool GenerateEuclidean(const CInstruction *pInstruction, int Seed);

	// paste the structs of a non euclidean floor onto its base map
	bool GenerateNoneuclid(const CInstruction *pInstruction, int Seed);

public:
	CMapGen(class IStorage *pStorage, class IConsole *pConsole);
	~CMapGen();

	// Generates a new map for the given floor (the floor name, e.g. the map
	// name prefix "<FloorName>_<Seed>"). The floor's instruction carries the
	// actual base map file to load and the world kind, which selects the
	// generation strategy. The result is written to
	// "generatedmaps/<FloorName>_<Seed>.map". Returns true on success.
	bool RequestNewMap(const char *pFloorName, int Seed);

	// the kind of a floor, or false when the floor is unknown. Used by the
	// server to decide whether a map should be generated at all.
	bool GetFloorKind(const char *pFloorName, EWorldKind *pKind) const;
};

#endif // ENGINE_SERVER_MAPGEN_H
