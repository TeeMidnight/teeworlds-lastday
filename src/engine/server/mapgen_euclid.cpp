/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Euclidean floor generation: the procedural terrain, its cave wall, its
// background, the vegetation doodads and the layer assembly.
#include <base/math.h>
#include <base/system.h>
#include <base/tl/stream.h>
#include <base/vmath.h>
#include <engine/map/generatedmaps.h>
#include <engine/map/mapcreator.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include "automap.h"
#include "mapgen.h"
#include "mapgen_internal.h"
#include "noise.h"
#include "worldgen.h"

#include <climits>

// ---------------------------------------------------------------------------
// surface resources
// ---------------------------------------------------------------------------

// A non euclidean floor places its configured "resources" on the red team spawn
// points of its base map. A euclidean floor has no spawn points to hang them on,
// so its "resources" list is scattered over the open ground line instead: one
// deterministic roll per eligible column, per config. The same worlds.json
// format therefore works for both kinds, and this is what makes healing plants
// reachable in the generated surface floors.
static void GenerateSurfaceResources(const CInstruction *pInstruction, int Seed, CGeneratedWorld *pWorld)
{
	if(!pInstruction->m_ResourcesJson.size() || !pWorld->m_lLayers.size())
		return;

	const int StoredLen = pInstruction->m_ResourcesJson.size();
	char *pStored = new char[StoredLen + 1];
	if(StoredLen > 0)
		mem_copy(pStored, pInstruction->m_ResourcesJson.base_ptr(), StoredLen);
	pStored[StoredLen] = 0;
	array<CResourceConfig> lConfigs;
	ParseResources(pStored, lConfigs);
	delete[] pStored;
	if(!lConfigs.size())
		return;

	CGeneratedWorld::CLayer *pMain = &pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
	const int Width = pMain->m_Width;
	const int Height = pMain->m_Height;
	const CWorldRing Ring = WorldRing(pInstruction, Seed);
	int Placed = 0;

	for(int c = 0; c < lConfigs.size(); c++)
	{
		const CResourceConfig &Config = lConfigs[c];
		for(int x = 0; x < Width && x < pWorld->m_Surface.size(); x++)
		{
			// the open tile just above the ground line
			const int StandY = pWorld->m_Surface[x] - 1;
			if(StandY <= 0 || StandY >= Height - 1)
				continue;
			// it has to stand on solid ground with clear air above and in front,
			// so the node is neither buried in a hill nor floating over a cave
			if(pMain->m_pTiles[StandY * Width + x].m_Index != TILE_AIR)
				continue;
			if(pMain->m_pTiles[(StandY - 1) * Width + x].m_Index != TILE_AIR)
				continue;
			if(pMain->m_pTiles[(StandY + 1) * Width + x].m_Index == TILE_AIR)
				continue;

			const unsigned Hash = DeterministicHash((int) Ring.m_NoiseSeed + c * 104729 + 17, Ring.m_OffsetX + x);
			if(HashToUnitFloat(Hash) >= Config.m_Proba)
				continue;

			int Hardness = Config.m_HardnessMin;
			const int HardRange = Config.m_HardnessMax - Config.m_HardnessMin + 1;
			if(HardRange > 0)
				Hardness += (int) ((Hash >> 8) % (unsigned) HardRange);
			int RespawnTime = Config.m_RespawnTimeMin;
			const int RespawnRange = Config.m_RespawnTimeMax - Config.m_RespawnTimeMin + 1;
			if(RespawnRange > 0)
				RespawnTime += (int) ((Hash >> 16) % (unsigned) RespawnRange);

			// Ring: the left shared strip belongs to the previous instance, so a
			// node there is not spawned twice
			if(Ring.m_Enabled && x < pInstruction->m_Generation.m_Ring.m_SharedMargin)
				continue;

			CGeneratedWorld::CResource &Resource = pWorld->m_lResources.emplace();
			str_copy(Resource.m_aResId, Config.m_aResId, sizeof(Resource.m_aResId));
			str_copy(Resource.m_aDisplay, Config.m_aDisplay, sizeof(Resource.m_aDisplay));
			Resource.m_X = x;
			Resource.m_Y = StandY;
			Resource.m_Hardness = Hardness;
			Resource.m_RespawnTime = RespawnTime;
			Placed++;
		}
	}

	MAPGEN_LOG("worldgen", "surface resources: %d node(s)", Placed);
}

// ---------------------------------------------------------------------------
// euclidean structure pasting
//
// A euclidean floor has no authored base map, so it cannot carry the flag
// stand anchors a static floor uses. Instead the structures listed in its
// "structs" block are scattered along the generated ground line: each one is
// dropped onto a flat stretch of surface, clear of the entrances, the spawn
// points, the ring's shared strip and the other structures. Their tiles merge
// into the terrain/game layers and their world-space quads merge into a
// world-parallax group of the generated map, which is what lets a structure be
// drawn from coloured quads instead of a baked tileset.
// ---------------------------------------------------------------------------

struct SImageRequest
{
	char m_aName[64];
	int m_Width;
	int m_Height;
	bool m_Optional;
};

// One tile layer of a struct map that is merged into a layer of the generated
// map. Only layers of the struct's "Template" group are collected.
struct SEuclidStructTemplateLayer
{
	char m_aName[32];
	int m_Width;
	int m_Height;
	CTile *m_pTiles;
};

// One quad layer of a struct map, restricted to world-space groups (parallax
// 100). Background groups (sky, parallax 0) are deliberately not collected:
// pasting a screen sized sky quad over a generated world would hide it.
struct SEuclidStructQuadLayer
{
	char m_aImage[32];
	int m_OffsetX; // group offset, map pixels
	int m_OffsetY;
	array<CQuad> m_lQuads;
};

// A struct map decoded into memory. The caller owns the tile buffers and must
// call FreeEuclidStructMap.
struct SEuclidStructMap
{
	CTile *m_pGameTiles;
	int m_GameWidth;
	int m_GameHeight;
	int m_RedX, m_RedY, m_BlueX, m_BlueY;
	int m_MinX, m_MinY, m_MaxX, m_MaxY;
	array<SEuclidStructTemplateLayer> m_lTemplates;
	array<SEuclidStructQuadLayer> m_lQuadLayers;

	// the paste region (struct coordinates), derived from the flag stands
	int m_RegionX1, m_RegionY1, m_RegionX2, m_RegionY2;
	int m_BottomLeftX, m_BottomLeftY;
	int m_TopRightX, m_TopRightY;
	int m_StructLeftX, m_StructTopY;
	int m_ContentWidth;
	int m_ContentHeight;

	// The columns (struct coordinates) where the structure actually touches
	// the ground: the solid tiles of its bottom two content rows. Placement
	// requires level ground under exactly these, so a gate or tower rests on
	// its pillars/legs instead of floating over a dip between them. -1 when the
	// structure has no collision at its foot (art only); the configured
	// flat_width is used then.
	int m_ContactMinX;
	int m_ContactMaxX;
};

struct SEuclidPlacementRect
{
	int m_X0, m_Y0, m_X1, m_Y1;
};

static void FreeEuclidStructMap(SEuclidStructMap &rMap)
{
	delete[] rMap.m_pGameTiles;
	rMap.m_pGameTiles = nullptr;
	for(int i = 0; i < rMap.m_lTemplates.size(); i++)
		delete[] rMap.m_lTemplates[i].m_pTiles;
	rMap.m_lTemplates.clear();
	rMap.m_lQuadLayers.clear();
}

static CCreatorImage *FindRequestedImage(const array<SImageRequest> &lRequests,
	const array<CCreatorImage *> &lCreatorImages, const char *pName)
{
	if(!pName || !pName[0])
		return nullptr;
	for(int i = 0; i < lRequests.size(); i++)
	{
		if(str_comp(lRequests[i].m_aName, pName) == 0)
			return lCreatorImages[i];
	}
	return nullptr;
}

// Decodes a struct map: the game layer, the template tile layers and the
// world-space quad layers. The paste region is the one bounded by the red and
// blue flag stands, exactly like the static (non euclidean) merge; a euclidean
// structure therefore has to carry both flags even though they are consumed by
// the merge and never reach the generated map.
static bool LoadEuclidStructMap(IStorage *pStorage, const char *pBaseMap, bool ConsoleOutput, SEuclidStructMap &rMap)
{
	rMap.m_pGameTiles = nullptr;
	rMap.m_GameWidth = 0;
	rMap.m_GameHeight = 0;
	rMap.m_RedX = rMap.m_RedY = rMap.m_BlueX = rMap.m_BlueY = -1;
	rMap.m_MinX = rMap.m_MinY = 1 << 30;
	rMap.m_MaxX = rMap.m_MaxY = -1;
	rMap.m_RegionX1 = rMap.m_RegionY1 = 0;
	rMap.m_RegionX2 = rMap.m_RegionY2 = -1;
	rMap.m_BottomLeftX = rMap.m_BottomLeftY = 0;
	rMap.m_TopRightX = rMap.m_TopRightY = 0;
	rMap.m_StructLeftX = rMap.m_StructTopY = 0;
	rMap.m_ContentWidth = 0;
	rMap.m_ContentHeight = 0;
	rMap.m_ContactMinX = rMap.m_ContactMaxX = -1;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "maps/struct/%s.map", pBaseMap);

	CDataFileReader File;
	File.SetConsoleOutput(ConsoleOutput);
	if(!File.Open(pStorage, aPath, IStorage::TYPE_ALL))
	{
		MAPGEN_LOG("mapgen", "failed to open the struct map '%s'", aPath);
		return false;
	}

	int GroupsStart, GroupsNum, LayersStart, LayersNum, ImagesStart, ImagesNum;
	File.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
	File.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
	File.GetType(MAPITEMTYPE_IMAGE, &ImagesStart, &ImagesNum);

	// external image names, so a quad layer can be resolved to a creator image
	struct SStructImageName
	{
		char m_aName[32];
	};
	array<SStructImageName> lImageNames;
	for(int i = 0; i < ImagesNum; i++)
	{
		CMapItemImage *pImage = (CMapItemImage *) File.GetItem(ImagesStart + i, 0, 0);
		SStructImageName &Name = lImageNames.emplace();
		Name.m_aName[0] = 0;
		if(pImage && pImage->m_External)
		{
			const char *pImageName = (const char *) File.GetData(pImage->m_ImageName);
			if(pImageName)
				str_copy(Name.m_aName, pImageName, sizeof(Name.m_aName));
		}
	}

	for(int g = 0; g < GroupsNum; g++)
	{
		CMapItemGroup *pGroup = (CMapItemGroup *) File.GetItem(GroupsStart + g, 0, 0);
		if(!pGroup)
			continue;
		char aGroupName[32];
		IntsToStr(pGroup->m_aName, sizeof(pGroup->m_aName) / sizeof(int), aGroupName);
		const bool IsTemplateGroup = str_comp(aGroupName, "Template") == 0;

		// Only world-space quads are structure art. Sky/quads at another
		// parallax belong to the struct's own preview canvas.
		const bool IsWorldGroup = pGroup->m_ParallaxX == 100 && pGroup->m_ParallaxY == 100;

		for(int l = 0; l < pGroup->m_NumLayers; l++)
		{
			const int LayerItem = LayersStart + pGroup->m_StartLayer + l;
			if(LayerItem < 0 || LayerItem >= LayersStart + LayersNum)
				continue;
			CMapItemLayer *pLayer = (CMapItemLayer *) File.GetItem(LayerItem, 0, 0);
			if(!pLayer)
				continue;

			if(pLayer->m_Type == LAYERTYPE_TILES || pLayer->m_Type == LAYERTYPE_GAME)
			{
				CMapItemLayerTilemap *pTilemap = (CMapItemLayerTilemap *) pLayer;
				char aLayerName[32];
				IntsToStr(pTilemap->m_aName, sizeof(pTilemap->m_aName) / sizeof(int), aLayerName);
				CTile *pTiles = LoadTiles(&File, pTilemap->m_Data, pTilemap->m_Width, pTilemap->m_Height);

				if(pTilemap->m_Flags & TILESLAYERFLAG_GAME)
				{
					delete[] rMap.m_pGameTiles;
					rMap.m_pGameTiles = pTiles;
					rMap.m_GameWidth = pTilemap->m_Width;
					rMap.m_GameHeight = pTilemap->m_Height;
				}
				else if(IsTemplateGroup)
				{
					SEuclidStructTemplateLayer &Template = rMap.m_lTemplates.emplace();
					str_copy(Template.m_aName, aLayerName, sizeof(Template.m_aName));
					Template.m_Width = pTilemap->m_Width;
					Template.m_Height = pTilemap->m_Height;
					Template.m_pTiles = pTiles;
				}
				else
					delete[] pTiles;
			}
			else if(pLayer->m_Type == LAYERTYPE_QUADS && IsWorldGroup)
			{
				CMapItemLayerQuads *pQuadsLayer = (CMapItemLayerQuads *) pLayer;
				SEuclidStructQuadLayer &QuadLayer = rMap.m_lQuadLayers.emplace();
				QuadLayer.m_OffsetX = pGroup->m_OffsetX;
				QuadLayer.m_OffsetY = pGroup->m_OffsetY;
				QuadLayer.m_aImage[0] = 0;
				if(pQuadsLayer->m_Image >= 0 && pQuadsLayer->m_Image < lImageNames.size())
					str_copy(QuadLayer.m_aImage, lImageNames[pQuadsLayer->m_Image].m_aName, sizeof(QuadLayer.m_aImage));

				if(pQuadsLayer->m_NumQuads > 0)
				{
					CQuad *pSrc = (CQuad *) File.GetData(pQuadsLayer->m_Data);
					if(pSrc)
					{
						QuadLayer.m_lQuads.set_size(pQuadsLayer->m_NumQuads);
						mem_copy(QuadLayer.m_lQuads.base_ptr(), pSrc, sizeof(CQuad) * pQuadsLayer->m_NumQuads);
					}
				}
			}
		}
	}

	if(!rMap.m_pGameTiles)
	{
		MAPGEN_LOG("mapgen", "the euclid struct map '%s' has no game layer, skipping", pBaseMap);
		FreeEuclidStructMap(rMap);
		return false;
	}

	ScanStructTiles(rMap.m_pGameTiles, rMap.m_GameWidth, rMap.m_GameHeight,
		&rMap.m_RedX, &rMap.m_RedY, &rMap.m_BlueX, &rMap.m_BlueY,
		&rMap.m_MinX, &rMap.m_MinY, &rMap.m_MaxX, &rMap.m_MaxY);
	for(int t = 0; t < rMap.m_lTemplates.size(); t++)
	{
		SEuclidStructTemplateLayer &Template = rMap.m_lTemplates[t];
		ScanStructTiles(Template.m_pTiles, Template.m_Width, Template.m_Height,
			&rMap.m_RedX, &rMap.m_RedY, &rMap.m_BlueX, &rMap.m_BlueY,
			&rMap.m_MinX, &rMap.m_MinY, &rMap.m_MaxX, &rMap.m_MaxY);
	}

	// A euclidean structure needs both flags: there is no other way to know
	// where its content starts and ends, and a missing flag would make the
	// bounding box of the art decide the paste region.
	if(rMap.m_RedX < 0 || rMap.m_RedY < 0 || rMap.m_BlueX < 0 || rMap.m_BlueY < 0)
	{
		MAPGEN_LOG("mapgen", "the euclid struct map '%s' has no red/blue flag stand anchors, skipping", pBaseMap);
		FreeEuclidStructMap(rMap);
		return false;
	}

	// The same region math as CMapGen::GenerateNoneuclid. The red flag sits
	// one tile left of and one tile below the content's bottom-left corner, the
	// blue flag one tile right of and one tile above its top-right corner.
	const bool HasContent = rMap.m_MaxX >= 0 && rMap.m_MaxY >= 0;
	rMap.m_BottomLeftX = rMap.m_RedX + 1;
	rMap.m_BottomLeftY = rMap.m_RedY - 1;
	rMap.m_TopRightX = rMap.m_BlueX - 1;
	rMap.m_TopRightY = rMap.m_BlueY + 1;
	rMap.m_StructLeftX = rMap.m_BottomLeftX;
	rMap.m_StructTopY = rMap.m_TopRightY;
	rMap.m_RegionX1 = rMap.m_StructLeftX;
	rMap.m_RegionY1 = rMap.m_StructTopY;
	rMap.m_RegionX2 = rMap.m_TopRightX;
	rMap.m_RegionY2 = rMap.m_BottomLeftY;
	(void) HasContent;
	rMap.m_ContentWidth = rMap.m_RegionX2 - rMap.m_RegionX1 + 1;
	rMap.m_ContentHeight = rMap.m_RegionY2 - rMap.m_RegionY1 + 1;
	if(rMap.m_ContentWidth <= 0 || rMap.m_ContentHeight <= 0)
	{
		MAPGEN_LOG("mapgen", "the euclid struct map '%s' has an empty paste region, skipping", pBaseMap);
		FreeEuclidStructMap(rMap);
		return false;
	}

	// Footprint contact points: the bottom two content rows of the game layer.
	// They are where the structure's own collision meets the ground.
	int ContactMinX = 1 << 30;
	int ContactMaxX = -1;
	for(int y = maximum(rMap.m_RegionY1, rMap.m_RegionY2 - 1); y <= rMap.m_RegionY2; y++)
	{
		for(int x = rMap.m_RegionX1; x <= rMap.m_RegionX2; x++)
		{
			const int Index = rMap.m_pGameTiles[y * rMap.m_GameWidth + x].m_Index;
			if(Index == 0 || Index >= ENTITY_OFFSET)
				continue;
			ContactMinX = minimum(ContactMinX, x);
			ContactMaxX = maximum(ContactMaxX, x);
		}
	}
	if(ContactMaxX >= 0)
	{
		rMap.m_ContactMinX = ContactMinX;
		rMap.m_ContactMaxX = ContactMaxX;
	}
	return true;
}

// Pastes the struct's tiles into the already emitted creator layers. The merge
// is additive (transparent paste): the generated terrain stays where the
// structure has no tile, so a quad-only structure does not punch holes into the
// ground and a structure never erases an entrance frame or a spawn point.
static void PasteEuclidStructTiles(const SEuclidStructMap &rMap,
	int OffsetX, int OffsetY,
	CCreatorLayerTilemap *pTerrain, CCreatorLayerTilemap *pGame, CCreatorLayerTilemap *pUnhookable)
{
	if(pGame && rMap.m_pGameTiles)
	{
		PasteTiles(pGame->m_pTiles, pGame->m_Width, pGame->m_Height,
			rMap.m_pGameTiles, rMap.m_GameWidth, rMap.m_GameHeight,
			OffsetX, OffsetY, false, rMap.m_RegionX1, rMap.m_RegionY1, rMap.m_RegionX2, rMap.m_RegionY2, true);
	}

	for(int t = 0; t < rMap.m_lTemplates.size(); t++)
	{
		const SEuclidStructTemplateLayer &Template = rMap.m_lTemplates[t];
		CCreatorLayerTilemap *pDst = pTerrain;
		if(pUnhookable && str_comp(Template.m_aName, "Unhookable") == 0)
			pDst = pUnhookable;
		if(!pDst)
			continue;
		PasteTiles(pDst->m_pTiles, pDst->m_Width, pDst->m_Height,
			Template.m_pTiles, Template.m_Width, Template.m_Height,
			OffsetX, OffsetY, false, rMap.m_RegionX1, rMap.m_RegionY1, rMap.m_RegionX2, rMap.m_RegionY2);
	}
}

// Merges the struct's world-space quads into the "Structures" group. Quads are
// translated in fixed point (the map stores 22.10 pixel coordinates) by the
// paste offset and by the struct group's own offset; the pivot (point 4) moves
// with them. Envelopes are not carried over: the generated maps already use
// their envelope table for the day/night cycle, and a structure that needs an
// animated quad can be drawn as a static one instead.
struct SEuclidDestQuadLayer
{
	char m_aImage[32];
	CCreatorLayerQuads *m_pLayer;
};

static void PasteEuclidStructQuads(const SEuclidStructMap &rMap, int OffsetX, int OffsetY,
	const char *pStructName, CCreatorGroupInfo *pGroup,
	const array<SImageRequest> &lRequests, const array<CCreatorImage *> &lCreatorImages)
{
	if(!rMap.m_lQuadLayers.size())
		return;

	array<SEuclidDestQuadLayer> lDestLayers;

	for(int l = 0; l < rMap.m_lQuadLayers.size(); l++)
	{
		const SEuclidStructQuadLayer &Src = rMap.m_lQuadLayers[l];

		SEuclidDestQuadLayer *pDest = nullptr;
		for(int d = 0; d < lDestLayers.size(); d++)
		{
			if(str_comp(lDestLayers[d].m_aImage, Src.m_aImage) == 0)
			{
				pDest = &lDestLayers[d];
				break;
			}
		}
		if(!pDest)
		{
			SEuclidDestQuadLayer &NewDest = lDestLayers.emplace();
			str_copy(NewDest.m_aImage, Src.m_aImage, sizeof(NewDest.m_aImage));

			char aLayerName[32];
			if(lDestLayers.size() == 1)
				str_copy(aLayerName, pStructName, sizeof(aLayerName));
			else
				str_format(aLayerName, sizeof(aLayerName), "%s_%d", pStructName, lDestLayers.size());

			NewDest.m_pLayer = pGroup->AddQuadsLayer(aLayerName);
			NewDest.m_pLayer->m_pImage = FindRequestedImage(lRequests, lCreatorImages, Src.m_aImage);
			pDest = &NewDest;
		}

		// group offsets are map pixels; quad corners are fixed point pixels
		const int FixedX = (OffsetX * 32 + Src.m_OffsetX) * 1024;
		const int FixedY = (OffsetY * 32 + Src.m_OffsetY) * 1024;

		for(int q = 0; q < Src.m_lQuads.size(); q++)
		{
			const CQuad &SrcQuad = Src.m_lQuads[q];
			CCreatorQuad *pDst = pDest->m_pLayer->AddQuad(vec2(0.0f, 0.0f), vec2(0.0f, 0.0f));
			for(int k = 0; k < 4; k++)
			{
				pDst->m_aPoints[k] = ivec2(SrcQuad.m_aPoints[k].x + FixedX, SrcQuad.m_aPoints[k].y + FixedY);
				pDst->m_aTexcoords[k] = ivec2(SrcQuad.m_aTexcoords[k].x, SrcQuad.m_aTexcoords[k].y);
				pDst->m_aColors[k] = ColorRGBA(
					SrcQuad.m_aColors[k].r, SrcQuad.m_aColors[k].g,
					SrcQuad.m_aColors[k].b, SrcQuad.m_aColors[k].a);
			}
			pDst->m_Pos = ivec2(SrcQuad.m_aPoints[4].x + FixedX, SrcQuad.m_aPoints[4].y + FixedY);
			pDst->m_pColorEnv = nullptr;
			pDst->m_pPosEnv = nullptr;
		}
	}
}

// A unique structure is pasted on exactly one instance of a floor. The seed is
// derived from the floor name and the structure id, so two different unique
// structures naturally land on different instances.
static int UniqueStructureSeed(const CInstruction::CStruct &Struct, const CInstruction::CGeneration &Gen, const char *pFloorName, int Seed)
{
	if(!Struct.m_Unique)
		return -1;
	if(!Gen.m_Ring.m_Enabled || Gen.m_Ring.m_Size <= 1)
		return Seed;
	const unsigned Hash = str_quickhash(pFloorName) ^ (str_quickhash(Struct.m_aId) * 2654435761u);
	return 1 + (int) (Hash % (unsigned) Gen.m_Ring.m_Size);
}

// Scatters every euclidean structure over the generated ground line and merges
// the placed ones. Called after the terrain/game layers were emitted and before
// the foreground and night shade groups, so the structure art sits over the
// terrain but under the final shade.
static void PasteEuclideanStructures(const CInstruction *pInstruction, int Seed, CGeneratedWorld *pWorld,
	IStorage *pStorage, bool ConsoleOutput, CCreatorGroupInfo *pStructGroup,
	CCreatorLayerTilemap *pTerrainLayer, CCreatorLayerTilemap *pGameLayer,
	CCreatorLayerTilemap *pUnhookableLayer, CCreatorLayerTilemap *pWallLayer,
	CCreatorLayerTilemap *pDoodadsLayer,
	const array<SImageRequest> &lRequests, const array<CCreatorImage *> &lCreatorImages)
{
	const CInstruction::CGeneration &Gen = pInstruction->m_Generation;
	const int Width = Gen.m_Width;
	const int Height = Gen.m_Height;
	const int SharedMargin = Gen.m_Ring.m_Enabled ? Gen.m_Ring.m_SharedMargin : 0;
	if(pWorld->m_Surface.size() < Width || pWorld->m_lLayers.size() < 3)
		return;

	array<SEuclidPlacementRect> lOccupied;
	int TotalPlaced = 0;

	for(int s = 0; s < pInstruction->m_Structs.size(); s++)
	{
		const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];
		if(Struct.m_Count <= 0)
			continue;
		if(Struct.m_Unique && Seed != UniqueStructureSeed(Struct, Gen, pInstruction->m_aFloorName, Seed))
			continue;

		const int VariantIndex = PickStructVariant(Struct, Seed, (s + 1) * 100000 + 13);
		if(VariantIndex < 0)
		{
			MAPGEN_LOG("mapgen", "euclid struct '%s': no usable variant", Struct.m_aId);
			continue;
		}
		const CStructVariant &Variant = Struct.m_lVariants[VariantIndex];
		const char *pStructMapName = Variant.m_aBaseMap;

		SEuclidStructMap StructMap;
		if(!LoadEuclidStructMap(pStorage, pStructMapName, ConsoleOutput, StructMap))
			continue;

		const int RegionWidth = StructMap.m_ContentWidth;
		const int RegionHeight = StructMap.m_ContentHeight;

		// Place on a flat span that may be narrower than the whole paste
		// region: a gate or a tower can bridge a gentle slope at its feet.
		// The span is the structure's actual foot contact (bottom collision
		// tiles) when it has one, so two pillars cannot land on different
		// levels; an art-only structure falls back to the configured width.
		int ContactStart = 0; // the span that has to be level
		int ContactEnd = RegionWidth - 1;
		int FullStart = 0; // every foot contact, used to pick the base line
		int FullEnd = RegionWidth - 1;
		if(StructMap.m_ContactMaxX >= 0)
		{
			FullStart = clamp(StructMap.m_ContactMinX - StructMap.m_RegionX1, 0, RegionWidth - 1);
			FullEnd = clamp(StructMap.m_ContactMaxX - StructMap.m_RegionX1, FullStart, RegionWidth - 1);
			ContactStart = FullStart;
			ContactEnd = FullEnd;

			// A wide camp can have its footprint spread over several tiles of
			// terrain. The optional flat_width caps the part that has to be
			// level; the outer contacts may sit a little higher and simply sink
			// into the ground, which is what a real foundation does.
			const int FlatWidth = clamp(Struct.m_FlatWidth, 0, RegionWidth);
			if(FlatWidth > 0 && ContactEnd - ContactStart + 1 > FlatWidth)
			{
				const int Centre = (ContactStart + ContactEnd) / 2;
				ContactStart = clamp(Centre - FlatWidth / 2, 0, RegionWidth - FlatWidth);
				ContactEnd = ContactStart + FlatWidth - 1;
			}
		}
		else
		{
			const int FlatWidth = clamp(Struct.m_FlatWidth > 0 ? Struct.m_FlatWidth : RegionWidth, 1, RegionWidth);
			ContactStart = (RegionWidth - FlatWidth) / 2;
			ContactEnd = ContactStart + FlatWidth - 1;
			FullStart = ContactStart;
			FullEnd = ContactEnd;
		}

		const int RangeStart = Struct.m_Margin + SharedMargin;
		const int RangeEnd = Width - Struct.m_Margin - SharedMargin - RegionWidth;
		if(RangeEnd < RangeStart)
		{
			MAPGEN_LOG("mapgen", "euclid struct '%s': no room on a %d tile wide floor", pStructMapName, Width);
			FreeEuclidStructMap(StructMap);
			continue;
		}

		for(int p = 0; p < Struct.m_Count; p++)
		{
			// Keep the per-instance determinism of the rest of the generator;
			// the salt is distinct from the static anchor rolls.
			if(!RollChance(Seed, (s + 1) * 1000 + (p + 1) * 37, Struct.m_GenerateProba))
				continue;

			// One placement per equal band, so the structures cover the whole
			// width instead of clustering where the first flat spot happens.
			const int Span = RangeEnd - RangeStart + 1;
			const int BandStart = RangeStart + (int) ((long long) Span * p / Struct.m_Count);
			const int BandEnd = RangeStart + (int) ((long long) Span * (p + 1) / Struct.m_Count) - 1;
			if(BandEnd < BandStart)
				continue;
			const int BandLength = BandEnd - BandStart + 1;
			const int StartX = BandStart + (int) (DeterministicHash(Seed, (s + 1) * 100000 + (p + 1) * 777 + 13) % (unsigned) BandLength);

			auto IsEligible = [&](int ContentX, int *pRegionVar, int *pFullVar, int *pFlatVar) -> bool
			{
				const int FlatStart = ContentX + ContactStart;
				const int FlatEnd = ContentX + ContactEnd;
				const int FullFlatStart = ContentX + FullStart;
				const int FullFlatEnd = ContentX + FullEnd;
				int FlatMinY = 1 << 30;
				int FlatMaxY = -1;
				int FullMinY = 1 << 30;
				int FullMaxY = -1;
				int RegionMinY = 1 << 30;
				int RegionMaxY = -1;
				for(int tx = ContentX; tx < ContentX + RegionWidth; tx++)
				{
					if(tx < 0 || tx >= pWorld->m_Surface.size())
						return false;
					const int SurfaceY = pWorld->m_Surface[tx];
					RegionMinY = minimum(RegionMinY, SurfaceY);
					RegionMaxY = maximum(RegionMaxY, SurfaceY);
					if(tx >= FlatStart && tx <= FlatEnd)
					{
						FlatMinY = minimum(FlatMinY, SurfaceY);
						FlatMaxY = maximum(FlatMaxY, SurfaceY);
					}
					if(tx >= FullFlatStart && tx <= FullFlatEnd)
					{
						FullMinY = minimum(FullMinY, SurfaceY);
						FullMaxY = maximum(FullMaxY, SurfaceY);
					}
				}
				// Two tiles of tolerance under the feet, and four across the
				// whole footprint. The terrain is procedural, so demanding a
				// perfectly level 20 tile footprint would leave most floors
				// without room for a structure. Aligning to the lower ground
				// line embeds the higher contacts by at most two tiles, which
				// reads as a foundation rather than a floating structure.
				if(FlatMaxY - FlatMinY > 2)
					return false;
				// The outer contacts may sit higher than the level span, but
				// not so much that they would be buried completely.
				if(FullMaxY - FullMinY > 4)
					return false;
				if(RegionMaxY - RegionMinY > 4)
					return false;
				*pRegionVar = RegionMaxY - RegionMinY;
				*pFullVar = FullMaxY - FullMinY;
				*pFlatVar = FlatMaxY - FlatMinY;

				const int BaseY = FlatMaxY; // the lower ground line under the feet
				const int AnchorY = BaseY - 1 + Struct.m_SurfaceOffset;
				const int TopY = AnchorY - (RegionHeight - 1);
				if(TopY < 0 || AnchorY >= Height)
					return false;

				const SEuclidPlacementRect Candidate = {ContentX, TopY, ContentX + RegionWidth - 1, AnchorY};

				// Do not cover a surface entrance. An unrestricted x range is
				// ignored rather than treated as "the whole map": an entrance
				// that spans the world does not pin a single structure.
				const int Pad = Gen.m_SurfaceEntrances.m_Frame + 3;
				for(int e = 0; e < pWorld->m_lEntrances.size(); e++)
				{
					const CGeneratedWorld::CEntrance &Entrance = pWorld->m_lEntrances[e];
					if(Entrance.m_StartX == INT_MIN || Entrance.m_EndX == INT_MAX)
						continue;
					if(ContentX <= Entrance.m_EndX + Pad && Candidate.m_X1 >= Entrance.m_StartX - Pad)
						return false;
				}

				// Keep the configured gap from the structures already placed.
				const int Sep = Struct.m_MinSeparation;
				for(int o = 0; o < lOccupied.size(); o++)
				{
					const SEuclidPlacementRect &Other = lOccupied[o];
					if(Candidate.m_X0 <= Other.m_X1 + Sep && Candidate.m_X1 >= Other.m_X0 - Sep &&
						Candidate.m_Y0 <= Other.m_Y1 + Sep && Candidate.m_Y1 >= Other.m_Y0 - Sep)
						return false;
				}
				return true;
			};

			// Score every eligible column in the band and take the flattest,
			// not the first one. Now that the art is drawn behind the terrain,
			// a column whose ground rises in front of the structure hides a
			// large part of it; the flattest available shelf reads best.
			int ChosenX = -1;
			int BestScore = INT_MAX;
			for(int attempt = 0; attempt < BandLength; attempt++)
			{
				const int ContentX = BandStart + (StartX - BandStart + attempt) % BandLength;
				int RegionVar = 0, FullVar = 0, FlatVar = 0;
				if(!IsEligible(ContentX, &RegionVar, &FullVar, &FlatVar))
					continue;
				const int Score = RegionVar * 100 + FullVar * 10 + FlatVar;
				if(Score < BestScore)
				{
					BestScore = Score;
					ChosenX = ContentX;
				}
			}
			if(ChosenX < 0)
			{
				MAPGEN_LOG("mapgen", "euclid struct '%s': no flat ground in band %d/%d",
					pStructMapName, p + 1, Struct.m_Count);
				continue;
			}

			const int BaseY = [&]()
			{
				// Align to the lowest ground under any foot contact, so no
				// contact floats; the higher ones sink into the terrain.
				const int FlatStart = ChosenX + FullStart;
				const int FlatEnd = ChosenX + FullEnd;
				int MaxY = 0;
				for(int tx = FlatStart; tx <= FlatEnd; tx++)
					MaxY = maximum(MaxY, pWorld->m_Surface[tx]);
				return MaxY;
			}();
			const int AnchorY = BaseY - 1 + Struct.m_SurfaceOffset;
			const int OffsetX = ChosenX - StructMap.m_BottomLeftX;
			const int OffsetY = AnchorY - StructMap.m_BottomLeftY;

			// Cut a level shelf for the structure. The structures are drawn
			// behind the terrain, so a hillside in front would hide their lower
			// part; levelling the footprint makes the ground meet the plinth
			// instead of covering the art. Only higher ground is cut away, the
			// lower ground under an arch is left alone.
			if(pTerrainLayer && pGameLayer)
			{
				for(int tx = ChosenX; tx < ChosenX + RegionWidth && tx < Width; tx++)
				{
					if(tx < 0 || tx >= pWorld->m_Surface.size())
						continue;
					const int SurfaceY = pWorld->m_Surface[tx];
					if(SurfaceY >= BaseY)
						continue;
					for(int ty = maximum(0, SurfaceY); ty < BaseY; ty++)
					{
						CTile &Terrain = pTerrainLayer->m_pTiles[ty * Width + tx];
						Terrain.m_Index = TILE_AIR;
						Terrain.m_Flags = 0;
						Terrain.m_Skip = 0;
						if(pGameLayer)
						{
							CTile &Game = pGameLayer->m_pTiles[ty * Width + tx];
							Game.m_Index = TILE_AIR;
							Game.m_Flags = 0;
							Game.m_Skip = 0;
						}
						if(pWallLayer && ty < pWallLayer->m_Height && tx < pWallLayer->m_Width)
						{
							CTile &Wall = pWallLayer->m_pTiles[ty * pWallLayer->m_Width + tx];
							Wall.m_Index = TILE_AIR;
							Wall.m_Flags = 0;
							Wall.m_Skip = 0;
						}
						if(pDoodadsLayer && ty < pDoodadsLayer->m_Height && tx < pDoodadsLayer->m_Width)
						{
							CTile &Doodad = pDoodadsLayer->m_pTiles[ty * pDoodadsLayer->m_Width + tx];
							Doodad.m_Index = TILE_AIR;
							Doodad.m_Flags = 0;
							Doodad.m_Skip = 0;
						}
					}
					pWorld->m_Surface[tx] = BaseY;
				}
			}

			// Nothing should grow in front of the structure either.
			if(pDoodadsLayer)
			{
				for(int ty = maximum(0, AnchorY - (RegionHeight - 1)); ty <= minimum(pDoodadsLayer->m_Height - 1, AnchorY); ty++)
				{
					for(int tx = maximum(0, ChosenX); tx <= minimum(pDoodadsLayer->m_Width - 1, ChosenX + RegionWidth - 1); tx++)
					{
						CTile &Doodad = pDoodadsLayer->m_pTiles[ty * pDoodadsLayer->m_Width + tx];
						Doodad.m_Index = TILE_AIR;
						Doodad.m_Flags = 0;
						Doodad.m_Skip = 0;
					}
				}
			}

			PasteEuclidStructTiles(StructMap, OffsetX, OffsetY, pTerrainLayer, pGameLayer, pUnhookableLayer);
			if(pStructGroup)
				PasteEuclidStructQuads(StructMap, OffsetX, OffsetY, pStructMapName,
					pStructGroup, lRequests, lCreatorImages);

			// Containers carried by this variant go on a reachable floor spot
			// inside the pasted region. The tiles are final here, so the scan
			// sees the structure's own collision and the level shelf cut above.
			if(Variant.m_NumContainers > 0 && pGameLayer)
			{
				int aUsedX[CStructVariant::MAX_CONTAINERS];
				int aUsedY[CStructVariant::MAX_CONTAINERS];
				int NumUsed = 0;
				for(int c = 0; c < Variant.m_NumContainers; c++)
				{
					const int PreferredX = ChosenX + (RegionWidth * (c + 1)) / (Variant.m_NumContainers + 1);
					int SpotX = 0;
					int SpotY = 0;
					if(!FindContainerSpot(pGameLayer->m_pTiles, pGameLayer->m_Width, pGameLayer->m_Height,
						   ChosenX, AnchorY - (RegionHeight - 1), ChosenX + RegionWidth - 1, AnchorY,
						   PreferredX, &SpotX, &SpotY))
					{
						MAPGEN_LOG("mapgen", "euclid struct '%s': no floor spot for its '%s' container",
							pStructMapName, Variant.m_aContainers[c].m_aKind);
						continue;
					}

					bool Used = false;
					for(int u = 0; u < NumUsed; u++)
					{
						if(aUsedX[u] == SpotX && aUsedY[u] == SpotY)
						{
							Used = true;
							break;
						}
					}
					if(Used)
						continue;

					SContainerSpawn &Spawn = pWorld->m_lContainers.emplace();
					// The id names the structure, its placement and the container
					// inside it, not the order of the list: editing another
					// structure cannot move a saved stack to a different prop.
					Spawn.m_ID = (s + 1) * 100000 + (p + 1) * 100 + c;
					str_copy(Spawn.m_aKind, Variant.m_aContainers[c].m_aKind, sizeof(Spawn.m_aKind));
					str_copy(Spawn.m_aLoot, Variant.m_aContainers[c].m_aLoot, sizeof(Spawn.m_aLoot));
					Spawn.m_X = SpotX;
					Spawn.m_Y = SpotY;
					if(NumUsed < (int) (sizeof(aUsedX) / sizeof(aUsedX[0])))
					{
						aUsedX[NumUsed] = SpotX;
						aUsedY[NumUsed] = SpotY;
						NumUsed++;
					}
				}
			}

			// Creatures carried by this variant. The spot search is the same
			// reachable-floor scan containers use, so a boss or guard never
			// spawns inside a wall. All creatures of one placement share a
			// group key; the boss's death persists the whole encounter.
			if(Variant.m_NumMobs > 0 && pGameLayer)
			{
				int TotalMobs = 0;
				for(int m = 0; m < Variant.m_NumMobs; m++)
					TotalMobs += maximum(1, Variant.m_aMobs[m].m_Count);
				int MobIndex = 0;
				const int Group = (s + 1) * 100000 + (p + 1) * 100 + 90;
				for(int m = 0; m < Variant.m_NumMobs; m++)
				{
					const CStructMob &MobDef = Variant.m_aMobs[m];
					const int Count = maximum(1, MobDef.m_Count);
					for(int c = 0; c < Count; c++)
					{
						const int PreferredX = ChosenX + (RegionWidth * (MobIndex + 1)) / (TotalMobs + 1);
						int SpotX = 0;
						int SpotY = 0;
						if(!FindContainerSpot(pGameLayer->m_pTiles, pGameLayer->m_Width, pGameLayer->m_Height,
							   ChosenX, AnchorY - (RegionHeight - 1), ChosenX + RegionWidth - 1, AnchorY,
							   PreferredX, &SpotX, &SpotY))
						{
							MAPGEN_LOG("mapgen", "euclid struct '%s': no floor spot for mob '%s'", pStructMapName, MobDef.m_aMob);
							continue;
						}

						SGeneratedMob &Spawn = pWorld->m_lMobs.emplace();
						Spawn.m_ID = Group * 10 + MobIndex;
						Spawn.m_Group = Group;
						str_copy(Spawn.m_aMob, MobDef.m_aMob, sizeof(Spawn.m_aMob));
						Spawn.m_Boss = MobDef.m_Boss;
						Spawn.m_X = SpotX;
						Spawn.m_Y = SpotY;
						MAPGEN_LOG("mapgen", "euclid struct '%s': mob '%s'%s at (%d,%d), group %d",
							pStructMapName, MobDef.m_aMob, MobDef.m_Boss ? " (boss)" : "", SpotX, SpotY, Group);
						MobIndex++;
					}
				}
			}

			// The structure claims this piece of ground. A spawn point inside
			// it would drop the player inside a wall, so it is moved out of the
			// way by clearing the entity cell; the floor keeps its other spawns.
			{
				CGeneratedWorld::CLayer &Game = pWorld->m_lLayers[CGeneratedWorld::LAYER_GAME];
				for(int ty = maximum(0, AnchorY - (RegionHeight - 1)); ty <= minimum(Height - 1, AnchorY); ty++)
				{
					for(int tx = maximum(0, ChosenX); tx <= minimum(Width - 1, ChosenX + RegionWidth - 1); tx++)
					{
						CTile &Tile = Game.m_pTiles[ty * Width + tx];
						if(Tile.m_Index >= ENTITY_OFFSET)
						{
							Tile.m_Index = TILE_AIR;
							Tile.m_Flags = 0;
						}
					}
				}
			}

			// Drop the resources that were scattered onto this ground, instead
			// of leaving them buried in a wall or unreachable under a platform.
			for(int r = pWorld->m_lResources.size() - 1; r >= 0; r--)
			{
				const CGeneratedWorld::CResource &Resource = pWorld->m_lResources[r];
				if(Resource.m_X >= ChosenX && Resource.m_X < ChosenX + RegionWidth &&
					Resource.m_Y >= AnchorY - (RegionHeight - 1) && Resource.m_Y <= AnchorY)
					pWorld->m_lResources.remove_index(r);
			}

			SEuclidPlacementRect &Occupied = lOccupied.emplace();
			Occupied.m_X0 = ChosenX;
			Occupied.m_Y0 = AnchorY - (RegionHeight - 1);
			Occupied.m_X1 = ChosenX + RegionWidth - 1;
			Occupied.m_Y1 = AnchorY;

			TotalPlaced++;
			char aLogName[96];
			if(Struct.m_aId[0])
				str_format(aLogName, sizeof(aLogName), "%s/%s", Struct.m_aId,
					Variant.m_aSubId[0] ? Variant.m_aSubId : pStructMapName);
			else
				str_copy(aLogName, pStructMapName, sizeof(aLogName));
			MAPGEN_LOG("mapgen", "euclid struct '%s': anchor tile (%d,%d), region %dx%d, %d quad(s)",
				aLogName, ChosenX, AnchorY, RegionWidth, RegionHeight, StructMap.m_lQuadLayers.size());
		}

		FreeEuclidStructMap(StructMap);
	}

	if(TotalPlaced > 0)
		MAPGEN_LOG("mapgen", "euclid structures: %d placement(s)", TotalPlaced);
}

// ---------------------------------------------------------------------------
// euclidean floor generation
// ---------------------------------------------------------------------------

bool CMapGen::GenerateEuclidean(const CInstruction *pInstruction, int Seed, SGeneratedMap *pOut)
{
	if(!pOut)
		return false;

	const char *pFloorName = pInstruction->m_aFloorName;
	const CWorldRing Ring = WorldRing(pInstruction, Seed);
	if(Ring.m_Enabled)
		MAPGEN_LOG("mapgen", "ring instance %d/%d, shared margin %d, stride %d, period %d",
			Seed, pInstruction->m_Generation.m_Ring.m_Size, pInstruction->m_Generation.m_Ring.m_SharedMargin, Ring.m_Stride, Ring.m_Period);

	// A euclidean floor does not read a base map at all. Its terrain is entirely
	// procedural, and its look comes from the tileset, the background and the
	// automap rule sets named in its own generation block. Requiring a base map
	// here used to mean "this file must exist" while nothing was ever read from
	// it, which made the "base" field misleading: Darkness.map, for instance, is
	// an empty 50x50 canvas, and Heaven.map carries no tileset image at all.
	//
	// The static floors (hub and non euclidean) still use their base map, they
	// just go through the structure pasting path in RequestNewMap instead.

	// build the terrain first: a failure here means we never touch the disk
	CGeneratedWorld *pWorld = GenerateEuclideanWorld(pInstruction, Seed);
	if(!pWorld)
		return false;

	// Style the terrain with the automapper rules: the generator only decided
	// solid vs air, and the rule set turns that into autotile variants (edges,
	// corners, random variants, flips).
	CAutoMapper Mapper;
	{
		const int NumRuleSets = Mapper.LoadAll(Storage(), Console());
		if(NumRuleSets == 0)
		{
			MAPGEN_LOG("mapgen", "no automap rule sets found in 'automap/', the terrain stays unstyled");
		}
		else
		{
			const char *pMainSet = pInstruction->m_Generation.m_aMainRuleSet;
			// the terrain is the only layer styled here: the game layer is
			// derived from it and the unhookable layer does not exist yet
			CGeneratedWorld::CLayer &Layer = pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];

			// The terrain is styled on its real shape, caves included: the rule
			// set draws an edge tile wherever it sees air, which is exactly the
			// autotiling an editor painting this layer would get. Filling the
			// caves first and unpicking them afterwards would leave the whole
			// underground with interior variants and break the grass_main look
			// around every cave mouth.
			//
			// the world is a cylinder, so the rules sample across the seam
			// instead of hitting a wall; the outermost columns are held out of
			// that wrap so the border does not wear corner tiles
			if(!Mapper.Proceed(pMainSet, Layer.m_pTiles, Layer.m_Width, Layer.m_Height, Ring.m_NoiseSeed, !Ring.m_Enabled, pInstruction->m_Generation.m_WrapMargin, nullptr, Ring.m_OffsetX))
				MAPGEN_LOG("mapgen", "automap rule set '%s' is unknown, the layer '%s' stays unstyled", pMainSet, Layer.m_aName);
		}
	}

	// Entrances are cut after the automapper, which resets every solid tile to
	// its basetile and would erase the frame that marks the cave mouth.
	BuildWorldEntrances(pInstruction, Seed, pWorld);

	// the configured surface resources stand on the finished ground line, after
	// the entrances were cut so nothing is placed inside a cave mouth
	GenerateSurfaceResources(pInstruction, Seed, pWorld);

	CMapCreator Creator(Storage(), Console());

	// -----------------------------------------------------------------
	// register the images the generated layers draw with
	//
	// Images are loaded from mapres/ by name, not copied from the base map: a
	// base only carries the images its author happened to use, which would tie
	// the generator to whatever that was. A missing image is reported but not
	// fatal; the map still loads, it just renders without that texture.
	// -----------------------------------------------------------------

	// The terrain tileset, the frame sheet, and the background images. Sizes are
	// read from the png rather than assumed: the cloud sheets are 2048x1024 and
	// the mountain sheets 1024x512, and declaring them as 1024x1024 made the
	// client sample the wrong region.
	array<SImageRequest> lRequests;

	// Reads the pixel size from a png header (IHDR); false when unreadable, in
	// which case the caller falls back.
	auto ReadPngSize = [&](const char *pFound, int *pWidth, int *pHeight) -> bool
	{
		IOHANDLE File = Storage()->OpenFile(pFound, IOFLAG_READ, IStorage::TYPE_ALL);
		if(!File)
			return false;
		unsigned char aHeader[24];
		const unsigned Bytes = io_read(File, aHeader, sizeof(aHeader));
		io_close(File);
		if(Bytes < sizeof(aHeader))
			return false;
		// png signature, then IHDR: width and height as big endian 32 bit
		static const unsigned char s_aSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		if(mem_comp(aHeader, s_aSignature, 8) != 0)
			return false;
		if(mem_comp(aHeader + 12, "IHDR", 4) != 0)
			return false;

		const auto ReadBE32 = [](const unsigned char *p) -> int
		{
			return ((int) p[0] << 24) | ((int) p[1] << 16) | ((int) p[2] << 8) | (int) p[3];
		};
		const int W = ReadBE32(aHeader + 16);
		const int H = ReadBE32(aHeader + 20);
		if(W <= 0 || H <= 0)
			return false;
		*pWidth = W;
		*pHeight = H;
		return true;
	};

	auto AddImageRequest = [&](const char *pName, int Width, int Height, bool Optional)
	{
		if(!pName || !pName[0])
			return;
		for(int i = 0; i < lRequests.size(); i++)
		{
			if(str_comp(lRequests[i].m_aName, pName) == 0)
				return; // already requested
		}
		SImageRequest &Req = lRequests.emplace();
		str_copy(Req.m_aName, pName, sizeof(Req.m_aName));
		Req.m_Width = Width;
		Req.m_Height = Height;
		Req.m_Optional = Optional;
	};

	AddImageRequest(pInstruction->m_Generation.m_aTileset, 1024, 1024, false);
	// the entrance frame sheet, when the floor names one of its own
	if(pInstruction->m_Generation.m_aUnhookableTileset[0])
		AddImageRequest(pInstruction->m_Generation.m_aUnhookableTileset, 1024, 1024, true);
	// the vegetation doodads sheet and the sun/moon are both optional
	AddImageRequest(pInstruction->m_Generation.m_aDoodadsTileset, 1024, 1024, true);
	AddImageRequest(pInstruction->m_Generation.m_aSunImage, 512, 512, true);
	// the moon of the day/night cycle, also optional
	AddImageRequest(pInstruction->m_Generation.m_DayNight.m_aMoonImage, 512, 512, true);
	// every quad pass in every layer asks for its own image
	for(int i = 0; i < pInstruction->m_Generation.m_Layers.size(); i++)
	{
		const CInstruction::CLayerConfig &Layer = pInstruction->m_Generation.m_Layers[i];
		if(!Layer.m_Enabled)
			continue;
		for(int q = 0; q < Layer.m_Quads.size(); q++)
			if(Layer.m_Quads[q].m_aImage[0])
				AddImageRequest(Layer.m_Quads[q].m_aImage, 1024, 1024, true);
	}

	// Euclidean structure art may name its own images, so those have to join
	// the same request table before the creator images are created. The struct
	// maps are small and are opened once more by the merge below.
	for(int s = 0; s < pInstruction->m_Structs.size(); s++)
	{
		const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];
		if(Struct.m_Count <= 0)
			continue;
		for(int v = 0; v < Struct.m_lVariants.size(); v++)
		{
			char aStructPath[IO_MAX_PATH_LENGTH];
			str_format(aStructPath, sizeof(aStructPath), "maps/struct/%s.map", Struct.m_lVariants[v].m_aBaseMap);
			CDataFileReader StructFile;
			StructFile.SetConsoleOutput(m_DatafileConsoleOutput);
			if(!StructFile.Open(Storage(), aStructPath, IStorage::TYPE_ALL))
				continue;
			int StructImagesStart = 0, StructImagesNum = 0;
			StructFile.GetType(MAPITEMTYPE_IMAGE, &StructImagesStart, &StructImagesNum);
			for(int i = 0; i < StructImagesNum; i++)
			{
				CMapItemImage *pImage = (CMapItemImage *) StructFile.GetItem(StructImagesStart + i, 0, 0);
				if(!pImage || !pImage->m_External)
					continue;
				const char *pImageName = (const char *) StructFile.GetData(pImage->m_ImageName);
				if(pImageName && pImageName[0])
					AddImageRequest(pImageName, pImage->m_Width, pImage->m_Height, true);
			}
		}
	}

	array<CCreatorImage *> lCreatorImages; // parallel to lRequests
	for(int i = 0; i < lRequests.size(); i++)
	{
		const SImageRequest &Req = lRequests[i];

		// only reference an image that really is on disk; registering a name
		// that cannot be loaded yields a layer that renders as a blank sheet
		char aFile[IO_MAX_PATH_LENGTH];
		char aFound[IO_MAX_PATH_LENGTH];
		str_format(aFile, sizeof(aFile), "%s.png", Req.m_aName);
		if(!Storage()->FindFile(aFile, "mapres", IStorage::TYPE_ALL, aFound, sizeof(aFound)))
		{
			MAPGEN_LOG("mapgen", "%s image '%s' is missing from mapres/, the layer will render without a texture",
				Req.m_Optional ? "background" : "tileset", Req.m_aName);
			lCreatorImages.add(nullptr);
			continue;
		}
		// take the real size from the file; the requested values are only the
		// fallback for a png whose header cannot be read
		int Width = Req.m_Width;
		int Height = Req.m_Height;
		if(ReadPngSize(aFound, &Width, &Height))
			MAPGEN_LOG("mapgen", "image '%s' is %dx%d", Req.m_aName, Width, Height);
		else
			MAPGEN_LOG("mapgen", "image '%s' has an unreadable png header, assuming %dx%d",
				Req.m_aName, Width, Height);
		lCreatorImages.add(Creator.AddExternalImage(Req.m_aName, Width, Height));
	}

	// looks an already requested image up by name
	auto FindCreatorImage = [&](const char *pName) -> CCreatorImage *
	{
		if(!pName || !pName[0])
			return nullptr;
		for(int i = 0; i < lRequests.size(); i++)
			if(str_comp(lRequests[i].m_aName, pName) == 0)
				return lCreatorImages[i];
		return nullptr;
	};

	// one background group (parallax 0, the sky) and one main group holding the
	// terrain, the entrance frames and the game layer
	const CGeneratedWorld::CLayer *pMainLayer = 0;
	const CGeneratedWorld::CLayer *pGameLayer = 0;
	const CGeneratedWorld::CLayer *pUnhookableLayer = 0;
	for(int i = 0; i < pWorld->m_lLayers.size(); i++)
	{
		const CGeneratedWorld::CLayer &Layer = pWorld->m_lLayers[i];
		// the layers are addressed by their fixed index rather than by the
		// IsGameLayer flag: that flag only says "this one carries collision",
		// and the unhookable layer must not be mistaken for the terrain
		if(i == CGeneratedWorld::LAYER_GAME)
			pGameLayer = &Layer;
		else if(i == CGeneratedWorld::LAYER_UNHOOKABLE)
			pUnhookableLayer = &Layer;
		else
			pMainLayer = &Layer;
	}

	const int Width = pInstruction->m_Generation.m_Width;

	// The ground line the scenery is anchored to: the highest ground, not the
	// average. Scenery is drawn behind the terrain, so anchoring at the average
	// would let mountains poke through wherever the ground dips below it. On a
	// ring the generator supplies the highest ground of the whole ring, so
	// neighbouring instances put their scenery at the same screen height.
	int GroundY = pWorld->m_BackgroundGroundY;
	if(GroundY < 0)
	{
		GroundY = pInstruction->m_Generation.m_SurfaceLevel;
		if(pWorld->m_Surface.size())
		{
			GroundY = pWorld->m_Surface[0];
			for(int i = 1; i < pWorld->m_Surface.size(); i++)
				GroundY = minimum(GroundY, pWorld->m_Surface[i]);
		}
	}

	const CInstruction::CGeneration &Gen = pInstruction->m_Generation;

	// -----------------------------------------------------------------
	// the cave background wall
	//
	// The terrain layer is a single lair: a carved cave is air, so without a
	// layer behind it the cave shows the sky. This layer fills the whole
	// underground with the floor's cave look. It is styled by the cave rule set
	// when the floor names one that belongs to the same sheet as the terrain
	// (its source file is the sheet name); otherwise the terrain's own rule set
	// is used and the layer is darkened, the plain underground the config asks
	// for when no cave rule exists.
	// -----------------------------------------------------------------
	const int SubWidth = pMainLayer ? pMainLayer->m_Width : 0;
	const int SubHeight = pMainLayer ? pMainLayer->m_Height : 0;
	const CAutoMapper::CRuleSet *pMainSet = Mapper.FindRuleSet(Gen.m_aMainRuleSet);
	const char *pMainSource = pMainSet ? pMainSet->m_aSource : "";

	array<CTile> lWallTiles;
	bool WallDarkened = false;
	const char *pWallSet = Gen.m_aMainRuleSet;
	if(SubWidth > 0 && SubHeight > 0)
	{
		lWallTiles.set_size(SubWidth * SubHeight);
		for(int x = 0; x < SubWidth; x++)
		{
			const int SurfaceY = x < pWorld->m_Surface.size() ? clamp(pWorld->m_Surface[x], 0, SubHeight) : 0;
			for(int y = 0; y < SubHeight; y++)
			{
				CTile &Tile = lWallTiles[y * SubWidth + x];
				// above the ground stays transparent, below is rock or the wall
				// behind a cave
				Tile.m_Index = y >= SurfaceY ? 1 : 0;
				Tile.m_Flags = 0;
				Tile.m_Skip = 0;
				Tile.m_Reserved = 0;
			}
		}

		const CAutoMapper::CRuleSet *pCaveSet = Gen.m_aCaveRuleSet[0] ? Mapper.FindRuleSet(Gen.m_aCaveRuleSet) : nullptr;
		const bool CaveMatches = pCaveSet && str_comp(pCaveSet->m_aSource, pMainSource) == 0;
		pWallSet = CaveMatches ? Gen.m_aCaveRuleSet : Gen.m_aMainRuleSet;
		if(!Mapper.Proceed(pWallSet, lWallTiles.base_ptr(), SubWidth, SubHeight, Ring.m_NoiseSeed + 0x51ed, !Ring.m_Enabled, Gen.m_WrapMargin, nullptr, Ring.m_OffsetX))
			MAPGEN_LOG("mapgen", "cave rule set '%s' is unknown, the wall stays plain", pWallSet);
		WallDarkened = !CaveMatches;

		// On the ground line the foreground is a grass edge whose blades leave
		// transparent pixels; a wall tile right behind it shows the dark cave
		// colour through the grass, as if the cave leaked onto the surface. The
		// tile on the ground line goes, and only that one: deeper down the wall
		// is what backs the cave mouths, so it stays.
		for(int x = 0; x < SubWidth; x++)
		{
			const int SurfaceY = x < pWorld->m_Surface.size() ? clamp(pWorld->m_Surface[x], 0, SubHeight - 1) : 0;
			const int Index = SurfaceY * SubWidth + x;
			if(pMainLayer->m_pTiles[Index].m_Index == TILE_AIR)
				continue; // a carved mouth on the ground line: the wall is the mouth
			if(SurfaceY > 0 && pMainLayer->m_pTiles[(SurfaceY - 1) * SubWidth + x].m_Index != TILE_AIR)
				continue; // not actually the open surface
			lWallTiles[Index].m_Index = 0;
		}
	}
	CCreatorImage *pWallImage = (lCreatorImages.size() && lCreatorImages[0]) ? lCreatorImages[0] : nullptr;

	// -----------------------------------------------------------------
	// the vegetation doodads
	//
	// A doodad rule is a rectangle of the doodads sheet (m_StartId top-left to
	// m_EndId bottom-right); m_Ry places that top-left relative to the ground
	// line, so a height-5 tree with ry -5 rests on the ground. The sheet only
	// matches the floor when it is the one the rule set was authored for, which
	// the set records as its source file name.
	// -----------------------------------------------------------------
	const CAutoMapper::CDoodadSet *pDoodadSet = Gen.m_aDoodadsRuleset[0] ?
							    Mapper.FindDoodadSet(Gen.m_aDoodadsRuleset, Gen.m_aDoodadsTileset) :
							    nullptr;
	CCreatorImage *pDoodadsImage = FindCreatorImage(Gen.m_aDoodadsTileset);
	const bool DoodadsMatch = pDoodadSet && pDoodadsImage && SubWidth > 0 &&
				  str_comp(pDoodadSet->m_aSource, Gen.m_aDoodadsTileset) == 0;
	array<CTile> lDoodadTiles;
	if(DoodadsMatch)
	{
		lDoodadTiles.set_size(SubWidth * SubHeight);
		for(int i = 0; i < SubWidth * SubHeight; i++)
		{
			lDoodadTiles[i].m_Index = 0;
			lDoodadTiles[i].m_Flags = 0;
			lDoodadTiles[i].m_Skip = 0;
			lDoodadTiles[i].m_Reserved = 0;
		}

		// do not plant on an entrance: the same padding the spawns use
		auto OnEntrance = [&](int X) -> bool
		{
			for(int i = 0; i < pWorld->m_lEntrances.size(); i++)
			{
				const CGeneratedWorld::CEntrance &Entrance = pWorld->m_lEntrances[i];
				const int Pad = Gen.m_SurfaceEntrances.m_Frame + 2;
				if(X >= Entrance.m_StartX - Pad && X <= Entrance.m_EndX + Pad)
					return true;
			}
			return false;
		};

		for(int x = 0; x < SubWidth; x++)
		{
			if(OnEntrance(x))
				continue;
			const int SurfaceY = x < pWorld->m_Surface.size() ? pWorld->m_Surface[x] : 0;

			for(int r = 0; r < pDoodadSet->m_aDoodads.size(); r++)
			{
				const CAutoMapper::CDoodad &Doodad = pDoodadSet->m_aDoodads[r];
				if(!RollOneIn((int) Ring.m_NoiseSeed, (Ring.m_OffsetX + x) * 131 + r * 7919, Doodad.m_Random))
					continue;

				// Only ground vegetation is placed. Ceiling and wall doodads
				// live underground, which is the wall's job now, and a plant
				// should never poke out of solid rock.
				if(Doodad.m_Location != CAutoMapper::CDoodad::LOCATION_FLOOR)
					continue;

				const int SX = Doodad.m_StartId % 16;
				const int SY = Doodad.m_StartId / 16;
				const int EX = Doodad.m_EndId % 16;
				const int EY = Doodad.m_EndId / 16;
				const int DW = maximum(1, EX - SX + 1);
				const int DH = maximum(1, EY - SY + 1);
				// The footprint has to stay clear of the map edge. The world
				// wraps, so a stamp that touches the border would have its
				// tiles read again from the far side (and a stamp that crosses
				// it would be cut in two); either way it is not placed.
				const int FirstX = x + Doodad.m_Rx;
				const int BaseY = SurfaceY + Doodad.m_Ry;
				bool Fits = BaseY >= 0 && BaseY + DH <= SubHeight && FirstX > 0 && FirstX + DW < SubWidth;

				// A doodad only goes on ground that is flat under its whole
				// footprint and free of the entrance frames. On a step the
				// anchor column's line is wrong for the rest (uphill buried,
				// downhill floating); on a frame the structure would cut the
				// sprite apart. Either way there is no place, so it is dropped.
				for(int dx = 0; dx < DW && Fits; dx++)
				{
					const int TX = FirstX + dx;
					const int GroundY = TX < pWorld->m_Surface.size() ? pWorld->m_Surface[TX] : -1;
					if(GroundY != SurfaceY || OnEntrance(TX))
						Fits = false;
				}

				// and it must not land on another doodad: the whole rectangle is
				// treated as occupied, so two trees never grow into each other
				for(int dy = 0; dy < DH && Fits; dy++)
				{
					const int TY = BaseY + dy;
					for(int dx = 0; dx < DW; dx++)
					{
						if(lDoodadTiles[TY * SubWidth + FirstX + dx].m_Index != 0)
						{
							Fits = false;
							break;
						}
					}
				}
				if(!Fits)
					continue;

				for(int dy = 0; dy < DH; dy++)
				{
					const int TY = BaseY + dy;
					for(int dx = 0; dx < DW; dx++)
					{
						CTile &Tile = lDoodadTiles[TY * SubWidth + FirstX + dx];
						Tile.m_Index = (unsigned char) ((SY + dy) * 16 + (SX + dx));
						Tile.m_Flags = 0;
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------
	// emit the layers
	//
	// Groups are drawn in the order they are written, so the order here is the
	// stacking order: background, terrain, game layer, then foreground. A
	// foreground layer is created only after the game layer exists, which is
	// what keeps it in front instead of buried under the terrain. Each layer
	// gets its own group at its own parallax, like the official maps.
	// -----------------------------------------------------------------

	// The alpha of the foreground night shade: one screen sized black quad
	// drawn over everything so a single curve darkens sky, clouds, terrain and
	// the celestial bodies together. Null while the floor has no day/night
	// cycle, in which case no shade group is emitted.
	CCreatorEnvelope *pNightShadeEnv = nullptr;

	// -----------------------------------------------------------------
	// the sky, the sun and the moon
	//
	// Everything lives in one parallax 0 group, which pins it to the viewport
	// exactly like the official skies, so the quad only has to cover the
	// screen. A static floor emits one gradient quad. A day/night floor emits
	// the day gradient, a night gradient that blends over it, a warm band at
	// sunrise and sunset, and a sun and a moon that ride synchronized
	// envelopes around the screen once per cycle: the client animates the
	// whole thing on its own.
	// -----------------------------------------------------------------
	auto EmitSky = [&]()
	{
		CCreatorGroupInfo *pGroup = Creator.AddGroup("Sky");
		pGroup->m_ParallaxX = 0;
		pGroup->m_ParallaxY = 0;
		pGroup->m_OffsetX = 0;
		pGroup->m_OffsetY = 0;

		// about 1530x1534 px covers a 1430x1050 screen, which is what the
		// official maps use; the same size is kept here
		const float SkySize = 1536.0f;

		// A full screen gradient from the zenith colour (points 0/1) down to the
		// horizon colour (points 2/3). The texcoords span the whole (unused)
		// texture: leaving them all at zero makes the renderer sample one
		// degenerate texel, which comes out black because there is no image.
		auto AddGradientQuad = [&](CCreatorLayerQuads *pLayer, const ColorRGBA &Top, const ColorRGBA &Horizon) -> CCreatorQuad *
		{
			CCreatorQuad *pQuad = pLayer->AddQuad(vec2(0.0f, 0.0f), vec2(SkySize, SkySize));
			pQuad->m_aColors[0] = Top;
			pQuad->m_aColors[1] = Top;
			pQuad->m_aColors[2] = Horizon;
			pQuad->m_aColors[3] = Horizon;
			pQuad->m_aTexcoords[0] = ivec2(0, 0);
			pQuad->m_aTexcoords[1] = ivec2(1024, 0);
			pQuad->m_aTexcoords[2] = ivec2(0, 1024);
			pQuad->m_aTexcoords[3] = ivec2(1024, 1024);
			return pQuad;
		};

		const ColorRGBA DayTop(Gen.m_SkyColor[0], Gen.m_SkyColor[1], Gen.m_SkyColor[2], Gen.m_SkyColor[3]);
		const ColorRGBA DayHorizon(Gen.m_SkyHorizonColor[0], Gen.m_SkyHorizonColor[1],
			Gen.m_SkyHorizonColor[2], Gen.m_SkyHorizonColor[3]);

		// a celestial body: its own quad layer, placed by the position envelope
		// and faded in and out by the colour envelope
		auto AddBodyQuad = [&](const char *pImageName, float Size, CCreatorEnvelope *pPosEnv, CCreatorEnvelope *pColorEnv)
		{
			if(!pImageName || !pImageName[0])
				return;
			CCreatorImage *pImage = FindCreatorImage(pImageName);
			if(!pImage)
				return;
			CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer(pImageName);
			pLayer->m_pImage = pImage;
			// the body's centre is the origin; its position comes entirely from
			// the position envelope
			CCreatorQuad *pQuad = pLayer->AddQuad(vec2(0.0f, 0.0f), vec2(Size, Size));
			pQuad->m_aTexcoords[0] = ivec2(0, 0);
			pQuad->m_aTexcoords[1] = ivec2(1024, 0);
			pQuad->m_aTexcoords[2] = ivec2(0, 1024);
			pQuad->m_aTexcoords[3] = ivec2(1024, 1024);
			pQuad->m_pPosEnv = pPosEnv;
			pQuad->m_pColorEnv = pColorEnv;
		};

		if(!Gen.m_DayNight.m_Enabled)
		{
			CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer("Sky");
			AddGradientQuad(pLayer, DayTop, DayHorizon);

			// the sun (or moon), a single sprite in the same screen space. Its
			// position is relative to the sky quad's centre, so it sits in the
			// upper right of whatever viewport the parallax 0 group covers.
			if(Gen.m_aSunImage[0])
			{
				CCreatorImage *pSunImage = FindCreatorImage(Gen.m_aSunImage);
				if(pSunImage)
				{
					CCreatorLayerQuads *pSunLayer = pGroup->AddQuadsLayer(Gen.m_aSunImage);
					pSunLayer->m_pImage = pSunImage;
					CCreatorQuad *pSun = pSunLayer->AddQuad(vec2(430.0f, -170.0f), vec2(170.0f, 170.0f));
					pSun->m_aTexcoords[0] = ivec2(0, 0);
					pSun->m_aTexcoords[1] = ivec2(1024, 0);
					pSun->m_aTexcoords[2] = ivec2(0, 1024);
					pSun->m_aTexcoords[3] = ivec2(1024, 1024);
				}
			}
			return;
		}

		// ---- the generated day/night cycle ----
		const CInstruction::CDayNightConfig &DayNight = Gen.m_DayNight;
		const int DayMs = DayNight.m_DayLengthSeconds * 1000;

		// one envelope key: a cycle phase plus up to four channels
		struct SKey
		{
			float m_Phase;
			float m_aValues[4];
		};

		auto BuildEnvelope = [&](const char *pName, EEnvType Type, const array<SKey> &lKeys) -> CCreatorEnvelope *
		{
			CCreatorEnvelope *pEnv = Creator.AddEnvelope(pName, Type, true);
			if(!pEnv)
				return nullptr;
			for(int i = 0; i < lKeys.size(); i++)
			{
				CCreatorEnvPoint *pPoint = pEnv->AddEnvPoint((int) (lKeys[i].m_Phase * DayMs + 0.5f), CURVETYPE_LINEAR);
				for(int c = 0; c < 4; c++)
					pPoint->m_aValues[c] = lKeys[i].m_aValues[c];
			}
			return pEnv;
		};

		auto PushKey = [](array<SKey> &lKeys, float Phase, float C0, float C1, float C2, float C3)
		{
			SKey &Key = lKeys.emplace();
			Key.m_Phase = Phase;
			Key.m_aValues[0] = C0;
			Key.m_aValues[1] = C1;
			Key.m_aValues[2] = C2;
			Key.m_aValues[3] = C3;
		};

		// Every alpha envelope starts at phase 0 and ends at phase 1 with the
		// same value, so the loop has no jump, and every one spans the whole
		// cycle so the client gives them all the same synchronized duration.
		array<SKey> lNightAlpha, lGlowAlpha, lSunAlpha, lMoonAlpha;
		PushKey(lNightAlpha, 0.00f, 1, 1, 1, 0.50f);
		PushKey(lNightAlpha, 0.10f, 1, 1, 1, 0.00f);
		PushKey(lNightAlpha, 0.40f, 1, 1, 1, 0.00f);
		PushKey(lNightAlpha, 0.50f, 1, 1, 1, 0.50f);
		PushKey(lNightAlpha, 0.60f, 1, 1, 1, 1.00f);
		PushKey(lNightAlpha, 0.90f, 1, 1, 1, 1.00f);
		PushKey(lNightAlpha, 1.00f, 1, 1, 1, 0.50f);

		PushKey(lGlowAlpha, 0.00f, 1, 1, 1, 1.00f);
		PushKey(lGlowAlpha, 0.08f, 1, 1, 1, 0.00f);
		PushKey(lGlowAlpha, 0.42f, 1, 1, 1, 0.00f);
		PushKey(lGlowAlpha, 0.50f, 1, 1, 1, 1.00f);
		PushKey(lGlowAlpha, 0.58f, 1, 1, 1, 0.00f);
		PushKey(lGlowAlpha, 0.92f, 1, 1, 1, 0.00f);
		PushKey(lGlowAlpha, 1.00f, 1, 1, 1, 1.00f);

		PushKey(lSunAlpha, 0.00f, 1, 1, 1, 1.00f);
		PushKey(lSunAlpha, 0.50f, 1, 1, 1, 1.00f);
		PushKey(lSunAlpha, 0.55f, 1, 1, 1, 0.00f);
		PushKey(lSunAlpha, 0.95f, 1, 1, 1, 0.00f);
		PushKey(lSunAlpha, 1.00f, 1, 1, 1, 1.00f);

		PushKey(lMoonAlpha, 0.00f, 1, 1, 1, 0.00f);
		PushKey(lMoonAlpha, 0.46f, 1, 1, 1, 0.00f);
		PushKey(lMoonAlpha, 0.50f, 1, 1, 1, 1.00f);
		PushKey(lMoonAlpha, 0.95f, 1, 1, 1, 1.00f);
		PushKey(lMoonAlpha, 1.00f, 1, 1, 1, 0.00f);

		// The arc both bodies follow: rise at the left horizon at phase 0,
		// cross high at phase 0.25, set at the right horizon at phase 0.5, then
		// return below the horizon in the second half. The sun uses the first
		// half of the cycle, the moon the second, which is what swaps them.
		const float ArcX = 620.0f;
		const float ArcY = 520.0f;
		const float HorizonY = 140.0f;
		const int PosSamples = 48;
		auto SampleArc = [&](float ArcPhase, float KeyPhase, SKey &Key)
		{
			// the outbound leg sweeps left to right above the horizon, the return
			// leg sweeps right to left below it, so the two ends meet at the same
			// point and the loop never jumps
			float U, X, Sign;
			if(ArcPhase < 0.5f)
			{
				U = ArcPhase / 0.5f;
				Sign = -1.0f;
				X = -ArcX + 2.0f * ArcX * U;
			}
			else
			{
				U = (ArcPhase - 0.5f) / 0.5f;
				Sign = 1.0f;
				X = ArcX - 2.0f * ArcX * U;
			}
			Key.m_Phase = KeyPhase;
			Key.m_aValues[0] = X;
			Key.m_aValues[1] = HorizonY + Sign * ArcY * sinf(pi * U);
			Key.m_aValues[2] = 0.0f;
			Key.m_aValues[3] = 0.0f;
		};
		array<SKey> lSunPos, lMoonPos;
		for(int i = 0; i <= PosSamples; i++)
		{
			const float Phase = (float) i / (float) PosSamples;
			SKey &Sun = lSunPos.emplace();
			SampleArc(Phase, Phase, Sun);
			SKey &Moon = lMoonPos.emplace();
			// the moon is half a cycle behind: it is up while the sun is down
			SampleArc(Phase < 0.5f ? Phase + 0.5f : Phase - 0.5f, Phase, Moon);
		}

		CCreatorEnvelope *pNightAlpha = BuildEnvelope("DayNight_NightAlpha", EEnvType::Color, lNightAlpha);
		CCreatorEnvelope *pGlowAlpha = BuildEnvelope("DayNight_DawnGlow", EEnvType::Color, lGlowAlpha);
		CCreatorEnvelope *pSunAlpha = BuildEnvelope("DayNight_SunAlpha", EEnvType::Color, lSunAlpha);
		CCreatorEnvelope *pMoonAlpha = BuildEnvelope("DayNight_MoonAlpha", EEnvType::Color, lMoonAlpha);
		CCreatorEnvelope *pSunPos = BuildEnvelope("DayNight_SunPos", EEnvType::Pos, lSunPos);
		CCreatorEnvelope *pMoonPos = BuildEnvelope("DayNight_MoonPos", EEnvType::Pos, lMoonPos);

		// The night shade's alpha. It rides the same phases as the night sky so
		// the whole scene turns at the same moment, and it stays clear through
		// the middle of the day.
		array<SKey> lNightShade;
		auto PushShade = [&](float Phase, float Shade)
		{
			// rgb is unused because the quad is black; only alpha matters
			PushKey(lNightShade, Phase, 1.0f, 1.0f, 1.0f, Shade);
		};
		PushShade(0.00f, 0.25f);
		PushShade(0.10f, 0.00f);
		PushShade(0.40f, 0.00f);
		PushShade(0.50f, 0.25f);
		PushShade(0.60f, 0.55f);
		PushShade(0.90f, 0.55f);
		PushShade(1.00f, 0.25f);
		pNightShadeEnv = BuildEnvelope("DayNight_Shade", EEnvType::Color, lNightShade);

		CCreatorLayerQuads *pSkyLayer = pGroup->AddQuadsLayer("Sky");

		// the day gradient is the opaque base; the night gradient blends over it
		AddGradientQuad(pSkyLayer, DayTop, DayHorizon);
		const ColorRGBA NightTop(DayNight.m_NightSkyColor[0], DayNight.m_NightSkyColor[1],
			DayNight.m_NightSkyColor[2], DayNight.m_NightSkyColor[3]);
		const ColorRGBA NightHorizon(DayNight.m_NightHorizonColor[0], DayNight.m_NightHorizonColor[1],
			DayNight.m_NightHorizonColor[2], DayNight.m_NightHorizonColor[3]);
		CCreatorQuad *pNight = AddGradientQuad(pSkyLayer, NightTop, NightHorizon);
		pNight->m_pColorEnv = pNightAlpha;

		// the warm band that washes the sky at sunrise and sunset: two quads
		// meeting at the horizon line, each fading away from it, so the band
		// peaks softly instead of ending in a hard edge
		{
			const ColorRGBA Dawn(DayNight.m_DawnColor[0], DayNight.m_DawnColor[1],
				DayNight.m_DawnColor[2], DayNight.m_DawnColor[3]);
			ColorRGBA Clear = Dawn;
			Clear.a = 0;

			const float BandHeight = 300.0f;
			CCreatorQuad *pUpper = pSkyLayer->AddQuad(vec2(0.0f, HorizonY - BandHeight * 0.5f), vec2(SkySize, BandHeight));
			pUpper->m_aColors[0] = Clear;
			pUpper->m_aColors[1] = Clear;
			pUpper->m_aColors[2] = Dawn;
			pUpper->m_aColors[3] = Dawn;
			pUpper->m_pColorEnv = pGlowAlpha;

			CCreatorQuad *pLower = pSkyLayer->AddQuad(vec2(0.0f, HorizonY + BandHeight * 0.5f), vec2(SkySize, BandHeight));
			pLower->m_aColors[0] = Dawn;
			pLower->m_aColors[1] = Dawn;
			pLower->m_aColors[2] = Clear;
			pLower->m_aColors[3] = Clear;
			pLower->m_pColorEnv = pGlowAlpha;
		}

		AddBodyQuad(Gen.m_aSunImage, (float) DayNight.m_SunSize, pSunPos, pSunAlpha);
		AddBodyQuad(DayNight.m_aMoonImage, (float) DayNight.m_MoonSize, pMoonPos, pMoonAlpha);
	};

	// emits one configured layer as a group. Terrain layers take their tiles
	// from the generated world, decoration layers scatter their quads.
	auto EmitLayer = [&](const CInstruction::CLayerConfig &Layer)
	{
		if(!Layer.m_Enabled)
			return;

		CCreatorGroupInfo *pGroup = Creator.AddGroup(Layer.m_aName);
		// On a ring the decorative layers are identical in every instance. A
		// horizontal parallax would shift that identical picture by
		// stride * parallax / 100 on each crossing, which is the visible jump:
		// pinning it to the screen makes the neighbours line up exactly. The
		// vertical parallax is kept (a crossing does not change y) and the
		// cloud drift still animates the layer.
		const bool ScreenAnchored = Ring.m_Enabled && !Layer.m_IsTerrain;
		const int ParallaxX = ScreenAnchored ? 0 : Layer.m_ParallaxX;
		pGroup->m_ParallaxX = ParallaxX;
		pGroup->m_ParallaxY = Layer.m_ParallaxY;
		// group offsets are fixed point pixels (value / 32 is the tile count),
		// which is how the official maps store them
		pGroup->m_OffsetX = Layer.m_OffsetX * 32;
		pGroup->m_OffsetY = Layer.m_OffsetY * 32;

		if(Layer.m_IsTerrain)
		{
			// the terrain: the generated tiles, drawn with the floor's tileset
			if(pMainLayer)
			{
				CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer(Layer.m_aName);
				CTile *pTiles = pLayer->AddTiles(pMainLayer->m_Width, pMainLayer->m_Height);
				mem_copy(pTiles, pMainLayer->m_pTiles, sizeof(CTile) * pMainLayer->m_Width * pMainLayer->m_Height);
				if(lCreatorImages.size() && lCreatorImages[0])
					pLayer->m_pImage = lCreatorImages[0];
			}
			return;
		}

		// The drift is one synchronized position envelope per drifting layer. It
		// runs from zero to exactly one world width; because the layer is emitted
		// three times, the end of the envelope is the same picture as its start, so
		// the client can wrap it without a visible jump.
		CCreatorEnvelope *pDriftEnv = nullptr;
		if(Layer.m_Drift && Gen.m_Clouds.m_DriftEnabled)
		{
			// nearer layers (higher parallax) cross faster, so the cloud decks
			// keep their depth as they drift
			const int Para = maximum(1, Layer.m_ParallaxX);
			const int DriftMs = maximum(1000, Gen.m_Clouds.m_DriftSeconds * 1000 * 5 / Para);

			char aEnvName[32];
			str_format(aEnvName, sizeof(aEnvName), "CloudDrift_%s", Layer.m_aName);
			pDriftEnv = Creator.AddEnvelope(aEnvName, EEnvType::Pos, true);
			if(pDriftEnv)
			{
				const float Distance = (float) Gen.m_Clouds.m_Direction * (float) Width * 32.0f;
				CCreatorEnvPoint *pStart = pDriftEnv->AddEnvPoint(0, CURVETYPE_LINEAR);
				pStart->m_aValues[0] = 0.0f;
				pStart->m_aValues[1] = 0.0f;
				pStart->m_aValues[2] = 0.0f;
				CCreatorEnvPoint *pEnd = pDriftEnv->AddEnvPoint(DriftMs, CURVETYPE_LINEAR);
				pEnd->m_aValues[0] = Distance;
				pEnd->m_aValues[1] = 0.0f;
				pEnd->m_aValues[2] = 0.0f;
			}
		}

		// a decoration layer: every pass becomes its own quad layer in the group
		for(int q = 0; q < Layer.m_Quads.size(); q++)
		{
			const CInstruction::CQuadPassConfig &Pass = Layer.m_Quads[q];
			CCreatorImage *pImage = Pass.m_aImage[0] ? FindCreatorImage(Pass.m_aImage) : nullptr;
			if(Pass.m_aImage[0] && !pImage)
			{
				MAPGEN_LOG("mapgen", "layer '%s': image '%s' is missing from mapres/, skipping that pass", Layer.m_aName, Pass.m_aImage);
				continue;
			}

			CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer(Layer.m_aName);
			pLayer->m_pImage = pImage;

			// A drifting layer is emitted at -world, 0 and +world: the three
			// copies make it exactly periodic over the world width, so the
			// envelope can be wrapped without a visible seam. A band pass already
			// covers the camera's whole reach, so it needs no copies.
			const int Copies = (!Pass.m_Band && (Layer.m_Drift || Layer.m_Wrap)) ? 3 : 1;
			const float WorldWidthPx = (float) Width * 32.0f;

			// The horizontal range the copies are spread over. By default it is
			// the whole world. A band pass instead uses the strip the group can
			// ever show: its centre travels from 0 to Width * parallax/100 as the
			// camera crosses the world, and the widest client window adds a
			// margin on each side. Spreading the copies over a strip that starts
			// left of 0 keeps every quad edge outside the visible area.
			float ScatterStartTiles = 0.0f;
			float ScatterSpanTiles = (float) Width;
			if(Pass.m_Band)
			{
				const float GroupSpanPx = (float) Width * 32.0f * (float) maximum(1, ParallaxX) / 100.0f;
				const float ViewMarginPx = 2048.0f;
				ScatterStartTiles = -ViewMarginPx / 32.0f;
				ScatterSpanTiles = (GroupSpanPx + 2.0f * ViewMarginPx) / 32.0f;
			}
			const int PerPass = maximum(1, Pass.m_Count);
			for(int i = 0; i < PerPass * Copies; i++)
			{
				const int n = i % PerPass;
				const float CopyShift = Copies > 1 ? (float) (i / PerPass - 1) * WorldWidthPx : 0.0f;
				const unsigned Hash = DeterministicHash(Pass.m_SeedOffset, n * 1000 + q);
				const unsigned Hash2 = DeterministicHash(Pass.m_SeedOffset + 31337, n * 1000 + q);

				// spread the copies across the chosen range with jitter, so they
				// do not line up in a regular grid.
				const float Slot = ScatterSpanTiles / (float) maximum(1, Pass.m_Count);
				const float Jitter = Slot > 1.0f ? (float) (Hash % 1000u) / 1000.0f * Slot * 0.8f : 0.0f;
				const float CentreX = (ScatterStartTiles + n * Slot + Jitter) * 32.0f + CopyShift;

				// The y is a *screen* coordinate, not a world one.
				//
				// The renderer places a group at quad - camera * parallax/100,
				// so a group at parallax 100 moves with the world (a quad's y is
				// a world height) while one at low parallax is anchored to the
				// screen: at parallax 0 the quad ignores the camera entirely.
				// Authoring a world height into a parallax 3 group therefore
				// pushes it thousands of pixels off screen, which is exactly what
				// happened before.
				//
				// The anchor is measured from where the ground line sits on
				// screen, so scenery keeps its place over the horizon: the gap
				// between the two shrinks by parallax/100 as the camera moves,
				// which is the drift that gives the layer its depth.
				const float Para = (float) maximum(1, Layer.m_ParallaxY);
				const float ScreenGround = (float) GroundY * 32.0f * (Para / 100.0f);
				float CentreY = ScreenGround + (float) Pass.m_AnchorY * 32.0f;
				if(Pass.m_SpreadY != 0)
					CentreY += ((float) (Hash2 % 1000u) / 1000.0f - 0.5f) * (float) Pass.m_SpreadY * 32.0f;

				const vec2 Size((float) Pass.m_ScaleX * 32.0f, (float) Pass.m_ScaleY * 32.0f);
				CCreatorQuad *pQuad = pLayer->AddQuad(vec2(CentreX, CentreY), Size,
					ColorRGBA(Pass.m_ColorTop[0], Pass.m_ColorTop[1], Pass.m_ColorTop[2], Pass.m_ColorTop[3]));

				// the vertical tint gradient: clouds fade out towards one edge,
				// which stops them ending in a hard horizontal line
				pQuad->m_aColors[0] = ColorRGBA(Pass.m_ColorTop[0], Pass.m_ColorTop[1], Pass.m_ColorTop[2], Pass.m_ColorTop[3]);
				pQuad->m_aColors[1] = pQuad->m_aColors[0];
				pQuad->m_aColors[2] = ColorRGBA(Pass.m_ColorBottom[0], Pass.m_ColorBottom[1], Pass.m_ColorBottom[2], Pass.m_ColorBottom[3]);
				pQuad->m_aColors[3] = pQuad->m_aColors[2];

				if(pImage)
				{
					// Texcoords are in 1024 units per full texture, so 0..1024
					// is "all of it" whatever the pixel size; only the repeat
					// count scales the texture across the quad.
					const int U = 1024 * maximum(1, Pass.m_RepeatX);
					const int V = 1024 * maximum(1, Pass.m_RepeatY);
					pQuad->m_aTexcoords[0] = ivec2(0, 0);
					pQuad->m_aTexcoords[1] = ivec2(U, 0);
					pQuad->m_aTexcoords[2] = ivec2(0, V);
					pQuad->m_aTexcoords[3] = ivec2(U, V);
				}

				if(pDriftEnv)
					pQuad->m_pPosEnv = pDriftEnv;
			}
		}
	};

	// 1. the sky, then every decorative background layer. Terrain layers are
	//    skipped here: they belong in the Game group below, because the game
	//    layer has to sit directly on top of them.
	EmitSky();
	for(int i = 0; i < Gen.m_Layers.size(); i++)
		if(Gen.m_Layers[i].m_Role == CInstruction::LAYERROLE_BACKGROUND && !Gen.m_Layers[i].m_IsTerrain)
			EmitLayer(Gen.m_Layers[i]);

	// 1b. The structure art group has to be created BEFORE the Game group:
	// groups are drawn in file order, so this puts the structures behind the
	// terrain and its cave wall. A structure that is partly buried in a hill is
	// then hidden by the ground instead of painting over it, while its visible
	// upper part still sits in front of the sky and the background layers.
	CCreatorGroupInfo *pStructGroup = nullptr;
	for(int i = 0; i < pInstruction->m_Structs.size(); i++)
	{
		if(pInstruction->m_Structs[i].m_Count <= 0)
			continue;
		pStructGroup = Creator.AddGroup("Structures");
		pStructGroup->m_ParallaxX = 100;
		pStructGroup->m_ParallaxY = 100;
		pStructGroup->m_OffsetX = 0;
		pStructGroup->m_OffsetY = 0;
		break;
	}

	// 2. the terrain and the game layer, always at parallax 100 so the collision
	//    lines up with the drawn tiles
	CCreatorLayerTilemap *pCreatorTerrain = nullptr;
	CCreatorLayerTilemap *pCreatorGame = nullptr;
	CCreatorLayerTilemap *pCreatorUnhookable = nullptr;
	CCreatorLayerTilemap *pCreatorWall = nullptr;
	CCreatorLayerTilemap *pCreatorDoodads = nullptr;
	{
		CCreatorGroupInfo *pGroup = Creator.AddGroup("Game");
		pGroup->m_ParallaxX = 100;
		pGroup->m_ParallaxY = 100;
		pGroup->m_OffsetX = 0;
		pGroup->m_OffsetY = 0;

		// The cave wall first, so it sits behind the terrain: where the terrain
		// is solid it is hidden, where a cave was carved it is what is seen.
		if(lWallTiles.size())
		{
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Background");
			CTile *pTiles = pLayer->AddTiles(SubWidth, SubHeight);
			mem_copy(pTiles, lWallTiles.base_ptr(), sizeof(CTile) * SubWidth * SubHeight);
			pLayer->m_pImage = pWallImage;
			if(WallDarkened)
				pLayer->m_Color = ColorRGBA(120, 120, 140, 255);
			pCreatorWall = pLayer;
		}

		// The terrain layer first, then the game layer, so the collision sits
		// directly on top of the tiles it belongs to. Any background layer the
		// config marked as terrain is emitted here rather than with the other
		// background layers, because it has to share this group.
		bool TerrainEmitted = false;
		for(int i = 0; i < Gen.m_Layers.size(); i++)
		{
			const CInstruction::CLayerConfig &Layer = Gen.m_Layers[i];
			if(!Layer.m_IsTerrain)
				continue;
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer(Layer.m_aName);
			if(pMainLayer)
			{
				CTile *pTiles = pLayer->AddTiles(pMainLayer->m_Width, pMainLayer->m_Height);
				mem_copy(pTiles, pMainLayer->m_pTiles, sizeof(CTile) * pMainLayer->m_Width * pMainLayer->m_Height);
			}
			if(lCreatorImages.size() && lCreatorImages[0])
				pLayer->m_pImage = lCreatorImages[0];
			pCreatorTerrain = pLayer;
			TerrainEmitted = true;
		}
		if(!TerrainEmitted && pMainLayer)
		{
			// no terrain layer was configured: emit a plainly named one so the
			// map is never left without its tiles
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Main");
			CTile *pTiles = pLayer->AddTiles(pMainLayer->m_Width, pMainLayer->m_Height);
			mem_copy(pTiles, pMainLayer->m_pTiles, sizeof(CTile) * pMainLayer->m_Width * pMainLayer->m_Height);
			if(lCreatorImages.size() && lCreatorImages[0])
				pLayer->m_pImage = lCreatorImages[0];
			pCreatorTerrain = pLayer;
		}

		// The vegetation stands in front of the terrain (it is drawn over it)
		// but behind the entrance frames, which are structures.
		if(lDoodadTiles.size())
		{
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Doodads");
			CTile *pTiles = pLayer->AddTiles(SubWidth, SubHeight);
			mem_copy(pTiles, lDoodadTiles.base_ptr(), sizeof(CTile) * SubWidth * SubHeight);
			pLayer->m_pImage = pDoodadsImage;
			pCreatorDoodads = pLayer;
		}

		if(pUnhookableLayer)
		{
			// The entrance frames draw with their own tileset when the floor
			// names one: the unhookable sheet is a different image from the rock
			// sheet, so reusing the terrain's image would map the frame's
			// indices into the wrong picture.
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Unhookable");
			CTile *pTiles = pLayer->AddTiles(pUnhookableLayer->m_Width, pUnhookableLayer->m_Height);
			mem_copy(pTiles, pUnhookableLayer->m_pTiles, sizeof(CTile) * pUnhookableLayer->m_Width * pUnhookableLayer->m_Height);

			CCreatorImage *pFrameImage = 0;
			if(pInstruction->m_Generation.m_aUnhookableTileset[0])
				pFrameImage = FindCreatorImage(pInstruction->m_Generation.m_aUnhookableTileset);
			if(!pFrameImage && lCreatorImages.size())
				pFrameImage = lCreatorImages[0]; // no dedicated sheet: the terrain's
			pLayer->m_pImage = pFrameImage;
			pCreatorUnhookable = pLayer;
		}

		if(pGameLayer)
		{
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Game");
			pLayer->m_Flags = TILESLAYERFLAG_GAME;
			CTile *pTiles = pLayer->AddTiles(pGameLayer->m_Width, pGameLayer->m_Height);
			mem_copy(pTiles, pGameLayer->m_pTiles, sizeof(CTile) * pGameLayer->m_Width * pGameLayer->m_Height);
			pCreatorGame = pLayer;
		}
	}

	// 2b. the euclidean structures: their tiles merge into the terrain and game
	//     layers above, their quads into a world-parallax group placed after the
	//     terrain, so the art sits in the world and the collision stays exact.
	PasteEuclideanStructures(pInstruction, Seed, pWorld, Storage(), m_DatafileConsoleOutput, pStructGroup,
		pCreatorTerrain, pCreatorGame, pCreatorUnhookable, pCreatorWall, pCreatorDoodads,
		lRequests, lCreatorImages);

	// 2c. Re-run the automapper on the finished terrain. A structure cuts a level
	// shelf into a slope, and without a second pass the exposed interior tiles
	// would lack the grass/edge variants the rest of the ground has. The cave
	// wall gets the same treatment so its new cut face is styled too. Structure
	// art is quads, so this pass cannot destroy it; a structure that carries its
	// own tile template should ship those tiles already styled.
	if(pCreatorTerrain && pMainSet)
	{
		if(!Mapper.Proceed(Gen.m_aMainRuleSet, pCreatorTerrain->m_pTiles,
			   pCreatorTerrain->m_Width, pCreatorTerrain->m_Height,
			   Ring.m_NoiseSeed, !Ring.m_Enabled, Gen.m_WrapMargin, nullptr, Ring.m_OffsetX))
			MAPGEN_LOG("mapgen", "automap rule set '%s' is unknown, the terrain stays unstyled", Gen.m_aMainRuleSet);
	}
	if(pCreatorWall)
	{
		if(!Mapper.Proceed(pWallSet, pCreatorWall->m_pTiles,
			   pCreatorWall->m_Width, pCreatorWall->m_Height,
			   Ring.m_NoiseSeed + 0x51ed, !Ring.m_Enabled, Gen.m_WrapMargin, nullptr, Ring.m_OffsetX))
			MAPGEN_LOG("mapgen", "cave rule set '%s' is unknown, the wall stays plain", pWallSet);
	}

	// 3. the foreground layers, only now that the game layer exists, so they are
	//    drawn on top of the terrain rather than behind it
	for(int i = 0; i < Gen.m_Layers.size(); i++)
		if(Gen.m_Layers[i].m_Role == CInstruction::LAYERROLE_FOREGROUND)
			EmitLayer(Gen.m_Layers[i]);

	// 4. the night shade, emitted as the very last group so it sits on top of
	//    the whole picture. It is a screen sized black quad at parallax 0, so it
	//    is pinned to the viewport and darkens sky, clouds, terrain, entities and
	//    the celestial bodies together as its alpha rises after sunset.
	if(pNightShadeEnv)
	{
		CCreatorGroupInfo *pGroup = Creator.AddGroup("NightShade");
		pGroup->m_ParallaxX = 0;
		pGroup->m_ParallaxY = 0;
		pGroup->m_OffsetX = 0;
		pGroup->m_OffsetY = 0;

		CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer("Shade");
		CCreatorQuad *pQuad = pLayer->AddQuad(vec2(0.0f, 0.0f), vec2(1536.0f, 1536.0f), ColorRGBA(0, 0, 0, 255));
		pQuad->m_pColorEnv = pNightShadeEnv;
	}

	// serialize the exits and the resources, then write the map
	array<char> lEntranceJson;
	{
		memory_stream<char> Stream(&lEntranceJson);
		CJsonWriter Writer(&Stream);
		Writer.BeginArray();
		for(int i = 0; i < pWorld->m_lEntrances.size(); i++)
		{
			const CGeneratedWorld::CEntrance &Entrance = pWorld->m_lEntrances[i];
			Writer.BeginObject();
			Writer.WriteAttribute("tiles");
			Writer.BeginArray();
			Writer.BeginObject();
			WriteTilesCoord(&Writer, "tiles_start_x", Entrance.m_StartX);
			WriteTilesCoord(&Writer, "tiles_start_y", Entrance.m_StartY);
			WriteTilesCoord(&Writer, "tiles_end_x", Entrance.m_EndX);
			WriteTilesCoord(&Writer, "tiles_end_y", Entrance.m_EndY);
			Writer.EndObject();
			Writer.EndArray();
			Writer.WriteAttribute("entrance");
			// A tiny fraction of the euclidean-to-euclidean openings is
			// rerouted into a non euclidean world; everything else keeps its
			// configured target. A target that is itself a generated floor gets
			// its instance seed appended, a static map (e.g. the hub Connector)
			// keeps its plain name.
			char aResolvedTarget[64];
			ResolveEntranceTarget(pInstruction, Seed, i, Entrance.m_aTarget, aResolvedTarget, sizeof(aResolvedTarget));
			EWorldKind TargetKind;
			if(GetFloorKind(aResolvedTarget, &TargetKind) && TargetKind != WORLDKIND_HUB)
			{
				char aTarget[64];
				str_format(aTarget, sizeof(aTarget), "%s_%d", aResolvedTarget, DeriveTargetSeed(Seed, aResolvedTarget));
				Writer.WriteStrValue(aTarget);
			}
			else
				Writer.WriteStrValue(aResolvedTarget);
			Writer.EndObject();
		}
		Writer.EndArray();
		lEntranceJson.add(0);
	}

	array<char> lResourceJson;
	{
		array<CResourceEntry> lEntries;
		for(int i = 0; i < pWorld->m_lResources.size(); i++)
		{
			const CGeneratedWorld::CResource &Resource = pWorld->m_lResources[i];
			CResourceEntry &Entry = lEntries.emplace();
			str_copy(Entry.m_aResId, Resource.m_aResId, sizeof(Entry.m_aResId));
			str_copy(Entry.m_aDisplay, Resource.m_aDisplay, sizeof(Entry.m_aDisplay));
			Entry.m_X = Resource.m_X;
			Entry.m_Y = Resource.m_Y;
			Entry.m_Hardness = Resource.m_Hardness;
			Entry.m_RespawnTime = Resource.m_RespawnTime;
		}
		memory_stream<char> Stream(&lResourceJson);
		CJsonWriter Writer(&Stream);
		WriteResources(&Writer, lEntries);
		lResourceJson.add(0);
	}

	array<char> lContainerJson;
	{
		memory_stream<char> Stream(&lContainerJson);
		CJsonWriter Writer(&Stream);
		WriteContainers(&Writer, pWorld->m_lContainers);
		lContainerJson.add(0);
	}

	array<char> lMobJson;
	{
		memory_stream<char> Stream(&lMobJson);
		CJsonWriter Writer(&Stream);
		WriteMobs(&Writer, pWorld->m_lMobs);
		lMobJson.add(0);
	}

	// The world geometry, as a third json item. The horizontal wrap is a
	// property of the world: a generated floor is a cylinder, a hand drawn map
	// has real edges. It goes here rather than in CMapItemInfo (fixed at version
	// 1 for 0.7 compatibility) because the game world must know it before it
	// walks the map to build collision.
	array<char> lWorldJson;
	{
		memory_stream<char> Stream(&lWorldJson);
		CJsonWriter Writer(&Stream);
		Writer.BeginObject();
		Writer.WriteAttribute("wrap_x");
		Writer.WriteIntValue(Ring.m_Enabled ? 0 : 1);
		// Ring geometry, so the game world can resolve the neighbouring instance
		// and a crossing's global offset without re-reading the floor config.
		Writer.WriteAttribute("ring");
		Writer.WriteIntValue(Ring.m_Enabled ? 1 : 0);
		Writer.WriteAttribute("ring_seed");
		Writer.WriteIntValue(Seed);
		Writer.WriteAttribute("ring_size");
		Writer.WriteIntValue(Gen.m_Ring.m_Size);
		Writer.WriteAttribute("shared_margin");
		Writer.WriteIntValue(Gen.m_Ring.m_SharedMargin);
		Writer.WriteAttribute("offset_x");
		Writer.WriteIntValue(Ring.m_OffsetX);
		Writer.WriteAttribute("period");
		Writer.WriteIntValue(Ring.m_Period);
		Writer.EndObject();
		lWorldJson.add(0);
	}

	// Build the instance in memory: the pack is what keeps it, and the caller
	// decides whether a cache file is worth writing.
	const bool Saved = Creator.SaveMapToMemory(pOut->m_Map);
	if(Saved)
		BuildGeneratedWorldMeta(lEntranceJson.base_ptr(), lEntranceJson.size(),
			lResourceJson.base_ptr(), lResourceJson.size(),
			lContainerJson.base_ptr(), lContainerJson.size(),
			lMobJson.base_ptr(), lMobJson.size(),
			lWorldJson.base_ptr(), lWorldJson.size(), pOut->m_Meta);
	delete pWorld;

	if(!Saved)
	{
		MAPGEN_LOG("mapgen", "failed to build the generated map '%s_%d'", pFloorName, Seed);
		return false;
	}

	MAPGEN_LOG("mapgen", "generated euclidean map '%s_%d' from base '%s'", pFloorName, Seed, pInstruction->m_aBaseMap);
	return true;
}
