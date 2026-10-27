/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/tl/array.h>
#include <engine/shared/datafile.h>
#include <engine/shared/worldpack.h>
#include <engine/storage.h>

#include "test.h"

#include <gtest/gtest.h>

// A minimal v4 map: the pack only has to keep the bytes intact, and the entry
// check refuses anything that is not a datafile.
static void MakeFakeMap(array<char> &Out, char Fill, int Size)
{
	Out.set_size(Size);
	mem_zero(Out.base_ptr(), Size);
	mem_copy(Out.base_ptr(), "DATA", 4);
	Out[4] = 4; // version
	for(int i = 36; i < Size; i++)
		Out[i] = Fill;
}

TEST(WorldPack, MapAndJsonRoundTrip)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".worldpack");
	IStorage *pStorage = CreateTestStorage();

	array<char> lMap1, lMap2;
	MakeFakeMap(lMap1, 1, 4096);
	MakeFakeMap(lMap2, 2, 512);
	const char aJson1[] = "{\"world\":{\"ring\":1,\"ring_seed\":1}}";
	const char aJson2[] = "{\"world\":{\"ring\":1,\"ring_seed\":2}}";

	CWorldPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename, "Verdant", 0));
	ASSERT_TRUE(Writer.AddMap("Verdant_1", 1, lMap1.base_ptr(), lMap1.size(), aJson1, sizeof(aJson1) - 1));
	ASSERT_TRUE(Writer.AddMap("Verdant_2", 2, lMap2.base_ptr(), lMap2.size(), aJson2, sizeof(aJson2) - 1));
	ASSERT_EQ(Writer.NumMaps(), 2);
	ASSERT_TRUE(Writer.Finish());

	CWorldPackReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, aFilename));
	EXPECT_STREQ(Reader.Floor(), "Verdant");
	EXPECT_EQ(Reader.Shard(), 0);
	ASSERT_EQ(Reader.NumMaps(), 2);

	EXPECT_EQ(Reader.FindMap("Verdant_2"), 1);
	EXPECT_EQ(Reader.FindMap("Verdant_9"), -1);

	const void *pMap = nullptr;
	const void *pJson = nullptr;
	int MapSize = 0, JsonSize = 0;
	ASSERT_TRUE(Reader.GetMap(0, &pMap, &MapSize, &pJson, &JsonSize));
	EXPECT_EQ(MapSize, lMap1.size());
	EXPECT_EQ(JsonSize, (int) sizeof(aJson1) - 1);
	EXPECT_TRUE(mem_comp(pMap, lMap1.base_ptr(), MapSize) == 0);
	EXPECT_TRUE(mem_comp(pJson, aJson1, JsonSize) == 0);
	EXPECT_TRUE(Reader.Verify(0));
	EXPECT_TRUE(Reader.Verify(1));

	// the recorded hash catches a payload that was replaced
	const SWorldPackEntryInfo *pEntry = Reader.Entry(0);
	ASSERT_TRUE(pEntry != nullptr);
	EXPECT_EQ(pEntry->m_Seed, 1);
	char aSha[SHA256_MAXSTRSIZE];
	sha256_str(sha256(lMap1.base_ptr(), lMap1.size()), aSha, sizeof(aSha));
	EXPECT_STREQ(pEntry->m_aSha256, aSha);

	// extraction writes a plain v4 map plus its json sidecar
	char aExtract[IO_MAX_PATH_LENGTH];
	str_format(aExtract, sizeof(aExtract), "%s.extracted.map", aFilename);
	ASSERT_TRUE(Reader.ExtractMap(1, pStorage, aExtract));
	CDataFileReader Extracted;
	ASSERT_TRUE(Extracted.Open(pStorage, aExtract, IStorage::TYPE_SAVE));
	EXPECT_TRUE(Extracted.Close());

	char aJsonPath[IO_MAX_PATH_LENGTH];
	str_format(aJsonPath, sizeof(aJsonPath), "%s.extracted.json", aFilename);
	void *pRead = nullptr;
	unsigned ReadSize = 0;
	ASSERT_TRUE(pStorage->ReadFile(aJsonPath, IStorage::TYPE_SAVE, &pRead, &ReadSize));
	EXPECT_EQ((int) ReadSize, (int) sizeof(aJson2) - 1);
	EXPECT_TRUE(mem_comp(pRead, aJson2, ReadSize) == 0);
	mem_free(pRead);

	EXPECT_TRUE(pStorage->RemoveFile(aExtract, IStorage::TYPE_SAVE));
	EXPECT_TRUE(pStorage->RemoveFile(aJsonPath, IStorage::TYPE_SAVE));
	Reader.Close();
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}

TEST(WorldPack, RejectsNonMapPayloads)
{
	CTestInfo Info;
	char aFilename[64];
	Info.Filename(aFilename, sizeof(aFilename), ".worldpack_bad");
	IStorage *pStorage = CreateTestStorage();

	CWorldPackWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, aFilename, "Verdant", 0));
	const char aNotAMap[64] = "not a datafile at all";
	EXPECT_FALSE(Writer.AddMap("Verdant_1", 1, aNotAMap, sizeof(aNotAMap), "{}", 2));
	EXPECT_EQ(Writer.NumMaps(), 0);
	EXPECT_TRUE(Writer.Finish());
	EXPECT_TRUE(pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE));
}
