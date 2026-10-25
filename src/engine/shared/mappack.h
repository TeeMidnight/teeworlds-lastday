/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SHARED_MAPPACK_H
#define ENGINE_SHARED_MAPPACK_H

#include <base/system.h>
#include <base/tl/array.h>
#include <engine/storage.h>

// An append-only pack of generated instances: the same map+json payload a
// world pack (datafile v5) holds, but laid out so that
//
//   - reading one map decompresses one blob and nothing else,
//   - adding one map appends a blob, one index record and a footer, and
//     touches nothing that is already written,
//   - an interrupted append leaves the previous pack state readable, because
//     the footer is written last and is what names the current index.
//
// A datafile v5 cannot do either: its item section is one zstd stream (so
// opening the file decompresses every item) and its offset tables sit before
// the payloads (so adding an item moves everything). This format exists for
// packs that are written to at runtime; a v5 world pack stays the right thing
// for a frozen distribution artifact.
//
// File layout (all integers little endian):
//
//   [header 16]  "MPK2", version, flags, reserved
//   [blob]       "BLB1", raw size, crc32(raw), compressed size, zstd payload
//   [index]      "IDX1", blob offset, comp size, raw size, crc, seed,
//                previous index offset, name[64], crc32(record)
//   [footer 20]  "MPKE", last index offset, record count, sequence,
//                crc32(footer without its crc)
//
// The index records form a chain: each names the previous one, so the writer
// appends one record per map instead of rewriting an index table. A record
// supersedes an older record with the same name, and the footer points at the
// newest record, so the reader walks the chain backwards to the oldest.
//
// The payload inside a blob is exactly a world pack entry:
//   SWorldPackEntryHeader, world json (padded to 4), v4 map (padded to 4).

enum
{
	MAPPACK_VERSION = 2,
	MAPPACK_HEADER_SIZE = 16,
	MAPPACK_FOOTER_SIZE = 20,
	MAPPACK_INDEX_SIZE = 96,
	MAPPACK_BLOB_HEADER_SIZE = 16,
	MAPPACK_NAME_LENGTH = 64,
};

struct SMapPackEntry
{
	char m_aName[MAPPACK_NAME_LENGTH];
	int m_Seed;
	unsigned m_BlobOffset;
	unsigned m_CompSize;
	unsigned m_RawSize;
	unsigned m_Crc;
};

class CMapPackWriter
{
	IStorage *m_pStorage;
	char m_aPath[IO_MAX_PATH_LENGTH];
	int m_CompressLevel;

	// reads the current footer (0/0 when the pack is empty or new)
	bool ReadTail(unsigned *pLastIndex, unsigned *pCount, unsigned *pSeq);

public:
	CMapPackWriter();
	void SetCompressLevel(int Level) { m_CompressLevel = Level; }
	// Creates the pack when it does not exist yet; an existing pack is opened
	// for appending.
	bool Open(IStorage *pStorage, const char *pFilename);
	// Append one map with its world json. The whole file is never rewritten.
	bool AddMap(const char *pName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize);

	// Rewrite a pack so it holds only the live instances: superseded blobs,
	// their index records and every old footer are dropped. The new pack is
	// built beside the old one and renamed into place, so an interrupted
	// compaction (or a crash) leaves the original readable.
	//
	// A running server keeps its pack reader open, so compaction is a
	// maintenance action: run it with the server stopped, or accept that the
	// server keeps using the pack it opened until it restarts.
	static bool Compact(IStorage *pStorage, const char *pFilename,
		int *pNumMaps = nullptr, int *pSizeBefore = nullptr, int *pSizeAfter = nullptr);
};

class CMapPackReader
{
	IOHANDLE m_File;
	array<SMapPackEntry> m_lEntries;
	// the last decompressed payload (header + json + map); pointers returned by
	// GetMap stay valid until the next GetMap or Close
	array<char> m_Payload;
	char m_aPath[IO_MAX_PATH_LENGTH];

	void CloseFile();
	bool LoadIndex();
	bool LoadBlob(const SMapPackEntry &Entry);

public:
	CMapPackReader();
	~CMapPackReader();

	bool Open(IStorage *pStorage, const char *pFilename, int StorageType = IStorage::TYPE_ALL);
	void Close();
	bool IsOpen() const { return m_File != nullptr; }

	int NumMaps() const { return m_lEntries.size(); }
	const SMapPackEntry *Entry(int Index) const;
	int FindMap(const char *pName) const;

	bool GetMap(int Index, const void **ppMap, int *pMapSize, const void **ppJson = nullptr, int *pJsonSize = nullptr);
	bool Verify(int Index);
	// Write the map as a plain v4 file, plus its json sidecar next to it.
	bool ExtractMap(int Index, IStorage *pStorage, const char *pFilename, bool WriteJson = true);
};

#endif
