/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <base/tl/stream.h>

#include <base/math.h>
#include <base/vmath.h>
#include <climits>

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

const char *WorldKindName(EWorldKind Kind)
{
	switch(Kind)
	{
		case WORLDKIND_EUCLID: return "euclid";
		case WORLDKIND_HUB: return "hub";
		case WORLDKIND_NONEUCLID: return "noneuclid";
	}
	return "unknown";
}

EWorldKind WorldKindFromString(const char *pName, bool *pValid)
{
	if(pValid)
		*pValid = true;
	if(!pName || !pName[0])
	{
		if(pValid)
			*pValid = false;
		return WORLDKIND_EUCLID;
	}
	if(str_comp(pName, "euclid") == 0)
		return WORLDKIND_EUCLID;
	if(str_comp(pName, "hub") == 0)
		return WORLDKIND_HUB;
	if(str_comp(pName, "noneuclid") == 0 || str_comp(pName, "non-euclid") == 0)
		return WORLDKIND_NONEUCLID;

	if(pValid)
		*pValid = false;
	return WORLDKIND_EUCLID;
}

// ---------------------------------------------------------------------------
// config parsing helpers
// ---------------------------------------------------------------------------

// reads an int field, leaving the default when the field is absent or of the
// wrong type. Note that json_value's int conversion is not itself able to
// distinguish "absent" from "0", so the type is checked explicitly.
static int JsonInt(const json_value &rValue, int Default)
{
	if(rValue.type != json_integer && rValue.type != json_double)
		return Default;
	return (int) (json_int_t) rValue;
}

static float JsonFloat(const json_value &rValue, float Default)
{
	if(rValue.type != json_integer && rValue.type != json_double)
		return Default;
	return (float) (double) rValue;
}

static bool JsonBool(const json_value &rValue, bool Default)
{
	if(rValue.type != json_boolean)
		return Default;
	return (bool) rValue;
}

static void JsonStr(const json_value &rValue, char *pOut, int OutSize, const char *pDefault)
{
	if(rValue.type == json_string)
		str_copy(pOut, rValue.u.string.ptr, OutSize);
	else
		str_copy(pOut, pDefault, OutSize);
}

// parses a noise config; every field has a default so a partially specified
// block stays usable
static void ParseNoise(const json_value &rValue, CInstruction::CNoiseConfig &rOut,
	int DefaultOctaves, float DefaultFrequency, int SeedOffsetDefault)
{
	rOut.m_SeedOffset = JsonInt(rValue["seed_offset"], SeedOffsetDefault);
	rOut.m_Octaves = JsonInt(rValue["octaves"], DefaultOctaves);
	rOut.m_Frequency = JsonFloat(rValue["frequency"], DefaultFrequency);
	rOut.m_Lacunarity = JsonFloat(rValue["lacunarity"], 2.0f);
	rOut.m_Persistence = JsonFloat(rValue["persistence"], 0.5f);
	rOut.m_Amplitude = JsonFloat(rValue["amplitude"], 0.0f);
}

static void ParseGeneration(const json_value &rValue, CInstruction::CGeneration &rOut)
{
	mem_zero(&rOut, sizeof(rOut));
	rOut.m_Enabled = rValue.type == json_object;
	if(!rOut.m_Enabled)
		return;

	rOut.m_Width = JsonInt(rValue["width"], 450);
	rOut.m_Height = JsonInt(rValue["height"], 200);
	rOut.m_SurfaceLevel = JsonInt(rValue["surface_level"], rOut.m_Height / 2);
	rOut.m_SurfaceAmplitude = JsonFloat(rValue["surface_amplitude"], 38.0f);
	rOut.m_EdgeWall = JsonInt(rValue["edge_wall"], 2);

	// how many columns at each end of the x axis stay out of the cylindrical
	// wrap; see m_WrapMargin. Clamped so the mirror bands can never meet and
	// leave no cylindrical region at all.
	rOut.m_WrapMargin = clamp(JsonInt(rValue["wrap_margin"], 2), 0, rOut.m_Width / 4);

	// how far the ground eases from the flat seam back to its natural shape; see
	// m_WrapBlend. Never shorter than the margin it has to grow out of.
	rOut.m_WrapBlend = clamp(JsonInt(rValue["wrap_blend"], 16), rOut.m_WrapMargin, rOut.m_Width / 2);

	ParseNoise(rValue["surface_noise"], rOut.m_Surface, 5, 0.008f, 0);

	// the tileset the terrain is drawn with, and the automap rule sets that
	// style it. The rule set names are the keys defined in automap/*.json
	// ("grass", "cave", "winter", "desert", ...).
	JsonStr(rValue["tileset"], rOut.m_aTileset, sizeof(rOut.m_aTileset), "grass_main");
	JsonStr(rValue["main_ruleset"], rOut.m_aMainRuleSet, sizeof(rOut.m_aMainRuleSet), "grass");
	JsonStr(rValue["cave_ruleset"], rOut.m_aCaveRuleSet, sizeof(rOut.m_aCaveRuleSet), "");

	// the entrance frame's own tileset and rule set; empty reuses the terrain's
	JsonStr(rValue["unhookable_tileset"], rOut.m_aUnhookableTileset, sizeof(rOut.m_aUnhookableTileset), "");
	JsonStr(rValue["unhookable_ruleset"], rOut.m_aUnhookableRuleSet, sizeof(rOut.m_aUnhookableRuleSet), "");

	// the sky colour: the official maps paint it as a quad with no texture
	{
		const json_value &rSky = rValue["sky_color"];
		rOut.m_SkyColor[0] = JsonInt(rSky[0], 94);
		rOut.m_SkyColor[1] = JsonInt(rSky[1], 132);
		rOut.m_SkyColor[2] = JsonInt(rSky[2], 174);
		rOut.m_SkyColor[3] = JsonInt(rSky[3], 255);

		// the horizon colour the sky fades to at the bottom of the screen; the
		// default is the pale tint the official maps use
		const json_value &rHorizon = rValue["sky_horizon_color"];
		rOut.m_SkyHorizonColor[0] = JsonInt(rHorizon[0], 204);
		rOut.m_SkyHorizonColor[1] = JsonInt(rHorizon[1], 232);
		rOut.m_SkyHorizonColor[2] = JsonInt(rHorizon[2], 255);
		rOut.m_SkyHorizonColor[3] = JsonInt(rHorizon[3], 255);
	}

	// The layers to emit, in order. Each entry says what the layer is for (role)
	// and how it moves (parallax), so the generator can place it correctly
	// relative to the terrain and the game layer.
	const json_value &rLayers = rValue["layers"];
	for(unsigned i = 0; i < rLayers.u.array.length; i++)
	{
		const json_value &rLayer = rLayers[i];
		if(rLayer.type != json_object)
			continue;

		CInstruction::CLayerConfig &Layer = rOut.m_Layers.emplace();
		mem_zero(&Layer, sizeof(Layer));

		JsonStr(rLayer["name"], Layer.m_aName, sizeof(Layer.m_aName), "Layer");
		Layer.m_Enabled = JsonBool(rLayer["enabled"], true);
		Layer.m_IsTerrain = JsonBool(rLayer["terrain"], false);
		Layer.m_ParallaxX = JsonInt(rLayer["parallax_x"], 100);
		Layer.m_ParallaxY = JsonInt(rLayer["parallax_y"], 100);
		Layer.m_OffsetX = JsonInt(rLayer["offset_x"], 0);
		Layer.m_OffsetY = JsonInt(rLayer["offset_y"], 0);

		const char *pRole = "background";
		{
			char aRole[32];
			JsonStr(rLayer["role"], aRole, sizeof(aRole), "background");
			pRole = aRole[0] ? aRole : "background";
			if(str_comp(aRole, "foreground") == 0)
				Layer.m_Role = CInstruction::LAYERROLE_FOREGROUND;
			else
				Layer.m_Role = CInstruction::LAYERROLE_BACKGROUND;
		}
		(void) pRole;

		// the quads of this layer
		const json_value &rQuads = rLayer["quads"];
		for(unsigned q = 0; q < rQuads.u.array.length; q++)
		{
			const json_value &rQuad = rQuads[q];
			if(rQuad.type != json_object)
				continue;
			CInstruction::CQuadPassConfig &Quad = Layer.m_Quads.emplace();
			mem_zero(&Quad, sizeof(Quad));

			JsonStr(rQuad["image"], Quad.m_aImage, sizeof(Quad.m_aImage), "");
			Quad.m_Count = JsonInt(rQuad["count"], 1);
			Quad.m_ScaleX = JsonInt(rQuad["scale_x"], 16);
			Quad.m_ScaleY = JsonInt(rQuad["scale_y"], 8);
			Quad.m_RepeatX = JsonInt(rQuad["repeat_x"], 1);
			Quad.m_RepeatY = JsonInt(rQuad["repeat_y"], 1);
			Quad.m_SpreadY = JsonInt(rQuad["spread_y"], 0);
			Quad.m_AnchorY = JsonInt(rQuad["anchor_y"], 0);
			Quad.m_SeedOffset = JsonInt(rQuad["seed_offset"], 40000 + (int) i * 977 + (int) q * 131);

			const json_value &rTop = rQuad["color"];
			Quad.m_ColorTop[0] = JsonInt(rTop[0], 255);
			Quad.m_ColorTop[1] = JsonInt(rTop[1], 255);
			Quad.m_ColorTop[2] = JsonInt(rTop[2], 255);
			Quad.m_ColorTop[3] = JsonInt(rTop[3], 255);

			const json_value &rBottom = rQuad["color_bottom"];
			if(rBottom.type == json_array)
			{
				Quad.m_ColorBottom[0] = JsonInt(rBottom[0], Quad.m_ColorTop[0]);
				Quad.m_ColorBottom[1] = JsonInt(rBottom[1], Quad.m_ColorTop[1]);
				Quad.m_ColorBottom[2] = JsonInt(rBottom[2], Quad.m_ColorTop[2]);
				Quad.m_ColorBottom[3] = JsonInt(rBottom[3], Quad.m_ColorTop[3]);
			}
			else
			{
				// no gradient given: flat tint
				mem_copy(Quad.m_ColorBottom, Quad.m_ColorTop, sizeof(Quad.m_ColorTop));
			}
		}
	}

	const json_value &rCaves = rValue["caves"];
	rOut.m_Caves.m_Enabled = JsonBool(rCaves["enabled"], true);
	ParseNoise(rCaves["noise"], rOut.m_Caves.m_Noise, 3, 0.045f, 2000);
	rOut.m_Caves.m_Threshold = JsonFloat(rCaves["threshold"], 0.62f);
	rOut.m_Caves.m_MinDepth = JsonInt(rCaves["min_depth"], 6);
	rOut.m_Caves.m_SurfaceMargin = JsonInt(rCaves["surface_margin"], 3);

	const json_value &rOres = rValue["ores"];
	for(unsigned i = 0; i < rOres.u.array.length; i++)
	{
		const json_value &rOre = rOres[i];
		if(rOre.type != json_object)
			continue;
		CInstruction::COreConfig &Ore = rOut.m_Ores.emplace();
		JsonStr(rOre["res_id"], Ore.m_aResId, sizeof(Ore.m_aResId), "stone");
		JsonStr(rOre["display"], Ore.m_aDisplay, sizeof(Ore.m_aDisplay), "armor");
		JsonStr(rOre["tile"], Ore.m_aTile, sizeof(Ore.m_aTile), "grass_main");
		ParseNoise(rOre["noise"], Ore.m_Noise, 3, 0.05f, 3000 + i * 137);
		Ore.m_Threshold = JsonFloat(rOre["threshold"], 0.78f);
		Ore.m_MinDepth = JsonInt(rOre["min_depth"], 12);
		Ore.m_Probes = JsonInt(rOre["probes"], 1);
		Ore.m_Proba = JsonInt(rOre["proba"], 30);
		Ore.m_HardnessMin = JsonInt(rOre["hardness_min"], 1);
		Ore.m_HardnessMax = JsonInt(rOre["hardness_max"], 5);
		Ore.m_RespawnTimeMin = JsonInt(rOre["respawntime_min"], 60);
		Ore.m_RespawnTimeMax = JsonInt(rOre["respawntime_max"], 120);
	}

	const json_value &rEntrances = rValue["surface_entrances"];
	rOut.m_SurfaceEntrances.m_Enabled = JsonBool(rEntrances["enabled"], true);
	rOut.m_SurfaceEntrances.m_Count = JsonInt(rEntrances["count"], 3);
	rOut.m_SurfaceEntrances.m_MinSeparation = JsonInt(rEntrances["min_separation"], 60);
	rOut.m_SurfaceEntrances.m_Width = JsonInt(rEntrances["width"], 7);
	rOut.m_SurfaceEntrances.m_Height = JsonInt(rEntrances["height"], 5);
	rOut.m_SurfaceEntrances.m_Clearance = JsonInt(rEntrances["clearance"], 2);
	rOut.m_SurfaceEntrances.m_Frame = JsonInt(rEntrances["frame"], 1);
	rOut.m_SurfaceEntrances.m_Margin = JsonInt(rEntrances["margin"], 24);
}

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

// a marker position on a game layer: a flag stand anchor (where structs are
// pasted) or a red team spawn point (where resources are placed)
struct CAnchor
{
	int m_Type; // ENTITY_FLAGSTAND_RED / ENTITY_FLAGSTAND_BLUE / 0 for spawns
	int m_X;
	int m_Y;
};

// pastes the tiles of the source layer into the destination layer at the
// given offset. Only the tiles inside the struct region (source coordinates
// RegionX1..RegionX2, RegionY1..RegionY2) are pasted; the rest of the struct
// canvas stays transparent. When PasteAir is set, empty source tiles are
// pasted as well (the game layer fully defines its region, overwriting the
// base), otherwise they are treated as transparent. The flag stand markers
// are never pasted (they are only anchors for the merge).
static void PasteTiles(CTile *pDst, int DstWidth, int DstHeight, const CTile *pSrc, int SrcWidth, int SrcHeight,
	int OffsetX, int OffsetY, bool PasteAir, int RegionX1, int RegionY1, int RegionX2, int RegionY2, bool IsGameLayer = false)
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
			if(IsGameLayer && (SrcTile.m_Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_RED || SrcTile.m_Index == ENTITY_OFFSET + ENTITY_FLAGSTAND_BLUE))
				continue;
			pDst[DstY * DstWidth + DstX] = SrcTile;
			pDst[DstY * DstWidth + DstX].m_Skip = 0;
			if(IsGameLayer && SrcTile.m_Index == ENTITY_OFFSET + ENTITY_SPAWN_RED)
				pDst[DstY * DstWidth + DstX].m_Index = TILE_AIR;
		}
	}
}

// scans a struct layer and records the positions of the red/blue flag stand
// anchors as well as the bounding box of the actual (non empty, non anchor)
// content. The bounding box is accumulated over all game/template layers.
// Red team spawn points inside the struct are collected into lRedSpawns
// (struct coordinates).
static void ScanStructTiles(const CTile *pTiles, int Width, int Height, int *pRedX, int *pRedY, int *pBlueX, int *pBlueY, int *pMinX, int *pMinY, int *pMaxX, int *pMaxY, array<CAnchor> *pRedSpawns = 0)
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
			else if(Index == ENTITY_OFFSET + ENTITY_SPAWN_RED)
			{
				if(pRedSpawns)
				{
					CAnchor &Spawn = pRedSpawns->emplace();
					Spawn.m_X = x;
					Spawn.m_Y = y;
				}
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
static unsigned DeterministicHash(int Seed, int Salt)
{
	unsigned h = (unsigned) Seed * 0x9E3779B1u;
	h ^= (unsigned) Salt * 0x85EBCA6Bu;
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	return h;
}

static bool RollChance(int Seed, int Salt, int Probability)
{
	if(Probability <= 0)
		return false;
	if(Probability >= 100)
		return true;
	return (DeterministicHash(Seed, Salt) % 100) < (unsigned) Probability;
}

// a resource configured in worlds.json (base or struct)
struct CResourceConfig
{
	char m_aResId[32];
	char m_aDisplay[32];
	int m_Proba;
	int m_HardnessMin;
	int m_HardnessMax;
	int m_RespawnTimeMin;
	int m_RespawnTimeMax;
};

// a resource placed on a red team spawn point of the generated map
struct CResourceEntry
{
	char m_aResId[32];
	char m_aDisplay[32];
	int m_X;
	int m_Y;
	int m_Hardness;
	int m_RespawnTime;
};

// parses a resources json array into configs
static void ParseResources(const char *pJsonData, array<CResourceConfig> &lOut)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "resources");
	if(!pJson || pJson->type != json_array)
		return;
	for(unsigned i = 0; i < pJson->u.array.length; i++)
	{
		const json_value &rRes = (*pJson)[i];
		if(rRes.type != json_object)
			continue;
		const json_value &rId = rRes["res_id"];
		if(rId.type != json_string)
			continue;
		CResourceConfig &Config = lOut.emplace();
		str_copy(Config.m_aResId, rId.u.string.ptr, sizeof(Config.m_aResId));
		str_copy(Config.m_aDisplay, rRes["display"], sizeof(Config.m_aDisplay));
		Config.m_Proba = (int) (json_int_t) rRes["proba"];
		Config.m_HardnessMin = (int) (json_int_t) rRes["hardness_min"];
		Config.m_HardnessMax = (int) (json_int_t) rRes["hardness_max"];
		Config.m_RespawnTimeMin = (int) (json_int_t) rRes["respawntime_min"];
		Config.m_RespawnTimeMax = (int) (json_int_t) rRes["respawntime_max"];
	}
}

// serializes the placed resources into the "world internal" json
static void WriteResources(CJsonWriter *pWriter, const array<CResourceEntry> &lEntries)
{
	pWriter->BeginArray();
	for(int i = 0; i < lEntries.size(); i++)
	{
		const CResourceEntry &Entry = lEntries[i];
		pWriter->BeginObject();
		pWriter->WriteAttribute("res_id");
		pWriter->WriteStrValue(Entry.m_aResId);
		pWriter->WriteAttribute("pos_x");
		pWriter->WriteIntValue(Entry.m_X);
		pWriter->WriteAttribute("pos_y");
		pWriter->WriteIntValue(Entry.m_Y);
		pWriter->WriteAttribute("hardness");
		pWriter->WriteIntValue(Entry.m_Hardness);
		pWriter->WriteAttribute("respawntime");
		pWriter->WriteIntValue(Entry.m_RespawnTime);
		pWriter->WriteAttribute("display");
		pWriter->WriteStrValue(Entry.m_aDisplay);
		pWriter->EndObject();
	}
	pWriter->EndArray();
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
	return 1 + (int) (h % 1024);
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

	// load the floor instructions from the "maps/worlds" directory; each
	// "<floorname>.json" file holds the config of one generated floor
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/worlds", ListWorldsCallback, this);
}

int CMapGen::ListWorldsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CMapGen *pSelf = static_cast<CMapGen *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// the floor name is the file name without the ".json" suffix
	char aFloorName[64];
	str_copy(aFloorName, pFilename, sizeof(aFloorName));
	aFloorName[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "maps/worlds/%s", pFilename);
	pSelf->LoadFloor(aFloorName, aPath);
	return 0;
}

void CMapGen::LoadFloor(const char *pFloorName, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("mapgen", "failed to load floor '%s' from '%s'", pFloorName, pFilePath);
		return;
	}

	const json_value &rBase = *pJson;
	CInstruction *pFloor = new CInstruction();
	mem_zero(pFloor, sizeof(CInstruction));
	str_copy(pFloor->m_aFloorName, pFloorName, sizeof(pFloor->m_aFloorName));
	str_copy(pFloor->m_aBaseMap, rBase["base"], sizeof(pFloor->m_aBaseMap));

	// the world kind decides which generation path this floor takes. When the
	// field is missing the kind is derived from the config so that the older
	// files keep working: a floor with generation settings is euclidean, the
	// hub is recognised by its name, everything else is non euclidean.
	{
		const json_value &rKind = rBase["kind"];
		if(rKind.type == json_string)
		{
			bool Valid = false;
			pFloor->m_Kind = WorldKindFromString(rKind.u.string.ptr, &Valid);
			if(!Valid)
				dbg_msg("mapgen", "floor '%s' has an unknown kind '%s', assuming euclid", pFloorName, rKind.u.string.ptr);
		}
		else if(rBase["generation"].type == json_object)
			pFloor->m_Kind = WORLDKIND_EUCLID;
		else if(str_comp(pFloorName, "Connector") == 0)
			pFloor->m_Kind = WORLDKIND_HUB;
		else
			pFloor->m_Kind = WORLDKIND_NONEUCLID;
	}

	// the euclidean floors carry their terrain settings here; the other kinds
	// simply leave the generation block disabled
	ParseGeneration(rBase["generation"], pFloor->m_Generation);

	// a base map is only meaningful for the static kinds, which paste their
	// structure maps onto it. A euclidean floor builds its terrain from noise
	// and takes its look from the tileset and rule sets in its generation
	// block, so "base" is optional there.
	if(pFloor->m_aBaseMap[0] == '\0' && pFloor->m_Kind != WORLDKIND_EUCLID)
		dbg_msg("mapgen", "floor '%s' is %s but has no base map", pFloorName, WorldKindName(pFloor->m_Kind));

	const json_value &rStructsArray = rBase["structs"];
	for(unsigned j = 0; j < rStructsArray.u.array.length; j++)
	{
		const json_value &rStruct = rStructsArray[j];
		if(rStruct.type != json_object)
			continue;
		CInstruction::CStruct &Struct = pFloor->m_Structs.emplace();
		str_copy(Struct.m_aBaseMap, rStruct["base"], sizeof(Struct.m_aBaseMap));
		Struct.m_GenerateProba = (int) (json_int_t) rStruct["proba"];

		// whether empty struct tiles overwrite the base map inside the struct
		// region (default true, matching the previous hardcoded behavior)
		const json_value &rPasteAir = rStruct["paste_air"];
		Struct.m_PasteAir = rPasteAir.type == json_boolean ? (bool) rPasteAir : true;

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

		// the struct may carry its own resources
		const json_value &rStructResources = rStruct["resources"];
		if(rStructResources.type == json_array)
		{
			memory_stream<char> StructResStream(&Struct.m_ResourcesJson);
			CJsonWriter StructResWriter(&StructResStream);
			WriteJsonValue(&StructResWriter, rStructResources);
		}
	}

	memory_stream<char> Stream(&pFloor->m_DefaultEntrances);
	CJsonWriter Writer(&Stream);
	WriteJsonValue(&Writer, rBase["entrances"]);

	// the base may carry its own resources
	const json_value &rBaseResources = rBase["resources"];
	if(rBaseResources.type == json_array)
	{
		memory_stream<char> ResStream(&pFloor->m_ResourcesJson);
		CJsonWriter ResWriter(&ResStream);
		WriteJsonValue(&ResWriter, rBaseResources);
	}

	// keyed by the unique floor name (the file name), so that two floors
	// sharing the same base map do not collide
	m_lInstructions.set(str_quickhash(pFloor->m_aFloorName), pFloor);

	dbg_msg("mapgen", "loaded floor '%s' (kind %s, base '%s')", pFloorName, WorldKindName(pFloor->m_Kind), pFloor->m_aBaseMap);
}

bool CMapGen::GetFloorKind(const char *pFloorName, EWorldKind *pKind) const
{
	CInstruction *const *ppInstruction = m_lInstructions.get(str_quickhash(pFloorName));
	if(!ppInstruction || !*ppInstruction)
		return false;
	if(pKind)
		*pKind = (*ppInstruction)->m_Kind;
	return true;
}

// ---------------------------------------------------------------------------
// euclidean floor generation
// ---------------------------------------------------------------------------

bool CMapGen::GenerateEuclidean(const CInstruction *pInstruction, int Seed)
{
	const char *pFloorName = pInstruction->m_aFloorName;

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
			dbg_msg("mapgen", "no automap rule sets found in 'automap/', the terrain stays unstyled");
		}
		else
		{
			const char *pMainSet = pInstruction->m_Generation.m_aMainRuleSet;
			// the terrain is the only layer styled here: the game layer is
			// derived from it and the unhookable layer does not exist yet
			CGeneratedWorld::CLayer &Layer = pWorld->m_lLayers[CGeneratedWorld::LAYER_TERRAIN];
			// the world is a cylinder, so the rules sample across the seam
			// instead of hitting a wall; the outermost columns are held out of
			// that wrap so the border does not wear corner tiles
			if(!Mapper.Proceed(pMainSet, Layer.m_pTiles, Layer.m_Width, Layer.m_Height, (unsigned) Seed, true, pInstruction->m_Generation.m_WrapMargin))
				dbg_msg("mapgen", "automap rule set '%s' is unknown, the layer '%s' stays unstyled", pMainSet, Layer.m_aName);
		}
	}

	// Entrances are cut after the automapper, which resets every solid tile to
	// its basetile and would erase the frame that marks the cave mouth.
	BuildWorldEntrances(pInstruction, Seed, pWorld);

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
	struct SImageRequest
	{
		char m_aName[64];
		int m_Width;
		int m_Height;
		bool m_Optional;
	};
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
			dbg_msg("mapgen", "%s image '%s' is missing from mapres/, the layer will render without a texture",
				Req.m_Optional ? "background" : "tileset", Req.m_aName);
			lCreatorImages.add(nullptr);
			continue;
		}
		// take the real size from the file; the requested values are only the
		// fallback for a png whose header cannot be read
		int Width = Req.m_Width;
		int Height = Req.m_Height;
		if(ReadPngSize(aFound, &Width, &Height))
			dbg_msg("mapgen", "image '%s' is %dx%d", Req.m_aName, Width, Height);
		else
			dbg_msg("mapgen", "image '%s' has an unreadable png header, assuming %dx%d",
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

	// The ground line the scenery is anchored to: the highest ground in the
	// world, not the average. Scenery is drawn behind the terrain, so anchoring
	// at the average would let mountains poke through wherever the ground dips
	// below it.
	int GroundY = pInstruction->m_Generation.m_SurfaceLevel;
	if(pWorld->m_Surface.size())
	{
		GroundY = pWorld->m_Surface[0];
		for(int i = 1; i < pWorld->m_Surface.size(); i++)
			GroundY = minimum(GroundY, pWorld->m_Surface[i]);
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

	const CInstruction::CGeneration &Gen = pInstruction->m_Generation;

	// the sky: a screen sized quad at parallax 0. That pins the group to the
	// camera, so the quad only has to cover the viewport; the official maps use
	// about 1530x1534 px for a 1430x1050 screen, and the same size is used here.
	auto EmitSky = [&]()
	{
		CCreatorGroupInfo *pGroup = Creator.AddGroup("Sky");
		pGroup->m_ParallaxX = 0;
		pGroup->m_ParallaxY = 0;
		pGroup->m_OffsetX = 0;
		pGroup->m_OffsetY = 0;

		CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer("Sky");
		CCreatorQuad *pQuad = pLayer->AddQuad(vec2(0.0f, 0.0f), vec2(1536.0f, 1536.0f));

		// a vertical gradient from the zenith colour down to the horizon, which
		// is what the official skies do. Points 0/1 are the top edge, 2/3 the
		// bottom.
		const ColorRGBA Top(Gen.m_SkyColor[0], Gen.m_SkyColor[1], Gen.m_SkyColor[2], Gen.m_SkyColor[3]);
		const ColorRGBA Horizon(Gen.m_SkyHorizonColor[0], Gen.m_SkyHorizonColor[1],
			Gen.m_SkyHorizonColor[2], Gen.m_SkyHorizonColor[3]);
		pQuad->m_aColors[0] = Top;
		pQuad->m_aColors[1] = Top;
		pQuad->m_aColors[2] = Horizon;
		pQuad->m_aColors[3] = Horizon;

		// The texcoords still span the whole (unused) texture: leaving them all
		// at zero makes the renderer sample one degenerate texel, which comes
		// out black because this quad has no image on purpose.
		pQuad->m_aTexcoords[0] = ivec2(0, 0);
		pQuad->m_aTexcoords[1] = ivec2(1024, 0);
		pQuad->m_aTexcoords[2] = ivec2(0, 1024);
		pQuad->m_aTexcoords[3] = ivec2(1024, 1024);
	};

	// emits one configured layer as a group. Terrain layers take their tiles
	// from the generated world, decoration layers scatter their quads.
	auto EmitLayer = [&](const CInstruction::CLayerConfig &Layer)
	{
		if(!Layer.m_Enabled)
			return;

		CCreatorGroupInfo *pGroup = Creator.AddGroup(Layer.m_aName);
		pGroup->m_ParallaxX = Layer.m_ParallaxX;
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

		// a decoration layer: every pass becomes its own quad layer in the group
		for(int q = 0; q < Layer.m_Quads.size(); q++)
		{
			const CInstruction::CQuadPassConfig &Pass = Layer.m_Quads[q];
			CCreatorImage *pImage = Pass.m_aImage[0] ? FindCreatorImage(Pass.m_aImage) : nullptr;
			if(Pass.m_aImage[0] && !pImage)
			{
				dbg_msg("mapgen", "layer '%s': image '%s' is missing from mapres/, skipping that pass", Layer.m_aName, Pass.m_aImage);
				continue;
			}

			CCreatorLayerQuads *pLayer = pGroup->AddQuadsLayer(Layer.m_aName);
			pLayer->m_pImage = pImage;

			for(int n = 0; n < maximum(1, Pass.m_Count); n++)
			{
				const unsigned Hash = DeterministicHash(Pass.m_SeedOffset, n * 1000 + q);
				const unsigned Hash2 = DeterministicHash(Pass.m_SeedOffset + 31337, n * 1000 + q);

				// spread the copies across the world width with jitter, so they
				// do not line up in a regular grid.
				const float Slot = (float) Width / (float) maximum(1, Pass.m_Count);
				const float Jitter = Slot > 1.0f ? (float) (Hash % 1000u) / 1000.0f * Slot * 0.8f : 0.0f;
				const float CentreX = (n * Slot + Jitter) * 32.0f;

				// Placed against the ground line, not the top of the world: an
				// anchor measured from y=0 pins the scenery to the sky. This
				// keeps mountains at the horizon and clouds above it whatever
				// the floor's surface height is.
				float CentreY = (float) (GroundY + Pass.m_AnchorY) * 32.0f;
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

	// 2. the terrain and the game layer, always at parallax 100 so the collision
	//    lines up with the drawn tiles
	{
		CCreatorGroupInfo *pGroup = Creator.AddGroup("Game");
		pGroup->m_ParallaxX = 100;
		pGroup->m_ParallaxY = 100;
		pGroup->m_OffsetX = 0;
		pGroup->m_OffsetY = 0;

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
		}

		if(pGameLayer)
		{
			CCreatorLayerTilemap *pLayer = pGroup->AddTileLayer("Game");
			pLayer->m_Flags = TILESLAYERFLAG_GAME;
			CTile *pTiles = pLayer->AddTiles(pGameLayer->m_Width, pGameLayer->m_Height);
			mem_copy(pTiles, pGameLayer->m_pTiles, sizeof(CTile) * pGameLayer->m_Width * pGameLayer->m_Height);
		}
	}

	// 3. the foreground layers, only now that the game layer exists, so they are
	//    drawn on top of the terrain rather than behind it
	for(int i = 0; i < Gen.m_Layers.size(); i++)
		if(Gen.m_Layers[i].m_Role == CInstruction::LAYERROLE_FOREGROUND)
			EmitLayer(Gen.m_Layers[i]);

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
			// a target that is itself a generated floor gets its instance
			// seed appended, a static map keeps its plain name
			if(m_lInstructions.get(str_quickhash(Entrance.m_aTarget)))
			{
				char aTarget[64];
				str_format(aTarget, sizeof(aTarget), "%s_%d", Entrance.m_aTarget, DeriveTargetSeed(Seed, Entrance.m_aTarget));
				Writer.WriteStrValue(aTarget);
			}
			else
				Writer.WriteStrValue(Entrance.m_aTarget);
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

	Storage()->CreateFolder("generatedmaps", IStorage::TYPE_SAVE);

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
		Writer.WriteIntValue(1);
		Writer.EndObject();
		lWorldJson.add(0);
	}

	char aOutName[64];
	str_format(aOutName, sizeof(aOutName), "%s_%d", pFloorName, Seed);
	Creator.AddJsonData(lEntranceJson.base_ptr(), lEntranceJson.size(), 0);
	Creator.AddJsonData(lResourceJson.base_ptr(), lResourceJson.size(), 1);
	Creator.AddJsonData(lWorldJson.base_ptr(), lWorldJson.size(), 2);

	const bool Saved = Creator.SaveMap(EMapType::MAPTYPE_NORMAL, aOutName);
	delete pWorld;

	if(!Saved)
	{
		dbg_msg("mapgen", "failed to save the generated map '%s'", aOutName);
		return false;
	}

	dbg_msg("mapgen", "generated euclidean map 'generatedmaps/%s.map' from base '%s'", aOutName, pInstruction->m_aBaseMap);
	return true;
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

bool CMapGen::RequestNewMap(const char *pFloorName, int Seed)
{
	CInstruction **ppInstruction = m_lInstructions.get(str_quickhash(pFloorName));
	CInstruction *pInstruction = ppInstruction ? *ppInstruction : nullptr;
	if(!pInstruction)
	{
		dbg_msg("mapgen", "the floor '%s' has no generation instructions", pFloorName);
		return false;
	}

	// the world kind selects the generation strategy. The hub is a hand
	// authored static map which the server loads directly, so asking the
	// generator for it is a configuration error rather than a failure.
	switch(pInstruction->m_Kind)
	{
		case WORLDKIND_HUB:
			dbg_msg("mapgen", "the hub world '%s' is a static map and is not generated", pFloorName);
			return false;
		case WORLDKIND_EUCLID:
			return GenerateEuclidean(pInstruction, Seed);
		case WORLDKIND_NONEUCLID:
			break; // handled by the structure pasting path below
	}

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
			dbg_msg("mapgen", "  %2d x '%s' (proba %3d%%, paste_air=%s)",
				lPlacementsPerStruct[s], Struct.m_aBaseMap, Struct.m_GenerateProba,
				Struct.m_PasteAir ? "true" : "false");
		}
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
				dbg_msg("mapgen", "merged struct '%s' into '%s' (%d anchor(s))", Struct.m_aBaseMap, pFloorName, MergedAnchors);

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

	// ---------------------------------------------------------------
	// write the generated map: "<base>_<seed>.map"
	// ---------------------------------------------------------------
	Storage()->CreateFolder("generatedmaps", IStorage::TYPE_SAVE);

	char aOutName[64];
	str_format(aOutName, sizeof(aOutName), "%s_%d", pFloorName, Seed);
	Creator.AddJsonData(lEntranceJson.base_ptr(), lEntranceJson.size(), 0);
	Creator.AddJsonData(lResourceJson.base_ptr(), lResourceJson.size(), 1);

	if(!Creator.SaveMap(EMapType::MAPTYPE_NORMAL, aOutName))
	{
		dbg_msg("mapgen", "failed to save the generated map '%s'", aOutName);
		return false;
	}

	dbg_msg("mapgen", "generated map 'generatedmaps/%s.map' from base '%s'", aOutName, pInstruction->m_aBaseMap);
	return true;
}
