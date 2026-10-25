/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "test.h"

#include <gtest/gtest.h>

#include <base/tl/array.h>

#include <engine/shared/mappack.h>
#include <engine/storage.h>

static void MakeFakeMap(array<char> &Out, char Fill, int Size)
{
	Out.set_size(Size);
	mem_zero(Out.base_ptr(), Size);
	mem_copy(Out.base_ptr(), "DATA", 4);
	Out[4] = 4;
	for(int i = 36; i < Size; i++)
		Out[i] = Fill;
}

TEST(MapPack, AppendWithoutRewriting)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".mappack");
	IStorage *pStorage = CreateTestStorage();

	array<char> lMap1, lMap2;
	MakeFakeMap(lMap1, 1, 2048);
	MakeFakeMap(lMap2, 2, 512);
	const char aJson[] = "{\"world\":{\"ring\":1}}";

	CMapPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename));
	ASSERT_TRUE(Writer.AddMap("Verdant_1", 1, lMap1.base_ptr(), lMap1.size(), aJson, sizeof(aJson) - 1));

	// the size after the first map is the base for the append below
	SHA256_DIGEST Sha;
	unsigned Crc = 0;
	unsigned SizeAfterFirst = 0;
	ASSERT_TRUE(pStorage->GetHashAndSize(aFilename, IStorage::TYPE_SAVE, &Sha, &Crc, &SizeAfterFirst));

	CMapPackReader First;
	ASSERT_TRUE(First.Open(pStorage, aFilename));
	ASSERT_EQ(First.NumMaps(), 1);

	ASSERT_TRUE(Writer.AddMap("Verdant_2", 2, lMap2.base_ptr(), lMap2.size(), aJson, sizeof(aJson) - 1));

	// appending only grew the file, the first blob is untouched, and a reader
	// that was already open still sees the state it opened (no reload)
	unsigned SizeAfterSecond = 0;
	ASSERT_TRUE(pStorage->GetHashAndSize(aFilename, IStorage::TYPE_SAVE, &Sha, &Crc, &SizeAfterSecond));
	EXPECT_GT(SizeAfterSecond, SizeAfterFirst);
	EXPECT_EQ(First.NumMaps(), 1);
	{
		const void *pMap = nullptr;
		int MapSize = 0;
		ASSERT_TRUE(First.GetMap(0, &pMap, &MapSize));
		EXPECT_EQ(MapSize, lMap1.size());
		EXPECT_TRUE(mem_comp(pMap, lMap1.base_ptr(), MapSize) == 0);
	}

	CMapPackReader Second;
	ASSERT_TRUE(Second.Open(pStorage, aFilename));
	ASSERT_EQ(Second.NumMaps(), 2);
	EXPECT_EQ(Second.FindMap("Verdant_2"), 0); // newest first
	EXPECT_EQ(Second.FindMap("Verdant_1"), 1);

	// reading one map must not require the other
	const void *pMap = nullptr;
	const void *pJson = nullptr;
	int MapSize = 0, JsonSize = 0;
	ASSERT_TRUE(Second.GetMap(Second.FindMap("Verdant_2"), &pMap, &MapSize, &pJson, &JsonSize));
	EXPECT_EQ(MapSize, lMap2.size());
	EXPECT_TRUE(mem_comp(pMap, lMap2.base_ptr(), MapSize) == 0);
	EXPECT_EQ(JsonSize, (int) sizeof(aJson) - 1);
	EXPECT_TRUE(mem_comp(pJson, aJson, JsonSize) == 0);
	EXPECT_TRUE(Second.Verify(0));

	Second.Close();
	First.Close();
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

TEST(MapPack, SameNameReplacesAndSurvivesReopen)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".mappack_dup");
	IStorage *pStorage = CreateTestStorage();

	array<char> lOld, lNew;
	MakeFakeMap(lOld, 7, 1024);
	MakeFakeMap(lNew, 9, 1024);

	CMapPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename));
	ASSERT_TRUE(Writer.AddMap("Hollow_7", 7, lOld.base_ptr(), lOld.size(), "{}", 2));
	ASSERT_TRUE(Writer.AddMap("Hollow_8", 8, lOld.base_ptr(), lOld.size(), "{}", 2));
	ASSERT_TRUE(Writer.AddMap("Hollow_7", 7, lNew.base_ptr(), lNew.size(), "{}", 2));

	CMapPackReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename));
	ASSERT_EQ(Reader.NumMaps(), 2);
	const int Index = Reader.FindMap("Hollow_7");
	ASSERT_GE(Index, 0);
	const void *pMap = nullptr;
	int MapSize = 0;
	ASSERT_TRUE(Reader.GetMap(Index, &pMap, &MapSize));
	EXPECT_TRUE(mem_comp(pMap, lNew.base_ptr(), MapSize) == 0);
	Reader.Close();

	// the chain must survive a reopen, including the replaced entry
	CMapPackReader Reopened;
	ASSERT_TRUE(Reopened.Open(pStorage, aFilename));
	EXPECT_EQ(Reopened.NumMaps(), 2);
	EXPECT_GE(Reopened.FindMap("Hollow_7"), 0);
	Reopened.Close();

	// a non pack file is never appended to
	const char aNotAPack[] = "definitely not a pack";
	IOHANDLE File = pStorage->OpenFile("not_a_pack.bin", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File != nullptr);
	io_write(File, aNotAPack, sizeof(aNotAPack));
	io_close(File);
	CMapPackWriter Reject;
	EXPECT_FALSE(Reject.Open(pStorage, "not_a_pack.bin"));
	EXPECT_TRUE(pStorage->RemoveFile("not_a_pack.bin", IStorage::TYPE_SAVE));

	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

TEST(MapPack, CompactDropsSupersededBlobs)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".mappack_compact");
	IStorage *pStorage = CreateTestStorage();

	array<char> lOld, lNew, lOther;
	MakeFakeMap(lOld, 1, 4096);
	MakeFakeMap(lNew, 2, 4096);
	MakeFakeMap(lOther, 3, 1024);

	CMapPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename));
	ASSERT_TRUE(Writer.AddMap("Verdant_1", 1, lOld.base_ptr(), lOld.size(), "{\"v\":1}", 7));
	ASSERT_TRUE(Writer.AddMap("Verdant_2", 2, lOther.base_ptr(), lOther.size(), "{}", 2));
	ASSERT_TRUE(Writer.AddMap("Verdant_1", 1, lNew.base_ptr(), lNew.size(), "{\"v\":2}", 7));
	// a few more overwrites make the garbage obvious
	for(int i = 0; i < 4; i++)
		ASSERT_TRUE(Writer.AddMap("Verdant_2", 2, lOther.base_ptr(), lOther.size(), "{}", 2));

	SHA256_DIGEST Sha;
	unsigned Crc = 0;
	unsigned SizeBefore = 0;
	ASSERT_TRUE(pStorage->GetHashAndSize(aFilename, IStorage::TYPE_SAVE, &Sha, &Crc, &SizeBefore));

	int Maps = 0, Before = 0, After = 0;
	ASSERT_TRUE(CMapPackWriter::Compact(pStorage, aFilename, &Maps, &Before, &After));
	EXPECT_EQ(Maps, 2); // Verdant_1 (newest) and Verdant_2
	EXPECT_EQ((unsigned) Before, SizeBefore);
	EXPECT_LT(After, Before);

	// the live bytes survive, the superseded ones are gone
	CMapPackReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename));
	ASSERT_EQ(Reader.NumMaps(), 2);
	const void *pMap = nullptr;
	const void *pJson = nullptr;
	int MapSize = 0, JsonSize = 0;
	const int Index = Reader.FindMap("Verdant_1");
	ASSERT_GE(Index, 0);
	ASSERT_TRUE(Reader.GetMap(Index, &pMap, &MapSize, &pJson, &JsonSize));
	EXPECT_TRUE(mem_comp(pMap, lNew.base_ptr(), MapSize) == 0);
	EXPECT_EQ(JsonSize, 7);
	EXPECT_TRUE(mem_comp(pJson, "{\"v\":2}", 7) == 0);
	Reader.Close();

	// compacting an already compacted pack is stable
	int Twice = 0;
	ASSERT_TRUE(CMapPackWriter::Compact(pStorage, aFilename, &Maps, &Before, &Twice));
	EXPECT_EQ(Maps, 2);
	EXPECT_LE(Twice, After);

	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

TEST(MapPack, ExtractWritesMapAndJson)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".mappack_extract");
	IStorage *pStorage = CreateTestStorage();

	array<char> lMap;
	MakeFakeMap(lMap, 3, 256);
	const char aJson[] = "{\"resources\":[]}";

	CMapPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename));
	ASSERT_TRUE(Writer.AddMap("Depths_3", 3, lMap.base_ptr(), lMap.size(), aJson, sizeof(aJson) - 1));

	CMapPackReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename));
	char aOut[IO_MAX_PATH_LENGTH];
	str_format(aOut, sizeof(aOut), "%s.out.map", aFilename);
	ASSERT_TRUE(Reader.ExtractMap(0, pStorage, aOut));

	void *pData = nullptr;
	unsigned Size = 0;
	ASSERT_TRUE(pStorage->ReadFile(aOut, IStorage::TYPE_SAVE, &pData, &Size));
	EXPECT_EQ((int) Size, lMap.size());
	EXPECT_TRUE(mem_comp(pData, lMap.base_ptr(), Size) == 0);
	mem_free(pData);

	char aJsonOut[IO_MAX_PATH_LENGTH];
	str_format(aJsonOut, sizeof(aJsonOut), "%s.out.json", aFilename);
	ASSERT_TRUE(pStorage->ReadFile(aJsonOut, IStorage::TYPE_SAVE, &pData, &Size));
	EXPECT_EQ((int) Size, (int) sizeof(aJson) - 1);
	EXPECT_TRUE(mem_comp(pData, aJson, Size) == 0);
	mem_free(pData);

	Reader.Close();
	EXPECT_TRUE(pStorage->RemoveFile(aOut, IStorage::TYPE_SAVE));
	EXPECT_TRUE(pStorage->RemoveFile(aJsonOut, IStorage::TYPE_SAVE));
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}
