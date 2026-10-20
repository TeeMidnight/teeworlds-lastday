/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// mapgentool - generate mapgen floors without starting the server.
//
// The server prepares one random train chunk per destination in the background
// at startup, and any other floor lazily the first time something asks for a
// "<floor>_<seed>" map; it writes the result into the storage save path.
// That makes the terrain awkward to inspect: it needs a running server, and the
// file lands somewhere the caller has to look up. This tool drives the same
// CMapGen the server uses, against the same data directory and the same
// storage.cfg, and writes the very same
// "<GENERATED_MAPS_DIR>/<floor>_<seed>.map". It reports the absolute path of
// every map it writes, so a headless renderer (for example Uniteed's
// render.map) can be pointed straight at the result.
//
// Usage:
//   mapgentool list               list the generated floors and their kind
//   mapgentool <floor> [seed...]  generate one floor for each seed (default 1)
//   mapgentool all [seed...]      generate every non-hub floor for each seed
//
// The floors are the "maps/worlds/*.json" files, exactly as the server reads
// them, so the tool and the server never disagree about a floor's shape.

#include <base/system.h>

#include <engine/console.h>
#include <engine/kernel.h>
#include <engine/map/generatedmaps.h>
#include <engine/server/mapgen.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>
#include <game/mapitems.h>

#include <base/tl/array.h>

namespace {

enum
{
	MAX_TOOL_FLOORS = 128,
	MAX_TOOL_SEEDS = 64,
};

// the floor file names collected from maps/worlds, without the ".json" suffix
struct SFloorNames
{
	int m_Num;
	char m_aaNames[MAX_TOOL_FLOORS][64];
};

int CollectFloorCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	SFloorNames *pNames = static_cast<SFloorNames *>(pUser);
	if(pNames->m_Num >= MAX_TOOL_FLOORS || Len - 5 >= (int) sizeof(pNames->m_aaNames[0]))
		return 0;

	// copy the name without the ".json" suffix: the terminating byte sits where
	// the suffix starts
	str_copy(pNames->m_aaNames[pNames->m_Num], pFilename, Len - 4);
	pNames->m_Num++;
	return 0;
}

bool GenerateFloor(CMapGen &rGen, IStorage *pStorage, const char *pFloor, int Seed)
{
	if(!rGen.RequestNewMap(pFloor, Seed))
	{
		dbg_msg("mapgentool", "FAIL %s_%d", pFloor, Seed);
		return false;
	}

	// the generator always writes under GENERATED_MAPS_DIR on the save path;
	// report where that really is so a renderer can open it without guessing
	char aRelative[128];
	char aAbsolute[IO_MAX_PATH_LENGTH];
	str_format(aRelative, sizeof(aRelative), GENERATED_MAPS_DIR "/%s_%d.map", pFloor, Seed);
	pStorage->GetCompletePath(IStorage::TYPE_SAVE, aRelative, aAbsolute, sizeof(aAbsolute));
	dbg_msg("mapgentool", "MAP %s", aAbsolute);
	return true;
}

// Decompresses a tile data chunk (skip run length encoded) into a flat RGBA
// style array of Count four byte tiles.
// Decompresses a tile data chunk (skip run length encoded) into a flat array of
// four byte tiles, the same layout the map stores.
void DecodeTiles(CDataFileReader &Reader, int DataIndex, int Count, unsigned char *pOut)
{
	const int Size = Reader.GetDataSize(DataIndex);
	const unsigned char *pStart = (const unsigned char *) Reader.GetData(DataIndex);
	if(!pStart || Size < 4)
		return;
	const unsigned char *pEnd = pStart + Size;
	const unsigned char *pSaved = pStart;
	int i = 0;
	while(i < Count && pSaved + 4 <= pEnd)
	{
		const int Skip = pSaved[2];
		for(int k = 0; k <= Skip && i < Count; k++)
		{
			pOut[(size_t) i * 4 + 0] = pSaved[0];
			pOut[(size_t) i * 4 + 1] = pSaved[1];
			pOut[(size_t) i * 4 + 2] = 0;
			pOut[(size_t) i * 4 + 3] = 0;
			i++;
		}
		pSaved += 4;
	}
}

// Regenerates two neighbouring instances of a ring floor and compares the strip
// they share straight out of the generated maps. This is the only check that
// also covers the automapper and the map writer, not just the in memory
// generator. A couple of columns at the outer edge of each strip are skipped:
// the automapper clamps there and those columns are beyond the transfer.
bool RingCheck(IStorage *pStorage, CMapGen &rGen, const char *pFloor)
{
	if(!rGen.RequestNewMap(pFloor, 1) || !rGen.RequestNewMap(pFloor, 2))
	{
		dbg_msg("mapgentool", "ringcheck: %s could not be generated", pFloor);
		return false;
	}

	CDataFileReader A, B;
	char aPathA[IO_MAX_PATH_LENGTH], aPathB[IO_MAX_PATH_LENGTH];
	str_format(aPathA, sizeof(aPathA), GENERATED_MAPS_DIR "/%s_1.map", pFloor);
	str_format(aPathB, sizeof(aPathB), GENERATED_MAPS_DIR "/%s_2.map", pFloor);
	if(!A.Open(pStorage, aPathA, IStorage::TYPE_SAVE) || !B.Open(pStorage, aPathB, IStorage::TYPE_SAVE))
	{
		dbg_msg("mapgentool", "ringcheck: could not open the generated maps of %s", pFloor);
		return false;
	}

	// the shared margin and the ring flag come from the world metadata sidecar
	int SharedMargin = 0, Ring = 0;
	{
		const json_value *pWorld = nullptr;
		char aMetaName[64];
		str_format(aMetaName, sizeof(aMetaName), "%s_1", pFloor);
		array<char> aMeta;
		CJsonParser MetaParser;
		if(LoadGeneratedWorldMeta(pStorage, aMetaName, aMeta))
		{
			json_value *pMeta = MetaParser.ParseString(aMeta.base_ptr(), "world meta");
			if(pMeta && pMeta->type == json_object)
			{
				const json_value &rWorld = (*pMeta)["world"];
				if(rWorld.type == json_object)
					pWorld = &rWorld;
			}
		}
		// old maps still embed the world json in the map file
		CJsonParser LegacyParser;
		if(!pWorld)
		{
			CMapItemJson *pJson = (CMapItemJson *) A.FindItem(MAPITEMTYPE_JSON, 2);
			const char *pData = pJson ? (const char *) A.GetData(pJson->m_Data) : nullptr;
			if(pData)
			{
				json_value *pOldWorld = LegacyParser.ParseString(pData, "world");
				if(pOldWorld && pOldWorld->type == json_object)
					pWorld = pOldWorld;
			}
		}
		if(!pWorld)
		{
			dbg_msg("mapgentool", "ringcheck: %s has no world metadata", pFloor);
			return false;
		}
		Ring = (int) (json_int_t) (*pWorld)["ring"];
		SharedMargin = (int) (json_int_t) (*pWorld)["shared_margin"];
	}
	if(Ring == 0 || SharedMargin <= 0)
	{
		dbg_msg("mapgentool", "ringcheck: %s is not a ring floor", pFloor);
		return false;
	}

	const int EDGE = 2;
	int Layers = 0, Mismatches = 0;
	int Start = 0, Num = 0;
	A.GetType(MAPITEMTYPE_LAYER, &Start, &Num);
	for(int i = 0; i < Num; i++)
	{
		int TypeA = 0, IDA = 0, TypeB = 0, IDB = 0;
		CMapItemLayerTilemap *pA = (CMapItemLayerTilemap *) A.GetItem(Start + i, &TypeA, &IDA);
		CMapItemLayerTilemap *pB = (CMapItemLayerTilemap *) B.GetItem(Start + i, &TypeB, &IDB);
		if(!pA || !pB || pA->m_Layer.m_Type != LAYERTYPE_TILES || pB->m_Layer.m_Type != LAYERTYPE_TILES)
			continue;
		const int W = pA->m_Width, H = pA->m_Height;
		const int Stride = W - SharedMargin;
		if(pB->m_Width != W || pB->m_Height != H || Stride <= 0)
			continue;
		array<unsigned char> TA, TB;
		TA.set_size(W * H * 4);
		TB.set_size(W * H * 4);
		mem_zero(TA.base_ptr(), TA.size());
		mem_zero(TB.base_ptr(), TB.size());
		DecodeTiles(A, pA->m_Data, W * H, TA.base_ptr());
		DecodeTiles(B, pB->m_Data, W * H, TB.base_ptr());
		for(int y = 0; y < H; y++)
		{
			for(int x = EDGE; x < SharedMargin - EDGE; x++)
			{
				const size_t ia = ((size_t) y * W + (Stride + x)) * 4;
				const size_t ib = ((size_t) y * W + x) * 4;
				if(TA[ia] != TB[ib] || TA[ia + 1] != TB[ib + 1] || TA[ia + 2] != TB[ib + 2])
					Mismatches++;
			}
		}
		Layers++;
	}

	dbg_msg("mapgentool", "ringcheck %s: %d tile layer(s), %d mismatching tile(s)", pFloor, Layers, Mismatches);
	A.Close();
	B.Close();
	return Layers > 0 && Mismatches == 0;
}

// Prints the world geometry json (MAPITEMTYPE_JSON id 2) of a generated map,
// so a caller can confirm the horizontal wrap and the ring metadata a floor
// wrote without decoding the map by hand.
bool DumpWorldJson(IStorage *pStorage, const char *pMap)
{
	// current maps keep the world geometry in the metadata sidecar
	array<char> aMeta;
	CJsonParser MetaParser;
	if(LoadGeneratedWorldMeta(pStorage, pMap, aMeta))
	{
		json_value *pMeta = MetaParser.ParseString(aMeta.base_ptr(), "world meta");
		if(pMeta && pMeta->type == json_object)
		{
			const json_value &rWorld = (*pMeta)["world"];
			if(rWorld.type == json_object)
			{
				dbg_msg("mapgentool", "world json of %s: wrap_x=%d ring=%d ring_seed=%d ring_size=%d shared_margin=%d offset_x=%d period=%d",
					pMap,
					(int) (json_int_t) rWorld["wrap_x"],
					(int) (json_int_t) rWorld["ring"],
					(int) (json_int_t) rWorld["ring_seed"],
					(int) (json_int_t) rWorld["ring_size"],
					(int) (json_int_t) rWorld["shared_margin"],
					(int) (json_int_t) rWorld["offset_x"],
					(int) (json_int_t) rWorld["period"]);
				return true;
			}
		}
	}

	// fallback: older maps embed the world geometry as a map item
	char aPath[128];
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", pMap);
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, aPath, IStorage::TYPE_SAVE))
	{
		dbg_msg("mapgentool", "cannot open %s", aPath);
		return false;
	}
	bool Ok = false;
	CMapItemJson *pJson = (CMapItemJson *) Reader.FindItem(MAPITEMTYPE_JSON, 2);
	if(!pJson)
		dbg_msg("mapgentool", "no world json in %s", aPath);
	else
	{
		const char *pData = (const char *) Reader.GetData(pJson->m_Data);
		if(pData)
		{
			// the writer pretty prints, so flatten it onto one log line
			char aFlat[1024];
			int n = 0;
			for(const char *p = pData; *p && n < (int) sizeof(aFlat) - 1; p++)
				aFlat[n++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
			aFlat[n] = '\0';
			dbg_msg("mapgentool", "world json of %s: %s", pMap, aFlat);
			Ok = true;
		}
	}
	Reader.Close();
	return Ok;
}

void PrintUsage(const char *pProgram)
{
	dbg_msg("mapgentool", "usage: %s list", pProgram);
	dbg_msg("mapgentool", "       %s <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s all [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s ringcheck <floor...>", pProgram);
	dbg_msg("mapgentool", "       %s worldjson <map...>", pProgram);
	dbg_msg("mapgentool", "runs the mapgen world generator without the server and writes");
	dbg_msg("mapgentool", GENERATED_MAPS_DIR "/<floor>_<seed>.map into the storage save path");
}

} // namespace

int main(int argc, const char **argv)
{
	cmdline_fix(&argc, &argv);

	// without a registered logger the generator's diagnostics vanish, which
	// would make a failed generation look like a silent one
	dbg_logger_stdout();

	const char *apArgs[] = {"mapgentool"};
	IStorage *pStorage = CreateStorage("Teeworlds", IStorage::STORAGETYPE_SERVER, 1, apArgs);
	if(!pStorage)
	{
		dbg_msg("mapgentool", "failed to create storage");
		return 1;
	}

	// the console resolves its config and storage through the kernel, so the
	// interfaces the server registers have to be present here as well
	IKernel *pKernel = IKernel::Create();
	IConfigManager *pConfigManager = CreateConfigManager();
	IConsole *pConsole = CreateConsole(0);

	pKernel->RegisterInterface(pStorage);
	pKernel->RegisterInterface(pConfigManager);
	pKernel->RegisterInterface(pConsole);

	pConfigManager->Init(0);
	pConsole->Init();

	// the same generator the server drives, reading the same maps/worlds/*.json
	CMapGen Gen(pStorage, pConsole);

	if(argc < 2)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	const char *pCommand = argv[1];
	if(str_comp(pCommand, "list") == 0)
	{
		SFloorNames Names;
		Names.m_Num = 0;
		pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/worlds", CollectFloorCallback, &Names);
		for(int i = 0; i < Names.m_Num; i++)
		{
			EWorldKind Kind = WORLDKIND_EUCLID;
			const bool Known = Gen.GetFloorKind(Names.m_aaNames[i], &Kind);
			dbg_msg("mapgentool", "%-16s %s", Names.m_aaNames[i], Known ? WorldKindName(Kind) : "unknown");
		}
		return 0;
	}
	if(str_comp(pCommand, "ringcheck") == 0)
	{
		int Failures = 0;
		for(int i = 2; i < argc; i++)
			if(!RingCheck(pStorage, Gen, argv[i]))
				Failures++;
		return Failures == 0 ? 0 : 1;
	}
	if(str_comp(pCommand, "worldjson") == 0)
	{
		int Failures = 0;
		for(int i = 2; i < argc; i++)
			if(!DumpWorldJson(pStorage, argv[i]))
				Failures++;
		return Failures == 0 ? 0 : 1;
	}

	// the seeds follow the command; a bare seed list defaults to seed 1
	int aSeeds[MAX_TOOL_SEEDS];
	int NumSeeds = 0;
	for(int i = 2; i < argc && NumSeeds < MAX_TOOL_SEEDS; i++)
		aSeeds[NumSeeds++] = str_toint(argv[i]);
	if(NumSeeds == 0)
		aSeeds[NumSeeds++] = 1;

	if(str_comp(pCommand, "all") == 0)
	{
		SFloorNames Names;
		Names.m_Num = 0;
		pStorage->ListDirectory(IStorage::TYPE_ALL, "maps/worlds", CollectFloorCallback, &Names);

		int Failures = 0;
		for(int i = 0; i < Names.m_Num; i++)
		{
			EWorldKind Kind = WORLDKIND_EUCLID;
			if(Gen.GetFloorKind(Names.m_aaNames[i], &Kind) && Kind == WORLDKIND_HUB)
			{
				dbg_msg("mapgentool", "SKIP %s (static hub map)", Names.m_aaNames[i]);
				continue;
			}
			for(int s = 0; s < NumSeeds; s++)
				if(!GenerateFloor(Gen, pStorage, Names.m_aaNames[i], aSeeds[s]))
					Failures++;
		}
		return Failures == 0 ? 0 : 1;
	}

	int Failures = 0;
	for(int s = 0; s < NumSeeds; s++)
		if(!GenerateFloor(Gen, pStorage, pCommand, aSeeds[s]))
			Failures++;
	return Failures == 0 ? 0 : 1;
}
