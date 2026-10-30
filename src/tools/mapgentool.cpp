/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// mapgentool - generate mapgen floors without starting the server.
//
// The server prepares the central train chunk of every destination in the
// background at startup, and any other floor lazily the first time something
// asks for a "<floor>_<seed>" map; it writes the result into the storage save
// path.
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
#include <base/tl/array.h>
#include <engine/console.h>
#include <engine/kernel.h>
#include <engine/map.h>
#include <engine/map/generatedmaps.h>
#include <engine/server/mapgen.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/mappack.h>
#include <engine/shared/worldpack.h>
#include <engine/storage.h>
#include <game/mapitems.h>

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

static int CollectFloorCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
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

static bool GenerateFloor(CMapGen &rGen, IStorage *pStorage, const char *pFloor, int Seed)
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
static void DecodeTiles(CDataFileReader &Reader, int DataIndex, int Count, unsigned char *pOut)
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
static bool RingCheck(IStorage *pStorage, CMapGen &rGen, const char *pFloor)
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
static bool DumpWorldJson(IStorage *pStorage, const char *pMap)
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

// Reads a whole file from the save path into memory.
static bool ReadSaveFile(IStorage *pStorage, const char *pPath, array<char> &Out)
{
	void *pData = nullptr;
	unsigned Size = 0;
	if(!pStorage->ReadFile(pPath, IStorage::TYPE_SAVE, &pData, &Size))
		return false;
	Out.set_size((int) Size);
	mem_copy(Out.base_ptr(), pData, Size);
	mem_free(pData);
	return true;
}

// Packs one floor's generated instances (map + world json sidecar) into a v5
// datafile. Each map becomes one item, so the metadata and the terrain travel
// together and one file replaces a directory of .map/.json pairs.
static bool PackFloor(IStorage *pStorage, CMapGen &rGen, const char *pFloor, const char *pOut, const int *pSeeds, int NumSeeds)
{
	CWorldPackWriter Writer;
	if(!Writer.Open(pStorage, pOut, pFloor, 0, 5))
	{
		dbg_msg("mapgentool", "pack: cannot create %s", pOut);
		return false;
	}

	int Added = 0;
	for(int i = 0; i < NumSeeds; i++)
	{
		const int Seed = pSeeds[i];
		if(!rGen.RequestNewMap(pFloor, Seed))
		{
			dbg_msg("mapgentool", "pack: FAIL %s_%d", pFloor, Seed);
			continue;
		}

		char aName[64];
		str_format(aName, sizeof(aName), "%s_%d", pFloor, Seed);
		char aMapPath[128];
		str_format(aMapPath, sizeof(aMapPath), GENERATED_MAPS_DIR "/%s.map", aName);
		array<char> lMap;
		if(!ReadSaveFile(pStorage, aMapPath, lMap))
		{
			dbg_msg("mapgentool", "pack: cannot read %s", aMapPath);
			continue;
		}

		// the world metadata is optional: an older map may embed it instead
		char aJsonPath[128];
		str_format(aJsonPath, sizeof(aJsonPath), GENERATED_MAPS_DIR "/%s.json", aName);
		array<char> lJson;
		if(!ReadSaveFile(pStorage, aJsonPath, lJson))
			lJson.clear();

		if(!Writer.AddMap(aName, Seed, lMap.base_ptr(), lMap.size(), lJson.base_ptr(), lJson.size()))
		{
			dbg_msg("mapgentool", "pack: rejected %s", aName);
			continue;
		}
		Added++;
	}

	if(!Writer.Finish())
	{
		dbg_msg("mapgentool", "pack: finish failed");
		return false;
	}
	dbg_msg("mapgentool", "packed %d map(s) into %s", Added, pOut);
	return Added > 0;
}

// Extracts every map of a pack back into the generated map directory as a plain
// v4 map plus its json sidecar, so the existing server path can serve it.
static bool UnpackFloor(IStorage *pStorage, const char *pPack)
{
	CWorldPackReader Reader;
	if(!Reader.Open(pStorage, pPack, IStorage::TYPE_SAVE))
	{
		dbg_msg("mapgentool", "unpack: cannot open %s", pPack);
		return false;
	}

	int Extracted = 0;
	for(int i = 0; i < Reader.NumMaps(); i++)
	{
		const SWorldPackEntryInfo *pEntry = Reader.Entry(i);
		if(!Reader.Verify(i))
			dbg_msg("mapgentool", "unpack: WARNING %s does not match its recorded hash", pEntry->m_aName);

		char aPath[128];
		str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", pEntry->m_aName);
		if(!Reader.ExtractMap(i, pStorage, aPath))
		{
			dbg_msg("mapgentool", "unpack: failed to write %s", aPath);
			continue;
		}
		Extracted++;
	}
	dbg_msg("mapgentool", "unpacked %d map(s) of floor %s into " GENERATED_MAPS_DIR, Extracted, Reader.Floor());
	Reader.Close();
	return Extracted > 0;
}

// Appends generated instances to an append-only pack. Unlike "pack", an
// existing file is never rewritten: each map is one blob plus one index
// record, so a running server can add a map while clients read the others.
static bool PackAppend(IStorage *pStorage, CMapGen &rGen, const char *pPack, const char *pFloor, const int *pSeeds, int NumSeeds)
{
	CMapPackWriter Writer;
	if(!Writer.Open(pStorage, pPack))
	{
		dbg_msg("mapgentool", "mappack: cannot open %s", pPack);
		return false;
	}

	int Added = 0;
	for(int i = 0; i < NumSeeds; i++)
	{
		const int Seed = pSeeds[i];
		if(!rGen.RequestNewMap(pFloor, Seed))
		{
			dbg_msg("mapgentool", "mappack: FAIL %s_%d", pFloor, Seed);
			continue;
		}
		char aName[64];
		str_format(aName, sizeof(aName), "%s_%d", pFloor, Seed);
		char aPath[128];
		str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", aName);
		array<char> lMap;
		if(!ReadSaveFile(pStorage, aPath, lMap))
		{
			dbg_msg("mapgentool", "mappack: cannot read %s", aPath);
			continue;
		}
		str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.json", aName);
		array<char> lJson;
		if(!ReadSaveFile(pStorage, aPath, lJson))
			lJson.clear();

		if(!Writer.AddMap(aName, Seed, lMap.base_ptr(), lMap.size(), lJson.base_ptr(), lJson.size()))
		{
			dbg_msg("mapgentool", "mappack: rejected %s", aName);
			continue;
		}
		Added++;
	}
	dbg_msg("mapgentool", "appended %d map(s) to %s", Added, pPack);
	return Added > 0;
}

static bool PackCompact(IStorage *pStorage, const char *pPack)
{
	int Maps = 0, Before = 0, After = 0;
	if(!CMapPackWriter::Compact(pStorage, pPack, &Maps, &Before, &After))
	{
		dbg_msg("mapgentool", "mappack: cannot compact %s", pPack);
		return false;
	}
	dbg_msg("mapgentool", "compacted %s: %d map(s), %d -> %d bytes", pPack, Maps, Before, After);
	return true;
}

static bool PackList(IStorage *pStorage, const char *pPack)
{
	CMapPackReader Reader;
	if(!Reader.Open(pStorage, pPack, IStorage::TYPE_SAVE))
	{
		dbg_msg("mapgentool", "mappack: cannot open %s", pPack);
		return false;
	}
	// opening reads the index only: no map blob is decompressed here
	for(int i = 0; i < Reader.NumMaps(); i++)
	{
		const SMapPackEntry *pEntry = Reader.Entry(i);
		dbg_msg("mapgentool", "%-24s seed=%-5d raw=%d comp=%d", pEntry->m_aName, pEntry->m_Seed, pEntry->m_RawSize, pEntry->m_CompSize);
	}
	Reader.Close();
	return true;
}

static bool PackGet(IStorage *pStorage, const char *pPack, const char *pName)
{
	CMapPackReader Reader;
	if(!Reader.Open(pStorage, pPack, IStorage::TYPE_SAVE))
	{
		dbg_msg("mapgentool", "mappack: cannot open %s", pPack);
		return false;
	}
	const int Index = Reader.FindMap(pName);
	if(Index < 0)
	{
		dbg_msg("mapgentool", "mappack: no map '%s' in %s", pName, pPack);
		Reader.Close();
		return false;
	}
	char aPath[128];
	str_format(aPath, sizeof(aPath), GENERATED_MAPS_DIR "/%s.map", pName);
	const bool Ok = Reader.ExtractMap(Index, pStorage, aPath);
	dbg_msg("mapgentool", "%s %s", Ok ? "extracted" : "FAILED", pName);
	Reader.Close();
	return Ok;
}

// Loads one instance the way the server does: bytes from the floor's pack,
// fed straight into the engine map. No cache file is created.
static bool AcquireInstance(IStorage *pStorage, CMapGen &rGen, const char *pFloor, int Seed)
{
	CMapGen::SMapInstance Instance;
	if(!rGen.AcquireMapOrGenerate(pFloor, Seed, &Instance))
	{
		dbg_msg("mapgentool", "acquire: FAILED %s_%d", pFloor, Seed);
		return false;
	}

	// prove the bytes are a loadable map without touching a file
	IEngineMap *pMap = CreateEngineMap();
	char aName[64];
	str_format(aName, sizeof(aName), "%s_%d", pFloor, Seed);
	const bool Loaded = pMap->LoadFromMemory(aName, Instance.m_pMap, Instance.m_MapSize);
	int Groups = 0;
	if(Loaded)
	{
		int Start = 0;
		pMap->GetType(MAPITEMTYPE_GROUP, &Start, &Groups);
	}
	pMap->Unload();

	char aCacheName[64];
	str_format(aCacheName, sizeof(aCacheName), "%s_%d.map", pFloor, Seed);
	char aCache[IO_MAX_PATH_LENGTH];
	const bool CacheFile = pStorage->FindFile(aCacheName, GENERATED_MAPS_DIR, IStorage::TYPE_SAVE, aCache, sizeof(aCache));

	dbg_msg("mapgentool", "acquire %s: map=%d bytes json=%d bytes loaded=%d groups=%d cache_file=%d",
		aName, Instance.m_MapSize, Instance.m_JsonSize, Loaded, Groups, CacheFile);
	CMapGen::FreeInstance(&Instance);
	return Loaded;
}

static void PrintUsage(const char *pProgram)
{
	dbg_msg("mapgentool", "usage: %s list", pProgram);
	dbg_msg("mapgentool", "       %s <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s all [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s ringcheck <floor...>", pProgram);
	dbg_msg("mapgentool", "       %s worldjson <map...>", pProgram);
	dbg_msg("mapgentool", "       %s pack <pack-file> <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s unpack <pack-file>", pProgram);
	dbg_msg("mapgentool", "       %s ensure <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s acquire <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s mappack add <pack-file> <floor> [seed...]", pProgram);
	dbg_msg("mapgentool", "       %s mappack list <pack-file>", pProgram);
	dbg_msg("mapgentool", "       %s mappack get <pack-file> <map-name>", pProgram);
	dbg_msg("mapgentool", "       %s mappack compact <pack-file>", pProgram);
	dbg_msg("mapgentool", "runs the mapgen world generator without the server and writes");
	dbg_msg("mapgentool", GENERATED_MAPS_DIR "/<floor>_<seed>.map into the storage save path");
}

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
	if(str_comp(pCommand, "acquire") == 0)
	{
		if(argc < 3)
		{
			PrintUsage(argv[0]);
			return 1;
		}
		// the server path: the pack is the durable copy, no cache file is kept
		Gen.SetDeleteGeneratedFiles(true);
		int Failures = 0;
		const char *pFloor = argv[2];
		int NumSeeds = 0;
		for(int i = 3; i < argc && NumSeeds < MAX_TOOL_SEEDS; i++, NumSeeds++)
			if(!AcquireInstance(pStorage, Gen, pFloor, str_toint(argv[i])))
				Failures++;
		if(NumSeeds == 0)
			return AcquireInstance(pStorage, Gen, pFloor, 1) ? 0 : 1;
		return Failures == 0 ? 0 : 1;
	}
	if(str_comp(pCommand, "ensure") == 0)
	{
		// the exact path the server takes when it loads a map: restore from
		// the floor's pack when possible, generate and pack otherwise
		if(argc < 3)
		{
			PrintUsage(argv[0]);
			return 1;
		}
		const char *pFloor = argv[2];
		int aSeeds[MAX_TOOL_SEEDS];
		int NumSeeds = 0;
		for(int i = 3; i < argc && NumSeeds < MAX_TOOL_SEEDS; i++)
			aSeeds[NumSeeds++] = str_toint(argv[i]);
		if(NumSeeds == 0)
			aSeeds[NumSeeds++] = 1;
		int Failures = 0;
		for(int i = 0; i < NumSeeds; i++)
		{
			const bool Ok = Gen.EnsureNewMap(pFloor, aSeeds[i]);
			char aPack[IO_MAX_PATH_LENGTH];
			Gen.PackPath(pFloor, aPack, sizeof(aPack));
			dbg_msg("mapgentool", "ensure %s_%d: %s (pack %s)", pFloor, aSeeds[i], Ok ? "ok" : "FAIL", aPack);
			if(!Ok)
				Failures++;
		}
		return Failures == 0 ? 0 : 1;
	}
	if(str_comp(pCommand, "mappack") == 0)
	{
		if(argc < 4)
		{
			PrintUsage(argv[0]);
			return 1;
		}
		const char *pSub = argv[2];
		const char *pPack = argv[3];
		if(str_comp(pSub, "list") == 0)
			return PackList(pStorage, pPack) ? 0 : 1;
		if(str_comp(pSub, "compact") == 0)
			return PackCompact(pStorage, pPack) ? 0 : 1;
		if(str_comp(pSub, "get") == 0)
		{
			if(argc < 5)
			{
				PrintUsage(argv[0]);
				return 1;
			}
			return PackGet(pStorage, pPack, argv[4]) ? 0 : 1;
		}
		if(str_comp(pSub, "add") == 0)
		{
			if(argc < 5)
			{
				PrintUsage(argv[0]);
				return 1;
			}
			const char *pFloor = argv[4];
			int aSeeds[MAX_TOOL_SEEDS];
			int NumSeeds = 0;
			for(int i = 5; i < argc && NumSeeds < MAX_TOOL_SEEDS; i++)
				aSeeds[NumSeeds++] = str_toint(argv[i]);
			if(NumSeeds == 0)
				aSeeds[NumSeeds++] = 1;
			return PackAppend(pStorage, Gen, pPack, pFloor, aSeeds, NumSeeds) ? 0 : 1;
		}
		PrintUsage(argv[0]);
		return 1;
	}
	if(str_comp(pCommand, "unpack") == 0)
	{
		if(argc < 3)
		{
			PrintUsage(argv[0]);
			return 1;
		}
		return UnpackFloor(pStorage, argv[2]) ? 0 : 1;
	}
	if(str_comp(pCommand, "pack") == 0)
	{
		if(argc < 4)
		{
			PrintUsage(argv[0]);
			return 1;
		}
		const char *pOut = argv[2];
		const char *pFloor = argv[3];
		int aSeeds[MAX_TOOL_SEEDS];
		int NumSeeds = 0;
		for(int i = 4; i < argc && NumSeeds < MAX_TOOL_SEEDS; i++)
			aSeeds[NumSeeds++] = str_toint(argv[i]);
		if(NumSeeds == 0)
			aSeeds[NumSeeds++] = 1;
		return PackFloor(pStorage, Gen, pFloor, pOut, aSeeds, NumSeeds) ? 0 : 1;
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
