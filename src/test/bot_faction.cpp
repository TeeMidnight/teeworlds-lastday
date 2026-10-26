/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <gtest/gtest.h>

#include <game/server/bot.h>

TEST(BotFaction, ParsesKnownNames)
{
	EXPECT_EQ(BotFactionFromString("hostile"), BOTFACTION_HOSTILE);
	EXPECT_EQ(BotFactionFromString("HOSTILE"), BOTFACTION_HOSTILE);
	EXPECT_EQ(BotFactionFromString("enemy"), BOTFACTION_HOSTILE);
	EXPECT_EQ(BotFactionFromString("neutral"), BOTFACTION_NEUTRAL);
	EXPECT_EQ(BotFactionFromString("friendly"), BOTFACTION_FRIENDLY);
	EXPECT_EQ(BotFactionFromString("ally"), BOTFACTION_FRIENDLY);
}

TEST(BotFaction, FallsBackToDefault)
{
	EXPECT_EQ(BotFactionFromString("", BOTFACTION_FRIENDLY), BOTFACTION_FRIENDLY);
	EXPECT_EQ(BotFactionFromString("nonsense", BOTFACTION_NEUTRAL), BOTFACTION_NEUTRAL);
	EXPECT_EQ(BotFactionFromString(nullptr, BOTFACTION_HOSTILE), BOTFACTION_HOSTILE);
}

TEST(BotFaction, NamesRoundTrip)
{
	EXPECT_STREQ(BotFactionName(BOTFACTION_HOSTILE), "hostile");
	EXPECT_STREQ(BotFactionName(BOTFACTION_NEUTRAL), "neutral");
	EXPECT_STREQ(BotFactionName(BOTFACTION_FRIENDLY), "friendly");
}
