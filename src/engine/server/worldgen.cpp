/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>

#include <engine/shared/jsonparser.h>

#include <game/mapitems.h>

#include "mapgen.h"
#include "noise.h"
#include "worldgen.h"

// The euclidean world generator, in order:
//
//   1. a 2D fractal heightmap defines the ground line of every column
//   2. columns are filled: air above the line, solid below
//   3. 3D fractal noise carves caves, kept away from the surface
//   4. a second 3D field paints ore veins and records their resources
//   5. the floor is sealed
//   6. surface openings are carved and turned into exits
//
// Everything is a deterministic function of the floor config and the seed.

CGeneratedWorld::CGeneratedWorld()
{
	m_Surface.clear();
	m_lLayers.clear();
	m_lResources.clear();
	m_lEntrances.clear();
}

CGeneratedWorld::~CGeneratedWorld()
{
	for(int i = 0; i < m_lLayers.size(); i++)
		delete[] m_lLayers[i].m_pTiles;
}

CGeneratedWorld::CLayer *CGeneratedWorld::AddLayer(const char *pName, int Width, int Height, bool IsGameLayer)
{
	CLayer &Layer = m_lLayers.emplace();
	str_copy(Layer.m_aName, pName, sizeof(Layer.m_aName));
	Layer.m_Width = Width;
	Layer.m_Height = Height;
	Layer.m_ParallaxX = 100;
	Layer.m_ParallaxY = 100;
	Layer.m_Flags = IsGameLayer ? TILESLAYERFLAG_GAME : 0;
	Layer.m_IsGameLayer = IsGameLayer;
	Layer.m_pImageName = 0;
	Layer.m_pTiles = new CTile[Width * Height];
	mem_zero(Layer.m_pTiles, sizeof(CTile) * Width * Height);
	return &Layer;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static noise::CFractalParams ToNoiseParams(const CInstruction::CNoiseConfig &rConfig)
{
	noise::CFractalParams Params;
	Params.m_Octaves = rConfig.m_Octaves;
	Params.m_Frequency = rConfig.m_Frequency;
	Params.m_Lacunarity = rConfig.m_Lacunarity;
	Params.m_Persistence = rConfig.m_Persistence;
	Params.m_Amplitude = rConfig.m_Amplitude;
	return Params;
}

// The tile a generated solid cell carries before the automapper runs. It matches
// the shipped rule sets' "basetile", so the generator only decides solid vs air
// and lets the rules pick the autotile variant.
enum
{
	GENERATED_SOLID = 1,
};

// places a solid cell, keeping the tile flags clean
static void SetSolid(CTile *pTile)
{
	pTile->m_Index = GENERATED_SOLID;
	pTile->m_Flags = 0;
}

static void SetAir(CTile *pTile)
{
	pTile->m_Index = TILE_AIR;
	pTile->m_Flags = 0;
}

// hashes a tile coordinate into a stable pseudo random value, used for the
// small irregularities of the terrain (noise would be wasteful here)
static unsigned TileHash(int Seed, int X, int Y)
{
	unsigned h = (unsigned) Seed * 0x9E3779B1u;
	h ^= (unsigned) X * 0x85EBCA6Bu;
	h ^= (unsigned) Y * 0xC2B2AE35u;
	h ^= h >> 15;
	h *= 0x2545F491u;
	h ^= h >> 13;
	return h;
}

static int RollRange(unsigned Hash, int Min, int Max)
{
	if(Max <= Min)
		return Min;
	return Min + (int) (Hash % (unsigned) (Max - Min + 1));
}

// ---------------------------------------------------------------------------
// terrain
// ---------------------------------------------------------------------------

// Builds the surface height of every column.
//
// The world is a cylinder, so the field is sampled periodically: that makes the
// seam continuous, but not flat, and the autotiler still draws a corner tile
// where the slope starts. The outermost columns are therefore pinned to a common
// seam height and the ground eases back to its natural shape over m_WrapBlend
// columns. Pinning only the outer couple would trade the corner for a step a few
// tiles in, which is worse.
static void GenerateHeightmap(const CInstruction::CGeneration &rGen, int Seed, array<int> &lSurface)
{
	const noise::CFractalParams SurfaceParams = ToNoiseParams(rGen.m_Surface);

	// a second, much lower frequency field warps the height so the world gets
	// continents instead of a uniform ripple
	const noise::CFractalParams WarpParams = ToNoiseParams(rGen.m_Surface);
	noise::CFractalParams LowWarp = WarpParams;
	LowWarp.m_Frequency = WarpParams.m_Frequency * 0.25f;
	LowWarp.m_Octaves = maximum(1, WarpParams.m_Octaves - 2);

	const float Period = (float) rGen.m_Width;

	lSurface.clear();
	lSurface.hint_size(rGen.m_Width);
	for(int x = 0; x < rGen.m_Width; x++)
	{
		// sampled on the cylindrical axis, so the field runs continuously
		// through the wrap and the two edges really are the same place
		const float Base = noise::Fractal2DPeriodic((unsigned) (Seed + rGen.m_Surface.m_SeedOffset), SurfaceParams,
			(float) x, 0.0f, Period);
		const float Warp = noise::Fractal2DPeriodic((unsigned) (Seed * 3u + 77u + rGen.m_Surface.m_SeedOffset), LowWarp,
			(float) x, 1000.0f, Period);
		const float Height = Base + Warp * 0.5f;

		int Y = rGen.m_SurfaceLevel - (int) (Height * rGen.m_SurfaceAmplitude);
		// keep the ground line inside the map, clear of the sealed top and bottom
		// leave room for the open sky above and the sealed floor below
		Y = clamp(Y, 16, rGen.m_Height - rGen.m_EdgeWall - 8);
		lSurface.add(Y);
	}

	// -----------------------------------------------------------------
	// flatten the seam
	// -----------------------------------------------------------------
	const int Width = rGen.m_Width;
	const int Band = rGen.m_WrapMargin; // columns pinned flat to the seam height
	const int Blend = rGen.m_WrapBlend; // ... and the window the ground eases back over

	// the parser keeps Blend >= Band, so only a disabled or absurd window is
	// rejected here. Band may be 0: the two edge columns are still pinned
	// exactly, they just have no flat band behind them.
	if(Blend <= 0 || Width <= 2 * Blend)
		return;

	// the height the two edges meet at: half way between the last column and the
	// first, so neither edge is favoured and the seam has no step at all
	const int Seam = (lSurface[0] + lSurface[Width - 1]) / 2;

	for(int x = 0; x < Width; x++)
	{
		// the outer band is exactly flat, which keeps the autotiler from
		// stamping a corner onto the border column
		if(x < Band || x >= Width - Band)
		{
			lSurface[x] = Seam;
			continue;
		}

		// distance to the seam, measured to the tile edge so both sides match
		const float Distance = minimum((float) x + 0.5f, (float) Width - (float) x - 0.5f);
		float T = (Distance - 0.5f) / maximum(1.0f, (float) Blend - 0.5f);
		T = clamp(T, 0.0f, 1.0f);
		// smoothstep, so the ground leaves the band without a crease at the
		// far end of the window
		const float W = T * T * (3.0f - 2.0f * T);

		lSurface[x] = (int) ((float) Seam + ((float) lSurface[x] - (float) Seam) * W + 0.5f);
	}
}

// Biomes used to pick a tileset per column here. The automapper does that now
// via "main_ruleset", which is closer to the original editor and lets a floor
// mix tilesets through rules instead of a hardcoded table.


static void FillTerrain(const CInstruction::CGeneration &rGen, int Seed, CGeneratedWorld *pWorld)
{
	CGeneratedWorld::CLayer *pMain = pWorld->AddLayer("Main", rGen.m_Width, rGen.m_Height, false);
	CGeneratedWorld::CLayer *pGame = pWorld->AddLayer("Game", rGen.m_Width, rGen.m_Height, true);
	// the entrance frames live on their own layer; see LAYER_UNHOOKABLE. It is
	// added here so every later step can address it by index.
	CGeneratedWorld::CLayer *pUnhookable = pWorld->AddLayer("Unhookable", rGen.m_Width, rGen.m_Height, false);

	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;

	// Everything below the ground line is solid, everything above is air; caves
	// are carved out afterwards. The seed only decides where the line is, which
	// is what makes two instances of a floor differ.
	for(int x = 0; x < Width; x++)
	{
		const int SurfaceY = pWorld->m_Surface[x];

		for(int y = SurfaceY; y < Height; y++)
			SetSolid(&pMain->m_pTiles[y * Width + x]);

		// the game layer stays empty; entities are added by the features below
		(void) pGame;
		(void) pUnhookable;
	}
}

// Carves the open sky above the ground line. Separate from FillTerrain so the
// sparse "solid" loops above stay simple.
static void CarveSky(const CInstruction::CGeneration &rGen, CGeneratedWorld *pWorld)
{
	if(!pWorld->m_lLayers.size())
		return;
	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	const int Width = rGen.m_Width;

	for(int x = 0; x < Width; x++)
		for(int y = 0; y < pWorld->m_Surface[x]; y++)
			SetAir(&pMain->m_pTiles[y * Width + x]);
}

// ---------------------------------------------------------------------------
// caves
// ---------------------------------------------------------------------------

// Carves the cave system.
//
// The caves come from a 3D field sampled over the real map volume, not a 2D
// slice: a field at constant z degenerates into sheets, which carves slots
// instead of tunnels. It is sampled coarser than one sample per tile, so the
// result is caverns rather than speckle.
static void CarveCaves(const CInstruction::CGeneration &rGen, int Seed, CGeneratedWorld *pWorld)
{
	if(!rGen.m_Caves.m_Enabled || !pWorld->m_lLayers.size())
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;
	const noise::CFractalParams Params = ToNoiseParams(rGen.m_Caves.m_Noise);
	const float Threshold = rGen.m_Caves.m_Threshold;
	const int MaxY = Height - rGen.m_EdgeWall - 1;

	// the cave field is evaluated once per tile, then the thresholded result is
	// cleaned up, so a tile can be removed or re-filled based on its neighbours
	unsigned char *pCave = new unsigned char[Width * Height];
	mem_zero(pCave, sizeof(unsigned char) * Width * Height);

	for(int x = 0; x < Width; x++)
	{
		// the roof above the ground line always stays solid
		const int StartY = maximum(rGen.m_EdgeWall + 1, pWorld->m_Surface[x] + rGen.m_Caves.m_MinDepth);
		// the field is sampled periodically, so a cave that reaches the left
		// edge continues on the right edge instead of being cut off
		for(int y = StartY; y < MaxY; y++)
		{
			const float Value = noise::Fractal3DPeriodic((unsigned) (Seed + rGen.m_Caves.m_Noise.m_SeedOffset), Params,
				(float) x, (float) y, (float) (x + y) * 0.5f, (float) Width);
			if(Value > Threshold)
				pCave[y * Width + x] = 1;
		}
	}

	// Cleanup: drop single tiles and one-wide tendrils. Without it the threshold
	// boundary leaves a rash of lone air tiles that reads as perforated rock.
	// The lookups wrap horizontally, or the cleanup would eat a notch at the
	// seam that the wrapped field just created.
	unsigned char *pClean = new unsigned char[Width * Height];
	for(int Pass = 0; Pass < 2; Pass++)
	{
		mem_copy(pClean, pCave, sizeof(unsigned char) * Width * Height);
		for(int x = 0; x < Width; x++)
		{
			for(int y = rGen.m_EdgeWall + 1; y < MaxY; y++)
			{
				const int Idx = y * Width + x;
				if(!pCave[Idx])
					continue;

				// count the 8 neighbours that are also cave
				int Neighbours = 0;
				for(int dy = -1; dy <= 1; dy++)
				{
					const int Ny = y + dy;
					if(Ny < 0 || Ny >= Height)
						continue;
					for(int dx = -1; dx <= 1; dx++)
					{
						if(!dx && !dy)
							continue;
						const int Nx = (x + dx + Width) % Width; // wrap around
						if(pCave[Ny * Width + Nx])
							Neighbours++;
					}
				}

				// 0 or 1 neighbours means a speck or a one-wide tendril
				if(Neighbours <= 1)
					pClean[Idx] = 0;
			}
		}
		mem_copy(pCave, pClean, sizeof(unsigned char) * Width * Height);
	}

	int Carved = 0;
	for(int x = 0; x < Width; x++)
	{
		const int StartY = maximum(rGen.m_EdgeWall + 1, pWorld->m_Surface[x] + rGen.m_Caves.m_MinDepth);
		for(int y = StartY; y < MaxY; y++)
		{
			if(pCave[y * Width + x])
			{
				pMain->m_pTiles[y * Width + x].m_Index = TILE_AIR;
				pMain->m_pTiles[y * Width + x].m_Flags = 0;
				Carved++;
			}
		}
	}

	delete[] pCave;
	delete[] pClean;
	dbg_msg("worldgen", "carved %d cave tile(s)", Carved);
}

// ---------------------------------------------------------------------------
// ores
// ---------------------------------------------------------------------------

static void GenerateOres(const CInstruction::CGeneration &rGen, int Seed, CGeneratedWorld *pWorld)
{
	if(!rGen.m_Ores.size() || !pWorld->m_lLayers.size())
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;

	for(int o = 0; o < rGen.m_Ores.size(); o++)
	{
		const CInstruction::COreConfig &Ore = rGen.m_Ores[o];
		const noise::CFractalParams Params = ToNoiseParams(Ore.m_Noise);
		int VeinCount = 0;

		for(int x = 0; x < Width; x++)
		{
			for(int y = rGen.m_EdgeWall; y < Height - rGen.m_EdgeWall; y++)
			{
				CTile *pTile = &pMain->m_pTiles[y * Width + x];
				if(pTile->m_Index == TILE_AIR)
					continue;
				const int Depth = y - pWorld->m_Surface[x];
				if(Depth < Ore.m_MinDepth)
					continue;

				// the third axis varies with position, or the vein would be a
				// flat sheet extruded through the rock rather than a blob
				const float Value = noise::Fractal3D((unsigned) (Seed + Ore.m_Noise.m_SeedOffset), Params,
					(float) x, (float) y, (float) (x - y) * 0.5f + (float) o * 100.0f);
				if(Value <= Ore.m_Threshold)
					continue;

				// the tile stays solid; this only records the resource, so the
				// automapper still picks the rock variant
				VeinCount++;
				for(int p = 0; p < Ore.m_Probes; p++)
				{
					const unsigned Hash = TileHash(Seed + o * 7919, x * 31 + p, y);
					if(Ore.m_Proba < 100 && (int) (Hash % 100u) >= Ore.m_Proba)
						continue;

					CGeneratedWorld::CResource &Resource = pWorld->m_lResources.emplace();
					str_copy(Resource.m_aResId, Ore.m_aResId, sizeof(Resource.m_aResId));
					str_copy(Resource.m_aDisplay, Ore.m_aDisplay, sizeof(Resource.m_aDisplay));
					Resource.m_X = x;
					Resource.m_Y = y;
					Resource.m_Hardness = RollRange(Hash >> 8, Ore.m_HardnessMin, Ore.m_HardnessMax);
					Resource.m_RespawnTime = RollRange(Hash >> 16, Ore.m_RespawnTimeMin, Ore.m_RespawnTimeMax);
				}
			}
		}
		dbg_msg("worldgen", "ore '%s': %d vein tile(s)", Ore.m_aResId, VeinCount);
	}
}

// ---------------------------------------------------------------------------
// border and entrances
// ---------------------------------------------------------------------------

// Seals the floor. The top stays open for the sky, and the left and right edges
// are open because they are the same place on a cylinder.
static void SealBorder(const CInstruction::CGeneration &rGen, CGeneratedWorld *pWorld)
{
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;
	const int T = rGen.m_EdgeWall;
	if(T <= 0 || !pWorld->m_lLayers.size())
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	for(int x = 0; x < Width; x++)
	{
		for(int y = Height - T; y < Height; y++)
		{
			// solid rock, so nothing can fall out of the world; the automapper
			// styles it like any other rock
			SetSolid(&pMain->m_pTiles[y * Width + x]);
		}
	}
}

// Builds the visible cave mouths that lead to other floors.
//
// An entrance is a real structure cut into the terrain, not an invisible
// coordinate box: a shallow framed recess whose floor is the trigger. Positions
// come from the terrain rather than an even split of the width, so the opening
// sits on flat ground and reads as part of the landscape.
static void CarveSurfaceEntrances(const CInstruction::CGeneration &rGen, int Seed, CGeneratedWorld *pWorld, const array<char> &lDefaultEntrances)
{
	const CInstruction::CSurfaceEntranceConfig &Config = rGen.m_SurfaceEntrances;
	if(!Config.m_Enabled || Config.m_Count <= 0 || pWorld->m_lLayers.size() < 3)
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;

	// The exits this floor was configured with. The names are copied out of the
	// parsed json because the parser owns (and frees) its tree.
	struct CTargetName
	{
		char m_aName[64];
	};
	array<CTargetName> lTargetNames;
	{
		CJsonParser Parser;
		json_value *pJson = 0;
		if(lDefaultEntrances.size())
		{
			char *pStored = new char[lDefaultEntrances.size() + 1];
			mem_copy(pStored, lDefaultEntrances.base_ptr(), lDefaultEntrances.size());
			pStored[lDefaultEntrances.size()] = 0;
			pJson = Parser.ParseString(pStored, "entrances");
			delete[] pStored;
		}
		if(pJson && pJson->type == json_array)
		{
			lTargetNames.hint_size((int) pJson->u.array.length);
			for(unsigned i = 0; i < pJson->u.array.length; i++)
			{
				const json_value &rEntrance = (*pJson)[i];
				const json_value &rTarget = rEntrance["entrance"];
				if(rTarget.type != json_string)
					continue;
				CTargetName &Target = lTargetNames.emplace();
				str_copy(Target.m_aName, rTarget.u.string.ptr, sizeof(Target.m_aName));
			}
		}
	}

	if(!lTargetNames.size())
	{
		dbg_msg("worldgen", "floor '%s' wants surface entrances but lists no exit targets", rGen.m_aTileset);
		return;
	}

	// How suitable a column is: needs headroom above the ground line, solid rock
	// below, and ground flat enough that the opening does not look glued on.
	auto ScoreColumn = [&](int X) -> int
	{
		const int Half = Config.m_Width / 2;
		const int SurfaceY = pWorld->m_Surface[X];
		int Score = 0;

		// flatness: compare the ground line across the mouth
		for(int dx = -Half; dx <= Half; dx++)
		{
			const int Nx = (X + dx + Width) % Width;
			const int Delta = abs(pWorld->m_Surface[Nx] - SurfaceY);
			if(Delta > 3)
				return -1; // too steep, the frame would float
			Score += 3 - Delta;
		}

		// the chamber needs solid rock underneath and clear air above
		const int ChamberTop = SurfaceY + Config.m_Clearance;
		const int ChamberBottom = ChamberTop + Config.m_Height;
		if(ChamberBottom + Config.m_Frame >= Height - rGen.m_EdgeWall)
			return -1; // no room before the sealed floor
		for(int y = SurfaceY - Config.m_Height - Config.m_Clearance; y < SurfaceY; y++)
		{
			if(y < rGen.m_EdgeWall)
				return -1; // no room before the sealed ceiling
			if(pMain->m_pTiles[y * Width + X].m_Index != TILE_AIR)
				return -1; // the approach is blocked
		}
		for(int dy = 0; dy <= Config.m_Height + Config.m_Frame; dy++)
		{
			for(int dx = -Half - Config.m_Frame; dx <= Half + Config.m_Frame; dx++)
			{
				const int Nx = (X + dx + Width) % Width;
				if(pMain->m_pTiles[(ChamberTop + dy) * Width + Nx].m_Index == TILE_AIR)
					return -1; // the rock is already open, the frame would leak
			}
		}

		// a framed recess sunk into the ground, so beyond flatness it only
		// needs headroom above and rock below
		const int FloorY = SurfaceY + Config.m_Clearance;
		const int CeilingY = FloorY - Config.m_Height;
		if(CeilingY - Config.m_Frame < rGen.m_EdgeWall + 1)
			return -1; // no headroom before the sealed ceiling

		// prefer a hollow: a cave mouth in a dip reads better than on a ridge
		Score += (rGen.m_SurfaceLevel - SurfaceY);

		// stable per column jitter, so seeds differ without picking bad spots
		Score += (int) (TileHash(Seed, X, 90210) % 7u);
		return Score;
	};

	// pick the best column in each of Count bands, keeping them separated
	const int Margin = maximum(Config.m_Margin, Config.m_Width + Config.m_Frame + 2);
	const int BandWidth = maximum(1, (Width - 2 * Margin) / Config.m_Count);
	array<int> lChosen;
	for(int b = 0; b < Config.m_Count; b++)
	{
		int BestX = -1, BestScore = -1;
		const int BandStart = Margin + b * BandWidth;
		const int BandEnd = minimum(Width - Margin, BandStart + BandWidth);
		for(int x = BandStart; x < BandEnd; x++)
		{
			// keep the openings apart
			bool TooClose = false;
			for(int c = 0; c < lChosen.size(); c++)
				if(abs(lChosen[c] - x) < Config.m_MinSeparation)
					TooClose = true;
			if(TooClose)
				continue;

			const int Score = ScoreColumn(x);
			if(Score > BestScore)
			{
				BestScore = Score;
				BestX = x;
			}
		}
		if(BestX >= 0)
			lChosen.add(BestX);
	}

	if(!lChosen.size())
	{
		dbg_msg("worldgen", "no suitable ground for a surface entrance was found");
		return;
	}

	for(int i = 0; i < lChosen.size(); i++)
	{
		const int CenterX = lChosen[i];
		const int SurfaceY = pWorld->m_Surface[CenterX];
		const int Half = Config.m_Width / 2;
		const int Left = CenterX - Half;
		const int Right = CenterX + Half;
		// the recess: its floor sits a little below the ground line, its ceiling
		// a few tiles above, so it is a shallow framed step rather than a shaft
		const int FloorY = SurfaceY + Config.m_Clearance;
		const int CeilingY = FloorY - Config.m_Height;

		// Cut the recess. Its floor sits just below the surrounding ground line,
		// so it reads as a step down into a built entrance rather than a shaft
		// sunk into the landscape.
		for(int x = Left; x <= Right; x++)
		{
			const int Nx = (x + Width) % Width;
			for(int y = CeilingY; y <= FloorY; y++)
			{
				if(y < rGen.m_EdgeWall || y >= Height - rGen.m_EdgeWall)
					continue;
				SetAir(&pMain->m_pTiles[y * Width + Nx]);
			}
			// solid, so the player stands in the recess
			if(FloorY + 1 < Height - rGen.m_EdgeWall)
				SetSolid(&pMain->m_pTiles[(FloorY + 1) * Width + Nx]);
		}

		// Frame the recess on the unhookable layer, not the terrain: "unhookable"
		// is collision and the terrain layer is only the look. The game layer
		// reads this layer for its collision.
		//
		// Every cell gets the same single tile. 129 was picked by measuring which
		// index of generic_unhookable repeats with the least visible grid (seam
		// error 0.45 of 255); the sheet has no borderless fill, so the grey
		// variants all grid visibly.
		{
			const int FrameTile = 129;
			CGeneratedWorld::CLayer *pFrame = &pWorld->m_lLayers[CGeneratedWorld::LAYER_UNHOOKABLE];
			for(int x = Left - Config.m_Frame; x <= Right + Config.m_Frame; x++)
			{
				const int Nx = (x + Width) % Width;
				for(int y = CeilingY - Config.m_Frame; y <= FloorY + Config.m_Frame + 1; y++)
				{
					if(y < rGen.m_EdgeWall || y >= Height - rGen.m_EdgeWall)
						continue;
					const bool InRecess = x >= Left && x <= Right && y >= CeilingY && y <= FloorY;
					if(InRecess)
						continue;
					// the frame hugs the recess, so it is drawn wherever the
					// terrain used to be solid
					if(pMain->m_pTiles[y * Width + Nx].m_Index == TILE_AIR)
						continue;
					pFrame->m_pTiles[y * Width + Nx].m_Index = FrameTile;
					pFrame->m_pTiles[y * Width + Nx].m_Flags = 0;
				}
			}
		}

		// The trigger is the floor of the recess, not an invisible box in the
		// open: the player walks down into something they can see.
		CGeneratedWorld::CEntrance &Entrance = pWorld->m_lEntrances.emplace();
		Entrance.m_StartX = Left;
		Entrance.m_StartY = FloorY;
		Entrance.m_EndX = Right;
		Entrance.m_EndY = FloorY;
		str_copy(Entrance.m_aTarget, lTargetNames[i % lTargetNames.size()].m_aName, sizeof(Entrance.m_aTarget));

		dbg_msg("worldgen", "surface entrance %d: recess x %d..%d, floor y %d -> %s",
			i + 1, Left, Right, FloorY, Entrance.m_aTarget);
	}
}

// Places team neutral spawn points on the game layer, on the open tile just
// above the ground line. Without them CanSpawn fails and nobody can play.
//
// A column is only eligible when the tile above its ground line is clear, which
// keeps spawns out of the inside of a hill.
static void PlaceSpawns(const CInstruction::CGeneration &rGen, int Seed, CGeneratedWorld *pWorld)
{
	if(pWorld->m_lLayers.size() < 3)
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	CGeneratedWorld::CLayer *pGame = &pWorld->m_lLayers[CGeneratedWorld::LAYER_GAME];
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;
	const int Edge = rGen.m_EdgeWall;

	// A spawn may never sit on an entrance: arriving in a world uses one of
	// these, so a spawn that overlaps a trigger teleports the player straight
	// back out and the two worlds bounce them forever. The check covers the
	// whole recess plus its frame, since the player lands a tile or two above
	// the trigger and falls onto it.
	auto OnEntrance = [&](int X) -> bool
	{
		for(int i = 0; i < pWorld->m_lEntrances.size(); i++)
		{
			const CGeneratedWorld::CEntrance &Entrance = pWorld->m_lEntrances[i];
			// generous on both sides: the frame and a landing tile of slack
			const int Pad = rGen.m_SurfaceEntrances.m_Frame + 2;
			if(X >= Entrance.m_StartX - Pad && X <= Entrance.m_EndX + Pad)
				return true;
		}
		return false;
	};

	// one candidate per bucket, so spawns cover the width instead of clustering
	// wherever the ground happens to be flat
	const int BucketSize = 48;
	int Placed = 0;
	for(int StartX = Edge + 8; StartX < Width - Edge - 8; StartX += BucketSize)
	{
		const int EndX = minimum(StartX + BucketSize, Width - Edge - 8);
		for(int x = StartX; x < EndX; x++)
		{
			// m_Surface is the first *solid* row of the column, so the open tile
			// the spawn stands on is the one just above it
			const int GroundY = pWorld->m_Surface[x];
			const int StandY = GroundY - 1;
			if(GroundY <= Edge + 5 || GroundY >= Height - Edge - 1)
				continue;
			if(OnEntrance(x))
				continue;
			// the spawn needs the tile it stands on and the one above it clear,
			// so the character is not spawned inside rock
			if(pMain->m_pTiles[StandY * Width + x].m_Index != TILE_AIR)
				continue;
			if(pMain->m_pTiles[(StandY - 1) * Width + x].m_Index != TILE_AIR)
				continue;

			CTile *pTile = &pGame->m_pTiles[StandY * Width + x];
			pTile->m_Index = ENTITY_OFFSET + ENTITY_SPAWN;
			pTile->m_Flags = 0;
			Placed++;
			break;
		}
	}

	dbg_msg("worldgen", "placed %d spawn point(s)", Placed);
}

// Builds the collision layer from the finished terrain. This is the layer the
// engine reads; Main is only decoration, and an empty game layer means no
// collision at all.
//
// Runs last, after the caves and entrances, so collision matches what the player
// sees.
static void BuildGameLayer(const CInstruction::CGeneration &rGen, CGeneratedWorld *pWorld)
{
	if(pWorld->m_lLayers.size() < 3)
		return;

	const CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	CGeneratedWorld::CLayer *pGame = &pWorld->m_lLayers[CGeneratedWorld::LAYER_GAME];
	const CGeneratedWorld::CLayer *pFrame = &pWorld->m_lLayers[CGeneratedWorld::LAYER_UNHOOKABLE];
	const int Width = rGen.m_Width;
	const int Height = rGen.m_Height;

	for(int x = 0; x < Width; x++)
	{
		for(int y = 0; y < Height; y++)
		{
			CTile *pGameTile = &pGame->m_pTiles[y * Width + x];
			// spawn points were placed on the game layer already; keep them
			if(pGameTile->m_Index >= ENTITY_OFFSET)
				continue;

			const int Terrain = pMain->m_pTiles[y * Width + x].m_Index;
			if(Terrain == TILE_AIR)
			{
				pGameTile->m_Index = TILE_AIR;
				pGameTile->m_Flags = 0;
				continue;
			}

			// Only the floor is sealed. The top stays open for the sky and the
			// edges are solid only where the terrain is, being the wrap seam.
			const bool IsFloor = y >= Height - rGen.m_EdgeWall;

			// Any solid tile on the frame layer is unhookable collision. Tested
			// for solidity, not a fixed index, since the index is whatever
			// variant the frame used.
			const bool IsFrame = pFrame->m_pTiles[y * Width + x].m_Index != TILE_AIR;

			pGameTile->m_Index = (IsFrame || IsFloor) ? TILE_UNHOOKABLE : TILE_HOOKABLE;
			pGameTile->m_Flags = 0;
		}
	}
}

// ---------------------------------------------------------------------------

CGeneratedWorld *GenerateEuclideanWorld(const CInstruction *pInstruction, int Seed)
{
	if(!pInstruction)
		return 0;

	const CInstruction::CGeneration &Gen = pInstruction->m_Generation;
	if(!Gen.m_Enabled)
	{
		dbg_msg("worldgen", "floor '%s' has no generation settings", pInstruction->m_aFloorName);
		return 0;
	}
	if(Gen.m_Width <= 0 || Gen.m_Height <= 0)
	{
		dbg_msg("worldgen", "floor '%s' has an invalid size %dx%d", pInstruction->m_aFloorName, Gen.m_Width, Gen.m_Height);
		return 0;
	}

	CGeneratedWorld *pWorld = new CGeneratedWorld();

	// the surface first: every later step needs the ground line
	pWorld->m_Surface.clear();
	GenerateHeightmap(Gen, Seed, pWorld->m_Surface);

	FillTerrain(Gen, Seed, pWorld);
	CarveSky(Gen, pWorld);
	CarveCaves(Gen, Seed, pWorld);
	GenerateOres(Gen, Seed, pWorld);
	SealBorder(Gen, pWorld);

	// The automapper runs in the caller, between this and the entrances: it
	// resets every solid tile to its basetile, so anything marked earlier would
	// be wiped. The entrances are therefore built afterwards.

	dbg_msg("worldgen", "generated euclidean floor '%s' (%dx%d, %d resource(s))",
		pInstruction->m_aFloorName, Gen.m_Width, Gen.m_Height, pWorld->m_lResources.size());
	return pWorld;
}

void BuildWorldEntrances(const CInstruction *pInstruction, int Seed, CGeneratedWorld *pWorld)
{
	if(!pInstruction || !pWorld)
		return;

	const CInstruction::CGeneration &Gen = pInstruction->m_Generation;

	CarveSurfaceEntrances(Gen, Seed, pWorld, pInstruction->m_DefaultEntrances);

	// No automap pass over the frame: its cells already hold exact tiles, and a
	// rule set would replace them with the per-tile fragments this avoids.

	// spawns go on the game layer before it is filled from the terrain, so
	// BuildGameLayer can preserve them
	PlaceSpawns(Gen, Seed, pWorld);

	// collision lives in the game layer: derive it from the finished terrain and
	// the frame just marked
	BuildGameLayer(Gen, pWorld);
}
