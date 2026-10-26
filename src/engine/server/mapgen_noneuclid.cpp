/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Non euclidean floor generation: paste the hand authored structs of a base
// map onto the floor, merging the game and template layers and carrying the
// struct's own entrances and resources across.
#include <base/system.h>
#include <base/tl/stream.h>

#include <base/math.h>
#include <base/vmath.h>
#include <climits>

#include <engine/map/generatedmaps.h>
#include <engine/map/mapcreator.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>

#include <game/gamecore.h>
#include <game/mapitems.h>

#include "automap.h"
#include "mapgen.h"
#include "noise.h"
#include "worldgen.h"

#include "mapgen_internal.h"

// ---------------------------------------------------------------------------
// non euclidean floor generation
// ---------------------------------------------------------------------------

bool CMapGen::GenerateNoneuclid(const CInstruction *pInstruction, int Seed, SGeneratedMap *pOut)
{
	if(!pOut)
		return false;

	const char *pFloorName = pInstruction->m_aFloorName;
	char aBasePath[IO_MAX_PATH_LENGTH];
	str_format(aBasePath, sizeof(aBasePath), "maps/%s.map", pInstruction->m_aBaseMap);

	CDataFileReader BaseFile;
	if(!BaseFile.Open(Storage(), aBasePath, IStorage::TYPE_ALL))
	{
		dbg_msg("mapgen", "failed to open the base map '%s'", aBasePath);
		return false;
	}

	// the generated map is built with the map creator
	CMapCreator Creator(Storage(), Console());

	int GroupsStart, GroupsNum, LayersStart, LayersNum, ImagesStart, ImagesNum, EnvelopesStart, EnvelopesNum, EPointsStart, EPointsNum;
	BaseFile.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
	BaseFile.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
	BaseFile.GetType(MAPITEMTYPE_IMAGE, &ImagesStart, &ImagesNum);
	BaseFile.GetType(MAPITEMTYPE_ENVELOPE, &EnvelopesStart, &EnvelopesNum);
	BaseFile.GetType(MAPITEMTYPE_ENVPOINTS, &EPointsStart, &EPointsNum);

	// ---------------------------------------------------------------
	// convert the base map into the creator: images
	// ---------------------------------------------------------------
	array<CCreatorImage *> lCreatorImages;
	for(int i = 0; i < ImagesNum; i++)
	{
		CMapItemImage *pImage = (CMapItemImage *) BaseFile.GetItem(ImagesStart + i, 0, 0);
		if(!pImage)
		{
			lCreatorImages.add(nullptr);
			continue;
		}
		char aImageName[32];
		str_copy(aImageName, (char *) BaseFile.GetData(pImage->m_ImageName), sizeof(aImageName));
		if(pImage->m_External)
			lCreatorImages.add(Creator.AddExternalImage(aImageName, pImage->m_Width, pImage->m_Height));
		else
		{
			dbg_msg("mapgen", "base map '%s' uses embedded images, which are not supported by the generator", pInstruction->m_aBaseMap);
			lCreatorImages.add(nullptr);
		}
	}

	// ---------------------------------------------------------------
	// convert the base map into the creator: envelopes
	// ---------------------------------------------------------------
	array<CCreatorEnvelope *> lCreatorEnvs;
	if(EPointsNum > 0)
	{
		CEnvPoint *pEnvPoints = (CEnvPoint *) BaseFile.GetItem(EPointsStart, 0, 0);
		for(int i = 0; i < EnvelopesNum; i++)
		{
			CMapItemEnvelope *pEnv = (CMapItemEnvelope *) BaseFile.GetItem(EnvelopesStart + i, 0, 0);
			if(!pEnv)
				continue;
			char aEnvName[32];
			IntsToStr(pEnv->m_aName, sizeof(pEnv->m_aName) / sizeof(int), aEnvName);
			EEnvType Type = pEnv->m_Channels == 3 ? EEnvType::Pos : (pEnv->m_Channels == 4 ? EEnvType::Color : EEnvType::Sound);
			CCreatorEnvelope *pCreatorEnv = Creator.AddEnvelope(aEnvName, Type, pEnv->m_Synchronized != 0);
			for(int p = 0; p < pEnv->m_NumPoints; p++)
			{
				CEnvPoint *pPoint = &pEnvPoints[pEnv->m_StartPoint + p];
				CCreatorEnvPoint *pEnvPoint = pCreatorEnv->AddEnvPoint(pPoint->m_Time, pPoint->m_Curvetype);
				pEnvPoint->m_aValues[0] = fx2f(pPoint->m_aValues[0]);
				pEnvPoint->m_aValues[1] = fx2f(pPoint->m_aValues[1]);
				pEnvPoint->m_aValues[2] = fx2f(pPoint->m_aValues[2]);
				pEnvPoint->m_aValues[3] = fx2f(pPoint->m_aValues[3]);
			}
			lCreatorEnvs.add(pCreatorEnv);
		}
	}

	// ---------------------------------------------------------------
	// convert the base map into the creator: groups and layers, locate
	// the game layer, the template layers and the paste anchors
	// ---------------------------------------------------------------
	int GameWidth = 0, GameHeight = 0;
	CTile *pGameTiles = nullptr;

	array<CAnchor> lAnchors;
	// the red team spawn points where the configured resources are placed
	array<CAnchor> lRedSpawns;

	// resource spots contributed by the structs: a struct's own resources
	// are only placed on the red team spawn points inside that struct
	struct CStructResourceSpot
	{
		int m_X;
		int m_Y;
		int m_ConfigStart; // first index into lStructResourceConfigs
		int m_ConfigCount;
	};
	array<CResourceConfig> lStructResourceConfigs;
	array<CStructResourceSpot> lStructResourceSpots;

	// containers contributed by the structs, in map tile coordinates
	array<SContainerSpawn> lContainers;

	struct CBaseTemplate
	{
		char m_aName[32];
		int m_Width;
		int m_Height;
		CTile *m_pTiles;
	};
	array<CBaseTemplate> lBaseTemplates;

	for(int g = 0; g < GroupsNum; g++)
	{
		CMapItemGroup *pGroup = (CMapItemGroup *) BaseFile.GetItem(GroupsStart + g, 0, 0);
		if(!pGroup)
			continue;
		char aGroupName[32];
		IntsToStr(pGroup->m_aName, sizeof(pGroup->m_aName) / sizeof(int), aGroupName);
		const bool IsTemplateGroup = str_comp(aGroupName, "Template") == 0;

		CCreatorGroupInfo *pCreatorGroup = Creator.AddGroup(aGroupName);
		pCreatorGroup->m_ParallaxX = pGroup->m_ParallaxX;
		pCreatorGroup->m_ParallaxY = pGroup->m_ParallaxY;
		pCreatorGroup->m_OffsetX = pGroup->m_OffsetX;
		pCreatorGroup->m_OffsetY = pGroup->m_OffsetY;
		pCreatorGroup->m_UseClipping = pGroup->m_UseClipping != 0;
		pCreatorGroup->m_ClipX = pGroup->m_ClipX;
		pCreatorGroup->m_ClipY = pGroup->m_ClipY;
		pCreatorGroup->m_ClipW = pGroup->m_ClipW;
		pCreatorGroup->m_ClipH = pGroup->m_ClipH;

		for(int l = 0; l < pGroup->m_NumLayers; l++)
		{
			const int LayerItem = LayersStart + pGroup->m_StartLayer + l;
			if(LayerItem < 0 || LayerItem >= LayersStart + LayersNum)
				continue;
			CMapItemLayer *pLayer = (CMapItemLayer *) BaseFile.GetItem(LayerItem, 0, 0);
			if(!pLayer)
				continue;

			if(pLayer->m_Type == LAYERTYPE_TILES || pLayer->m_Type == LAYERTYPE_GAME)
			{
				CMapItemLayerTilemap *pTilemap = (CMapItemLayerTilemap *) pLayer;
				char aLayerName[32];
				IntsToStr(pTilemap->m_aName, sizeof(pTilemap->m_aName) / sizeof(int), aLayerName);

				CCreatorLayerTilemap *pNewLayer = pCreatorGroup->AddTileLayer(aLayerName);
				pNewLayer->m_Flags = pTilemap->m_Flags;
				pNewLayer->m_Color = ColorRGBA(pTilemap->m_Color.r, pTilemap->m_Color.g, pTilemap->m_Color.b, pTilemap->m_Color.a);
				if(pTilemap->m_Image >= 0 && pTilemap->m_Image < lCreatorImages.size())
					pNewLayer->m_pImage = lCreatorImages[pTilemap->m_Image];
				if(pTilemap->m_ColorEnv >= 0 && pTilemap->m_ColorEnv < lCreatorEnvs.size())
					pNewLayer->m_pColorEnv = lCreatorEnvs[pTilemap->m_ColorEnv];

				CTile *pTiles = pNewLayer->AddTiles(pTilemap->m_Width, pTilemap->m_Height);
				LoadTilesInto(&BaseFile, pTilemap->m_Data, pTiles, pTilemap->m_Width, pTilemap->m_Height);

				if(pTilemap->m_Flags & TILESLAYERFLAG_GAME)
				{
					GameWidth = pTilemap->m_Width;
					GameHeight = pTilemap->m_Height;
					pGameTiles = pTiles;

					// the flag stand entities of the base game layer mark
					// the positions where structures are pasted; the red
					// team spawn points mark where resources are placed
					for(int y = 0; y < GameHeight; y++)
					{
						for(int x = 0; x < GameWidth; x++)
						{
							const int Index = pGameTiles[y * GameWidth + x].m_Index;
							if(Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_RED || Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_BLUE)
							{
								CAnchor &Anchor = lAnchors.emplace();
								Anchor.m_Type = Index - ENTITY_OFFSET;
								Anchor.m_X = x;
								Anchor.m_Y = y;
							}
							else if(Index == ENTITY_OFFSET + ENTITY_SPAWN_RED)
							{
								CAnchor &Spawn = lRedSpawns.emplace();
								Spawn.m_X = x;
								Spawn.m_Y = y;
							}
						}
					}
				}
				else if(IsTemplateGroup)
				{
					CBaseTemplate &BaseTemplate = lBaseTemplates.emplace();
					str_copy(BaseTemplate.m_aName, aLayerName, sizeof(BaseTemplate.m_aName));
					BaseTemplate.m_Width = pTilemap->m_Width;
					BaseTemplate.m_Height = pTilemap->m_Height;
					BaseTemplate.m_pTiles = pTiles;
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				CMapItemLayerQuads *pQuadsLayer = (CMapItemLayerQuads *) pLayer;
				char aLayerName[32];
				IntsToStr(pQuadsLayer->m_aName, sizeof(pQuadsLayer->m_aName) / sizeof(int), aLayerName);

				CCreatorLayerQuads *pNewQuads = pCreatorGroup->AddQuadsLayer(aLayerName);
				if(pQuadsLayer->m_Image >= 0 && pQuadsLayer->m_Image < lCreatorImages.size())
					pNewQuads->m_pImage = lCreatorImages[pQuadsLayer->m_Image];
				if(pQuadsLayer->m_NumQuads > 0)
				{
					CQuad *pSrcQuads = (CQuad *) BaseFile.GetData(pQuadsLayer->m_Data);
					for(int q = 0; q < pQuadsLayer->m_NumQuads; q++)
					{
						CCreatorQuad *pQuad = pNewQuads->AddQuad(
							vec2(fx2f(pSrcQuads[q].m_aPoints[4].x), fx2f(pSrcQuads[q].m_aPoints[4].y)),
							vec2(32.0f, 32.0f));
						for(int k = 0; k < 4; k++)
						{
							pQuad->m_aPoints[k] = ivec2(pSrcQuads[q].m_aPoints[k].x, pSrcQuads[q].m_aPoints[k].y);
							pQuad->m_aTexcoords[k] = ivec2(pSrcQuads[q].m_aTexcoords[k].x, pSrcQuads[q].m_aTexcoords[k].y);
							pQuad->m_aColors[k] = ColorRGBA(
								pSrcQuads[q].m_aColors[k].r, pSrcQuads[q].m_aColors[k].g,
								pSrcQuads[q].m_aColors[k].b, pSrcQuads[q].m_aColors[k].a);
						}
						pQuad->m_Pos = ivec2(pSrcQuads[q].m_aPoints[4].x, pSrcQuads[q].m_aPoints[4].y);
						if(pSrcQuads[q].m_ColorEnv >= 0 && pSrcQuads[q].m_ColorEnv < lCreatorEnvs.size())
							pQuad->m_pColorEnv = lCreatorEnvs[pSrcQuads[q].m_ColorEnv];
						if(pSrcQuads[q].m_PosEnv >= 0 && pSrcQuads[q].m_PosEnv < lCreatorEnvs.size())
							pQuad->m_pPosEnv = lCreatorEnvs[pSrcQuads[q].m_PosEnv];
					}
				}
			}
		}
	}

	// the anchor markers were only used to position the structures, so they
	// have to be removed before pasting, otherwise the pasted content that
	// lands exactly on an anchor position would be erased again
	if(pGameTiles)
	{
		for(int a = 0; a < lAnchors.size(); a++)
		{
			pGameTiles[lAnchors[a].m_Y * GameWidth + lAnchors[a].m_X].m_Index = 0;
			pGameTiles[lAnchors[a].m_Y * GameWidth + lAnchors[a].m_X].m_Flags = 0;
		}
		for(int a = 0; a < lRedSpawns.size(); a++)
		{
			pGameTiles[lRedSpawns[a].m_Y * GameWidth + lRedSpawns[a].m_X].m_Index = 0;
			pGameTiles[lRedSpawns[a].m_Y * GameWidth + lRedSpawns[a].m_X].m_Flags = 0;
		}
	}

	// the entrances carried by the struct maps, translated into the base
	// map coordinates; merged into the generated map's entrance json below
	array<CStructEntrance> lStructEntrances;

	// ---------------------------------------------------------------
	// plan the struct placements: decide first, for every anchor, which
	// struct types are rolled in, and tally the results. The struct maps
	// are only loaded below when at least one anchor actually needs them,
	// instead of loading every struct map up front and only then deciding
	// whether to paste it.
	// ---------------------------------------------------------------
	struct CStructPlacement
	{
		int m_StructIndex; // index into pInstruction->m_Structs
		int m_AnchorIndex; // index into lAnchors
	};
	array<CStructPlacement> lStructPlacements;
	array<int> lPlacementsPerStruct; // placement count per struct, parallel to pInstruction->m_Structs
	lPlacementsPerStruct.hint_size(pInstruction->m_Structs.size());
	for(int s = 0; s < pInstruction->m_Structs.size(); s++)
	{
		lPlacementsPerStruct.add(0);
	}
	{
		for(int a = 0; a < lAnchors.size(); a++)
		{
			for(int s = 0; s < pInstruction->m_Structs.size(); s++)
			{
				const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];
				// every anchor is an independent structure instance: each of structure
				// them rolls its own probability. The salt is unchanged, so
				// the generated maps stay identical to the previous version.
				if(!RollChance(Seed, (s + 1) * 1000 + (a + 1), Struct.m_GenerateProba))
					continue;
				CStructPlacement &Placement = lStructPlacements.emplace();
				Placement.m_StructIndex = s;
				Placement.m_AnchorIndex = a;
				lPlacementsPerStruct[s]++;
				break;
			}
		}

		// statistics: summarize how the anchors are distributed over the
		// struct types, so the merge step below knows what it will place
		dbg_msg("mapgen", "anchor plan for '%s': %d anchor(s) -> %d placement(s)",
			pFloorName, lAnchors.size(), lStructPlacements.size());
		for(int s = 0; s < pInstruction->m_Structs.size(); s++)
		{
			if(lPlacementsPerStruct[s] <= 0)
				continue;
			const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];
			dbg_msg("mapgen", "  %2d x '%s' (proba %3.0f%%, paste_air=%s)",
				lPlacementsPerStruct[s], Struct.m_aBaseMap, Struct.m_GenerateProba * 100.0f,
				Struct.m_PasteAir ? "true" : "false");
		}
	}

	// The plan appended placements anchor by anchor, but the merge walks one
	// contiguous run per struct type. Stable-sort them by struct index (keeping
	// the anchor order inside a run) so the counts above line up with the runs.
	for(int i = 1; i < lStructPlacements.size(); i++)
	{
		const CStructPlacement Key = lStructPlacements[i];
		int j = i - 1;
		while(j >= 0 && lStructPlacements[j].m_StructIndex > Key.m_StructIndex)
		{
			lStructPlacements[j + 1] = lStructPlacements[j];
			j--;
		}
		lStructPlacements[j + 1] = Key;
	}

	// ---------------------------------------------------------------
	// merge the planned structs into the base layers
	// ---------------------------------------------------------------
	if(pGameTiles)
	{
		// the placements are grouped by struct index in ascending order, so
		// every struct type owns a contiguous run of lStructPlacements
		int PlacementPos = 0; // first placement of the next struct type
		for(int s = 0; s < pInstruction->m_Structs.size(); s++)
		{
			const int NumPlacements = lPlacementsPerStruct[s];
			if(NumPlacements == 0)
				continue; // the roll came up empty on every anchor, skip the map

			const int FirstPlacement = PlacementPos;
			PlacementPos += NumPlacements;

			const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];

			const int VariantIndex = PickStructVariant(Struct, Seed, (s + 1) * 100000 + 13);
			if(VariantIndex < 0)
			{
				dbg_msg("mapgen", "struct id '%s' has no usable variant", Struct.m_aId);
				continue;
			}
			const CStructVariant &Variant = Struct.m_lVariants[VariantIndex];
			const char *pStructMapName = Variant.m_aBaseMap;

			char aStructPath[IO_MAX_PATH_LENGTH];
			str_format(aStructPath, sizeof(aStructPath), "maps/struct/%s.map", pStructMapName);

			CDataFileReader StructFile;
			if(!StructFile.Open(Storage(), aStructPath, IStorage::TYPE_ALL))
			{
				dbg_msg("mapgen", "failed to open the struct map '%s'", aStructPath);
				continue;
			}

			int SGroupsStart, SGroupsNum, SLayersStart, SLayersNum;
			StructFile.GetType(MAPITEMTYPE_GROUP, &SGroupsStart, &SGroupsNum);
			StructFile.GetType(MAPITEMTYPE_LAYER, &SLayersStart, &SLayersNum);

			CTile *pStructGameTiles = nullptr;
			int StructGameWidth = 0, StructGameHeight = 0;

			struct CStructTemplate
			{
				char m_aName[32];
				int m_Width;
				int m_Height;
				CTile *m_pTiles;
			};
			array<CStructTemplate> lStructTemplates;

			int RedX = -1, RedY = -1, BlueX = -1, BlueY = -1;
			int MinX = 1 << 30, MinY = 1 << 30, MaxX = -1, MaxY = -1;
			// red team spawn points inside the struct (struct coordinates)
			array<CAnchor> lStructRedSpawns;

			for(int g = 0; g < SGroupsNum; g++)
			{
				CMapItemGroup *pGroup = (CMapItemGroup *) StructFile.GetItem(SGroupsStart + g, 0, 0);
				if(!pGroup)
					continue;
				char aGroupName[32];
				IntsToStr(pGroup->m_aName, sizeof(pGroup->m_aName) / sizeof(int), aGroupName);
				const bool IsTemplateGroup = str_comp(aGroupName, "Template") == 0;

				for(int l = 0; l < pGroup->m_NumLayers; l++)
				{
					const int LayerItem = SLayersStart + pGroup->m_StartLayer + l;
					if(LayerItem < 0 || LayerItem >= SLayersStart + SLayersNum)
						continue;
					CMapItemLayer *pLayer = (CMapItemLayer *) StructFile.GetItem(LayerItem, 0, 0);
					if(!pLayer)
						continue;
					if(pLayer->m_Type != LAYERTYPE_TILES && pLayer->m_Type != LAYERTYPE_GAME)
						continue;

					CMapItemLayerTilemap *pTilemap = (CMapItemLayerTilemap *) pLayer;
					char aLayerName[32];
					IntsToStr(pTilemap->m_aName, sizeof(pTilemap->m_aName) / sizeof(int), aLayerName);

					CTile *pTiles = LoadTiles(&StructFile, pTilemap->m_Data, pTilemap->m_Width, pTilemap->m_Height);

					if(pTilemap->m_Flags & TILESLAYERFLAG_GAME)
					{
						pStructGameTiles = pTiles;
						StructGameWidth = pTilemap->m_Width;
						StructGameHeight = pTilemap->m_Height;
						ScanStructTiles(pTiles, pTilemap->m_Width, pTilemap->m_Height, &RedX, &RedY, &BlueX, &BlueY, &MinX, &MinY, &MaxX, &MaxY, &lStructRedSpawns);
					}
					else if(IsTemplateGroup)
					{
						CStructTemplate &StructTemplate = lStructTemplates.emplace();
						str_copy(StructTemplate.m_aName, aLayerName, sizeof(StructTemplate.m_aName));
						StructTemplate.m_Width = pTilemap->m_Width;
						StructTemplate.m_Height = pTilemap->m_Height;
						StructTemplate.m_pTiles = pTiles;
						ScanStructTiles(pTiles, pTilemap->m_Width, pTilemap->m_Height, &RedX, &RedY, &BlueX, &BlueY, &MinX, &MinY, &MaxX, &MaxY);
					}
					else
					{
						delete[] pTiles;
					}
				}
			}

			if(!pStructGameTiles)
			{
				dbg_msg("mapgen", "the struct map '%s' has no game layer, skipping", pStructMapName);
				for(int st = 0; st < lStructTemplates.size(); st++)
					delete[] lStructTemplates[st].m_pTiles;
				continue;
			}

			// the struct may carry its own entrance definitions, configured
			// in worlds.json (struct coordinates)
			array<CStructEntranceRect> lStructEntranceRects;
			if(Struct.m_EntrancesJson.size())
			{
				const int StoredLen = Struct.m_EntrancesJson.size();
				char *pStored = new char[StoredLen + 1];
				if(StoredLen > 0)
					mem_copy(pStored, Struct.m_EntrancesJson.base_ptr(), StoredLen);
				pStored[StoredLen] = 0;
				ParseStructEntrances(pStored, lStructEntranceRects);
				delete[] pStored;
			}

			// the struct's own resources are only placed on the red team
			// spawn points inside the struct; remember the config range for
			// this struct (empty structs contribute nothing)
			const int StructConfigStart = lStructResourceConfigs.size();
			if(Struct.m_ResourcesJson.size())
			{
				const int StoredLen = Struct.m_ResourcesJson.size();
				char *pStored = new char[StoredLen + 1];
				if(StoredLen > 0)
					mem_copy(pStored, Struct.m_ResourcesJson.base_ptr(), StoredLen);
				pStored[StoredLen] = 0;
				ParseResources(pStored, lStructResourceConfigs);
				delete[] pStored;
			}
			const int StructConfigCount = lStructResourceConfigs.size() - StructConfigStart;

			// corners of the struct content: the struct's own flag stands
			// define them, otherwise fall back to the content bounding box.
			// The red flag is placed at (x-1, y+1) of the content's
			// bottom-left corner and the blue flag at (x+1, y-1) of the
			// content's top-right corner (a game layer tile cannot overlap
			// the content), so the anchors are shifted back accordingly.
			const bool HasContent = MaxX >= 0 && MaxY >= 0;
			const int BottomLeftX = RedX >= 0 ? RedX + 1 : (HasContent ? MinX : 0);
			const int BottomLeftY = RedY >= 0 ? RedY - 1 : (HasContent ? MaxY : StructGameHeight - 1);
			const int TopRightX = BlueX >= 0 ? BlueX - 1 : (HasContent ? MaxX : 0);
			const int TopRightY = BlueY >= 0 ? BlueY + 1 : (HasContent ? MinY : 0);

			// the top-left corner of the struct region: the red flag defines
			// the left edge, the blue flag the top edge; without any flag the
			// content bounding box is used
			const int StructLeftX = RedX >= 0 ? BottomLeftX : (HasContent ? MinX : 0);
			const int StructTopY = BlueX >= 0 ? TopRightY : (HasContent ? MinY : 0);

			// the struct region (source coordinates) that gets pasted: the
			// red flag defines the bottom-left corner and the blue flag the
			// top-right corner of the region; without flags the content
			// bounding box is used. Only this region is merged, the rest of
			// the struct canvas stays transparent.
			const int RegionX1 = StructLeftX;
			const int RegionY1 = StructTopY;
			const int RegionX2 = BlueX >= 0 ? TopRightX : (HasContent ? MaxX : StructGameWidth - 1);
			const int RegionY2 = RedY >= 0 ? BottomLeftY : (HasContent ? MaxY : StructGameHeight - 1);

			int MergedAnchors = 0;
			for(int p = FirstPlacement; p < PlacementPos; p++)
			{
				// paste the pre-planned placements of this struct type
				const CAnchor &Anchor = lAnchors[lStructPlacements[p].m_AnchorIndex];
				const int CornerX = Anchor.m_Type == ENTITY_FLAGSTAND_RED ? BottomLeftX : TopRightX;
				const int CornerY = Anchor.m_Type == ENTITY_FLAGSTAND_RED ? BottomLeftY : TopRightY;
				const int OffsetX = Anchor.m_X - CornerX;
				const int OffsetY = Anchor.m_Y - CornerY;

				// merge the game layer: the struct region fully defines its
				// tiles (air included), overwriting the base
				PasteTiles(pGameTiles, GameWidth, GameHeight, pStructGameTiles, StructGameWidth, StructGameHeight,
					OffsetX, OffsetY, Struct.m_PasteAir, RegionX1, RegionY1, RegionX2, RegionY2, true);

				// merge the layers with the same name under the "Template"
				// group; inside the struct region the layer is fully defined
				// as well (air included), outside it stays transparent
				for(int st = 0; st < lStructTemplates.size(); st++)
				{
					for(int bt = 0; bt < lBaseTemplates.size(); bt++)
					{
						if(str_comp(lStructTemplates[st].m_aName, lBaseTemplates[bt].m_aName) == 0)
						{
							PasteTiles(lBaseTemplates[bt].m_pTiles, lBaseTemplates[bt].m_Width, lBaseTemplates[bt].m_Height,
								lStructTemplates[st].m_pTiles, lStructTemplates[st].m_Width, lStructTemplates[st].m_Height,
								OffsetX, OffsetY, Struct.m_PasteAir, RegionX1, RegionY1, RegionX2, RegionY2);
						}
					}
				}

				// The variant's containers go on a reachable floor spot inside
				// the pasted region. The base game tiles now carry the merged
				// collision, so the scan can tell a floor from a wall.
				if(Variant.m_NumContainers > 0 && pGameTiles)
				{
					const int DestX1 = RegionX1 + OffsetX;
					const int DestY1 = RegionY1 + OffsetY;
					const int DestX2 = RegionX2 + OffsetX;
					const int DestY2 = RegionY2 + OffsetY;
					int aUsedX[CStructVariant::MAX_CONTAINERS];
					int aUsedY[CStructVariant::MAX_CONTAINERS];
					int NumUsed = 0;
					for(int c = 0; c < Variant.m_NumContainers; c++)
					{
						const int PreferredX = DestX1 + ((DestX2 - DestX1 + 1) * (c + 1)) / (Variant.m_NumContainers + 1);
						int SpotX = 0;
						int SpotY = 0;
						if(!FindContainerSpot(pGameTiles, GameWidth, GameHeight,
							   DestX1, DestY1, DestX2, DestY2, PreferredX, &SpotX, &SpotY))
						{
							dbg_msg("mapgen", "struct '%s': no floor spot for its '%s' container",
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

						SContainerSpawn &Spawn = lContainers.emplace();
						// stable per (struct entry, placement, container index)
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

				// the struct's own entrances are offsets relative to the
				// top-left corner of the struct region (defined by the red
				// flag as the left edge and the blue flag as the top edge,
				// or the content bounding box when there are no flags):
				// entrance position = struct top-left + paste offset + offset
				for(int e = 0; e < lStructEntranceRects.size(); e++)
				{
					const CStructEntranceRect &Rect = lStructEntranceRects[e];
					CStructEntrance &Entrance = lStructEntrances.emplace();
					Entrance.m_StartX = Rect.m_StartX == INT_MIN ? INT_MIN : StructLeftX + OffsetX + Rect.m_StartX;
					Entrance.m_StartY = Rect.m_StartY == INT_MIN ? INT_MIN : StructTopY + OffsetY + Rect.m_StartY;
					Entrance.m_EndX = Rect.m_EndX == INT_MAX ? INT_MAX : StructLeftX + OffsetX + Rect.m_EndX;
					Entrance.m_EndY = Rect.m_EndY == INT_MAX ? INT_MAX : StructTopY + OffsetY + Rect.m_EndY;
					str_copy(Entrance.m_aTarget, Rect.m_aTarget, sizeof(Entrance.m_aTarget));
				}

				// the struct's resources go to the red spawn points inside
				// the pasted struct, translated into base map coordinates
				if(StructConfigCount > 0)
				{
					for(int rs = 0; rs < lStructRedSpawns.size(); rs++)
					{
						CStructResourceSpot &Spot = lStructResourceSpots.emplace();
						Spot.m_X = lStructRedSpawns[rs].m_X + OffsetX;
						Spot.m_Y = lStructRedSpawns[rs].m_Y + OffsetY;
						Spot.m_ConfigStart = StructConfigStart;
						Spot.m_ConfigCount = StructConfigCount;
					}
				}
				MergedAnchors++;
			}

			if(MergedAnchors > 0)
			{
				char aLogName[96];
				if(Struct.m_aId[0])
					str_format(aLogName, sizeof(aLogName), "%s/%s", Struct.m_aId,
						Variant.m_aSubId[0] ? Variant.m_aSubId : pStructMapName);
				else
					str_copy(aLogName, pStructMapName, sizeof(aLogName));
				dbg_msg("mapgen", "merged struct '%s' into '%s' (%d anchor(s))", aLogName, pFloorName, MergedAnchors);
			}

			delete[] pStructGameTiles;
			for(int st = 0; st < lStructTemplates.size(); st++)
				delete[] lStructTemplates[st].m_pTiles;
		}
	}

	// ---------------------------------------------------------------
	// serialize the entrances (base + struct) into the json item
	// ---------------------------------------------------------------
	array<char> lEntranceJson;
	{
		const int StoredLen = pInstruction->m_DefaultEntrances.size();
		char *pStored = new char[StoredLen + 1];
		if(StoredLen > 0)
			mem_copy(pStored, pInstruction->m_DefaultEntrances.base_ptr(), StoredLen);
		pStored[StoredLen] = 0;

		CJsonParser Parser;
		json_value *pEntrances = Parser.ParseString(pStored, "entrances");
		delete[] pStored;

		memory_stream<char> Stream(&lEntranceJson);
		CJsonWriter JsonWriter(&Stream);
		WriteEntrancesWithSeed(&JsonWriter, pEntrances ? *pEntrances : json_value_none, Seed, lStructEntrances);
		lEntranceJson.add(0); // null terminate
	}

	// ---------------------------------------------------------------
	// place the configured resources on the red team spawn points and
	// serialize them into a second json item (id 1)
	// ---------------------------------------------------------------
	array<CResourceEntry> lResources;
	{
		// base resources go to the base map's red team spawn points
		array<CResourceConfig> lBaseResourceConfigs;
		if(pInstruction->m_ResourcesJson.size())
		{
			const int StoredLen = pInstruction->m_ResourcesJson.size();
			char *pStored = new char[StoredLen + 1];
			if(StoredLen > 0)
				mem_copy(pStored, pInstruction->m_ResourcesJson.base_ptr(), StoredLen);
			pStored[StoredLen] = 0;
			ParseResources(pStored, lBaseResourceConfigs);
			delete[] pStored;
		}

		// roll a config at one spot; the salts keep base and struct rolls
		// deterministic and distinct from each other
		auto RollResource = [&](const CResourceConfig &Config, int X, int Y, int SpotIndex, int SaltBase)
		{
			if(!RollChance(Seed, SaltBase + SpotIndex * 100 + 0, Config.m_Proba))
				return;
			int Hardness = Config.m_HardnessMin;
			const int Range = Config.m_HardnessMax - Config.m_HardnessMin + 1;
			if(Range > 0)
				Hardness += (int) (DeterministicHash(Seed, SaltBase + 10000 + SpotIndex * 100 + 0) % (unsigned) Range);

			int RespawnTime = Config.m_RespawnTimeMin;
			const int RespawnRange = Config.m_RespawnTimeMax - Config.m_RespawnTimeMin + 1;
			if(RespawnRange > 0)
				RespawnTime += (int) (DeterministicHash(Seed, SaltBase + 20000 + SpotIndex * 100 + 0) % (unsigned) RespawnRange);

			CResourceEntry &Entry = lResources.emplace();
			str_copy(Entry.m_aResId, Config.m_aResId, sizeof(Entry.m_aResId));
			str_copy(Entry.m_aDisplay, Config.m_aDisplay, sizeof(Entry.m_aDisplay));
			Entry.m_X = X;
			Entry.m_Y = Y;
			Entry.m_Hardness = Hardness;
			Entry.m_RespawnTime = RespawnTime;
		};

		// base map resources on the base red team spawn points
		for(int r = 0; r < lRedSpawns.size(); r++)
		{
			for(int c = 0; c < lBaseResourceConfigs.size(); c++)
				RollResource(lBaseResourceConfigs[c], lRedSpawns[r].m_X, lRedSpawns[r].m_Y, r, 10000);
		}

		// struct resources only on the red spawn points inside the structs
		for(int s = 0; s < lStructResourceSpots.size(); s++)
		{
			const CStructResourceSpot &Spot = lStructResourceSpots[s];
			for(int c = 0; c < Spot.m_ConfigCount; c++)
				RollResource(lStructResourceConfigs[Spot.m_ConfigStart + c], Spot.m_X, Spot.m_Y, s, 40000);
		}
	}
	array<char> lResourceJson;
	{
		memory_stream<char> Stream(&lResourceJson);
		CJsonWriter JsonWriter(&Stream);
		WriteResources(&JsonWriter, lResources);
		lResourceJson.add(0); // null terminate
	}

	array<char> lContainerJson;
	{
		memory_stream<char> Stream(&lContainerJson);
		CJsonWriter JsonWriter(&Stream);
		WriteContainers(&JsonWriter, lContainers);
		lContainerJson.add(0); // null terminate
	}

	// ---------------------------------------------------------------
	// build the generated map in memory: the pack stores it, and a cache file
	// is only written when the caller asks for one
	// ---------------------------------------------------------------
	if(!Creator.SaveMapToMemory(pOut->m_Map))
	{
		dbg_msg("mapgen", "failed to build the generated map '%s_%d'", pFloorName, Seed);
		return false;
	}
	BuildGeneratedWorldMeta(lEntranceJson.base_ptr(), lEntranceJson.size(),
		lResourceJson.base_ptr(), lResourceJson.size(),
		lContainerJson.base_ptr(), lContainerJson.size(),
		nullptr, 0, pOut->m_Meta);

	dbg_msg("mapgen", "generated map '%s_%d' from base '%s'", pFloorName, Seed, pInstruction->m_aBaseMap);
	return true;
}
