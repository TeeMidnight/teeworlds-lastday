/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Silent step: while a character creeps, its self-propelled horizontal speed is
// half of the normal control speed. The state lives in CCharacterCore, so the
// player (toggled by the scoreboard flag) and an NPC share one physics path.
#include <gtest/gtest.h>

#include <game/collision.h>
#include <game/gamecore.h>

namespace {

const int W = 32;
const int H = 4;

// a flat floor from row 2 down
void MakeFloor(int *pTiles)
{
	for(int i = 0; i < W * H; i++)
		pTiles[i] = 0;
	for(int y = 2; y < H; y++)
		for(int x = 0; x < W; x++)
			pTiles[y * W + x] = CCollision::COLFLAG_SOLID;
}

// place the tee one physics box above the floor with the given input
void InitCore(CCharacterCore &Core, CWorldCore &World, CCollision &Coll)
{
	Core.Init(&World, &Coll);
	Core.Reset();
	Core.m_Pos = vec2(16 * 32.0f, 2 * 32.0f - CCharacterCore::PHYS_SIZE * 0.5f - 5.0f);
	mem_zero(&Core.m_Input, sizeof(Core.m_Input));
	Core.m_Input.m_Direction = 1;
}

void StepN(CCharacterCore &Core, int Ticks, bool LastDay = true)
{
	for(int i = 0; i < Ticks; i++)
		Core.Tick(true, LastDay);
}

} // namespace

// Both cores saturate at their control speed, and the silent one sits exactly at
// half of the other.
TEST(SilentStep, CreepsAtHalfOfTheNormalSpeed)
{
	int aTiles[W * H];
	MakeFloor(aTiles);
	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	CCharacterCore Normal;
	InitCore(Normal, World, Coll);

	CCharacterCore Silent;
	InitCore(Silent, World, Coll);
	Silent.m_SilentStep = true;

	StepN(Normal, 50);
	StepN(Silent, 50);

	EXPECT_FLOAT_EQ(Normal.m_Vel.x, 10.0f);
	EXPECT_FLOAT_EQ(Silent.m_Vel.x, 5.0f);
	EXPECT_FLOAT_EQ(Silent.m_Vel.x, Normal.m_Vel.x * 0.5f);
}

// The creep only caps the acceleration the core adds itself: a run that already
// carries more than the creep speed keeps its momentum while the direction is
// held, so turning it on is not a retroactive brake.
TEST(SilentStep, TogglingOnDoesNotSlowAnExistingRun)
{
	int aTiles[W * H];
	MakeFloor(aTiles);
	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	CCharacterCore Core;
	InitCore(Core, World, Coll);
	StepN(Core, 50);
	ASSERT_FLOAT_EQ(Core.m_Vel.x, 10.0f);

	Core.m_SilentStep = true;
	StepN(Core, 1);
	EXPECT_FLOAT_EQ(Core.m_Vel.x, 10.0f);
}

// The non-LastDay path is the dead-reckoning / legacy prediction one: it must
// keep the vanilla speed, or the client's extrapolation would diverge.
TEST(SilentStep, NonLastDayPredictionIsUnaffected)
{
	int aTiles[W * H];
	MakeFloor(aTiles);
	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	CCharacterCore Core;
	InitCore(Core, World, Coll);
	Core.m_SilentStep = true;
	StepN(Core, 50, false);

	EXPECT_FLOAT_EQ(Core.m_Vel.x, 10.0f);
}

// A core starts loud; only the owner of the decision turns the creep on.
TEST(SilentStep, ResetClearsTheFlag)
{
	CCharacterCore Core;
	Core.Reset();
	EXPECT_FALSE(Core.m_SilentStep);
}
