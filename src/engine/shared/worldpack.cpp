/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "worldpack.h"

#include <base/hash.h>
#include <base/tl/stream.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <zlib.h>

namespace {

int Align4(int Size)
{
	return (Size + 3) & ~3;
}

// A pack carries v4 map files. Accepting anything else here would defer the
// failure to the moment a client loads the extracted file.
bool LooksLikeMap(const void *pData, int Size)
{
	// the v4 header is 36 bytes: 4 signature bytes and eight 32 bit fields
	if(!pData || Size < 36)
		return false;
	const unsigned char *p = (const unsigned char *) pData;
	return p[0] == 'D' && p[1] == 'A' && p[2] == 'T' && p[3] == 'A';
}

} // namespace

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
CWorldPackWriter::CWorldPackWriter()
{
	m_aFloor[0] = 0;
	m_Shard = 0;
	m_Open = false;
}

CWorldPackWriter::~CWorldPackWriter()
{
	if(m_Open)
		m_DataFile.Finish();
}

bool CWorldPackWriter::Open(IStorage *pStorage, const char *pFilename, const char *pFloor, int Shard, int Version)
{
	m_Open = m_DataFile.Open(pStorage, pFilename, Version);
	if(!m_Open)
		return false;
	str_copy(m_aFloor, pFloor ? pFloor : "", sizeof(m_aFloor));
	m_Shard = Shard;
	m_lEntries.clear();
	return true;
}

bool CWorldPackWriter::AddMap(const char *pName, int Seed, const void *pMap, int MapSize, const void *pJson, int JsonSize)
{
	if(!m_Open || !pName || !pName[0])
		return false;
	if(!LooksLikeMap(pMap, MapSize) || JsonSize < 0)
		return false;

	// one item per map: header, the world json, then the v4 map file. Both
	// blobs are padded so the payload stays 4 byte aligned like every item.
	const int JsonPadded = Align4(JsonSize);
	const int MapPadded = Align4(MapSize);
	const int PayloadSize = (int) sizeof(SWorldPackEntryHeader) + JsonPadded + MapPadded;

	void *pPayload = mem_alloc(PayloadSize);
	mem_zero(pPayload, PayloadSize);
	SWorldPackEntryHeader Header;
	Header.m_Magic = WORLDPACK_ENTRY_MAGIC;
	Header.m_JsonSize = JsonSize;
	Header.m_MapSize = MapSize;
	Header.m_Flags = 0;
	mem_copy(pPayload, &Header, sizeof(Header));
	if(JsonSize > 0 && pJson)
		mem_copy((unsigned char *) pPayload + sizeof(Header), pJson, JsonSize);
	mem_copy((unsigned char *) pPayload + sizeof(Header) + JsonPadded, pMap, MapSize);

	const int ID = m_lEntries.size() + 1;
	m_DataFile.AddItem(WORLDPACK_ITEM_TYPE, ID, PayloadSize, pPayload);
	mem_free(pPayload);

	SWorldPackEntryInfo Info;
	mem_zero(&Info, sizeof(Info));
	str_copy(Info.m_aName, pName, sizeof(Info.m_aName));
	Info.m_Seed = Seed;
	Info.m_ID = ID;
	Info.m_MapSize = MapSize;
	Info.m_JsonSize = JsonSize;
	Info.m_Crc = (unsigned) crc32(0L, (const Bytef *) pMap, MapSize);
	sha256_str(sha256(pMap, MapSize), Info.m_aSha256, sizeof(Info.m_aSha256));
	m_lEntries.add(Info);
	return true;
}

bool CWorldPackWriter::Finish()
{
	if(!m_Open)
		return false;

	// the index is written last, once every entry is known
	array<char> lJson;
	memory_stream<char> Stream(&lJson);
	CJsonWriter Writer(&Stream);
	Writer.BeginObject();
	Writer.WriteAttribute("format");
	Writer.WriteIntValue(WORLDPACK_FORMAT);
	Writer.WriteAttribute("floor");
	Writer.WriteStrValue(m_aFloor);
	Writer.WriteAttribute("shard");
	Writer.WriteIntValue(m_Shard);
	Writer.WriteAttribute("maps");
	Writer.BeginArray();
	for(int i = 0; i < m_lEntries.size(); i++)
	{
		const SWorldPackEntryInfo &Entry = m_lEntries[i];
		Writer.BeginObject();
		Writer.WriteAttribute("id");
		Writer.WriteIntValue(Entry.m_ID);
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(Entry.m_aName);
		Writer.WriteAttribute("seed");
		Writer.WriteIntValue(Entry.m_Seed);
		Writer.WriteAttribute("map_size");
		Writer.WriteIntValue(Entry.m_MapSize);
		Writer.WriteAttribute("json_size");
		Writer.WriteIntValue(Entry.m_JsonSize);
		Writer.WriteAttribute("crc");
		Writer.WriteIntValue((int) Entry.m_Crc);
		Writer.WriteAttribute("sha256");
		Writer.WriteStrValue(Entry.m_aSha256);
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();
	lJson.add(0);

	const int JsonSize = (int) lJson.size() - 1; // without the terminator
	const int Padded = Align4(JsonSize);
	array<char> lPadded;
	lPadded.set_size(Padded);
	mem_zero(lPadded.base_ptr(), Padded);
	mem_copy(lPadded.base_ptr(), lJson.base_ptr(), JsonSize);
	m_DataFile.AddItem(WORLDPACK_ITEM_TYPE, WORLDPACK_INDEX_ID, Padded, lPadded.base_ptr());

	m_Open = false;
	return m_DataFile.Finish() != 0;
}

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------
CWorldPackReader::CWorldPackReader()
{
	m_pEntries = 0;
	m_NumEntries = 0;
	m_aFloor[0] = 0;
	m_Shard = 0;
}

CWorldPackReader::~CWorldPackReader()
{
	Close();
}

bool CWorldPackReader::Open(IStorage *pStorage, const char *pFilename, int StorageType)
{
	Close();
	if(!m_DataFile.Open(pStorage, pFilename, StorageType))
		return false;
	if(!LoadIndex())
	{
		m_DataFile.Close();
		return false;
	}
	return true;
}

void CWorldPackReader::Close()
{
	UnloadIndex();
	m_DataFile.Close();
}

void CWorldPackReader::UnloadIndex()
{
	delete[] m_pEntries;
	m_pEntries = 0;
	m_NumEntries = 0;
	m_aFloor[0] = 0;
	m_Shard = 0;
}

bool CWorldPackReader::LoadIndex()
{
	int Size = 0;
	const char *pJson = (const char *) m_DataFile.FindItem(WORLDPACK_ITEM_TYPE, WORLDPACK_INDEX_ID);
	if(!pJson)
		return false;
	// the index item is the only item found by the id, its size follows from
	// the pack's item table
	int Index = -1;
	{
		int Start = 0, Num = 0;
		m_DataFile.GetType(WORLDPACK_ITEM_TYPE, &Start, &Num);
		for(int i = 0; i < Num; i++)
		{
			int ID = 0;
			void *pItem = m_DataFile.GetItem(Start + i, nullptr, &ID);
			if(pItem == (void *) pJson)
			{
				Index = Start + i;
				break;
			}
		}
	}
	if(Index < 0)
		return false;
	Size = m_DataFile.GetItemSize(Index);
	if(Size <= 0)
		return false;

	CJsonParser Parser;
	// the item is padded with zeros, so it is a valid zero terminated string
	json_value *pRoot = Parser.ParseString(pJson, "world pack index");
	if(!pRoot || pRoot->type != json_object)
		return false;

	const json_value &rFormat = (*pRoot)["format"];
	if(rFormat.type != json_integer || (int) (json_int_t) rFormat != WORLDPACK_FORMAT)
		return false;
	const json_value &rFloor = (*pRoot)["floor"];
	if(rFloor.type == json_string)
		str_copy(m_aFloor, (const char *) rFloor, sizeof(m_aFloor));
	const json_value &rShard = (*pRoot)["shard"];
	if(rShard.type == json_integer)
		m_Shard = (int) (json_int_t) rShard;

	const json_value &rMaps = (*pRoot)["maps"];
	if(rMaps.type != json_array)
		return false;

	m_NumEntries = (int) rMaps.u.array.length;
	m_pEntries = new SWorldPackEntryInfo[m_NumEntries];
	for(int i = 0; i < m_NumEntries; i++)
	{
		SWorldPackEntryInfo &Entry = m_pEntries[i];
		mem_zero(&Entry, sizeof(Entry));
		const json_value &rMap = rMaps[i];
		if(rMap.type != json_object)
			return false;
		const json_value &rName = rMap["name"];
		if(rName.type != json_string)
			return false;
		str_copy(Entry.m_aName, (const char *) rName, sizeof(Entry.m_aName));
		const json_value &rSeed = rMap["seed"];
		Entry.m_Seed = rSeed.type == json_integer ? (int) (json_int_t) rSeed : -1;
		const json_value &rID = rMap["id"];
		const json_value &rMapSize = rMap["map_size"];
		const json_value &rJsonSize = rMap["json_size"];
		const json_value &rCrc = rMap["crc"];
		const json_value &rSha = rMap["sha256"];
		if(rID.type != json_integer || rMapSize.type != json_integer || rJsonSize.type != json_integer)
			return false;
		Entry.m_ID = (int) (json_int_t) rID;
		Entry.m_MapSize = (int) (json_int_t) rMapSize;
		Entry.m_JsonSize = (int) (json_int_t) rJsonSize;
		Entry.m_Crc = rCrc.type == json_integer ? (unsigned) (json_int_t) rCrc : 0;
		if(rSha.type == json_string)
			str_copy(Entry.m_aSha256, (const char *) rSha, sizeof(Entry.m_aSha256));
	}
	return true;
}

const SWorldPackEntryInfo *CWorldPackReader::Entry(int Index) const
{
	if(Index < 0 || Index >= m_NumEntries)
		return nullptr;
	return &m_pEntries[Index];
}

int CWorldPackReader::FindMap(const char *pName) const
{
	if(!pName)
		return -1;
	for(int i = 0; i < m_NumEntries; i++)
		if(str_comp(m_pEntries[i].m_aName, pName) == 0)
			return i;
	return -1;
}

bool CWorldPackReader::GetMap(int Index, const void **ppMap, int *pMapSize, const void **ppJson, int *pJsonSize)
{
	if(ppMap)
		*ppMap = nullptr;
	if(pMapSize)
		*pMapSize = 0;
	if(ppJson)
		*ppJson = nullptr;
	if(pJsonSize)
		*pJsonSize = 0;
	if(Index < 0 || Index >= m_NumEntries)
		return false;

	const SWorldPackEntryInfo &Entry = m_pEntries[Index];
	const unsigned char *pPayload = (const unsigned char *) m_DataFile.FindItem(WORLDPACK_ITEM_TYPE, Entry.m_ID);
	if(!pPayload)
		return false;
	const SWorldPackEntryHeader *pHeader = (const SWorldPackEntryHeader *) pPayload;
	if(pHeader->m_Magic != WORLDPACK_ENTRY_MAGIC)
		return false;
	if(pHeader->m_JsonSize != Entry.m_JsonSize || pHeader->m_MapSize != Entry.m_MapSize)
		return false;

	const unsigned char *pJsonData = pPayload + sizeof(SWorldPackEntryHeader);
	const unsigned char *pMapData = pJsonData + Align4(Entry.m_JsonSize);
	if(ppJson && Entry.m_JsonSize > 0)
		*ppJson = pJsonData;
	if(pJsonSize)
		*pJsonSize = Entry.m_JsonSize;
	if(ppMap)
		*ppMap = pMapData;
	if(pMapSize)
		*pMapSize = Entry.m_MapSize;
	return true;
}

bool CWorldPackReader::Verify(int Index)
{
	const void *pMap = nullptr;
	int MapSize = 0;
	if(!GetMap(Index, &pMap, &MapSize))
		return false;
	const SWorldPackEntryInfo *pEntry = Entry(Index);
	if(MapSize != pEntry->m_MapSize)
		return false;
	const unsigned Crc = (unsigned) crc32(0L, (const Bytef *) pMap, MapSize);
	if(pEntry->m_Crc != 0 && Crc != pEntry->m_Crc)
		return false;
	if(pEntry->m_aSha256[0])
	{
		char aSha[SHA256_MAXSTRSIZE];
		sha256_str(sha256(pMap, MapSize), aSha, sizeof(aSha));
		if(str_comp(aSha, pEntry->m_aSha256) != 0)
			return false;
	}
	return true;
}

bool CWorldPackReader::ExtractMap(int Index, IStorage *pStorage, const char *pFilename, bool WriteJson)
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
		// "<name>.map" -> "<name>.json"
		char aJsonPath[IO_MAX_PATH_LENGTH];
		str_copy(aJsonPath, pFilename, sizeof(aJsonPath));
		int Len = str_length(aJsonPath);
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
