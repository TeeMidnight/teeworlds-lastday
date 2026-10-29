/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// Per-Tee tuning: the server no longer hard-codes LastDay physics in
// CCharacterCore::Tick. Water, silent step and the speed bonus are baked into
// a CTuningParams that is simulated by the server and snapped to clients, so
// prediction uses exactly the server's values.
#include <game/gamecore.h>

#include <gtest/gtest.h>

TEST(CharacterTuning, SpeedBonusScalesTheControlSpeedOnly)
{
	const CTuningParams Base;
	const CTuningParams Tuning = BuildCharacterTuning(Base, 1.25f, false, false, false, 0.0f);

	EXPECT_FLOAT_EQ(Tuning.m_GroundControlSpeed, (float) Base.m_GroundControlSpeed * 1.25f);
	EXPECT_FLOAT_EQ(Tuning.m_AirControlSpeed, (float) Base.m_AirControlSpeed * 1.25f);
	EXPECT_FLOAT_EQ(Tuning.m_GroundControlAccel, Base.m_GroundControlAccel);
	EXPECT_FLOAT_EQ(Tuning.m_AirControlAccel, Base.m_AirControlAccel);
	EXPECT_FLOAT_EQ(Tuning.m_Gravity, Base.m_Gravity);
}

TEST(CharacterTuning, SilentStepScalesCapAndAcceleration)
{
	const CTuningParams Base;
	const CTuningParams Tuning = BuildCharacterTuning(Base, 1.0f, true, false, false, 0.0f);

	EXPECT_FLOAT_EQ(Tuning.m_GroundControlSpeed, (float) Base.m_GroundControlSpeed * 0.5f);
	EXPECT_FLOAT_EQ(Tuning.m_AirControlSpeed, (float) Base.m_AirControlSpeed * 0.5f);
	EXPECT_FLOAT_EQ(Tuning.m_GroundControlAccel, (float) Base.m_GroundControlAccel * 0.5f);
	EXPECT_FLOAT_EQ(Tuning.m_AirControlAccel, (float) Base.m_AirControlAccel * 0.5f);
	EXPECT_FLOAT_EQ(Tuning.m_GroundFriction, Base.m_GroundFriction);
	EXPECT_FLOAT_EQ(Tuning.m_Gravity, Base.m_Gravity);
}

TEST(CharacterTuning, WaterScalesControlAndDisablesTheAirJump)
{
	const CTuningParams Base;
	const CTuningParams Tuning = BuildCharacterTuning(Base, 1.0f, false, true, false, 0.0f);

	// CTuningParams quantizes to 0.01, so compare within one step.
	EXPECT_NEAR(Tuning.m_GroundControlSpeed, (float) Base.m_GroundControlSpeed * 0.81f, 0.011f);
	EXPECT_NEAR(Tuning.m_AirControlSpeed, (float) Base.m_AirControlSpeed * 0.81f, 0.011f);
	EXPECT_NEAR(Tuning.m_GroundControlAccel, (float) Base.m_GroundControlAccel * 0.81f, 0.011f);
	EXPECT_NEAR(Tuning.m_AirControlAccel, (float) Base.m_AirControlAccel * 0.81f, 0.011f);
	EXPECT_NEAR(Tuning.m_GroundFriction, (float) Base.m_GroundFriction * 0.9f, 0.011f);
	EXPECT_NEAR(Tuning.m_AirFriction, (float) Base.m_AirFriction * 0.9f, 0.011f);
	EXPECT_NEAR(Tuning.m_Gravity, (float) Base.m_Gravity * 0.9216f, 0.011f);
	EXPECT_FLOAT_EQ(Tuning.m_AirJumpImpulse, 0.0f);
	// a ground jump still works while standing in shallow water
	EXPECT_FLOAT_EQ(Tuning.m_GroundJumpImpulse, Base.m_GroundJumpImpulse);
}

TEST(CharacterTuning, SwimmingBuildsAnUpwardGravity)
{
	const CTuningParams Base;
	// At rest the old branch added 0.96g and then 1.5g upward; the net is what
	// the standard core has to add in one step.
	const CTuningParams Tuning = BuildCharacterTuning(Base, 1.0f, false, true, true, 0.0f);
	EXPECT_NEAR(Tuning.m_Gravity, (float) Base.m_Gravity * -0.54f, 0.011f);
}

TEST(CharacterTuning, SwimmingClampsAtTheOldUpwardLimit)
{
	const CTuningParams Base;
	const float UpLimit = -(float) Base.m_Gravity * 7.0f;

	// already at or past the limit: no more upward push
	const CTuningParams AtLimit = BuildCharacterTuning(Base, 1.0f, false, true, true, UpLimit);
	EXPECT_NEAR(AtLimit.m_Gravity, 0.0f, 0.011f);

	// close to the limit: the delta fills exactly up to it
	const float Near = UpLimit + (float) Base.m_Gravity * 0.2f;
	const CTuningParams NearTuning = BuildCharacterTuning(Base, 1.0f, false, true, true, Near);
	const float After = Near + NearTuning.m_Gravity;
	EXPECT_NEAR(After, UpLimit, 0.011f);
}

TEST(CharacterTuning, CombiningBonusesMultipliesThem)
{
	const CTuningParams Base;
	const CTuningParams Tuning = BuildCharacterTuning(Base, 1.25f, true, true, false, 0.0f);

	EXPECT_NEAR(Tuning.m_GroundControlSpeed, (float) Base.m_GroundControlSpeed * 1.25f * 0.5f * 0.81f, 0.02f);
	EXPECT_NEAR(Tuning.m_AirControlSpeed, (float) Base.m_AirControlSpeed * 1.25f * 0.5f * 0.81f, 0.02f);
	EXPECT_NEAR(Tuning.m_GroundControlAccel, (float) Base.m_GroundControlAccel * 0.5f * 0.81f, 0.02f);
	EXPECT_NEAR(Tuning.m_AirControlAccel, (float) Base.m_AirControlAccel * 0.5f * 0.81f, 0.02f);
}
