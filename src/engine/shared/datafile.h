/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SHARED_DATAFILE_H
#define ENGINE_SHARED_DATAFILE_H

#include <base/hash.h>
#include <base/system.h>
#include <base/tl/stream.h>

// raw datafile access
class CDataFileReader
{
	struct CDatafile *m_pDataFile;
	// Progress lines (which file is opened, which layers are decompressed). A
	// map generation reads many base and structure maps, so it asks for these
	// to be silenced; errors are printed regardless.
	bool m_ConsoleOutput;
	void *GetDataImpl(int Index, int Swap);
	int GetFileDataSize(int Index) const;
	int GetFileItemSize(int Index) const;

public:
	CDataFileReader() :
		m_pDataFile(0), m_ConsoleOutput(true) {}
	~CDataFileReader() { Close(); }

	bool IsOpen() const { return m_pDataFile != 0; }
	void SetConsoleOutput(bool ConsoleOutput) { m_ConsoleOutput = ConsoleOutput; }

	bool Open(class IStorage *pStorage, const char *pFilename, int StorageType);
	// Read a datafile from any stream: a file (the overload above wraps one) or
	// a buffer that already exists, such as a map restored from a pack. With
	// OwnStream the reader deletes the stream when it closes.
	bool Open(stream *pStream, bool OwnStream = true);
	bool Close();

	void *GetData(int Index);
	void *GetDataSwapped(int Index); // makes sure that the data is 32bit LE ints when saved
	int GetDataSize(int Index) const;
	void ReplaceData(int Index, char *pData, int Size);
	void UnloadData(int Index);
	void *GetItem(int Index, int *pType, int *pID);
	int GetItemSize(int Index) const;
	void GetType(int Type, int *pStart, int *pNum);
	void *FindItem(int Type, int ID);
	int NumItems() const;
	int NumData() const;
	void Unload();

	SHA256_DIGEST Sha256() const;
	unsigned Crc() const;

	static bool CheckSha256(IOHANDLE Handle, const void *pSha256);
};

// write access
class CDataFileWriter
{
	struct CDataInfo
	{
		int m_UncompressedSize;
		int m_CompressedSize;
		void *m_pCompressedData;
	};

	struct CItemInfo
	{
		int m_Type;
		int m_ID;
		int m_Size;
		int m_Next;
		int m_Prev;
		void *m_pData;
	};

	struct CItemTypeInfo
	{
		int m_Num;
		int m_First;
		int m_Last;
	};

	enum
	{
		MAX_ITEM_TYPES = 0xffff,
		MAX_ITEMS = 4096,
		MAX_DATAS = 4096,
	};

	// the writer writes through a stream as well, so a map can be built in
	// memory (a pack entry, a network buffer) instead of a file
	stream *m_pStream;
	bool m_OwnsStream;
	// 4 (zlib, readable by 0.7) or 5 (zstd, item section compressed as a
	// whole). Maps are written as 4 so legacy clients can still load them.
	int m_Version;
	int m_CompressLevel;
	int m_NumItems;
	int m_NumDatas;
	int m_NumItemTypes;
	CItemTypeInfo *m_pItemTypes;
	CItemInfo *m_pItems;
	CDataInfo *m_pDatas;

public:
	CDataFileWriter();
	~CDataFileWriter();
	bool Open(class IStorage *pStorage, const char *Filename, int Version = 4);
	// Write into any writable stream. With OwnStream the writer deletes the
	// stream when it finishes.
	bool Open(stream *pStream, int Version = 4, bool OwnStream = true);
	void SetCompressLevel(int Level) { m_CompressLevel = Level; }
	int AddData(int Size, const void *pData);
	int AddDataSwapped(int Size, const void *pData);
	int AddItem(int Type, int ID, int Size, const void *pData);
	int Finish();
};

#endif
