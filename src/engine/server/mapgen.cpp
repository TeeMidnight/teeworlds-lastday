/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>
#include <base/tl/algorithm.h>
#include <base/tl/stream.h>
#include <base/vmath.h>
#include <engine/engine.h>
#include <engine/map/generatedmaps.h>
#include <engine/map/mapcreator.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/mappack.h>
#include <engine/storage.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include "automap.h"
#include "mapgen.h"
#include "noise.h"
#include "worldgen.h"

#include <climits>

void GeneratedWorldMetaPath(const char *pMapName, char *pBuf, int Size)
{
	str_format(pBuf, Size, GENERATED_MAPS_DIR "/%s.json", pMapName);
}

void BuildGeneratedWorldMeta(const char *pEntrances, int EntrancesSize,
	const char *pResources, int ResourcesSize,
	const char *pContainers, int ContainersSize,
	const char *pMobs, int MobsSize,
	const char *pWorld, int WorldSize,
	array<char> &Out)
{
	Out.clear();
	memory_stream<char> Stream(&Out);

	auto WritePart = [&](const char *pData, int Size)
	{
		if(Size <= 0)
			return;
		Stream.write((const unsigned char *) pData, (unsigned) Size);
	};
	// the blobs are null terminated (and may carry more than one trailing
	// terminator); write only the JSON itself
	auto TrimSize = [](const char *pData, int Size)
	{
		if(!pData)
			return 0;
		while(Size > 0 && (pData[Size - 1] == '\0' || pData[Size - 1] == '\n' || pData[Size - 1] == '\r'))
			Size--;
		return Size;
	};
	auto WriteBlob = [&](const char *pData, int Size)
	{
		const int Len = TrimSize(pData, Size);
		if(Len > 0)
			WritePart(pData, Len);
		else
			WritePart("[]", 2);
	};

	WritePart("{\"entrances\":", (int) (sizeof("{\"entrances\":") - 1));
	WriteBlob(pEntrances, EntrancesSize);
	WritePart(",\"resources\":", (int) (sizeof(",\"resources\":") - 1));
	WriteBlob(pResources, ResourcesSize);
	WritePart(",\"containers\":", (int) (sizeof(",\"containers\":") - 1));
	WriteBlob(pContainers, ContainersSize);
	WritePart(",\"mobs\":", (int) (sizeof(",\"mobs\":") - 1));
	WriteBlob(pMobs, MobsSize);
	const int WorldLen = TrimSize(pWorld, WorldSize);
	if(WorldLen > 0)
	{
		WritePart(",\"world\":", (int) (sizeof(",\"world\":") - 1));
		WritePart(pWorld, WorldLen);
	}
	WritePart("}", 1);
}

bool SaveGeneratedWorldMeta(IStorage *pStorage, const char *pMapName,
	const char *pEntrances, int EntrancesSize,
	const char *pResources, int ResourcesSize,
	const char *pContainers, int ContainersSize,
	const char *pMobs, int MobsSize,
	const char *pWorld, int WorldSize)
{
	if(!pStorage || !pMapName || !pMapName[0])
		return false;

	array<char> lMeta;
	BuildGeneratedWorldMeta(pEntrances, EntrancesSize, pResources, ResourcesSize,
		pContainers, ContainersSize, pMobs, MobsSize, pWorld, WorldSize, lMeta);

	char aPath[IO_MAX_PATH_LENGTH];
	GeneratedWorldMetaPath(pMapName, aPath, sizeof(aPath));
	IOHANDLE File = pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	const bool Ok = io_write(File, lMeta.base_ptr(), (unsigned) lMeta.size()) == (unsigned) lMeta.size();
	io_close(File);
	return Ok;
}

bool LoadGeneratedWorldMeta(IStorage *pStorage, const char *pMapName, array<char> &OutJson)
{
	OutJson.clear();
	if(!pStorage || !pMapName || !pMapName[0])
		return false;

	char aPath[IO_MAX_PATH_LENGTH];
	GeneratedWorldMetaPath(pMapName, aPath, sizeof(aPath));

	void *pData = nullptr;
	unsigned Size = 0;
	if(!pStorage->ReadFile(aPath, IStorage::TYPE_ALL, &pData, &Size) || !pData || Size == 0)
		return false;

	OutJson.set_size((int) Size + 1);
	mem_copy(OutJson.base_ptr(), pData, Size);
	OutJson[Size] = 0;
	mem_free(pData);
	return true;
}

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

#include "mapgen_internal.h"
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
				EWorldKind TargetKind;
				if(GetFloorKind(rTarget.u.string.ptr, &TargetKind) && TargetKind != WORLDKIND_HUB)
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
		EWorldKind TargetKind;
		if(GetFloorKind(Entrance.m_aTarget, &TargetKind) && TargetKind != WORLDKIND_HUB)
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

// ---------------------------------------------------------------------------
// structure definitions
//
// One file per structure under maps/struct/<id>.json. A file owns one id and
// its variants; each variant names the containers it pastes and, for each
// container, which independent loot table it should use.
// ---------------------------------------------------------------------------
static void AddStructContainer(CStructContainer *pContainers, int *pNum, const char *pKind, const char *pLoot)
{
	if(!pKind || !pKind[0] || *pNum >= CStructVariant::MAX_CONTAINERS)
		return;
	CStructContainer &Container = pContainers[*pNum];
	str_copy(Container.m_aKind, pKind, sizeof(Container.m_aKind));
	str_copy(Container.m_aLoot, pLoot && pLoot[0] ? pLoot : "", sizeof(Container.m_aLoot));
	(*pNum)++;
}

// Accepts the current object form ({ "kind": ..., "loot": ... }) and the older
// plain kind string. A plain string has no loot table, so that container starts
// empty unless its structure file is updated.
static void ReadStructContainers(const json_value &rValue, CStructContainer *pContainers, int *pNum)
{
	if(rValue.type == json_string)
		AddStructContainer(pContainers, pNum, rValue.u.string.ptr, "");
	else if(rValue.type == json_array)
	{
		for(unsigned c = 0; c < rValue.u.array.length; c++)
		{
			const json_value &rEntry = *rValue.u.array.values[c];
			if(rEntry.type == json_string)
			{
				AddStructContainer(pContainers, pNum, rEntry.u.string.ptr, "");
				continue;
			}
			if(rEntry.type != json_object)
				continue;
			const json_value &rKind = rEntry["kind"];
			const json_value &rLoot = rEntry["loot"];
			if(rKind.type != json_string)
				continue;
			AddStructContainer(pContainers, pNum, rKind.u.string.ptr,
				rLoot.type == json_string ? rLoot.u.string.ptr : "");
		}
	}
}

static void ReadStructMobs(const json_value &rValue, CStructMob *pMobs, int *pNum)
{
	if(rValue.type != json_array)
		return;
	for(unsigned m = 0; m < rValue.u.array.length; m++)
	{
		const json_value &rEntry = rValue[m];
		if(rEntry.type != json_object || *pNum >= CStructVariant::MAX_MOBS)
			continue;
		const json_value &rMob = rEntry["mob"];
		if(rMob.type != json_string || !rMob.u.string.ptr[0])
			continue;
		CStructMob &Mob = pMobs[(*pNum)++];
		mem_zero(&Mob, sizeof(Mob));
		str_copy(Mob.m_aMob, rMob.u.string.ptr, sizeof(Mob.m_aMob));
		Mob.m_Count = maximum(1, JsonInt(rEntry["count"], 1));
		Mob.m_Boss = JsonBool(rEntry["boss"], false);
	}
}

int CMapGen::ListStructDefsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	char aDefaultId[64];
	if(Len - 5 >= (int) sizeof(aDefaultId))
		return 0;
	str_copy(aDefaultId, pFilename, sizeof(aDefaultId));
	aDefaultId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "maps/struct/%s", pFilename);
	static_cast<CMapGen *>(pUser)->LoadStructDefinition(aDefaultId, aPath);
	return 0;
}

void CMapGen::LoadStructDefinition(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("mapgen", "failed to load structure definition '%s'", pFilePath);
		return;
	}

	const json_value &rId = (*pJson)["id"];
	const char *pId = rId.type == json_string && rId.u.string.ptr[0] ? rId.u.string.ptr : pDefaultId;
	if(!pId || !pId[0])
		return;

	for(int i = 0; i < m_lStructCatalog.size(); i++)
	{
		if(str_comp(m_lStructCatalog[i].m_aId, pId) == 0)
		{
			dbg_msg("mapgen", "duplicate structure id '%s' in '%s', keeping the first", pId, pFilePath);
			return;
		}
	}

	CStructCatalogEntry &Entry = m_lStructCatalog.emplace();
	str_copy(Entry.m_aId, pId, sizeof(Entry.m_aId));
	Entry.m_GenerateProba = clamp(JsonFloat((*pJson)["proba"], 1.0f), 0.0f, 1.0f);
	Entry.m_PasteAir = JsonBool((*pJson)["paste_air"], true);
	Entry.m_Count = maximum(0, JsonInt((*pJson)["count"], 0));
	Entry.m_Margin = maximum(0, JsonInt((*pJson)["margin"], 24));
	Entry.m_MinSeparation = maximum(0, JsonInt((*pJson)["min_separation"], 48));
	Entry.m_FlatWidth = maximum(0, JsonInt((*pJson)["flat_width"], 0));
	Entry.m_SurfaceOffset = JsonInt((*pJson)["surface_offset"], 0);
	Entry.m_Unique = JsonBool((*pJson)["unique"], false);

	const json_value &rVariants = (*pJson)["variants"];
	for(unsigned v = 0; v < rVariants.u.array.length; v++)
	{
		const json_value &rVariant = rVariants[v];
		if(rVariant.type != json_object)
			continue;
		const json_value &rBase = rVariant["base"];
		if(rBase.type != json_string || !rBase.u.string.ptr[0])
			continue;

		CStructVariant &Variant = Entry.m_lVariants.emplace();
		Variant.m_aSubId[0] = 0;
		Variant.m_NumContainers = 0;
		Variant.m_NumMobs = 0;
		const json_value &rSubId = rVariant["sub_id"];
		if(rSubId.type == json_string)
			str_copy(Variant.m_aSubId, rSubId.u.string.ptr, sizeof(Variant.m_aSubId));
		str_copy(Variant.m_aBaseMap, rBase.u.string.ptr, sizeof(Variant.m_aBaseMap));
		ReadStructContainers(rVariant["containers"], Variant.m_aContainers, &Variant.m_NumContainers);
		ReadStructMobs(rVariant["mobs"], Variant.m_aMobs, &Variant.m_NumMobs);
	}

	if(!Entry.m_lVariants.size())
	{
		dbg_msg("mapgen", "structure '%s' has no usable variant", pId);
		m_lStructCatalog.remove_index(m_lStructCatalog.size() - 1);
	}
}

void CMapGen::LoadStructCatalog()
{
	m_lStructCatalog.clear();
	if(m_pStorage)
		m_pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/struct", ListStructDefsCallback, this);
	dbg_msg("mapgen", "loaded %d struct id(s) from maps/struct/*.json", m_lStructCatalog.size());
}

const CStructCatalogEntry *CMapGen::FindStructCatalog(const char *pId) const
{
	if(!pId || !pId[0])
		return nullptr;
	for(int i = 0; i < m_lStructCatalog.size(); i++)
	{
		if(str_comp(m_lStructCatalog[i].m_aId, pId) == 0)
			return &m_lStructCatalog[i];
	}
	return nullptr;
}

CMapGen::CMapGen(IStorage *pStorage, IConsole *pConsole)
{
	m_pStorage = pStorage;
	m_pConsole = pConsole;
	m_pEngine = nullptr;
	m_pGenHead = nullptr;
	m_pGenTail = nullptr;
	m_pGenRunning = nullptr;
	m_pWorkerJobs = nullptr;
	m_GenStarted = false;
	m_GenShutdown = false;
	m_GenWorkerActive = false;
	m_UseMapPacks = true;
	m_DeleteGeneratedFiles = false;

	LoadStructCatalog();

	// load the floor instructions from the "maps/worlds" directory; each
	// "<floorname>.json" file holds the config of one generated floor
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/worlds", ListWorldsCallback, this);
	RebuildFloorLists();
}

// One submitted engine-pool job. The wrapper is kept in CMapGen's list until
// the pool reports the embedded CJob as done, because the pool writes the job's
// final status after the entry point returns.
struct CMapGen::CWorkerJob
{
	CJob m_Job;
	CMapGen *m_pSelf;
	CWorkerJob *m_pNext;
};

int CMapGen::ListWorldsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CMapGen *pSelf = static_cast<CMapGen *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// the floor name is the file name without the ".json" suffix; reject a
	// name that does not fit instead of writing past the end of the buffer
	char aFloorName[64];
	if(Len - 5 >= (int) sizeof(aFloorName))
	{
		dbg_msg("mapgen", "floor file name too long: '%s'", pFilename);
		return 0;
	}
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
	// whether the travel menu may offer this euclidean floor as a train stop
	pFloor->m_TrainEnabled = JsonBool(rBase["train"], false);

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

		// Defaults match the old inline format, so a legacy entry that only
		// has "base" keeps behaving exactly as before. A catalog entry starts
		// from its own defaults below.
		Struct.m_aId[0] = 0;
		Struct.m_aSubId[0] = 0;
		Struct.m_aBaseMap[0] = 0;
		Struct.m_GenerateProba = 0.0f;
		Struct.m_PasteAir = true;
		Struct.m_Count = 0;
		Struct.m_Margin = 24;
		Struct.m_MinSeparation = 48;
		Struct.m_FlatWidth = 0;
		Struct.m_SurfaceOffset = 0;
		Struct.m_Unique = false;

		const json_value &rId = rStruct["id"];
		const json_value &rBaseValue = rStruct["base"];
		const char *pId = rId.type == json_string ? rId.u.string.ptr : "";
		const char *pBase = rBaseValue.type == json_string ? rBaseValue.u.string.ptr : "";

		auto HasNumber = [](const json_value &rValue) -> bool
		{
			return rValue.type == json_integer || rValue.type == json_double;
		};

		if(pId[0])
		{
			str_copy(Struct.m_aId, pId, sizeof(Struct.m_aId));
			const CStructCatalogEntry *pCatalogEntry = FindStructCatalog(pId);
			if(pCatalogEntry)
			{
				Struct.m_GenerateProba = pCatalogEntry->m_GenerateProba;
				Struct.m_PasteAir = pCatalogEntry->m_PasteAir;
				Struct.m_Count = pCatalogEntry->m_Count;
				Struct.m_Margin = pCatalogEntry->m_Margin;
				Struct.m_MinSeparation = pCatalogEntry->m_MinSeparation;
				Struct.m_FlatWidth = pCatalogEntry->m_FlatWidth;
				Struct.m_SurfaceOffset = pCatalogEntry->m_SurfaceOffset;
				Struct.m_Unique = pCatalogEntry->m_Unique;
				for(int v = 0; v < pCatalogEntry->m_lVariants.size(); v++)
					Struct.m_lVariants.add(pCatalogEntry->m_lVariants[v]);
				if(Struct.m_lVariants.size())
					str_copy(Struct.m_aBaseMap, Struct.m_lVariants[0].m_aBaseMap, sizeof(Struct.m_aBaseMap));
			}
			else
				dbg_msg("mapgen", "floor '%s': struct id '%s' has no definition in maps/struct", pFloorName, pId);
		}

		// A literal "base" still works. It also pins a catalog id to one map,
		// which is useful when debugging a single variant.
		if(pBase[0])
		{
			Struct.m_lVariants.clear();
			CStructVariant &Variant = Struct.m_lVariants.emplace();
			Variant.m_aSubId[0] = 0;
			Variant.m_NumContainers = 0;
			Variant.m_NumMobs = 0;
			str_copy(Variant.m_aBaseMap, pBase, sizeof(Variant.m_aBaseMap));
			str_copy(Struct.m_aBaseMap, pBase, sizeof(Struct.m_aBaseMap));
		}

		if(!Struct.m_lVariants.size())
		{
			dbg_msg("mapgen", "floor '%s': struct entry %u has no base map or catalog id, skipping", pFloorName, j);
			pFloor->m_Structs.remove_index(pFloor->m_Structs.size() - 1);
			continue;
		}

		// Optional pin: place exactly this variant of the id. Without it the
		// generator chooses a variant from the seed.
		const json_value &rVariant = rStruct["variant"];
		if(rVariant.type == json_string)
			str_copy(Struct.m_aSubId, rVariant.u.string.ptr, sizeof(Struct.m_aSubId));
		else if(rStruct["sub_id"].type == json_string)
			str_copy(Struct.m_aSubId, rStruct["sub_id"].u.string.ptr, sizeof(Struct.m_aSubId));

		// World overrides win over the catalog defaults. Only a present field
		// overrides, so a catalog default of count 0 is not clobbered by the
		// absent-field fallback.
		if(HasNumber(rStruct["proba"]))
			Struct.m_GenerateProba = clamp(JsonFloat(rStruct["proba"], Struct.m_GenerateProba), 0.0f, 1.0f);
		if(HasNumber(rStruct["count"]))
			Struct.m_Count = maximum(0, JsonInt(rStruct["count"], Struct.m_Count));
		if(HasNumber(rStruct["margin"]))
			Struct.m_Margin = maximum(0, JsonInt(rStruct["margin"], Struct.m_Margin));
		if(HasNumber(rStruct["min_separation"]))
			Struct.m_MinSeparation = maximum(0, JsonInt(rStruct["min_separation"], Struct.m_MinSeparation));
		if(HasNumber(rStruct["flat_width"]))
			Struct.m_FlatWidth = maximum(0, JsonInt(rStruct["flat_width"], Struct.m_FlatWidth));
		if(HasNumber(rStruct["surface_offset"]))
			Struct.m_SurfaceOffset = JsonInt(rStruct["surface_offset"], Struct.m_SurfaceOffset);
		const json_value &rPasteAir = rStruct["paste_air"];
		if(rPasteAir.type == json_boolean)
			Struct.m_PasteAir = (bool) rPasteAir;
		const json_value &rUnique = rStruct["unique"];
		if(rUnique.type == json_boolean)
			Struct.m_Unique = (bool) rUnique;

		// A floor can name the container kinds itself. This wins over the
		// catalog list, and is the only way an inline "base" structure (which
		// is not in the catalog) gets containers at all.
		const json_value &rStructContainers = rStruct["containers"];
		const json_value &rStructContainer = rStruct["container"];
		if(rStructContainers.type == json_string || rStructContainers.type == json_array || rStructContainer.type == json_string)
		{
			for(int v = 0; v < Struct.m_lVariants.size(); v++)
			{
				CStructVariant &Variant = Struct.m_lVariants[v];
				Variant.m_NumContainers = 0;
				ReadStructContainers(rStructContainers, Variant.m_aContainers, &Variant.m_NumContainers);
				if(Variant.m_NumContainers == 0)
					ReadStructContainers(rStructContainer, Variant.m_aContainers, &Variant.m_NumContainers);
			}
		}

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
void CMapGen::RebuildFloorLists()
{
	m_lTrainFloors.clear();
	m_lNonEuclidFloors.clear();

	m_lInstructions.for_each([](CInstruction *&pInstruction, void *pUser)
	{
		CMapGen *pSelf = static_cast<CMapGen *>(pUser);
		if(!pInstruction)
			return;
		if(pInstruction->m_Kind == WORLDKIND_EUCLID && pInstruction->m_TrainEnabled)
			pSelf->m_lTrainFloors.add(pInstruction);
		else if(pInstruction->m_Kind == WORLDKIND_NONEUCLID)
			pSelf->m_lNonEuclidFloors.add(pInstruction);
	}, this);

	const auto CompareName = [](const CInstruction *pA, const CInstruction *pB)
	{
		return str_comp(pA->m_aFloorName, pB->m_aFloorName) < 0;
	};
	sort(m_lTrainFloors.all(), CompareName);
	sort(m_lNonEuclidFloors.all(), CompareName);
}

const char *CMapGen::TrainDestinationName(int Index) const
{
	if(Index < 0 || Index >= m_lTrainFloors.size())
		return "";
	return m_lTrainFloors[Index]->m_aFloorName;
}

int CMapGen::TrainDestinationRingSize(int Index) const
{
	if(Index < 0 || Index >= m_lTrainFloors.size())
		return 0;
	const CInstruction *pFloor = m_lTrainFloors[Index];
	// The train can stop at any instance of the target floor. A ring names its
	// instances explicitly; a floor without a ring keeps the historic 1024
	// instance range that DeriveTargetSeed also uses.
	if(pFloor->m_Generation.m_Ring.m_Enabled)
		return maximum(1, pFloor->m_Generation.m_Ring.m_Size);
	return 1024;
}

void CMapGen::ResolveEntranceTarget(const CInstruction *pSource, int Seed, int EntranceIndex, const char *pConfigured, char *pOut, int OutSize) const
{
	str_copy(pOut, pConfigured, OutSize);

	if(!pSource || pSource->m_Kind != WORLDKIND_EUCLID || !m_lNonEuclidFloors.size())
		return;

	// Only a euclidean-to-euclidean exit rolls: an exit into the hub keeps
	// pointing at the hub.
	EWorldKind TargetKind;
	if(!GetFloorKind(pConfigured, &TargetKind) || TargetKind != WORLDKIND_EUCLID)
		return;

	const float Proba = clamp(pSource->m_Generation.m_SurfaceEntrances.m_NonEuclidProba, 0.0f, 1.0f);
	if(Proba <= 0.0f)
		return;

	// Deterministic per source instance and opening: regenerating the world
	// must produce the same exit, and one opening either is or is not the rare
	// non euclidean one.
	unsigned Hash = (unsigned) Seed * 0x9E3779B1u;
	Hash ^= (unsigned) (EntranceIndex + 1) * 0x85EBCA6Bu;
	Hash ^= Hash >> 16;
	Hash *= 0x7FEB352Du;
	Hash ^= Hash >> 15;
	if(HashToUnitFloat(Hash) >= Proba)
		return;

	unsigned PickHash = Hash * 0x846CA68Bu;
	PickHash ^= PickHash >> 16;
	const int Pick = (int) (PickHash % (unsigned) m_lNonEuclidFloors.size());
	str_copy(pOut, m_lNonEuclidFloors[Pick]->m_aFloorName, OutSize);
	dbg_msg("worldgen", "entrance %d of '%s' seed %d leads into the non euclidean world '%s'",
		EntranceIndex, pSource->m_aFloorName, Seed, pOut);
}

// ---------------------------------------------------------------------------
// background train chunk generation
// ---------------------------------------------------------------------------

int CMapGen::FindTrainFloor(const char *pFloorName) const
{
	if(!pFloorName || !pFloorName[0])
		return -1;
	for(int i = 0; i < m_lTrainFloors.size(); i++)
	{
		if(str_comp(m_lTrainFloors[i]->m_aFloorName, pFloorName) == 0)
			return i;
	}
	return -1;
}

int CMapGen::RollTrainSeed(int Index)
{
	const int Chunks = maximum(1, TrainDestinationRingSize(Index));
	// Match the old menu roll: the chunk numbers are 1..Chunks.
	return 1 + (int) ((unsigned) random_int() % (unsigned) Chunks);
}

// ---------------------------------------------------------------------------
// map packs
//
// One append-only pack per floor. The pack is the durable copy of every
// instance the generator ever produced for that floor: generation appends, and
// a lookup that misses on disk restores the instance from the pack instead of
// generating it again. The reader caches the pack index only, so a lookup never
// decompresses a map; GetMap decompresses exactly the one that is extracted.
// ---------------------------------------------------------------------------
struct CMapGen::SFloorPack
{
	char m_aFloor[64];
	// the file may not exist yet (a fresh server has no pack until the first
	// instance is generated), so an open failure is not cached as fatal
	bool m_Tried;
	bool m_Open;
	CMapPackReader m_Reader;
};

void CMapGen::PackPath(const char *pFloorName, char *pBuf, int Size) const
{
	str_format(pBuf, Size, MAPPACK_DIR "/%s.mappack", pFloorName);
}

CMapGen::SFloorPack *CMapGen::PackFor(const char *pFloorName)
{
	if(!m_UseMapPacks || !pFloorName || !pFloorName[0])
		return nullptr;

	for(int i = 0; i < m_lPacks.size(); i++)
		if(str_comp(m_lPacks[i]->m_aFloor, pFloorName) == 0)
			return m_lPacks[i];

	// the caller holds m_PackMutex, so a pack is never opened twice
	SFloorPack *pPack = new SFloorPack();
	str_copy(pPack->m_aFloor, pFloorName, sizeof(pPack->m_aFloor));
	pPack->m_Tried = false;
	pPack->m_Open = false;
	m_lPacks.add(pPack);
	return pPack;
}

long long CMapGen::InstanceKey(const char *pFloorName, int Seed)
{
	return ((long long) str_quickhash(pFloorName) << 32) | (unsigned) Seed;
}

void CMapGen::RememberInstance(const char *pFloorName, int Seed)
{
	if(!m_UseMapPacks || !pFloorName || !pFloorName[0] || Seed <= 0)
		return;
	scope_lock Lock(&m_GenLock);
	const long long Key = InstanceKey(pFloorName, Seed);
	if(!m_lKnownInstances.get(Key))
		m_lKnownInstances.set(Key, true);
}

void CMapGen::RememberPackInstances(SFloorPack *pPack)
{
	if(!pPack || !pPack->m_Open)
		return;
	scope_lock Lock(&m_GenLock);
	for(int i = 0; i < pPack->m_Reader.NumMaps(); i++)
	{
		const SMapPackEntry *pEntry = pPack->m_Reader.Entry(i);
		if(!pEntry || pEntry->m_Seed <= 0)
			continue;
		const long long Key = InstanceKey(pPack->m_aFloor, pEntry->m_Seed);
		if(!m_lKnownInstances.get(Key))
			m_lKnownInstances.set(Key, true);
	}
}

bool CMapGen::KnownInstance(const char *pFloorName, int Seed) const
{
	return m_lKnownInstances.get(InstanceKey(pFloorName, Seed)) != nullptr;
}

bool CMapGen::InstanceAvailable(const char *pFloorName, int Seed) const
{
	if(GeneratedMapExists(pFloorName, Seed))
		return true;
	return KnownInstance(pFloorName, Seed);
}

bool CMapGen::EnsurePackOpen(SFloorPack *pPack)
{
	// caller holds m_PackMutex; RememberPackInstances takes m_GenLock, so the
	// lock order is pack -> known set, never the other way round.
	if(!pPack)
		return false;
	if(pPack->m_Open)
		return true;
	if(pPack->m_Tried)
		return false; // the pack does not exist yet

	pPack->m_Tried = true;
	char aPath[IO_MAX_PATH_LENGTH];
	PackPath(pPack->m_aFloor, aPath, sizeof(aPath));
	pPack->m_Open = pPack->m_Reader.Open(m_pStorage, aPath, IStorage::TYPE_SAVE);
	if(!pPack->m_Open)
		return false;
	RememberPackInstances(pPack);
	return true;
}

bool CMapGen::PackContains(const char *pFloorName, int Seed)
{
	SFloorPack *pPack = PackFor(pFloorName);
	if(!pPack || !EnsurePackOpen(pPack))
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);
	return pPack->m_Reader.FindMap(aMapName) >= 0;
}

bool CMapGen::ReadFromPack(const char *pFloorName, int Seed, SMapInstance *pOut)
{
	if(!m_UseMapPacks || !pOut || !PackContains(pFloorName, Seed))
		return false;

	SFloorPack *pPack = PackFor(pFloorName);
	if(!pPack || !pPack->m_Open)
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);
	const int Index = pPack->m_Reader.FindMap(aMapName);
	const void *pMap = nullptr, *pJson = nullptr;
	int MapSize = 0, JsonSize = 0;
	if(Index < 0 || !pPack->m_Reader.GetMap(Index, &pMap, &MapSize, &pJson, &JsonSize) || !pMap || MapSize <= 0)
		return false;

	pOut->m_pMap = mem_alloc(MapSize);
	mem_copy(pOut->m_pMap, pMap, MapSize);
	pOut->m_MapSize = MapSize;
	if(JsonSize > 0 && pJson)
	{
		pOut->m_pJson = mem_alloc(JsonSize);
		mem_copy(pOut->m_pJson, pJson, JsonSize);
		pOut->m_JsonSize = JsonSize;
	}
	RememberInstance(pFloorName, Seed);
	return true;
}

bool CMapGen::ExtractFromPack(const char *pFloorName, int Seed)
{
	if(!PackContains(pFloorName, Seed))
		return false;

	SFloorPack *pPack = PackFor(pFloorName);
	if(!pPack || !pPack->m_Open)
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);
	const int Index = pPack->m_Reader.FindMap(aMapName);
	if(Index < 0)
		return false;

	EnsureGeneratedMapsDir(m_pStorage);
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", aMapName);
	const bool Extracted = pPack->m_Reader.ExtractMap(Index, m_pStorage, aPath, true);
	if(Extracted)
		RememberInstance(pFloorName, Seed);
	dbg_msg("mapgen", "map pack: %s %s", Extracted ? "restored" : "FAILED to restore", aMapName);
	return Extracted;
}

bool CMapGen::AppendInstanceToPack(const char *pFloorName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize)
{
	if(!m_UseMapPacks || !m_pStorage || !pFloorName || !pFloorName[0] || !pMap || MapSize <= 0)
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);

	EnsureMappackDir(m_pStorage);
	char aPackPath[IO_MAX_PATH_LENGTH];
	PackPath(pFloorName, aPackPath, sizeof(aPackPath));
	CMapPackWriter Writer;
	bool Added = false;
	if(Writer.Open(m_pStorage, aPackPath))
		Added = Writer.AddMap(aMapName, Seed, pMap, MapSize, pJson, JsonSize);
	else
		dbg_msg("mapgen", "map pack: cannot open %s", aPackPath);

	// the cached reader may predate this append (or the pack's creation)
	SFloorPack *pPack = PackFor(pFloorName);
	if(pPack)
	{
		pPack->m_Reader.Close();
		pPack->m_Open = false;
		pPack->m_Tried = false;
	}

	if(Added)
		RememberInstance(pFloorName, Seed);
	dbg_msg("mapgen", "map pack: %s %s into %s", Added ? "packed" : "FAILED to pack", aMapName, aPackPath);
	if(!Added)
		dbg_msg("mapgen", "map pack: keeping sidecar files for %s", aMapName);
	return Added;
}

void CMapGen::AppendToPack(const char *pFloorName, int Seed)
{
	// migration path: the instance exists as files (an older cache, or a tool
	// that asked for them) and is added to the pack from there
	if(!m_UseMapPacks || !m_pStorage || !pFloorName || !pFloorName[0])
		return;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", aMapName);
	void *pMap = nullptr;
	unsigned MapSize = 0;
	if(!m_pStorage->ReadFile(aPath, IStorage::TYPE_SAVE, &pMap, &MapSize) || !pMap)
	{
		dbg_msg("mapgen", "map pack: cannot read %s", aPath);
		return;
	}
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.json", aMapName);
	void *pJson = nullptr;
	unsigned JsonSize = 0;
	if(!m_pStorage->ReadFile(aPath, IStorage::TYPE_SAVE, &pJson, &JsonSize))
		JsonSize = 0;

	AppendInstanceToPack(pFloorName, Seed, pMap, (int) MapSize, pJson, (int) JsonSize);

	mem_free(pMap);
	if(pJson)
		mem_free(pJson);
}

void CMapGen::WriteInstanceFiles(const char *pFloorName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize)
{
	if(!m_pStorage || !pMap || MapSize <= 0)
		return;

	EnsureGeneratedMapsDir(m_pStorage);
	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", aMapName);
	IOHANDLE File = m_pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		dbg_msg("mapgen", "cannot write %s", aPath);
		return;
	}
	const bool MapOk = io_write(File, pMap, (unsigned) MapSize) == (unsigned) MapSize;
	io_close(File);
	if(!MapOk)
		dbg_msg("mapgen", "short write of %s", aPath);

	if(!pJson || JsonSize <= 0)
		return;
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.json", aMapName);
	File = m_pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		dbg_msg("mapgen", "cannot write %s", aPath);
		return;
	}
	io_write(File, pJson, (unsigned) JsonSize);
	io_close(File);
}

void CMapGen::FreeInstance(SMapInstance *pInstance)
{
	if(!pInstance)
		return;
	if(pInstance->m_pMap)
		mem_free(pInstance->m_pMap);
	if(pInstance->m_pJson)
		mem_free(pInstance->m_pJson);
	mem_zero(pInstance, sizeof(*pInstance));
}

bool CMapGen::TryAcquireMap(const char *pFloorName, int Seed, SMapInstance *pOut)
{
	if(!pOut)
		return false;
	mem_zero(pOut, sizeof(*pOut));
	if(!pFloorName || !pFloorName[0])
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);

	scope_lock Lock(&m_PackMutex);

	// the pack is the source of truth: take the bytes straight out of it
	if(ReadFromPack(pFloorName, Seed, pOut))
		return true;

	char aPath[IO_MAX_PATH_LENGTH];
	const bool Existed = GeneratedMapExists(pFloorName, Seed);
	if(!Existed)
		return false;

	// read the bytes the generator (or an older cache) produced
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", aMapName);
	void *pMap = nullptr;
	unsigned MapSize = 0;
	if(!m_pStorage->ReadFile(aPath, IStorage::TYPE_SAVE, &pMap, &MapSize) || !pMap || MapSize == 0)
	{
		if(pMap)
			mem_free(pMap);
		dbg_msg("mapgen", "cannot read the instance %s", aMapName);
		return false;
	}

	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.json", aMapName);
	void *pJson = nullptr;
	unsigned JsonSize = 0;
	if(!m_pStorage->ReadFile(aPath, IStorage::TYPE_SAVE, &pJson, &JsonSize))
		JsonSize = 0;

	if(m_UseMapPacks && !PackContains(pFloorName, Seed))
	{
		// a cache from before packs existed: migrate it once
		AppendToPack(pFloorName, Seed);
	}

	pOut->m_pMap = pMap;
	pOut->m_MapSize = (int) MapSize;
	pOut->m_pJson = pJson;
	pOut->m_JsonSize = (int) JsonSize;
	return true;
}

bool CMapGen::AcquireMapOrGenerate(const char *pFloorName, int Seed, SMapInstance *pOut)
{
	if(TryAcquireMap(pFloorName, Seed, pOut))
		return true;

	// Tooling path only. The server must never reach this, otherwise a whole
	// world would be built on the server tick thread.
	if(m_pEngine)
	{
		dbg_msg("mapgen", "refusing to generate %s_%d on the running server", pFloorName, Seed);
		return false;
	}
	if(!GenerateNow(pFloorName, Seed))
		return false;
	return TryAcquireMap(pFloorName, Seed, pOut);
}

bool CMapGen::IsMapAvailable(const char *pFloorName, int Seed)
{
	if(!pFloorName || !pFloorName[0] || Seed <= 0)
		return false;

	scope_lock Lock(&m_GenLock);
	return InstanceAvailable(pFloorName, Seed);
}

void CMapGen::PrefetchMap(const char *pFloorName, int Seed)
{
	if(!pFloorName || !pFloorName[0] || Seed <= 0)
		return;

	scope_lock Lock(&m_GenLock);
	QueuePrefetchLocked(pFloorName, Seed);
}

void CMapGen::ClosePacks()
{
	scope_lock Lock(&m_PackMutex);
	for(int i = 0; i < m_lPacks.size(); i++)
	{
		m_lPacks[i]->m_Reader.Close();
		delete m_lPacks[i];
	}
	m_lPacks.clear();
}

bool CMapGen::GeneratedMapExists(const char *pFloorName, int Seed) const
{
	if(!m_pStorage || !pFloorName || !pFloorName[0])
		return false;

	char aMapName[64];
	str_format(aMapName, sizeof(aMapName), "%s_%d", pFloorName, Seed);
	char aMapFile[IO_MAX_PATH_LENGTH];
	str_format(aMapFile, sizeof(aMapFile), "%s.map", aMapName);
	char aFound[IO_MAX_PATH_LENGTH];
	return m_pStorage->FindFile(aMapFile, GENERATED_MAPS_DIR, IStorage::TYPE_ALL, aFound, sizeof(aFound));
}

void CMapGen::PromoteReadyLocked(int Index)
{
	if(Index < 0 || Index >= m_lTrainChunks.size() || Index >= m_lTrainFloors.size())
		return;

	STrainChunk &Chunk = m_lTrainChunks[Index];
	if(Chunk.m_ReadySeed || !Chunk.m_PendingSeed)
		return;

	// The worker renames a completed map into place, so the file appearing
	// before the worker updates the state is a valid readiness signal. A pack
	// backed server keeps no file, so the pack index counts as well.
	if(!InstanceAvailable(m_lTrainFloors[Index]->m_aFloorName, Chunk.m_PendingSeed))
		return;

	Chunk.m_ReadySeed = Chunk.m_PendingSeed;
	Chunk.m_PendingSeed = 0;
}

void CMapGen::QueueChunkLocked(int Index, int Seed)
{
	if(Index < 0 || Index >= m_lTrainChunks.size() || Index >= m_lTrainFloors.size())
		return;

	STrainChunk &Chunk = m_lTrainChunks[Index];
	if(Chunk.m_PendingSeed)
		return;

	const char *pFloorName = m_lTrainFloors[Index]->m_aFloorName;
	if(InstanceAvailable(pFloorName, Seed))
	{
		// an earlier run (its file or its pack) already has this chunk
		Chunk.m_ReadySeed = Seed;
		QueueRingNeighboursLocked(Index, Seed);
		return;
	}

	Chunk.m_PendingSeed = Seed;

	CGenJob *pJob = new CGenJob;
	mem_zero(pJob, sizeof(*pJob));
	pJob->m_TrainIndex = Index;
	pJob->m_Seed = Seed;
	str_copy(pJob->m_aFloorName, pFloorName, sizeof(pJob->m_aFloorName));

	if(m_pGenTail)
		m_pGenTail->m_pNext = pJob;
	else
		m_pGenHead = pJob;
	m_pGenTail = pJob;

	// The ring world needs both neighbours live as soon as the player arrives;
	// queue them behind the center chunk so they are already on disk when the
	// first tick after the switch asks for them.
	QueueRingNeighboursLocked(Index, Seed);
	ScheduleWorkerLocked();
}

void CMapGen::QueuePrefetchLocked(const char *pFloorName, int Seed)
{
	if(!pFloorName || !pFloorName[0] || Seed <= 0)
		return;
	if(InstanceAvailable(pFloorName, Seed))
		return;

	// already waiting, or being generated right now
	for(CGenJob *pJob = m_pGenHead; pJob; pJob = pJob->m_pNext)
	{
		if(pJob->m_Seed == Seed && str_comp(pJob->m_aFloorName, pFloorName) == 0)
			return;
	}
	if(m_pGenRunning && m_pGenRunning->m_Seed == Seed &&
		str_comp(m_pGenRunning->m_aFloorName, pFloorName) == 0)
		return;

	CGenJob *pJob = new CGenJob;
	mem_zero(pJob, sizeof(*pJob));
	pJob->m_TrainIndex = -1;
	pJob->m_Seed = Seed;
	str_copy(pJob->m_aFloorName, pFloorName, sizeof(pJob->m_aFloorName));

	if(m_pGenTail)
		m_pGenTail->m_pNext = pJob;
	else
		m_pGenHead = pJob;
	m_pGenTail = pJob;
	ScheduleWorkerLocked();
}

void CMapGen::QueueRingNeighboursLocked(int Index, int Seed)
{
	if(Index < 0 || Index >= m_lTrainFloors.size() || Seed <= 0)
		return;

	const CInstruction *pFloor = m_lTrainFloors[Index];
	const CInstruction::CGeneration::CRingConfig &Ring = pFloor->m_Generation.m_Ring;
	if(!Ring.m_Enabled || Ring.m_Size < 1)
		return;

	for(int Direction = -1; Direction <= 1; Direction += 2)
	{
		const int Neighbour = RingNeighbourSeed(Seed, Direction, Ring.m_Size);
		if(Neighbour == Seed)
			continue;
		QueuePrefetchLocked(pFloor->m_aFloorName, Neighbour);
	}
}

bool CMapGen::RingNeighboursReadyLocked(int Index, int Seed)
{
	if(Index < 0 || Index >= m_lTrainFloors.size() || Seed <= 0)
		return true;

	const CInstruction *pFloor = m_lTrainFloors[Index];
	const CInstruction::CGeneration::CRingConfig &Ring = pFloor->m_Generation.m_Ring;
	if(!Ring.m_Enabled || Ring.m_Size < 1)
		return true;

	// The game layer brings both ring neighbours in on the first tick after the
	// switch. Generating one of them there blocks the whole server, so boarding
	// waits until they are all on disk; the click is refused for a moment.
	bool Ready = true;
	for(int Direction = -1; Direction <= 1; Direction += 2)
	{
		const int Neighbour = RingNeighbourSeed(Seed, Direction, Ring.m_Size);
		if(Neighbour == Seed)
			continue;
		if(InstanceAvailable(pFloor->m_aFloorName, Neighbour))
			continue;
		QueuePrefetchLocked(pFloor->m_aFloorName, Neighbour);
		Ready = false;
	}
	return Ready;
}

void CMapGen::ScheduleWorkerLocked()
{
	if(m_GenShutdown || m_GenWorkerActive || !m_pGenHead || !m_pEngine)
		return;

	// One drain job at a time is enough: it keeps taking entries until the
	// queue is empty, so the pool never has two threads competing for the
	// generator's shared state. A fresh wrapper is used for every run so the
	// previous CJob is never touched while the pool writes its final status.
	CWorkerJob *pWorker = new CWorkerJob;
	pWorker->m_pSelf = this;
	pWorker->m_pNext = m_pWorkerJobs;
	m_pWorkerJobs = pWorker;
	m_GenWorkerActive = true;
	m_pEngine->AddJob(&pWorker->m_Job, GenWorker, pWorker);
}

void CMapGen::ReapWorkerJobsLocked()
{
	CWorkerJob **ppWorker = &m_pWorkerJobs;
	while(*ppWorker)
	{
		CWorkerJob *pWorker = *ppWorker;
		if(pWorker->m_Job.Status() == CJob::STATE_DONE)
		{
			*ppWorker = pWorker->m_pNext;
			delete pWorker;
		}
		else
			ppWorker = &pWorker->m_pNext;
	}
}

int CMapGen::GenWorker(void *pUser)
{
	CWorkerJob *pWorker = static_cast<CWorkerJob *>(pUser);
	CMapGen *pSelf = pWorker->m_pSelf;

	for(;;)
	{
		CGenJob *pJob = nullptr;
		{
			scope_lock Lock(&pSelf->m_GenLock);
			if(pSelf->m_GenShutdown)
				break;
			pJob = pSelf->m_pGenHead;
			if(pJob)
			{
				pSelf->m_pGenHead = pJob->m_pNext;
				if(!pSelf->m_pGenHead)
					pSelf->m_pGenTail = nullptr;
				pSelf->m_pGenRunning = pJob;
			}
		}

		if(!pJob)
			break;

		// Check the pack (and the generated file) once more before spending a
		// generation: while this job waited in the queue a player may have
		// loaded the same instance, and generating it again would append a
		// duplicate record to the pack.
		bool Available;
		{
			scope_lock Lock(&pSelf->m_GenLock);
			Available = pSelf->InstanceAvailable(pJob->m_aFloorName, pJob->m_Seed);
		}
		// The known set only covers packs that were opened on the main thread.
		// An entrance target floor may never have been opened there, so read
		// the instance here (on the worker) and skip instead of generating a
		// duplicate. This also validates the pack record before trusting it:
		// a corrupt entry falls through to a fresh generation.
		if(!Available)
		{
			SMapInstance Existing;
			if(pSelf->TryAcquireMap(pJob->m_aFloorName, pJob->m_Seed, &Existing))
			{
				FreeInstance(&Existing);
				Available = true;
			}
		}

		bool Generated;
		if(Available)
		{
			dbg_msg("mapgen", "map pack: %s_%d already available, skipping generation", pJob->m_aFloorName, pJob->m_Seed);
			Generated = true;
		}
		else
			Generated = pSelf->GenerateNow(pJob->m_aFloorName, pJob->m_Seed);

		{
			scope_lock Lock(&pSelf->m_GenLock);
			if(pJob->m_TrainIndex >= 0 && pJob->m_TrainIndex < pSelf->m_lTrainChunks.size())
			{
				STrainChunk &Chunk = pSelf->m_lTrainChunks[pJob->m_TrainIndex];
				// Do not overwrite a newer seed if the player already consumed
				// this one and queued the following trip in the meantime.
				if(Chunk.m_PendingSeed == pJob->m_Seed)
				{
					if(Generated)
						Chunk.m_ReadySeed = pJob->m_Seed;
					Chunk.m_PendingSeed = 0;
				}
			}
			pSelf->m_pGenRunning = nullptr;
			delete pJob;
		}
	}

	{
		scope_lock Lock(&pSelf->m_GenLock);
		pSelf->m_GenWorkerActive = false;
	}
	return 0;
}

void CMapGen::StopGeneration()
{
	{
		scope_lock Lock(&m_GenLock);
		m_GenShutdown = true;
	}

	// The server frees CMapGen from CServer::Run, before the engine pool is
	// shut down, so a submitted drain job still gets to run and can be waited
	// for safely. It exits at the shutdown check without starting more work.
	for(;;)
	{
		bool AnyRunning = false;
		{
			scope_lock Lock(&m_GenLock);
			for(CWorkerJob *pWorker = m_pWorkerJobs; pWorker; pWorker = pWorker->m_pNext)
			{
				if(pWorker->m_Job.Status() != CJob::STATE_DONE)
				{
					AnyRunning = true;
					break;
				}
			}
		}
		if(!AnyRunning)
			break;
		thread_sleep(1);
	}

	// No pool job can touch the queue or the wrappers anymore.
	CGenJob *pJob = m_pGenHead;
	while(pJob)
	{
		CGenJob *pNext = pJob->m_pNext;
		delete pJob;
		pJob = pNext;
	}
	m_pGenHead = nullptr;
	m_pGenTail = nullptr;
	m_pGenRunning = nullptr;

	CWorkerJob *pWorker = m_pWorkerJobs;
	while(pWorker)
	{
		CWorkerJob *pNext = pWorker->m_pNext;
		delete pWorker;
		pWorker = pNext;
	}
	m_pWorkerJobs = nullptr;
}

void CMapGen::StartPreGeneration(IEngine *pEngine)
{
	if(m_GenStarted)
		return;
	m_GenStarted = true;
	m_GenShutdown = false;
	m_pEngine = pEngine;

	// The state array is sized before the first job can run, so a completion
	// can never index an empty array.
	m_lTrainChunks.clear();
	m_lTrainChunks.set_size(m_lTrainFloors.size());
	for(int i = 0; i < m_lTrainChunks.size(); i++)
	{
		m_lTrainChunks[i].m_ReadySeed = 0;
		m_lTrainChunks[i].m_PendingSeed = 0;
	}

	// Seed the known-instance table from the pack indexes: a restart must not
	// regenerate instances that are already packed, and the per tick train code
	// must not wait on a generation to find that out.
	if(m_UseMapPacks)
	{
		scope_lock Lock(&m_PackMutex);
		for(int i = 0; i < m_lTrainFloors.size(); i++)
		{
			SFloorPack *pPack = PackFor(m_lTrainFloors[i]->m_aFloorName);
			EnsurePackOpen(pPack);
		}
	}

	scope_lock Lock(&m_GenLock);
	for(int i = 0; i < m_lTrainFloors.size(); i++)
		QueueChunkLocked(i, RollTrainSeed(i));
}

void CMapGen::UpdateBackgroundGeneration()
{
	scope_lock Lock(&m_GenLock);
	ReapWorkerJobsLocked();
	ScheduleWorkerLocked();
}

bool CMapGen::TakeTrainChunk(const char *pFloorName, char *pOutMapName, int OutSize)
{
	if(!pFloorName || !pFloorName[0] || !pOutMapName || OutSize <= 0)
		return false;

	const int Index = FindTrainFloor(pFloorName);
	if(Index < 0)
		return false;

	int Seed = 0;
	{
		scope_lock Lock(&m_GenLock);
		if(Index >= m_lTrainChunks.size())
			return false;

		STrainChunk &Chunk = m_lTrainChunks[Index];
		PromoteReadyLocked(Index);
		if(Chunk.m_ReadySeed)
		{
			// Do not board into a ring until its two neighbours are generated
			// as well; the game layer needs them live on the next tick.
			if(!RingNeighboursReadyLocked(Index, Chunk.m_ReadySeed))
				return false;
			Seed = Chunk.m_ReadySeed;
			Chunk.m_ReadySeed = 0;
		}
		else if(!Chunk.m_PendingSeed)
		{
			// Pre-generation never ran or its job failed. Queue a chunk now;
			// the click is refused once and the next one finds it ready.
			QueueChunkLocked(Index, RollTrainSeed(Index));
			return false;
		}

		// Roll the following trip immediately: its map is built while the
		// player travels through the one just consumed.
		if(Seed && !Chunk.m_PendingSeed)
			QueueChunkLocked(Index, RollTrainSeed(Index));
	}

	if(!Seed)
		return false;

	str_format(pOutMapName, OutSize, "%s_%d", pFloorName, Seed);
	return true;
}

CMapGen::~CMapGen()
{
	StopGeneration();
	ClosePacks();
	m_lInstructions.for_each(FreeInstruction, nullptr);
}

void CMapGen::FreeInstruction(CInstruction *&pInstruction, void *pUser)
{
	delete pInstruction;
	pInstruction = nullptr;
}

bool CMapGen::GenerateNow(const char *pFloorName, int Seed)
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
	// the instance is built entirely in memory: no staging file is involved
	SGeneratedMap GeneratedMap;
	bool Generated = false;
	{
		// Hold the generator lock only while the world is being built. The
		// pack file must not be touched here: the main thread reads packs when
		// a world loads and must never wait for a generation.
		scope_lock Lock(&m_GenMutex);
		switch(pInstruction->m_Kind)
		{
			case WORLDKIND_HUB:
				dbg_msg("mapgen", "the hub world '%s' is a static map and is not generated", pFloorName);
				return false;
			case WORLDKIND_EUCLID:
				Generated = GenerateEuclidean(pInstruction, Seed, &GeneratedMap);
				break;
			case WORLDKIND_NONEUCLID:
				Generated = GenerateNoneuclid(pInstruction, Seed, &GeneratedMap);
				break;
		}
	}

	if(!Generated)
		return false;

	// The map is finished; store it under the pack lock only. A reader that
	// needs an already generated map waits for this short write, never for
	// the generation above.
	scope_lock PackLock(&m_PackMutex);

	// every instance the generator produces joins its floor's pack, so the
	// pack becomes the complete set of that floor's worlds
	const bool Packed = AppendInstanceToPack(pFloorName, Seed,
		GeneratedMap.m_Map.base_ptr(), GeneratedMap.m_Map.size(),
		GeneratedMap.m_Meta.base_ptr(), GeneratedMap.m_Meta.size());

	// A cache file is written when the caller wants one (tooling), when packs
	// are disabled, or when the append failed. The last case is essential:
	// a reader must be able to find the instance after GenerateNow; otherwise
	// a ring neighbour or an entrance target cannot be created.
	if(!Packed || !m_DeleteGeneratedFiles || !m_UseMapPacks)
		WriteInstanceFiles(pFloorName, Seed,
			GeneratedMap.m_Map.base_ptr(), GeneratedMap.m_Map.size(),
			GeneratedMap.m_Meta.base_ptr(), GeneratedMap.m_Meta.size());

	return true;
}

bool CMapGen::RequestNewMap(const char *pFloorName, int Seed)
{
	// Explicit callers (mapgentool, tests) always rebuild the map, even when
	// an older file with the same name exists.
	if(m_pEngine)
	{
		dbg_msg("mapgen", "refusing to rebuild %s_%d on the running server", pFloorName, Seed);
		return false;
	}
	return GenerateNow(pFloorName, Seed);
}

bool CMapGen::EnsureNewMap(const char *pFloorName, int Seed)
{
	if(!pFloorName || !pFloorName[0])
		return false;

	// Check the pack and the generated file under the pack lock, then release
	// it before generating: GenerateNow takes the generation and pack locks
	// itself, and lock order must stay consistent.
	{
		scope_lock Lock(&m_PackMutex);
		if(GeneratedMapExists(pFloorName, Seed))
		{
			// a map generated before its pack existed (or with packs disabled)
			// is migrated once, on first use, so the pack converges on the
			// whole floor
			if(m_UseMapPacks && !PackContains(pFloorName, Seed))
				AppendToPack(pFloorName, Seed);
			return true;
		}

		// the pack is the durable copy: restore instead of generating again
		if(m_UseMapPacks && ExtractFromPack(pFloorName, Seed) && GeneratedMapExists(pFloorName, Seed))
			return true;
	}

	if(m_pEngine)
	{
		dbg_msg("mapgen", "refusing to generate %s_%d on the running server", pFloorName, Seed);
		return false;
	}
	return GenerateNow(pFloorName, Seed);
}
