/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <gtest/gtest.h>

#include <base/system.h>

#include <engine/storage.h>

#include <game/localization.h>

TEST(Localization, LanguageLookupAndFallback)
{
	CLocalizationDatabase DB;
	const int Zh = DB.AddLanguage("zh", "Chinese");
	const int En = DB.AddLanguage("en", "English");
	ASSERT_GE(Zh, 0);
	ASSERT_GE(En, 0);
	DB.SetDefaultLanguage(Zh);

	DB.AddString(Zh, "hello", "你好", "");
	DB.AddString(Zh, "hello", "你好 context", "ctx");
	DB.AddString(En, "hello", "hi", "");
	DB.AddString(Zh, "only-zh", "只有中文", "");

	DB.SetActiveLanguage(Zh);
	EXPECT_STREQ(DB.FindString(str_quickhash("hello"), str_quickhash("")), "你好");
	EXPECT_STREQ(DB.FindString(str_quickhash("hello"), str_quickhash("ctx")), "你好 context");

	DB.SetActiveLanguage(En);
	EXPECT_STREQ(DB.FindString(str_quickhash("hello"), str_quickhash("")), "hi");
	// a missing context falls back to the empty context of the same language
	EXPECT_STREQ(DB.FindString(str_quickhash("hello"), str_quickhash("ctx")), "hi");
	// a missing translation is left to Localize()'s source fallback; it must not
	// leak the server default language into another player's text
	EXPECT_EQ(DB.FindString(str_quickhash("only-zh"), str_quickhash("")), nullptr);

	// language codes accept a filename or a path, like sv_languagefile does
	EXPECT_EQ(DB.FindLanguage("languages/zh.json"), Zh);
	EXPECT_EQ(DB.FindLanguage("EN"), En);
	EXPECT_EQ(DB.FindLanguage("missing"), -1);
}

TEST(Localization, LoadAllScansLanguagesAndKeepsEnglishFallback)
{
	IStorage *pStorage = CreateTestStorage();
	ASSERT_TRUE(pStorage);
	pStorage->CreateFolder("languages", IStorage::TYPE_SAVE);

	const char *pJson = "{\"name\":\"Test Language\",\"server strings\":[{\"context\":\"Ctx\",\"or\":\"hello\",\"tr\":\"hallo\"}]}";
	IOHANDLE File = pStorage->OpenFile("languages/zz.json", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File);
	EXPECT_EQ(io_write(File, pJson, str_length(pJson)), str_length(pJson));
	io_close(File);

	CLocalizationDatabase DB;
	EXPECT_TRUE(DB.LoadAll("", pStorage, nullptr, true));
	// without sv_languagefile the source language (English) is the default
	EXPECT_EQ(DB.DefaultLanguage(), DB.FindLanguage("en"));
	const int Zz = DB.FindLanguage("zz");
	EXPECT_GE(Zz, 0);
	EXPECT_STREQ(DB.LanguageName(Zz), "Test Language");
	EXPECT_STREQ(DB.FindString(Zz, str_quickhash("hello"), str_quickhash("Ctx")), "hallo");

	// an explicit default file wins
	EXPECT_TRUE(DB.LoadAll("languages/zz.json", pStorage, nullptr, true));
	EXPECT_EQ(DB.DefaultLanguage(), DB.FindLanguage("zz"));

	pStorage->RemoveFile("languages/zz.json", IStorage::TYPE_SAVE);
}
