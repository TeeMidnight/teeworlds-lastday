/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SHARED_WORLDPACK_H
#define ENGINE_SHARED_WORLDPACK_H

#include <base/system.h>
#include <base/tl/array.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

// A world pack bundles the generated map instances of one floor into a single
// v5 datafile: the maps and their world metadata sidecars travel together, so
// one transfer replaces a directory of .map/.json pairs.
//
// Layout of a pack:
//   item type WORLDPACK_ITEM_TYPE, id 0        the index (json, see below)
//   item type WORLDPACK_ITEM_TYPE, id 1..n     one map each
//
// A map item's payload is:
//   SWorldPackEntryHeader
//   json bytes, padded to 4
//   map bytes (a complete v4 datafile), padded to 4
//
// v5 compresses the whole item section as one zstd block, so the maps and
// their metadata are compressed together by the container. A pack is meant to
// be a shard (a few dozen instances), not a whole 1024 instance floor: opening
// a pack decompresses every item in it, and the point of a shard is that this
// stays cheap while an edit only rewrites a small file.
//
// The index json is:
//   {"format":1,"floor":"Verdant","shard":0,"maps":[
//      {"id":1,"name":"Verdant_1","seed":1,"map_size":31218,"json_size":912,
//       "crc":123456789,"sha256":"..."}]}
//
// The map bytes stay a plain v4 datafile so a client never needs to know about
// packs: the server extracts the item and serves the file it always served.
enum
{
	WORLDPACK_ITEM_TYPE = 0x50,
	WORLDPACK_INDEX_ID = 0,
	WORLDPACK_FORMAT = 1,
	WORLDPACK_ENTRY_MAGIC = 0x31504b57, // "WKP1" little endian
};

struct SWorldPackEntryHeader
{
	int m_Magic;
	int m_JsonSize;
	int m_MapSize;
	int m_Flags;
};

struct SWorldPackEntryInfo
{
	char m_aName[64];
	int m_Seed;
	int m_ID;
	int m_MapSize;
	int m_JsonSize;
	unsigned m_Crc;
	char m_aSha256[SHA256_MAXSTRSIZE];
};

class CWorldPackWriter
{
	CDataFileWriter m_DataFile;
	array<SWorldPackEntryInfo> m_lEntries;
	char m_aFloor[64];
	int m_Shard;
	bool m_Open;

public:
	CWorldPackWriter();
	~CWorldPackWriter();

	bool Open(IStorage *pStorage, const char *pFilename, const char *pFloor, int Shard = 0, int Version = 5);
	// Add one map with its world json. Both blobs are copied. pName is the map
	// instance name ("Verdant_7"); Seed is its ring seed (or -1).
	bool AddMap(const char *pName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize);
	bool Finish();

	int NumMaps() const { return m_lEntries.size(); }
};

class CWorldPackReader
{
	CDataFileReader m_DataFile;
	SWorldPackEntryInfo *m_pEntries;
	int m_NumEntries;
	char m_aFloor[64];
	int m_Shard;

	bool LoadIndex();
	void UnloadIndex();

public:
	CWorldPackReader();
	~CWorldPackReader();

	bool Open(IStorage *pStorage, const char *pFilename, int StorageType = IStorage::TYPE_ALL);
	void Close();
	bool IsOpen() const { return m_DataFile.IsOpen(); }

	const char *Floor() const { return m_aFloor; }
	int Shard() const { return m_Shard; }
	int NumMaps() const { return m_NumEntries; }
	const SWorldPackEntryInfo *Entry(int Index) const;
	// index of the entry with this map name, or -1
	int FindMap(const char *pName) const;

	// Pointers are valid until the pack is closed. Either output may be null.
	bool GetMap(int Index, const void **ppMap, int *pMapSize, const void **ppJson = nullptr, int *pJsonSize = nullptr);

	// Verify the entry against the size/crc/sha256 recorded in the index.
	bool Verify(int Index);

	// Write one map as a plain v4 map file at pFilename. The json sidecar is
	// written next to it ("<pFilename without .map>.json") when pWriteJson.
	bool ExtractMap(int Index, IStorage *pStorage, const char *pFilename, bool WriteJson = true);
};

#endif
