/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/hash.h>
#include <engine/shared/worldpack.h>

#include "mappack.h"

#include <zlib.h>
#include <zstd.h>

static int Align4(int Size)
{
	return (Size + 3) & ~3;
}

static void PutU32(char *p, unsigned Value)
{
	p[0] = (char) (Value & 0xff);
	p[1] = (char) ((Value >> 8) & 0xff);
	p[2] = (char) ((Value >> 16) & 0xff);
	p[3] = (char) ((Value >> 24) & 0xff);
}

static unsigned GetU32(const char *p)
{
	return (unsigned) (unsigned char) p[0] |
		((unsigned) (unsigned char) p[1] << 8) |
		((unsigned) (unsigned char) p[2] << 16) |
		((unsigned) (unsigned char) p[3] << 24);
}

static bool HasId(const char *p, const char *pId)
{
	return p[0] == pId[0] && p[1] == pId[1] && p[2] == pId[2] && p[3] == pId[3];
}

static void WriteHeader(char *p)
{
	mem_copy(p, "MPK2", 4);
	PutU32(p + 4, MAPPACK_VERSION);
	PutU32(p + 8, 0);
	PutU32(p + 12, 0);
}

// Same acceptance rule as a world pack: a payload must be a v4 map file.
static bool LooksLikeMap(const void *pData, int Size)
{
	if(!pData || Size < 36)
		return false;
	const unsigned char *p = (const unsigned char *) pData;
	return p[0] == 'D' && p[1] == 'A' && p[2] == 'T' && p[3] == 'A';
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
CMapPackWriter::CMapPackWriter()
{
	m_pStorage = nullptr;
	m_aPath[0] = 0;
	m_CompressLevel = 9;
}

bool CMapPackWriter::ReadTail(unsigned *pLastIndex, unsigned *pCount, unsigned *pSeq)
{
	*pLastIndex = 0;
	*pCount = 0;
	*pSeq = 0;

	IOHANDLE File = m_pStorage->OpenFile(m_aPath, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	const long int Size = io_length(File);
	if(Size < MAPPACK_HEADER_SIZE)
	{
		io_close(File);
		return false;
	}
	char aHeader[MAPPACK_HEADER_SIZE];
	io_seek(File, 0, IOSEEK_START);
	if(io_read(File, aHeader, sizeof(aHeader)) != sizeof(aHeader) || !HasId(aHeader, "MPK2"))
	{
		io_close(File);
		return false;
	}
	if(Size < MAPPACK_HEADER_SIZE + MAPPACK_FOOTER_SIZE)
	{
		// header only: a valid empty pack
		io_close(File);
		return true;
	}
	char aFooter[MAPPACK_FOOTER_SIZE];
	io_seek(File, Size - MAPPACK_FOOTER_SIZE, IOSEEK_START);
	if(io_read(File, aFooter, sizeof(aFooter)) != sizeof(aFooter) || !HasId(aFooter, "MPKE"))
	{
		io_close(File);
		return true;
	}
	const unsigned FooterCrc = GetU32(aFooter + 16);
	if((unsigned) crc32(0L, (const Bytef *) aFooter, 16) != FooterCrc)
	{
		// a torn footer is ignored: the previous pack state stays valid
		io_close(File);
		return true;
	}
	*pLastIndex = GetU32(aFooter + 4);
	*pCount = GetU32(aFooter + 8);
	*pSeq = GetU32(aFooter + 12);
	io_close(File);
	return true;
}

bool CMapPackWriter::Open(IStorage *pStorage, const char *pFilename)
{
	if(!pStorage || !pFilename || !pFilename[0])
		return false;
	m_pStorage = pStorage;
	str_copy(m_aPath, pFilename, sizeof(m_aPath));

	// an existing file must at least carry our header, so that a stray path can
	// never be appended to (or truncated)
	IOHANDLE Probe = pStorage->OpenFile(m_aPath, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(Probe)
	{
		// a zero byte file is the leftover of a create that died before the
		// header was written, so it is initialized like a new pack
		const long int Length = io_length(Probe);
		io_close(Probe);
		if(Length > 0)
		{
			unsigned Last, Count, Seq;
			return ReadTail(&Last, &Count, &Seq);
		}
	}

	IOHANDLE File = pStorage->OpenFile(m_aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	char aHeader[MAPPACK_HEADER_SIZE];
	WriteHeader(aHeader);
	const bool Ok = io_write(File, aHeader, sizeof(aHeader)) == sizeof(aHeader);
	io_close(File);
	return Ok;
}

bool CMapPackWriter::AddMap(const char *pName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize)
{
	if(!m_pStorage || !pName || !pName[0] || str_length(pName) >= MAPPACK_NAME_LENGTH)
		return false;
	if(!LooksLikeMap(pMap, MapSize) || JsonSize < 0)
		return false;

	unsigned LastIndex = 0, Count = 0, Seq = 0;
	if(!ReadTail(&LastIndex, &Count, &Seq))
		return false;

	// the payload is a world pack entry, so both formats share the extraction
	// path and a frozen v5 pack can be imported entry by entry
	const int JsonPadded = Align4(JsonSize);
	const int MapPadded = Align4(MapSize);
	const int PayloadSize = (int) sizeof(SWorldPackEntryHeader) + JsonPadded + MapPadded;
	array<char> lPayload;
	lPayload.set_size(PayloadSize);
	mem_zero(lPayload.base_ptr(), PayloadSize);
	SWorldPackEntryHeader Header;
	Header.m_Magic = WORLDPACK_ENTRY_MAGIC;
	Header.m_JsonSize = JsonSize;
	Header.m_MapSize = MapSize;
	Header.m_Flags = 0;
	mem_copy(lPayload.base_ptr(), &Header, sizeof(Header));
	if(JsonSize > 0 && pJson)
		mem_copy(lPayload.base_ptr() + sizeof(Header), pJson, JsonSize);
	mem_copy(lPayload.base_ptr() + sizeof(Header) + JsonPadded, pMap, MapSize);

	const unsigned Crc = (unsigned) crc32(0L, (const Bytef *) lPayload.base_ptr(), PayloadSize);
	const size_t Bound = ZSTD_compressBound(PayloadSize);
	array<char> lCompressed;
	lCompressed.set_size((int) Bound);
	const size_t CompSize = ZSTD_compress(lCompressed.base_ptr(), Bound, lPayload.base_ptr(), PayloadSize, m_CompressLevel);
	if(ZSTD_isError(CompSize))
	{
		dbg_msg("mappack", "compression error %s", ZSTD_getErrorName(CompSize));
		return false;
	}

	// everything below is append only: the blob, then one index record that
	// chains to the previous one, then the footer that makes the new record
	// current. A failure before the footer leaves the old state readable.
	IOHANDLE File = m_pStorage->OpenFile(m_aPath, IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	if(io_length(File) < MAPPACK_HEADER_SIZE)
	{
		char aHeader[MAPPACK_HEADER_SIZE];
		WriteHeader(aHeader);
		io_write(File, aHeader, sizeof(aHeader));
	}

	const long int BlobOffset = io_length(File);
	char aBlobHeader[MAPPACK_BLOB_HEADER_SIZE];
	mem_copy(aBlobHeader, "BLB1", 4);
	PutU32(aBlobHeader + 4, (unsigned) PayloadSize);
	PutU32(aBlobHeader + 8, Crc);
	PutU32(aBlobHeader + 12, (unsigned) CompSize);
	bool Ok = io_write(File, aBlobHeader, sizeof(aBlobHeader)) == sizeof(aBlobHeader);
	Ok = Ok && io_write(File, lCompressed.base_ptr(), (unsigned) CompSize) == (unsigned) CompSize;

	const long int IndexOffset = io_length(File);
	char aIndex[MAPPACK_INDEX_SIZE];
	mem_zero(aIndex, sizeof(aIndex));
	mem_copy(aIndex, "IDX1", 4);
	PutU32(aIndex + 4, (unsigned) BlobOffset);
	PutU32(aIndex + 8, (unsigned) CompSize);
	PutU32(aIndex + 12, (unsigned) PayloadSize);
	PutU32(aIndex + 16, Crc);
	PutU32(aIndex + 20, (unsigned) Seed);
	PutU32(aIndex + 24, LastIndex);
	str_copy(aIndex + 28, pName, MAPPACK_NAME_LENGTH);
	PutU32(aIndex + 92, (unsigned) crc32(0L, (const Bytef *) aIndex, 92));
	Ok = Ok && io_write(File, aIndex, sizeof(aIndex)) == sizeof(aIndex);

	char aFooter[MAPPACK_FOOTER_SIZE];
	mem_copy(aFooter, "MPKE", 4);
	PutU32(aFooter + 4, (unsigned) IndexOffset);
	PutU32(aFooter + 8, Count + 1);
	PutU32(aFooter + 12, Seq + 1);
	PutU32(aFooter + 16, (unsigned) crc32(0L, (const Bytef *) aFooter, 16));
	Ok = Ok && io_write(File, aFooter, sizeof(aFooter)) == sizeof(aFooter);

	io_close(File);
	if(!Ok)
		dbg_msg("mappack", "short write while appending %s", pName);
	return Ok;
}

bool CMapPackWriter::Compact(IStorage *pStorage, const char *pFilename,
	int *pNumMaps, int *pSizeBefore, int *pSizeAfter)
{
	if(pNumMaps)
		*pNumMaps = 0;
	if(pSizeBefore)
		*pSizeBefore = 0;
	if(pSizeAfter)
		*pSizeAfter = 0;
	if(!pStorage || !pFilename || !pFilename[0])
		return false;

	CMapPackReader Reader;
	if(!Reader.Open(pStorage, pFilename, IStorage::TYPE_SAVE))
		return false;

	// the reader de-duplicates the chain, so Entry(i) is the live set
	const int NumMaps = Reader.NumMaps();

	SHA256_DIGEST Sha;
	unsigned Crc = 0;
	unsigned SizeBefore = 0;
	if(pStorage->GetHashAndSize(pFilename, IStorage::TYPE_SAVE, &Sha, &Crc, &SizeBefore))
	{
		if(pSizeBefore)
			*pSizeBefore = (int) SizeBefore;
	}

	char aTmp[IO_MAX_PATH_LENGTH];
	str_format(aTmp, sizeof(aTmp), "%s.compact", pFilename);
	// a leftover from an interrupted run is never a valid pack
	pStorage->RemoveFile(aTmp, IStorage::TYPE_SAVE);

	bool Ok = true;
	{
		CMapPackWriter Writer;
		if(!Writer.Open(pStorage, aTmp))
			Ok = false;

		for(int i = 0; Ok && i < NumMaps; i++)
		{
			const SMapPackEntry *pEntry = Reader.Entry(i);
			const void *pMap = nullptr, *pJson = nullptr;
			int MapSize = 0, JsonSize = 0;
			if(!pEntry || !Reader.GetMap(i, &pMap, &MapSize, &pJson, &JsonSize))
			{
				Ok = false;
				break;
			}
			if(!Writer.AddMap(pEntry->m_aName, pEntry->m_Seed, pMap, MapSize, pJson, JsonSize))
			{
				Ok = false;
				break;
			}
		}
		// every AddMap commits its own record and footer, so an empty pack is
		// simply a header and the last append already left a valid state
	}

	Reader.Close();

	if(!Ok)
	{
		pStorage->RemoveFile(aTmp, IStorage::TYPE_SAVE);
		return false;
	}

	// the old file must be closed before it is replaced (the reader above
	// still had it open for non-POSIX platforms)
	unsigned SizeAfter = 0;
	{
		SHA256_DIGEST NewSha;
		unsigned NewCrc = 0;
		if(pStorage->GetHashAndSize(aTmp, IStorage::TYPE_SAVE, &NewSha, &NewCrc, &SizeAfter))
		{
			if(pSizeAfter)
				*pSizeAfter = (int) SizeAfter;
		}
	}
	if(!pStorage->RenameFile(aTmp, pFilename, IStorage::TYPE_SAVE))
	{
		pStorage->RemoveFile(aTmp, IStorage::TYPE_SAVE);
		return false;
	}

	if(pNumMaps)
		*pNumMaps = NumMaps;
	return true;
}

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------
CMapPackReader::CMapPackReader()
{
	m_File = nullptr;
	m_aPath[0] = 0;
}

CMapPackReader::~CMapPackReader()
{
	Close();
}

void CMapPackReader::CloseFile()
{
	if(m_File)
	{
		io_close(m_File);
		m_File = nullptr;
	}
}

void CMapPackReader::Close()
{
	CloseFile();
	m_lEntries.clear();
	m_Payload.clear();
}

bool CMapPackReader::Open(IStorage *pStorage, const char *pFilename, int StorageType)
{
	Close();
	if(!pStorage || !pFilename)
		return false;
	m_File = pStorage->OpenFile(pFilename, IOFLAG_READ, StorageType);
	if(!m_File)
		return false;
	str_copy(m_aPath, pFilename, sizeof(m_aPath));
	if(!LoadIndex())
	{
		CloseFile();
		return false;
	}
	return true;
}

bool CMapPackReader::LoadIndex()
{
	const long int Size = io_length(m_File);
	if(Size < MAPPACK_HEADER_SIZE)
		return false;

	char aHeader[MAPPACK_HEADER_SIZE];
	io_seek(m_File, 0, IOSEEK_START);
	if(io_read(m_File, aHeader, sizeof(aHeader)) != sizeof(aHeader) || !HasId(aHeader, "MPK2"))
		return false;
	if(GetU32(aHeader + 4) != MAPPACK_VERSION)
		return false;

	if(Size < MAPPACK_HEADER_SIZE + MAPPACK_FOOTER_SIZE)
		return true; // an empty pack

	char aFooter[MAPPACK_FOOTER_SIZE];
	io_seek(m_File, Size - MAPPACK_FOOTER_SIZE, IOSEEK_START);
	if(io_read(m_File, aFooter, sizeof(aFooter)) != sizeof(aFooter) || !HasId(aFooter, "MPKE"))
		return true; // header only, no commit yet
	if((unsigned) crc32(0L, (const Bytef *) aFooter, 16) != GetU32(aFooter + 16))
		return false; // a torn footer is a corrupt pack, not an empty one

	unsigned Index = GetU32(aFooter + 4);
	const unsigned Count = GetU32(aFooter + 8);
	if(Count > 1u << 20)
		return false;

	// walk the chain newest to oldest; a name seen first is the live one
	for(unsigned i = 0; i < Count && Index >= MAPPACK_HEADER_SIZE; i++)
	{
		if((long int) Index + MAPPACK_INDEX_SIZE > Size - MAPPACK_FOOTER_SIZE)
			return false;
		char aIndex[MAPPACK_INDEX_SIZE];
		io_seek(m_File, Index, IOSEEK_START);
		if(io_read(m_File, aIndex, sizeof(aIndex)) != sizeof(aIndex))
			return false;
		if(!HasId(aIndex, "IDX1"))
			return false;
		if((unsigned) crc32(0L, (const Bytef *) aIndex, 92) != GetU32(aIndex + 92))
			return false;

		SMapPackEntry Entry;
		mem_zero(&Entry, sizeof(Entry));
		Entry.m_BlobOffset = GetU32(aIndex + 4);
		Entry.m_CompSize = GetU32(aIndex + 8);
		Entry.m_RawSize = GetU32(aIndex + 12);
		Entry.m_Crc = GetU32(aIndex + 16);
		Entry.m_Seed = (int) GetU32(aIndex + 20);
		str_copy(Entry.m_aName, aIndex + 28, MAPPACK_NAME_LENGTH);

		bool Superseded = false;
		for(int k = 0; k < m_lEntries.size(); k++)
		{
			if(str_comp(m_lEntries[k].m_aName, Entry.m_aName) == 0)
			{
				Superseded = true;
				break;
			}
		}
		if(!Superseded)
			m_lEntries.add(Entry);

		const unsigned Prev = GetU32(aIndex + 24);
		if(Prev == Index)
			break;
		Index = Prev;
	}
	return true;
}

const SMapPackEntry *CMapPackReader::Entry(int Index) const
{
	if(Index < 0 || Index >= m_lEntries.size())
		return nullptr;
	return &m_lEntries[Index];
}

int CMapPackReader::FindMap(const char *pName) const
{
	if(!pName)
		return -1;
	for(int i = 0; i < m_lEntries.size(); i++)
		if(str_comp(m_lEntries[i].m_aName, pName) == 0)
			return i;
	return -1;
}

bool CMapPackReader::LoadBlob(const SMapPackEntry &Entry)
{
	if(!m_File)
		return false;
	if(Entry.m_CompSize == 0 || Entry.m_CompSize > (1u << 26) || Entry.m_RawSize < sizeof(SWorldPackEntryHeader))
		return false;

	char aBlobHeader[MAPPACK_BLOB_HEADER_SIZE];
	io_seek(m_File, Entry.m_BlobOffset, IOSEEK_START);
	if(io_read(m_File, aBlobHeader, sizeof(aBlobHeader)) != sizeof(aBlobHeader))
		return false;
	if(!HasId(aBlobHeader, "BLB1"))
		return false;
	if(GetU32(aBlobHeader + 4) != Entry.m_RawSize || GetU32(aBlobHeader + 8) != Entry.m_Crc ||
		GetU32(aBlobHeader + 12) != Entry.m_CompSize)
		return false;

	array<char> lCompressed;
	lCompressed.set_size((int) Entry.m_CompSize);
	if(io_read(m_File, lCompressed.base_ptr(), Entry.m_CompSize) != Entry.m_CompSize)
		return false;

	m_Payload.set_size((int) Entry.m_RawSize);
	const size_t Result = ZSTD_decompress(m_Payload.base_ptr(), Entry.m_RawSize, lCompressed.base_ptr(), Entry.m_CompSize);
	if(ZSTD_isError(Result) || Result != Entry.m_RawSize)
	{
		m_Payload.clear();
		return false;
	}
	if((unsigned) crc32(0L, (const Bytef *) m_Payload.base_ptr(), Entry.m_RawSize) != Entry.m_Crc)
	{
		m_Payload.clear();
		return false;
	}

	const SWorldPackEntryHeader *pHeader = (const SWorldPackEntryHeader *) m_Payload.base_ptr();
	if(pHeader->m_Magic != WORLDPACK_ENTRY_MAGIC)
		return false;
	const int Needed = (int) sizeof(SWorldPackEntryHeader) + Align4(pHeader->m_JsonSize) + Align4(pHeader->m_MapSize);
	if(Needed > (int) Entry.m_RawSize || pHeader->m_MapSize <= 0 || pHeader->m_JsonSize < 0)
		return false;
	return true;
}

bool CMapPackReader::GetMap(int Index, const void **ppMap, int *pMapSize, const void **ppJson, int *pJsonSize)
{
	if(ppMap)
		*ppMap = nullptr;
	if(pMapSize)
		*pMapSize = 0;
	if(ppJson)
		*ppJson = nullptr;
	if(pJsonSize)
		*pJsonSize = 0;
	const SMapPackEntry *pEntry = Entry(Index);
	if(!pEntry || !LoadBlob(*pEntry))
		return false;

	const SWorldPackEntryHeader *pHeader = (const SWorldPackEntryHeader *) m_Payload.base_ptr();
	const char *pJsonData = m_Payload.base_ptr() + sizeof(SWorldPackEntryHeader);
	const char *pMapData = pJsonData + Align4(pHeader->m_JsonSize);
	if(ppJson && pHeader->m_JsonSize > 0)
		*ppJson = pJsonData;
	if(pJsonSize)
		*pJsonSize = pHeader->m_JsonSize;
	if(ppMap)
		*ppMap = pMapData;
	if(pMapSize)
		*pMapSize = pHeader->m_MapSize;
	return true;
}

bool CMapPackReader::Verify(int Index)
{
	// LoadBlob already checks the blob header, the payload crc and the entry
	// header against the index record, so a successful read is the check
	const void *pMap = nullptr;
	int MapSize = 0;
	return GetMap(Index, &pMap, &MapSize);
}

bool CMapPackReader::ExtractMap(int Index, IStorage *pStorage, const char *pFilename, bool WriteJson)
{
	const void *pMap = nullptr;
	const void *pJson = nullptr;
	int MapSize = 0;
	int JsonSize = 0;
	if(!GetMap(Index, &pMap, &MapSize, &pJson, &JsonSize))
		return false;

	IOHANDLE File = pStorage->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	const bool Written = io_write(File, pMap, MapSize) == (unsigned) MapSize;
	io_close(File);
	if(!Written)
		return false;

	if(WriteJson && JsonSize > 0)
	{
		char aJsonPath[IO_MAX_PATH_LENGTH];
		str_copy(aJsonPath, pFilename, sizeof(aJsonPath));
		const int Len = str_length(aJsonPath);
		if(Len > 4 && str_comp(aJsonPath + Len - 4, ".map") == 0)
			str_copy(aJsonPath + Len - 4, ".json", sizeof(aJsonPath) - (Len - 4));
		else
			str_append(aJsonPath, ".json", sizeof(aJsonPath));
		IOHANDLE JsonFile = pStorage->OpenFile(aJsonPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(!JsonFile)
			return false;
		const bool JsonWritten = io_write(JsonFile, pJson, JsonSize) == (unsigned) JsonSize;
		io_close(JsonFile);
		if(!JsonWritten)
			return false;
	}
	return true;
}
