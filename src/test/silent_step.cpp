/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Silent step and the accessory speed bonus used to be special cases inside
// CCharacterCore::Tick. They are now folded into the per-tee tuning, so these
// tests exercise the core together with BuildCharacterTuning().
#include <game/collision.h>
#include <game/gamecore.h>

#include <gtest/gtest.h>

static const int W = 32;
static const int H = 4;

// a flat floor from row 2 down
static void MakeFloor(int *pTiles)
{
	for(int i = 0; i < W * H; i++)
		pTiles[i] = 0;
	for(int y = 2; y < H; y++)
		for(int x = 0; x < W; x++)
			pTiles[y * W + x] = CCollision::COLFLAG_SOLID;
}

// place the tee one physics box above the floor with the given input
static void InitCore(CCharacterCore &Core, CWorldCore &World, CCollision &Coll)
{
	Core.Init(&World, &Coll);
	Core.Reset();
	Core.m_Pos = vec2(16 * 32.0f, 2 * 32.0f - CCharacterCore::PHYS_SIZE * 0.5f - 5.0f);
	mem_zero(&Core.m_Input, sizeof(Core.m_Input));
	Core.m_Input.m_Direction = 1;
}

static void StepN(CCharacterCore &Core, int Ticks)
{
	for(int i = 0; i < Ticks; i++)
		Core.Tick(true, true);
}

// Both cores saturate at their control speed, and the silent one sits exactly at
// half of the other.
TEST(SilentStep, CreepsAtHalfOfTheNormalSpeed)
{
	int aTiles[W * H];
	MakeFloor(aTiles);
	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	const CTuningParams NormalTuning = BuildCharacterTuning(CTuningParams(), 1.0f, false, false, false, 0.0f);
	const CTuningParams SilentTuning = BuildCharacterTuning(CTuningParams(), 1.0f, true, false, false, 0.0f);

	CCharacterCore Normal;
	InitCore(Normal, World, Coll);
	Normal.SetTuning(&NormalTuning);

	CCharacterCore Silent;
	InitCore(Silent, World, Coll);
	Silent.SetTuning(&SilentTuning);
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

	const CTuningParams NormalTuning = BuildCharacterTuning(CTuningParams(), 1.0f, false, false, false, 0.0f);
	const CTuningParams SilentTuning = BuildCharacterTuning(CTuningParams(), 1.0f, true, false, false, 0.0f);

	CCharacterCore Core;
	InitCore(Core, World, Coll);
	Core.SetTuning(&NormalTuning);
	StepN(Core, 50);
	ASSERT_FLOAT_EQ(Core.m_Vel.x, 10.0f);

	Core.m_SilentStep = true;
	Core.SetTuning(&SilentTuning);
	StepN(Core, 1);
	EXPECT_FLOAT_EQ(Core.m_Vel.x, 10.0f);
}

// A core starts loud; only the owner of the decision turns the creep on.
TEST(SilentStep, ResetClearsTheFlag)
{
	CCharacterCore Core;
	Core.Reset();
	EXPECT_FALSE(Core.m_SilentStep);
}

// ---- accessory speed bonus -------------------------------------------------

// An accessory raises the horizontal control speed cap. The client receives a
// tuning scaled by the same factor, so its prediction uses this same cap.
TEST(AccessorySpeed, ScalesTheControlSpeed)
{
	int aTiles[W * H];
	MakeFloor(aTiles);
	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);
	CWorldCore World;

	const CTuningParams NormalTuning = BuildCharacterTuning(CTuningParams(), 1.0f, false, false, false, 0.0f);
	const CTuningParams FastTuning = BuildCharacterTuning(CTuningParams(), 1.25f, false, false, false, 0.0f);

	CCharacterCore Normal;
	InitCore(Normal, World, Coll);
	Normal.SetTuning(&NormalTuning);

	CCharacterCore Fast;
	InitCore(Fast, World, Coll);
	Fast.SetTuning(&FastTuning);
	Fast.m_SpeedScale = 1.25f;

	StepN(Normal, 50);
	StepN(Fast, 50);

	EXPECT_FLOAT_EQ(Normal.m_Vel.x, 10.0f);
	EXPECT_FLOAT_EQ(Fast.m_Vel.x, 12.5f);
}

// Reset returns a core to the unmodified speed, so a recycled core (or a bot's
// core) never inherits another character's bonus.
TEST(AccessorySpeed, ResetClearsTheScale)
{
	CCharacterCore Core;
	Core.Reset();
	EXPECT_FLOAT_EQ(Core.m_SpeedScale, 1.0f);

	Core.m_SpeedScale = 1.5f;
	Core.Reset();
	EXPECT_FLOAT_EQ(Core.m_SpeedScale, 1.0f);
}
