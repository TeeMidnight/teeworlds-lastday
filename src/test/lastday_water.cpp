/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Water sets the air-jump bits on the LastDay path. That state cannot be
// represented by a tuning, so CCharacterCore::Tick keeps a LastDay parameter
// for this one rule; the water movement itself comes from Tuning().
#include <game/collision.h>
#include <game/gamecore.h>

#include <gtest/gtest.h>

TEST(LastDayWater, SetsJumpedOnlyOnTheLastDayPath)
{
	const int W = 32, H = 16;
	int aTiles[W * H];
	for(int i = 0; i < W * H; i++)
		aTiles[i] = CCollision::COLFLAG_WATER;

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	CCharacterCore OnPath, OffPath;
	OnPath.Init(&World, &Coll);
	OnPath.Reset();
	OnPath.m_Pos = vec2(10 * 32.0f, 8 * 32.0f);
	OffPath.Init(&World, &Coll);
	OffPath.Reset();
	OffPath.m_Pos = OnPath.m_Pos;

	OnPath.Tick(true, true);
	OffPath.Tick(true, false);

	EXPECT_EQ(OnPath.m_Jumped & 3, 3);
	EXPECT_EQ(OffPath.m_Jumped & 3, 0);
}
