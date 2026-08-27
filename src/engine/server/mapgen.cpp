#include <base/tl/stream.h>
#include <base/system.h>

#include <base/math.h>
#include <base/vmath.h>
#include <climits>

#include <engine/map/mapcreator.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>

#include <game/gamecore.h>
#include <game/mapitems.h>

#include "mapgen.h"

static void WriteJsonValue(CJsonWriter *pWriter, const json_value &rValue);
static void WriteJsonArray(CJsonWriter *pWriter, const json_value &rArray);
static void WriteJsonObject(CJsonWriter *pWriter, const json_value &rObject);

void WriteJsonValue(CJsonWriter *pWriter, const json_value &rValue)
{
	switch(rValue.type)
	{
		case json_object: WriteJsonObject(pWriter, rValue); break;
		case json_array: WriteJsonArray(pWriter, rValue); break;
		case json_integer: pWriter->WriteInt64Value((int64) rValue.u.integer); break;
		case json_double: break;
		case json_string: pWriter->WriteStrValue(rValue.u.string.ptr); break;
		case json_boolean: pWriter->WriteBoolValue(rValue.u.boolean); break;
		default: break;
	}
}

void WriteJsonArray(CJsonWriter *pWriter, const json_value &rArray)
{
	pWriter->BeginArray();
	for(unsigned i = 0; i < rArray.u.array.length; ++i)
	{
		WriteJsonValue(pWriter, *rArray.u.array.values[i]);
	}
	pWriter->EndArray();
}

void WriteJsonObject(CJsonWriter *pWriter, const json_value &rObject)
{
	pWriter->BeginObject();
	for(unsigned i = 0; i < rObject.u.object.length; ++i)
	{
		pWriter->WriteAttribute(rObject.u.object.values[i].name);
		WriteJsonValue(pWriter, *rObject.u.object.values[i].value);
	}
	pWriter->EndObject();
}

// decodes the tile data into an existing buffer (e.g. a layer of the map
// creator)
static void LoadTilesInto(CDataFileReader *pDataFile, int DataIndex, CTile *pTiles, int Width, int Height)
{
	const int Count = Width * Height;
	const CTile *pSavedTiles = (const CTile *) pDataFile->GetData(DataIndex);
	int i = 0;
	while(i < Count)
	{
		for(unsigned Counter = 0; Counter <= (unsigned) pSavedTiles->m_Skip && i < Count; Counter++)
		{
			pTiles[i] = *pSavedTiles;
			pTiles[i++].m_Skip = 0;
		}
		pSavedTiles++;
	}
}

// decodes the (possibly skip compressed) tile data of a tile layer into a
// plain Width*Height array. The caller owns the returned memory.
static CTile *LoadTiles(CDataFileReader *pDataFile, int DataIndex, int Width, int Height)
{
	CTile *pTiles = new CTile[Width * Height];
	LoadTilesInto(pDataFile, DataIndex, pTiles, Width, Height);
	return pTiles;
}

// pastes the tiles of the source layer into the destination layer at the
// given offset. Only the tiles inside the struct region (source coordinates
// RegionX1..RegionX2, RegionY1..RegionY2) are pasted; the rest of the struct
// canvas stays transparent. When PasteAir is set, empty source tiles are
// pasted as well (the game layer fully defines its region, overwriting the
// base), otherwise they are treated as transparent. The flag stand markers
// are never pasted (they are only anchors for the merge).
static void PasteTiles(CTile *pDst, int DstWidth, int DstHeight, const CTile *pSrc, int SrcWidth, int SrcHeight,
	int OffsetX, int OffsetY, bool PasteAir, int RegionX1, int RegionY1, int RegionX2, int RegionY2)
{
	for(int y = RegionY1; y <= RegionY2; y++)
	{
		if(y < 0 || y >= SrcHeight)
			continue;
		const int DstY = y + OffsetY;
		if(DstY < 0 || DstY >= DstHeight)
			continue;
		for(int x = RegionX1; x <= RegionX2; x++)
		{
			if(x < 0 || x >= SrcWidth)
				continue;
			const int DstX = x + OffsetX;
			if(DstX < 0 || DstX >= DstWidth)
				continue;
			const CTile &SrcTile = pSrc[y * SrcWidth + x];
			if(!PasteAir && SrcTile.m_Index == 0)
				continue;
			if(SrcTile.m_Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_RED || SrcTile.m_Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_BLUE)
				continue;
			pDst[DstY * DstWidth + DstX] = SrcTile;
			pDst[DstY * DstWidth + DstX].m_Skip = 0;
		}
	}
}

// scans a struct layer and records the positions of the red/blue flag stand
// anchors as well as the bounding box of the actual (non empty, non anchor)
// content. The bounding box is accumulated over all game/template layers.
static void ScanStructTiles(const CTile *pTiles, int Width, int Height, int *pRedX, int *pRedY, int *pBlueX, int *pBlueY, int *pMinX, int *pMinY, int *pMaxX, int *pMaxY)
{
	for(int y = 0; y < Height; y++)
	{
		for(int x = 0; x < Width; x++)
		{
			const int Index = pTiles[y * Width + x].m_Index;
			if(Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_RED)
			{
				*pRedX = x;
				*pRedY = y;
			}
			else if(Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_BLUE)
			{
				*pBlueX = x;
				*pBlueY = y;
			}
			else if(Index != 0)
			{
				if(x < *pMinX) *pMinX = x;
				if(x > *pMaxX) *pMaxX = x;
				if(y < *pMinY) *pMinY = y;
				if(y > *pMaxY) *pMaxY = y;
			}
		}
	}
}

// deterministic probability roll in percent
static bool RollChance(int Seed, int Salt, int Probability)
{
	if(Probability <= 0)
		return false;
	if(Probability >= 100)
		return true;
	unsigned h = (unsigned) Seed * 0x9E3779B1u;
	h ^= (unsigned) Salt * 0x85EBCA6Bu;
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	return (h % 100) < (unsigned) Probability;
}

// an entrance defined inside a struct map: the coordinates are offsets
// relative to the paste anchor (INT_MIN / INT_MAX mean "ignore" on that
// axis); the final position is anchor + offset
struct CStructEntranceRect
{
	int m_StartX, m_StartY, m_EndX, m_EndY;
	char m_aTarget[64];
};

// an entrance translated to the base map coordinates
struct CStructEntrance
{
	int m_StartX, m_StartY, m_EndX, m_EndY;
	char m_aTarget[64];
};

// parses a single struct entrance entry ({ "tiles": [...], "entrance": "..." })
// into struct-relative offsets
static void ParseStructEntranceEntry(const json_value &rEntrance, array<CStructEntranceRect> &lOut)
{
	if(rEntrance.type != json_object)
		return;
	const json_value &rTarget = rEntrance["entrance"];
	if(rTarget.type != json_string)
		return;
	const json_value &rTiles = rEntrance["tiles"];
	for(unsigned t = 0; t < rTiles.u.array.length; t++)
	{
		const json_value &rRect = rTiles[t];
		if(rRect.type != json_object)
			continue;
		const json_value &rStartX = rRect["tiles_start_x"];
		const json_value &rStartY = rRect["tiles_start_y"];
		const json_value &rEndX = rRect["tiles_end_x"];
		const json_value &rEndY = rRect["tiles_end_y"];
		CStructEntranceRect &Rect = lOut.emplace();
		Rect.m_StartX = rStartX.type == json_string ? INT_MIN : (int) (json_int_t) rStartX;
		Rect.m_StartY = rStartY.type == json_string ? INT_MIN : (int) (json_int_t) rStartY;
		Rect.m_EndX = rEndX.type == json_string ? INT_MAX : (int) (json_int_t) rEndX;
		Rect.m_EndY = rEndY.type == json_string ? INT_MAX : (int) (json_int_t) rEndY;
		str_copy(Rect.m_aTarget, rTarget.u.string.ptr, sizeof(Rect.m_aTarget));
	}
}

// parses the entrance json of a struct configuration; the entrances may be
// given as a single object or as an array of objects
static void ParseStructEntrances(const char *pJsonData, array<CStructEntranceRect> &lOut)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "entrances");
	if(!pJson)
		return;
	if(pJson->type == json_array)
	{
		for(unsigned i = 0; i < pJson->u.array.length; i++)
			ParseStructEntranceEntry((*pJson)[i], lOut);
	}
	else if(pJson->type == json_object)
	{
		ParseStructEntranceEntry(*pJson, lOut);
	}
}

// writes a tile coordinate as json: "ignore" for the unrestricted axes
static void WriteTilesCoord(CJsonWriter *pWriter, const char *pName, int Value)
{
	pWriter->WriteAttribute(pName);
	if(Value == INT_MIN || Value == INT_MAX)
		pWriter->WriteStrValue("ignore");
	else
		pWriter->WriteIntValue(Value);
}

// serializes the entrance array, appending "_<seed>" to every entrance map
// name so the generated worlds chain into the seeded worlds
// derives the instance seed of an exit target from the current world's
// seed and the target map name: a floor has multiple instances (seeds) and
// the exit randomly points to one of them, decided by the world's seed
static int DeriveTargetSeed(int Seed, const char *pTarget)
{
	unsigned h = (unsigned) Seed * 0x9E3779B1u;
	for(const char *p = pTarget; *p; p++)
	{
		h ^= (unsigned char) *p;
		h *= 0x1000193u;
	}
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	// 1 is the floor backbone, so the exits use the instances 1..64
	return 1 + (int) (h % 64);
}

void CMapGen::WriteEntrancesWithSeed(CJsonWriter *pWriter, const json_value &rEntrances, int Seed, const array<CStructEntrance> &lStructEntrances)
{
	pWriter->BeginArray();
	if(rEntrances.type == json_array)
	{
		for(unsigned i = 0; i < rEntrances.u.array.length; i++)
		{
			const json_value &rEntrance = rEntrances[i];
			if(rEntrance.type != json_object)
				continue;
			pWriter->BeginObject();
			pWriter->WriteAttribute("tiles");
			WriteJsonValue(pWriter, rEntrance["tiles"]);
			pWriter->WriteAttribute("entrance");
			const json_value &rTarget = rEntrance["entrance"];
			if(rTarget.type == json_string)
			{
				// only the generated worlds get a seed appended; static
				// maps (e.g. the main world Connector) keep their plain name
				if(m_lInstructions.get(str_quickhash(rTarget.u.string.ptr)))
				{
					char aTarget[64];
					str_format(aTarget, sizeof(aTarget), "%s_%d", rTarget.u.string.ptr, DeriveTargetSeed(Seed, rTarget.u.string.ptr));
					pWriter->WriteStrValue(aTarget);
				}
				else
				{
					pWriter->WriteStrValue(rTarget.u.string.ptr);
				}
			}
			else
			{
				WriteJsonValue(pWriter, rTarget);
			}
			pWriter->EndObject();
		}
	}

	// the entrances carried by the struct maps themselves (already
	// translated to base coordinates per paste instance)
	for(int e = 0; e < lStructEntrances.size(); e++)
	{
		const CStructEntrance &Entrance = lStructEntrances[e];
		pWriter->BeginObject();
		pWriter->WriteAttribute("tiles");
		pWriter->BeginArray();
		pWriter->BeginObject();
		WriteTilesCoord(pWriter, "tiles_start_x", Entrance.m_StartX);
		WriteTilesCoord(pWriter, "tiles_start_y", Entrance.m_StartY);
		WriteTilesCoord(pWriter, "tiles_end_x", Entrance.m_EndX);
		WriteTilesCoord(pWriter, "tiles_end_y", Entrance.m_EndY);
		pWriter->EndObject();
		pWriter->EndArray();
		pWriter->WriteAttribute("entrance");
		if(m_lInstructions.get(str_quickhash(Entrance.m_aTarget)))
		{
			char aTarget[64];
			str_format(aTarget, sizeof(aTarget), "%s_%d", Entrance.m_aTarget, DeriveTargetSeed(Seed, Entrance.m_aTarget));
			pWriter->WriteStrValue(aTarget);
		}
		else
		{
			pWriter->WriteStrValue(Entrance.m_aTarget);
		}
		pWriter->EndObject();
	}
	pWriter->EndArray();
}

CMapGen::CMapGen(IStorage *pStorage, IConsole *pConsole)
{
	m_pStorage = pStorage;
	m_pConsole = pConsole;

	// load maps instruction
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile("maps/worlds.json", pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_array)
	{
		dbg_msg("mapgen", "failed to load 'maps/worlds.json'");
		return;
	}
	const json_value &rBasesArray = *pJson;
	for(unsigned index = 0; index < rBasesArray.u.array.length; index++)
	{
		const json_value &rBase = rBasesArray[index];
		if(rBase.type != json_object)
			continue;
		CInstruction *pFloor = new CInstruction();
		str_copy(pFloor->m_aBaseMap, rBase["base"], sizeof(pFloor->m_aBaseMap));

		const json_value &rStructsArray = rBase["structs"];
		for(unsigned j = 0; j < rStructsArray.u.array.length; j++)
		{
			const json_value &rStruct = rStructsArray[j];
			if(rStruct.type != json_object)
				continue;
			CInstruction::CStruct &Struct = pFloor->m_Structs.emplace();
			str_copy(Struct.m_aBaseMap, rStruct["base"], sizeof(Struct.m_aBaseMap));
			Struct.m_GenerateProba = (int) (json_int_t) rStruct["proba"];

			// the struct may carry its own entrances, configured here in
			// worlds.json (a single object or an array); serialize them for
			// later use
			const json_value &rStructEntrances = rStruct["entrances"];
			if(rStructEntrances.type == json_array || rStructEntrances.type == json_object)
			{
				memory_stream<char> StructStream(&Struct.m_EntrancesJson);
				CJsonWriter StructWriter(&StructStream);
				WriteJsonValue(&StructWriter, rStructEntrances);
			}
		}

		memory_stream<char> Stream(&pFloor->m_DefaultEntrances);
		CJsonWriter Writer(&Stream);
		WriteJsonValue(&Writer, rBase["entrances"]);

		m_lInstructions.set(str_quickhash(pFloor->m_aBaseMap), pFloor);
	}
}

CMapGen::~CMapGen()
{
	m_lInstructions.for_each(FreeInstruction, nullptr);
}

void CMapGen::FreeInstruction(CInstruction *&pInstruction, void *pUser)
{
	delete pInstruction;
	pInstruction = nullptr;
}

bool CMapGen::RequestNewMap(const char *pBaseMap, int Seed)
{
	CInstruction **ppInstruction = m_lInstructions.get(str_quickhash(pBaseMap));
	CInstruction *pInstruction = ppInstruction ? *ppInstruction : nullptr;
	if(!pInstruction)
	{
		dbg_msg("mapgen", "the base map '%s' has no generation instructions", pBaseMap);
		return false;
	}

	char aBasePath[IO_MAX_PATH_LENGTH];
	str_format(aBasePath, sizeof(aBasePath), "maps/%s.map", pBaseMap);

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
			dbg_msg("mapgen", "base map '%s' uses embedded images, which are not supported by the generator", pBaseMap);
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

	struct CAnchor
	{
		int m_Type; // ENTITY_FLAGSTAND_RED / ENTITY_FLAGSTAND_BLUE
		int m_X;
		int m_Y;
	};
	array<CAnchor> lAnchors;

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
					// the positions where structures are pasted
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
	}

	// the entrances carried by the struct maps, translated into the base
	// map coordinates; merged into the generated map's entrance json below
	array<CStructEntrance> lStructEntrances;

	// ---------------------------------------------------------------
	// merge the structs into the base layers
	// ---------------------------------------------------------------
	if(pGameTiles)
	{
		for(int s = 0; s < pInstruction->m_Structs.size(); s++)
		{
			const CInstruction::CStruct &Struct = pInstruction->m_Structs[s];

			char aStructPath[IO_MAX_PATH_LENGTH];
			str_format(aStructPath, sizeof(aStructPath), "maps/struct/%s.map", Struct.m_aBaseMap);

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
						ScanStructTiles(pTiles, pTilemap->m_Width, pTilemap->m_Height, &RedX, &RedY, &BlueX, &BlueY, &MinX, &MinY, &MaxX, &MaxY);
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
				dbg_msg("mapgen", "the struct map '%s' has no game layer, skipping", Struct.m_aBaseMap);
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
			for(int a = 0; a < lAnchors.size(); a++)
			{
				// every anchor is an independent structure instance: each of
				// them rolls its own probability
				if(!RollChance(Seed, (s + 1) * 1000 + (a + 1), Struct.m_GenerateProba))
					continue;

				const CAnchor &Anchor = lAnchors[a];
				const int CornerX = Anchor.m_Type == ENTITY_FLAGSTAND_RED ? BottomLeftX : TopRightX;
				const int CornerY = Anchor.m_Type == ENTITY_FLAGSTAND_RED ? BottomLeftY : TopRightY;
				const int OffsetX = Anchor.m_X - CornerX;
				const int OffsetY = Anchor.m_Y - CornerY;

				// merge the game layer: the struct region fully defines its
				// tiles (air included), overwriting the base
				PasteTiles(pGameTiles, GameWidth, GameHeight, pStructGameTiles, StructGameWidth, StructGameHeight,
					OffsetX, OffsetY, true, RegionX1, RegionY1, RegionX2, RegionY2);

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
								OffsetX, OffsetY, true, RegionX1, RegionY1, RegionX2, RegionY2);
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
				MergedAnchors++;
			}

			if(MergedAnchors > 0)
				dbg_msg("mapgen", "merged struct '%s' into '%s' (%d anchor(s))", Struct.m_aBaseMap, pBaseMap, MergedAnchors);

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
	// write the generated map: "<base>_<seed>.map"
	// ---------------------------------------------------------------
	Storage()->CreateFolder("generatedmaps", IStorage::TYPE_SAVE);

	char aOutName[64];
	str_format(aOutName, sizeof(aOutName), "%s_%d", pBaseMap, Seed);
	Creator.AddJsonData(lEntranceJson.base_ptr(), lEntranceJson.size());

	if(!Creator.SaveMap(EMapType::MAPTYPE_NORMAL, aOutName))
	{
		dbg_msg("mapgen", "failed to save the generated map '%s'", aOutName);
		return false;
	}

	dbg_msg("mapgen", "generated map 'generatedmaps/%s.map' from base '%s'", aOutName, pBaseMap);
	return true;
}
