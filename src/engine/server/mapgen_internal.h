/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Shared helpers of the map generator. Not installed and not part of the
// public interface: mapgen.cpp, mapgen_euclid.cpp and mapgen_noneuclid.cpp
// each include this header, and every helper has internal linkage so the
// three translation units never collide.
#ifndef ENGINE_SERVER_MAPGEN_INTERNAL_H
#define ENGINE_SERVER_MAPGEN_INTERNAL_H

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

// ---------------------------------------------------------------------------
// config parsing helpers
// ---------------------------------------------------------------------------

// reads an int field, leaving the default when the field is absent or of the
// wrong type. Note that json_value's int conversion is not itself able to
// distinguish "absent" from "0", so the type is checked explicitly.
static inline int JsonInt(const json_value &rValue, int Default)
{
	if(rValue.type != json_integer && rValue.type != json_double)
		return Default;
	return (int) (json_int_t) rValue;
}

static inline float JsonFloat(const json_value &rValue, float Default)
{
	if(rValue.type != json_integer && rValue.type != json_double)
		return Default;
	return (float) (double) rValue;
}

static inline bool JsonBool(const json_value &rValue, bool Default)
{
	if(rValue.type != json_boolean)
		return Default;
	return (bool) rValue;
}

static inline void JsonStr(const json_value &rValue, char *pOut, int OutSize, const char *pDefault)
{
	if(rValue.type == json_string)
		str_copy(pOut, rValue.u.string.ptr, OutSize);
	else
		str_copy(pOut, pDefault, OutSize);
}

// parses a noise config; every field has a default so a partially specified
// block stays usable
static inline void ParseNoise(const json_value &rValue, CInstruction::CNoiseConfig &rOut,
	int DefaultOctaves, float DefaultFrequency, int SeedOffsetDefault)
{
	rOut.m_SeedOffset = JsonInt(rValue["seed_offset"], SeedOffsetDefault);
	rOut.m_Octaves = JsonInt(rValue["octaves"], DefaultOctaves);
	rOut.m_Frequency = JsonFloat(rValue["frequency"], DefaultFrequency);
	rOut.m_Lacunarity = JsonFloat(rValue["lacunarity"], 2.0f);
	rOut.m_Persistence = JsonFloat(rValue["persistence"], 0.5f);
	rOut.m_Amplitude = JsonFloat(rValue["amplitude"], 0.0f);
}

static inline void ParseGeneration(const json_value &rValue, CInstruction::CGeneration &rOut)
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

	// The vegetation doodads default to the terrain tileset's own sheet:
	// "grass_main" pairs with "grass_doodads", which is where the shipped
	// "vegetation" doodad set's indices point. An explicit value wins, and an
	// empty one disables the layer.
	{
		char aDefaultDoodads[64] = "";
		const int TilesetLen = str_length(rOut.m_aTileset);
		if(TilesetLen > 5 && str_comp(rOut.m_aTileset + TilesetLen - 5, "_main") == 0)
			str_format(aDefaultDoodads, sizeof(aDefaultDoodads), "%.*s_doodads", TilesetLen - 5, rOut.m_aTileset);
		JsonStr(rValue["doodads_tileset"], rOut.m_aDoodadsTileset, sizeof(rOut.m_aDoodadsTileset), aDefaultDoodads);
		JsonStr(rValue["doodads_ruleset"], rOut.m_aDoodadsRuleset, sizeof(rOut.m_aDoodadsRuleset), "vegetation");
	}

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

	// the sun (or moon) quad; empty disables it
	JsonStr(rValue["sun_image"], rOut.m_aSunImage, sizeof(rOut.m_aSunImage), "sun");

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
		Ore.m_SurfaceProba = JsonInt(rOre["surface_proba"], 0);
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

static inline void WriteJsonValue(CJsonWriter *pWriter, const json_value &rValue);
static inline void WriteJsonArray(CJsonWriter *pWriter, const json_value &rArray);
static inline void WriteJsonObject(CJsonWriter *pWriter, const json_value &rObject);

void WriteJsonValue(CJsonWriter *pWriter, const json_value &rValue)
{
	switch(rValue.type)
	{
		case json_object: WriteJsonObject(pWriter, rValue); break;
		case json_array: WriteJsonArray(pWriter, rValue); break;
		case json_integer: pWriter->WriteInt64Value((int64) rValue.u.integer); break;
		case json_double: pWriter->WriteDoubleValue(rValue.u.dbl); break;
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
static inline void LoadTilesInto(CDataFileReader *pDataFile, int DataIndex, CTile *pTiles, int Width, int Height)
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
static inline CTile *LoadTiles(CDataFileReader *pDataFile, int DataIndex, int Width, int Height)
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
static inline void PasteTiles(CTile *pDst, int DstWidth, int DstHeight, const CTile *pSrc, int SrcWidth, int SrcHeight,
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
static inline void ScanStructTiles(const CTile *pTiles, int Width, int Height, int *pRedX, int *pRedY, int *pBlueX, int *pBlueY, int *pMinX, int *pMinY, int *pMaxX, int *pMaxY, array<CAnchor> *pRedSpawns = 0)
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
static inline unsigned DeterministicHash(int Seed, int Salt)
{
	unsigned h = (unsigned) Seed * 0x9E3779B1u;
	h ^= (unsigned) Salt * 0x85EBCA6Bu;
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	return h;
}

static inline bool RollChance(int Seed, int Salt, int Probability)
{
	if(Probability <= 0)
		return false;
	if(Probability >= 100)
		return true;
	return (DeterministicHash(Seed, Salt) % 100) < (unsigned) Probability;
}

// deterministic 1-in-N roll, matching the "random" field of the automapper
static inline bool RollOneIn(int Seed, int Salt, int Chance)
{
	if(Chance <= 1)
		return true;
	return (DeterministicHash(Seed, Salt) % (unsigned) Chance) == 0;
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
static inline void ParseResources(const char *pJsonData, array<CResourceConfig> &lOut)
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
static inline void WriteResources(CJsonWriter *pWriter, const array<CResourceEntry> &lEntries)
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
static inline void ParseStructEntranceEntry(const json_value &rEntrance, array<CStructEntranceRect> &lOut)
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
static inline void ParseStructEntrances(const char *pJsonData, array<CStructEntranceRect> &lOut)
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
static inline void WriteTilesCoord(CJsonWriter *pWriter, const char *pName, int Value)
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
static inline int DeriveTargetSeed(int Seed, const char *pTarget)
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


#endif // ENGINE_SERVER_MAPGEN_INTERNAL_H
