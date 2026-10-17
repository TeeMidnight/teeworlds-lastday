/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// The single global CBotManager gives every bot a private CWorldCore tee id that
// is unique across every world, so a snapshot tee id names exactly one bot even
// when a client is shown its own and its ring neighbours' bots. This covers the
// pure allocator the manager delegates to (PickBotTeeID in bot.h).
#include <gtest/gtest.h>

#include <game/server/bot.h>

#include <set>

namespace {

Uuid MakeBot(int Index)
{
	Uuid Bot;
	for(int i = 0; i < (int) sizeof(Bot.m_aData); i++)
		Bot.m_aData[i] = (unsigned char) (Index * 131 + i * 17 + 7);
	return Bot;
}

} // namespace

TEST(BotIdentity, SameBotKeepsItsTeeID)
{
	const Uuid Bot = MakeBot(42);
	const int TeeID = PickBotTeeID(Bot, [](int) { return false; });
	EXPECT_GE(TeeID, BOT_TEEID_07_MIN);
	EXPECT_LE(TeeID, BOT_TEEID_08_MAX);
	// respawn: the same BotID resolves to the same private id when it is free
	EXPECT_EQ(TeeID, PickBotTeeID(Bot, [](int) { return false; }));
}

TEST(BotIdentity, AllocationsStayDistinctWhileTheRangeHasRoom)
{
	std::set<int> Used;
	for(int i = 0; i < 1024; i++)
	{
		const int TeeID = PickBotTeeID(MakeBot(i), [&Used](int Id) { return Used.count(Id) != 0; });
		ASSERT_GE(TeeID, BOT_TEEID_07_MIN);
		ASSERT_LE(TeeID, BOT_TEEID_08_MAX);
		EXPECT_TRUE(Used.insert(TeeID).second) << "id " << TeeID << " handed out twice";
	}
}

TEST(BotIdentity, ProbesPastUsedIDsAndGivesUpWhenFull)
{
	// fill everything except one id; the allocator has to land on exactly it
	std::set<int> Used;
	const int Kept = 4321;
	for(int Id = BOT_TEEID_07_MIN; Id <= BOT_TEEID_08_MAX; Id++)
		if(Id != Kept)
			Used.insert(Id);

	const Uuid Bot = MakeBot(7);
	EXPECT_EQ(PickBotTeeID(Bot, [&Used](int Id) { return Used.count(Id) != 0; }), Kept);

	// a fully used range has no answer
	Used.insert(Kept);
	EXPECT_EQ(PickBotTeeID(Bot, [&Used](int Id) { return Used.count(Id) != 0; }), -1);
}
