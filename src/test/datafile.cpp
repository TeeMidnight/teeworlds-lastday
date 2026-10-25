/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "test.h"

#include <gtest/gtest.h>

#include <base/tl/array.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

TEST(Datafile, RoundtripItemDataAndSize)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".datafile");
	IStorage *pStorage = CreateTestStorage();
	CDataFileWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename));

	static const char TEST_DATA[] = "Hello World!";
	int Index = Writer.AddData(sizeof(TEST_DATA), TEST_DATA);
	int Index4 = Writer.AddData(4, TEST_DATA);
	int aItem[2] = {Index, Index4};
	Writer.AddItem(12, 34, sizeof(aItem), aItem);
	EXPECT_TRUE(Writer.Finish());

	CDataFileReader Reader;
	EXPECT_FALSE(Reader.IsOpen());
	ASSERT_TRUE(Reader.Open(pStorage, aFilename, IStorage::TYPE_ALL));
	EXPECT_TRUE(Reader.IsOpen());

	ASSERT_EQ(Reader.GetDataSize(Index), sizeof(TEST_DATA));
	EXPECT_TRUE(mem_comp(Reader.GetData(Index), TEST_DATA, sizeof(TEST_DATA)) == 0);
	ASSERT_EQ(Reader.GetDataSize(Index4), 4);
	EXPECT_TRUE(mem_comp(Reader.GetData(Index), TEST_DATA, 4) == 0);

	static const char REPL_DATA[] = "Replacement";
	char *pReplace4 = (char *) mem_alloc(4);
	mem_copy(pReplace4, REPL_DATA, 4);
	char *pReplace = (char *) mem_alloc(sizeof(REPL_DATA));
	mem_copy(pReplace, REPL_DATA, sizeof(REPL_DATA));
	Reader.ReplaceData(Index, pReplace4, 4);
	Reader.ReplaceData(Index4, pReplace, sizeof(REPL_DATA));

	ASSERT_EQ(Reader.GetDataSize(Index), 4);
	EXPECT_TRUE(mem_comp(Reader.GetData(Index), REPL_DATA, 4) == 0);
	ASSERT_EQ(Reader.GetDataSize(Index4), sizeof(REPL_DATA));
	EXPECT_TRUE(mem_comp(Reader.GetData(Index4), REPL_DATA, sizeof(REPL_DATA)) == 0);

	bool FoundItem = false;
	for(int i = 0; i < Reader.NumItems(); i++)
	{
		int Type;
		int ID;
		void *pItem = Reader.GetItem(i, &Type, &ID);
		ASSERT_EQ(Reader.GetItemSize(i), sizeof(aItem));
		EXPECT_TRUE(mem_comp(pItem, aItem, sizeof(aItem)) == 0);
		EXPECT_EQ(Type, 12);
		EXPECT_EQ(ID, 34);

		FoundItem = true;
	}
	EXPECT_TRUE(FoundItem);

	EXPECT_TRUE(Reader.Close());

	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

// v5 compresses the item section as one zstd block and every data blob on its
// own. Reading it back must be indistinguishable from v4 for the caller, and
// a pack written as v5 must keep its items and data intact.
TEST(Datafile, RoundtripVersion5)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".datafile5");
	IStorage *pStorage = CreateTestStorage();
	CDataFileWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename, 5));

	// a large, compressible payload so the zstd paths are the ones exercised
	array<char> lData;
	lData.set_size(64 * 1024);
	for(int i = 0; i < lData.size(); i++)
		lData[i] = (char) ('a' + (i % 7));
	int Index = Writer.AddData(lData.size(), lData.base_ptr());
	int Index2 = Writer.AddData(4, "wxyz");

	int aItem[2] = {Index, Index2};
	Writer.AddItem(12, 34, sizeof(aItem), aItem);
	char aSecond[16] = "second item";
	Writer.AddItem(12, 35, sizeof(aSecond), aSecond);
	EXPECT_TRUE(Writer.Finish());

	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename, IStorage::TYPE_ALL));
	ASSERT_EQ(Reader.GetDataSize(Index), lData.size());
	EXPECT_TRUE(mem_comp(Reader.GetData(Index), lData.base_ptr(), lData.size()) == 0);
	ASSERT_EQ(Reader.GetDataSize(Index2), 4);
	EXPECT_TRUE(mem_comp(Reader.GetData(Index2), "wxyz", 4) == 0);

	ASSERT_EQ(Reader.NumItems(), 2);
	int Type, ID;
	int *pItem = (int *) Reader.GetItem(0, &Type, &ID);
	ASSERT_TRUE(pItem != nullptr);
	EXPECT_EQ(Type, 12);
	EXPECT_EQ(ID, 34);
	EXPECT_TRUE(mem_comp(pItem, aItem, sizeof(aItem)) == 0);

	char *pSecond = (char *) Reader.GetItem(1, &Type, &ID);
	ASSERT_TRUE(pSecond != nullptr);
	EXPECT_EQ(ID, 35);
	EXPECT_TRUE(mem_comp(pSecond, aSecond, sizeof(aSecond)) == 0);

	EXPECT_TRUE(Reader.Close());
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

// A datafile is read through a stream, so the same reader serves a file and a
// buffer that already exists (a map restored from a pack, a download buffer).
TEST(Datafile, ReadsFromMemoryStream)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".datafile_mem");
	IStorage *pStorage = CreateTestStorage();

	CDataFileWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename, 5));
	static const char TEST_DATA[] = "memory payload";
	int Index = Writer.AddData(sizeof(TEST_DATA), TEST_DATA);
	int aItem[2] = {Index, 7};
	Writer.AddItem(3, 4, sizeof(aItem), aItem);
	ASSERT_TRUE(Writer.Finish());

	void *pBytes = nullptr;
	unsigned Size = 0;
	ASSERT_TRUE(pStorage->ReadFile(aFilename, IStorage::TYPE_SAVE, &pBytes, &Size));

	// borrowed buffer
	{
		memory_read_stream Stream(pBytes, Size);
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(&Stream, false));
		ASSERT_EQ(Reader.GetDataSize(Index), sizeof(TEST_DATA));
		EXPECT_TRUE(mem_comp(Reader.GetData(Index), TEST_DATA, sizeof(TEST_DATA)) == 0);
		int Type, ID;
		int *pItem = (int *) Reader.GetItem(0, &Type, &ID);
		ASSERT_TRUE(pItem != nullptr);
		EXPECT_EQ(Type, 3);
		EXPECT_EQ(ID, 4);
		EXPECT_TRUE(mem_comp(pItem, aItem, sizeof(aItem)) == 0);
		EXPECT_TRUE(Reader.Close());
	}

	// a buffer handed over to the reader: deleting the stream frees it
	{
		unsigned char *pCopy = (unsigned char *) mem_alloc(Size);
		mem_copy(pCopy, pBytes, Size);
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(new memory_read_stream(pCopy, Size, true), true));
		ASSERT_EQ(Reader.GetDataSize(Index), sizeof(TEST_DATA));
		EXPECT_TRUE(mem_comp(Reader.GetData(Index), TEST_DATA, sizeof(TEST_DATA)) == 0);
		EXPECT_TRUE(Reader.Close());
	}

	mem_free(pBytes);
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

// The version is a property of the file, so a v4 written by the old writer
// must still be read after v5 support was added.
TEST(Datafile, ReadsVersion4)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".datafile4");
	IStorage *pStorage = CreateTestStorage();
	CDataFileWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename, 4));

	static const char TEST_DATA[] = "legacy payload";
	int Index = Writer.AddData(sizeof(TEST_DATA), TEST_DATA);
	Writer.AddItem(7, 8, 4, "abcd");
	EXPECT_TRUE(Writer.Finish());

	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename, IStorage::TYPE_ALL));
	ASSERT_EQ(Reader.GetDataSize(Index), sizeof(TEST_DATA));
	EXPECT_TRUE(mem_comp(Reader.GetData(Index), TEST_DATA, sizeof(TEST_DATA)) == 0);
	int Type, ID;
	ASSERT_TRUE(Reader.GetItem(0, &Type, &ID) != nullptr);
	EXPECT_EQ(Type, 7);
	EXPECT_EQ(ID, 8);
	EXPECT_TRUE(Reader.Close());
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}
