/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <gtest/gtest.h>

#include <game/item_id.h>

// The migration tooling computes item uuids outside the engine, so the mapping
// from a namespaced name to its uuid is part of the on-disk contract and must
// not drift. These vectors pin it down.
TEST(ItemID, NamespacedNameToUuid)
{
	const struct
	{
		const char *m_pName;
		const char *m_pUuid;
	} aCases[] = {
		{"hammer", "6f02fc34-1f02-3e84-b452-ff12dc0477c6"},
		{"gun", "ed9ba0d9-2a2d-3356-953c-2f431dfc483c"},
		{"pistol_bullet", "e8777a63-c8d4-30bd-9854-e55204778556"},
		{"sword", "3b657a4f-3c08-3ffb-8754-9767b87b487c"},
		{"laser", "3b850c03-1219-3208-9af7-b44d553669df"},
		{"stone", "d7724377-8bd3-320f-9e55-5f90d8a19609"},
		{"iron_ore", "a66c81e1-1de4-3e7a-af79-4221c3a9277e"},
		{"iron_ingot", "d5e73c06-d9c2-3c40-8990-ec1c30daaa1f"},
		{"heavy_bullet", "45b9429a-7d7a-3817-a265-0991a9abfff3"},
		{"shotgun_slug", "aaafdf1b-ff94-328e-987d-561a652bc001"},
		{"addon:steel_ingot", "33c3b49e-a1de-38b2-878d-94eeb7cf1f4d"},
	};

	for(const auto &Case : aCases)
	{
		char aUuid[UUID_MAXSTRSIZE];
		CItemID::FromName(Case.m_pName).FormatUuid(aUuid, sizeof(aUuid));
		EXPECT_STREQ(aUuid, Case.m_pUuid) << Case.m_pName;
	}
}

TEST(ItemID, BareNameUsesDefaultNamespace)
{
	EXPECT_EQ(CItemID::FromName("stone"), CItemID::FromName("lastday:stone"));
	EXPECT_STREQ(CItemID::FromName("stone").SourceName(), "lastday:stone");
}

TEST(ItemID, UuidStringRoundTrips)
{
	const CItemID Id = CItemID::FromName("addon:steel_ingot");
	char aUuid[UUID_MAXSTRSIZE];
	Id.FormatUuid(aUuid, sizeof(aUuid));

	CItemID Parsed;
	EXPECT_TRUE(CItemID::Parse(aUuid, &Parsed));
	EXPECT_EQ(Id, Parsed);
}
