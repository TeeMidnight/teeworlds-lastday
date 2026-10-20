/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <base/tl/algorithm.h>
#include <base/tl/stream.h>

#include <base/math.h>
#include <base/vmath.h>
#include <climits>

#include <engine/map/generatedmaps.h>
#include <engine/map/mapcreator.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>

#include <game/gamecore.h>
#include <game/mapitems.h>

#include "automap.h"
#include "mapgen.h"
#include "noise.h"
#include "worldgen.h"

void GeneratedWorldMetaPath(const char *pMapName, char *pBuf, int Size)
{
	str_format(pBuf, Size, GENERATED_MAPS_DIR "/%s.json", pMapName);
}

bool SaveGeneratedWorldMeta(IStorage *pStorage, const char *pMapName,
	const char *pEntrances, int EntrancesSize,
	const char *pResources, int ResourcesSize,
	const char *pWorld, int WorldSize)
{
	if(!pStorage || !pMapName || !pMapName[0])
		return false;

	char aPath[IO_MAX_PATH_LENGTH];
	GeneratedWorldMetaPath(pMapName, aPath, sizeof(aPath));
	IOHANDLE File = pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return false;

	bool Ok = true;
	auto WritePart = [&](const char *pData, int Size) {
		if(Size <= 0)
			return;
		if(io_write(File, pData, (unsigned) Size) != (unsigned) Size)
			Ok = false;
	};
	// the blobs are null terminated (and may carry more than one trailing
	// terminator); write only the JSON itself
	auto TrimSize = [](const char *pData, int Size) {
		if(!pData)
			return 0;
		while(Size > 0 && (pData[Size - 1] == '\0' || pData[Size - 1] == '\n' || pData[Size - 1] == '\r'))
			Size--;
		return Size;
	};
	auto WriteBlob = [&](const char *pData, int Size) {
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
	const int WorldLen = TrimSize(pWorld, WorldSize);
	if(WorldLen > 0)
	{
		WritePart(",\"world\":", (int) (sizeof(",\"world\":") - 1));
		WritePart(pWorld, WorldLen);
	}
	WritePart("}", 1);
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

CMapGen::CMapGen(IStorage *pStorage, IConsole *pConsole)
{
	m_pStorage = pStorage;
	m_pConsole = pConsole;

	// load the floor instructions from the "maps/worlds" directory; each
	// "<floorname>.json" file holds the config of one generated floor
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/worlds", ListWorldsCallback, this);
	RebuildFloorLists();
}

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
		str_copy(Struct.m_aBaseMap, rStruct["base"], sizeof(Struct.m_aBaseMap));
		Struct.m_GenerateProba = clamp(JsonFloat(rStruct["proba"], 0.0f), 0.0f, 1.0f);

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
void CMapGen::RebuildFloorLists()
{
	m_lTrainFloors.clear();
	m_lNonEuclidFloors.clear();

	m_lInstructions.for_each([](CInstruction *&pInstruction, void *pUser) {
		CMapGen *pSelf = static_cast<CMapGen *>(pUser);
		if(!pInstruction)
			return;
		if(pInstruction->m_Kind == WORLDKIND_EUCLID && pInstruction->m_TrainEnabled)
			pSelf->m_lTrainFloors.add(pInstruction);
		else if(pInstruction->m_Kind == WORLDKIND_NONEUCLID)
			pSelf->m_lNonEuclidFloors.add(pInstruction);
	}, this);

	const auto CompareName = [](const CInstruction *pA, const CInstruction *pB) {
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
			return GenerateNoneuclid(pInstruction, Seed);
	}

	return false;
}
