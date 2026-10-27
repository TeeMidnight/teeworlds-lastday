/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/building.h>

#include <gtest/gtest.h>

TEST(BuildingLock, ParsesNames)
{
	int Mode = -1;
	EXPECT_TRUE(BuildLockFromName("public", &Mode));
	EXPECT_EQ(Mode, BUILD_LOCK_PUBLIC);
	EXPECT_TRUE(BuildLockFromName("self", &Mode));
	EXPECT_EQ(Mode, BUILD_LOCK_SELF);
	EXPECT_TRUE(BuildLockFromName("team", &Mode));
	EXPECT_EQ(Mode, BUILD_LOCK_TEAM);
	EXPECT_TRUE(BuildLockFromName("PRIVATE", &Mode));
	EXPECT_EQ(Mode, BUILD_LOCK_SELF);
	EXPECT_TRUE(BuildLockFromName("guild", &Mode));
	EXPECT_EQ(Mode, BUILD_LOCK_TEAM);
	EXPECT_FALSE(BuildLockFromName("nonsense", &Mode));
}

TEST(BuildingLock, NamesRoundTrip)
{
	EXPECT_STREQ(BuildLockName(BUILD_LOCK_PUBLIC), "public");
	EXPECT_STREQ(BuildLockName(BUILD_LOCK_SELF), "self");
	EXPECT_STREQ(BuildLockName(BUILD_LOCK_TEAM), "team");
}

TEST(BuildingLock, DefaultIsSelf)
{
	CBuildAccess Access;
	EXPECT_EQ(Access.m_LockMode, BUILD_LOCK_SELF);
	EXPECT_TRUE(Access.m_Owner == UUID_ZEROED);
}
