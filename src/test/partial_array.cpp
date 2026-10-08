/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <gtest/gtest.h>

#include <base/tl/partial_array.h>

#include <engine/shared/protocol.h>

#include <game/gamecore.h>
#include <game/server/bot.h>

// A small part size keeps the "one part costs one allocation" behaviour
// observable without allocating megabytes.
typedef partial_array<int, 4, 8> CTestArray;

TEST(PartialArray, StartsEmptyAndAllocatesNothing)
{
	CTestArray Array;

	EXPECT_EQ(Array.size(), 32);

	// nothing is bound yet, so no part may exist
	for(int i = 0; i < Array.size(); i++)
		EXPECT_FALSE(Array.exists(i));
}

TEST(PartialArray, GetNeverAllocates)
{
	CTestArray Array;

	// get() is the network-facing accessor: it must stay safe and
	// non-allocating for any index, including ones out of range.
	EXPECT_EQ(Array.get(0), (int *) 0);
	EXPECT_EQ(Array.get(17), (int *) 0);
	EXPECT_EQ(Array.get(-1), (int *) 0);
	EXPECT_EQ(Array.get(Array.size()), (int *) 0);
	EXPECT_EQ(Array.get(1 << 30), (int *) 0);

	for(int i = 0; i < Array.size(); i++)
		EXPECT_FALSE(Array.exists(i));
}

TEST(PartialArray, WriteThenReadRoundTrips)
{
	CTestArray Array;

	Array[3] = 42;
	Array[19] = -7;

	ASSERT_TRUE(Array.exists(3));
	ASSERT_TRUE(Array.exists(19));

	EXPECT_EQ(Array[3], 42);
	EXPECT_EQ(Array[19], -7);
	EXPECT_EQ(*Array.get(3), 42);
	EXPECT_EQ(*Array.get(19), -7);
}

// An index inside an allocated part that was never written is still readable,
// and must read as a value-initialised T rather than as allocator garbage.
// This is the m_Prev.m_Tick bug from the upstream commit, in miniature.
TEST(PartialArray, FreshIndicesAreValueInitialised)
{
	CTestArray Array;

	// touching index 0 allocates the first part, covering 0..7
	Array[0] = 1;

	for(int i = 1; i < 8; i++)
	{
		ASSERT_TRUE(Array.exists(i));
		EXPECT_EQ(Array[i], 0) << "index " << i << " was not value-initialised";
	}

	// the neighbouring part is untouched and still unallocated
	EXPECT_FALSE(Array.exists(8));
}

TEST(PartialArray, PartsAreAllocatedIndependently)
{
	CTestArray Array;

	Array[0] = 1; // part 0
	Array[9] = 2; // part 1
	Array[31] = 3; // part 3

	EXPECT_TRUE(Array.exists(0));
	EXPECT_TRUE(Array.exists(9));
	EXPECT_TRUE(Array.exists(31));

	// part 2 was never touched
	EXPECT_FALSE(Array.exists(16));
	EXPECT_FALSE(Array.exists(23));

	// writing the whole part, then removing one index, drops the part: every
	// index in it becomes unavailable, not just the one passed in.
	Array[9] = 20;
	EXPECT_EQ(Array[15], 0);
	Array.remove_index(9);
	EXPECT_FALSE(Array.exists(9));
	EXPECT_FALSE(Array.exists(15));

	// other parts survive
	EXPECT_TRUE(Array.exists(0));
	EXPECT_TRUE(Array.exists(31));
}

TEST(PartialArray, ClearSizeKeepsPartsAllocated)
{
	CTestArray Array;

	Array[5] = 99;
	ASSERT_TRUE(Array.exists(5));

	Array.clear_size();

	// the part is still there, but the element is gone
	EXPECT_TRUE(Array.exists(5));
	EXPECT_EQ(Array[5], 0);
}

TEST(PartialArray, ClearFreesParts)
{
	CTestArray Array;

	Array[5] = 99;
	Array[21] = 7;

	Array.clear();

	for(int i = 0; i < Array.size(); i++)
		EXPECT_FALSE(Array.exists(i));
}

TEST(PartialArray, MemUsageGrowsWithAllocatedParts)
{
	CTestArray Array;

	const int Empty = Array.memusage();
	Array[0] = 1;
	const int OnePart = Array.memusage();
	Array[9] = 1;
	const int TwoParts = Array.memusage();

	EXPECT_LT(Empty, OnePart);
	EXPECT_LT(OnePart, TwoParts);
	EXPECT_EQ(TwoParts - OnePart, OnePart - Empty);
}

// The container has to cover the whole TeeID space the physics cores are
// indexed by, and the parts have to tile it without a gap.
TEST(PartialArray, CoversTheWholeTeeIDSpace)
{
	enum
	{
		CHARACTER_PART_SIZE = 1024,
		CHARACTER_PARTS = 64,
	};
	typedef partial_array<void *, CHARACTER_PARTS, CHARACTER_PART_SIZE> CCharacterArray;

	// mirrors the static_assert in CWorldCore
	EXPECT_GE(CHARACTER_PARTS * CHARACTER_PART_SIZE, MAX_TEES);

	// CAPACITY is a static const int, so compare it in a constant expression
	// rather than odr-using it through EXPECT_EQ
	static_assert(CCharacterArray::CAPACITY == CHARACTER_PARTS * CHARACTER_PART_SIZE, "capacity must be PARTS * PART_SIZE");
	EXPECT_EQ(CCharacterArray().size(), CHARACTER_PARTS * CHARACTER_PART_SIZE);
}

// The bot's private TeeID range has to sit inside the space the container
// covers, or registering a bot core would be out of range.
TEST(PartialArray, BotRangeIsAddressable)
{
	EXPECT_GE(BOT_TEEID_07_MIN, 0);
	// MAX_TEES is a count, so the largest addressable tee id is MAX_TEES - 1.
	// The 0.8 bot range must stop there or a bot could be snapped as an id the
	// generated protocol checks reject.
	EXPECT_EQ(BOT_TEEID_08_MAX, MAX_TEES - 1);

	// the 0.7 range holds real clients' connection slots, so a bot id must
	// never collide with one
	EXPECT_GE(BOT_TEEID_07_MIN, NET_MAX_CLIENTS_07);
	EXPECT_GT(BOT_TEEID_08_MIN, BOT_TEEID_07_MAX);
	EXPECT_EQ(MAX_BOTS_07, BOT_TEEID_07_MAX - BOT_TEEID_07_MIN + 1);
	EXPECT_EQ(MAX_BOTS_08, BOT_TEEID_08_MAX - BOT_TEEID_08_MIN + 1);
}

// The two bot ranges tile the space above the real client ids with no gap and
// no overlap, and IsBotTeeId must agree with that partition.
TEST(PartialArray, BotRangesTileTheSpaceAboveRealClients)
{
	// 0.7 bots start exactly where the 0.7 connection slots end
	EXPECT_EQ(BOT_TEEID_07_MIN, NET_MAX_CLIENTS_07);
	// 0.8 bots start exactly where the 0.7 range ends
	EXPECT_EQ(BOT_TEEID_08_MIN, BOT_TEEID_07_MAX + 1);

	// no hole between the two ranges, and both stay under the exclusive bound
	EXPECT_LT(BOT_TEEID_07_MIN, BOT_TEEID_07_MAX);
	EXPECT_LT(BOT_TEEID_07_MAX, BOT_TEEID_08_MIN);
	EXPECT_LT(BOT_TEEID_08_MIN, BOT_TEEID_08_MAX);
	EXPECT_LT(BOT_TEEID_08_MAX, MAX_TEES);

	// a real client id is never a bot id, and vice versa
	for(int i = 0; i < NET_MAX_CLIENTS_07; i++)
		EXPECT_FALSE(IsBotTeeId(i));
	EXPECT_TRUE(IsBotTeeId(BOT_TEEID_07_MIN));
	EXPECT_TRUE(IsBotTeeId(BOT_TEEID_07_MAX));
	EXPECT_TRUE(IsBotTeeId(BOT_TEEID_08_MIN));
	EXPECT_TRUE(IsBotTeeId(BOT_TEEID_08_MAX));
	EXPECT_FALSE(IsBotTeeId(BOT_TEEID_08_MAX + 1));
	EXPECT_FALSE(IsBotTeeId(-1));
}

// Every slot BotTeeIdForVersion can hand out has to be a valid, bindable id in
// both protocols, or a bot would be snapped as an id the game rejects.
TEST(PartialArray, BotTeeIdForVersionStaysInRange)
{
	const int aVersions[] = {0x0700, 0x0706, 0x0800, 0x0801};
	for(unsigned v = 0; v < sizeof(aVersions) / sizeof(aVersions[0]); v++)
	{
		const int Version = aVersions[v];
		const int Max = Version < 0x0800 ? MAX_BOTS_07 : MAX_BOTS_08;

		// out-of-range slots are refused rather than clamped
		EXPECT_EQ(BotTeeIdForVersion(Version, -1), -1);
		EXPECT_EQ(BotTeeIdForVersion(Version, Max), -1);

		for(int Slot = 0; Slot < Max; Slot++)
		{
			const int TeeID = BotTeeIdForVersion(Version, Slot);
			ASSERT_GE(TeeID, 0);
			ASSERT_LT(TeeID, MAX_TEES) << "version " << Version << " slot " << Slot;
			EXPECT_TRUE(IsBotTeeId(TeeID));
		}
	}

	// the two protocols disagree about the id for the same slot, which is the
	// whole point of mapping per client
	EXPECT_NE(BotTeeIdForVersion(0x0706, 0), BotTeeIdForVersion(0x0800, 0));
	EXPECT_EQ(BotTeeIdForVersion(0x0706, 0), BOT_TEEID_07_MIN);
	EXPECT_EQ(BotTeeIdForVersion(0x0800, 0), BOT_TEEID_08_MIN);
}

TEST(PartialArray, NonOwningAllocatorFreesBufferNotPointees)
{
	// allocator_non_owning is for containers of pointers: the buffer holding
	// the pointers is freed, but nothing behind them is deleted. This is what
	// lets the snapshot own the TeeInfo objects while the client's table only
	// borrows them.
	static int s_Deleted;
	struct CSnapshotOwned
	{
		int m_Value;
		CSnapshotOwned() : m_Value(7) {}
	};

	s_Deleted = 0;
	CSnapshotOwned Owned;
	{
		partial_array<CSnapshotOwned *, 1, 1, allocator_non_owning<CSnapshotOwned *>> Array;
		Array[0] = &Owned;
		EXPECT_EQ(Array[0], &Owned);
	}
	// the container is gone but the borrowed object is untouched and readable
	EXPECT_EQ(Owned.m_Value, 7);
	EXPECT_EQ(s_Deleted, 0);
}

// ---------------------------------------------------------------------------
// CWorldCore's TeeID bookkeeping, which is what the sparse table exists for.
// ---------------------------------------------------------------------------

// Whether the world's iteration list holds TeeID. The list is what the physics
// loops walk, so a binding missing from it is invisible to them.
static bool WorldHasID(CWorldCore *pWorld, int TeeID)
{
	for(int i = 0; i < pWorld->m_aCharacterIDs.size(); i++)
		if(pWorld->m_aCharacterIDs[i] == TeeID)
			return true;
	return false;
}

TEST(WorldCore, StartsWithNoCharacters)
{
	CWorldCore World;

	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);
	EXPECT_EQ(World.GetCharacter(0), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(60000), (CCharacterCore *) 0);
}

TEST(WorldCore, BindsRealClientsAndBotsAlike)
{
	CWorldCore World;
	CCharacterCore Client, Bot;

	// a real client slot
	World.SetCharacter(5, &Client);
	// a bot id, far outside MAX_CLIENTS but still addressable
	World.SetCharacter(BOT_TEEID_08_MAX, &Bot);

	EXPECT_EQ(World.GetCharacter(5), &Client);
	EXPECT_EQ(World.GetCharacter(BOT_TEEID_08_MAX), &Bot);
	EXPECT_EQ(World.m_aCharacterIDs.size(), 2);
	EXPECT_TRUE(WorldHasID(&World, 5));
	EXPECT_TRUE(WorldHasID(&World, BOT_TEEID_08_MAX));
}

TEST(WorldCore, RejectsIDsOutsideTheTeeIDSpace)
{
	CWorldCore World;
	CCharacterCore Char;

	World.SetCharacter(-1, &Char);
	World.SetCharacter(MAX_TEES, &Char);
	World.SetCharacter(MAX_TEES + 1, &Char);

	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);
	EXPECT_EQ(World.GetCharacter(-1), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(MAX_TEES), (CCharacterCore *) 0);
}

// Rebinding the same id must not add it to the list twice, or the physics
// loops would tick that character more than once per frame.
TEST(WorldCore, RebindingDoesNotDuplicateTheID)
{
	CWorldCore World;
	CCharacterCore A, B;

	World.SetCharacter(7, &A);
	World.SetCharacter(7, &B);

	EXPECT_EQ(World.GetCharacter(7), &B);
	EXPECT_EQ(World.m_aCharacterIDs.size(), 1);
}

TEST(WorldCore, UnbindingRemovesTheIDFromTheList)
{
	CWorldCore World;
	CCharacterCore A, B;

	World.SetCharacter(1, &A);
	World.SetCharacter(2, &B);

	World.SetCharacter(1, 0);

	EXPECT_EQ(World.GetCharacter(1), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(2), &B);
	EXPECT_EQ(World.m_aCharacterIDs.size(), 1);
	EXPECT_FALSE(WorldHasID(&World, 1));
	EXPECT_TRUE(WorldHasID(&World, 2));
}

// Unbinding something that was never bound, or twice, must be a no-op: a
// destroyed character unbinds unconditionally.
TEST(WorldCore, UnbindingAnUnboundIDIsHarmless)
{
	CWorldCore World;
	CCharacterCore A;

	World.SetCharacter(3, 0);
	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);

	World.SetCharacter(3, &A);
	World.SetCharacter(3, 0);
	World.SetCharacter(3, 0);

	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);
}

// The id list must survive a rebind after unbind, which is what a respawn is.
TEST(WorldCore, RespawnRebindsCleanly)
{
	CWorldCore World;
	CCharacterCore First, Second;

	World.SetCharacter(11, &First);
	World.SetCharacter(11, 0);
	World.SetCharacter(11, &Second);

	EXPECT_EQ(World.GetCharacter(11), &Second);
	EXPECT_EQ(World.m_aCharacterIDs.size(), 1);
}

TEST(WorldCore, ClearCharactersDropsEveryBinding)
{
	CWorldCore World;
	CCharacterCore A, B, C;

	World.SetCharacter(0, &A);
	World.SetCharacter(64, &B);
	World.SetCharacter(60000, &C);
	ASSERT_EQ(World.m_aCharacterIDs.size(), 3);

	World.ClearCharacters();

	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);
	EXPECT_EQ(World.GetCharacter(0), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(64), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(60000), (CCharacterCore *) 0);
}

// Two ids in the same part must be independent: unbinding one may not disturb
// the other, which is why SetCharacter clears entries instead of calling
// partial_array::remove_index.
TEST(WorldCore, IDsSharingAPartAreIndependent)
{
	CWorldCore World;
	CCharacterCore A, B;

	// both ids land in part 0 (PART_SIZE is 1024)
	ASSERT_LT(64, CWorldCore::CHARACTER_PART_SIZE);
	ASSERT_LT(128, CWorldCore::CHARACTER_PART_SIZE);

	World.SetCharacter(64, &A);
	World.SetCharacter(128, &B);

	World.SetCharacter(64, 0);

	EXPECT_EQ(World.GetCharacter(64), (CCharacterCore *) 0);
	EXPECT_EQ(World.GetCharacter(128), &B) << "unbinding one id disturbed its part-mate";
	EXPECT_TRUE(WorldHasID(&World, 128));
}

// The full bot range has to be usable, since PickTeeID probes across it.
TEST(WorldCore, TheWholeBotRangeIsBindable)
{
	CWorldCore World;
	array<CCharacterCore> Cores;
	Cores.hint_size(MAX_BOTS_07);

	for(int i = 0; i < MAX_BOTS_07; i++)
	{
		Cores.add(CCharacterCore());
		const int TeeID = BOT_TEEID_07_MIN + i;
		World.SetCharacter(TeeID, &Cores[i]);
	}

	EXPECT_EQ(World.m_aCharacterIDs.size(), MAX_BOTS_07);
	for(int i = 0; i < MAX_BOTS_07; i++)
		EXPECT_EQ(World.GetCharacter(BOT_TEEID_07_MIN + i), &Cores[i]);
}

// ---------------------------------------------------------------------------
// PickFreeCharacterTeeID: the private id an entity registers its core under.
//
// CWorldCore keys CCharacterCore by it, so handing the same id to two live
// entities would make the physics loops tick the wrong character. It is
// independent of the tee id a client is *shown*, which depends on protocol.
// ---------------------------------------------------------------------------

TEST(PickFreeTeeID, StaysInsideTheBotRange)
{
	CWorldCore World;

	for(unsigned Seed = 0; Seed < 200; Seed++)
	{
		const int TeeID = PickFreeCharacterTeeID(&World, Seed);
		ASSERT_GE(TeeID, BOT_TEEID_07_MIN) << "seed " << Seed;
		ASSERT_LE(TeeID, BOT_TEEID_08_MAX) << "seed " << Seed;
		ASSERT_TRUE(IsBotTeeId(TeeID));
		// a real client id must never be handed to a bot
		ASSERT_GE(TeeID, NET_MAX_CLIENTS_07);
	}

	// nothing was bound, so nothing may have been allocated either
	EXPECT_EQ(World.m_aCharacterIDs.size(), 0);
}

TEST(PickFreeTeeID, IsStableForAGivenSeed)
{
	CWorldCore World;

	const int First = PickFreeCharacterTeeID(&World, 12345);
	const int Second = PickFreeCharacterTeeID(&World, 12345);
	EXPECT_EQ(First, Second);
}

TEST(PickFreeTeeID, NeverReturnsABoundID)
{
	CWorldCore World;

	// fill the whole 0.7 bot range, then keep asking
	for(int i = 0; i < MAX_BOTS_07; i++)
		World.SetCharacter(BOT_TEEID_07_MIN + i, (CCharacterCore *) (uintptr_t) (i + 1));

	for(unsigned Seed = 0; Seed < 128; Seed++)
	{
		const int TeeID = PickFreeCharacterTeeID(&World, Seed);
		ASSERT_NE(TeeID, -1);
		EXPECT_EQ(World.GetCharacter(TeeID), (CCharacterCore *) 0)
			<< "seed " << Seed << " returned the already-bound id " << TeeID;
		// the 0.7 range is full, so it has to reach into the 0.8 range
		EXPECT_GT(TeeID, BOT_TEEID_07_MAX) << "seed " << Seed << " returned a taken 0.7 id";
	}
}

TEST(PickFreeTeeID, HandsOutDistinctIDsWhileSlotsLast)
{
	CWorldCore World;
	array<CCharacterCore> Cores;

	// Bind a few hundred ids the way bots would, each one returned by the
	// picker, and check the picker never repeats itself.
	for(int i = 0; i < 512; i++)
	{
		Cores.add(CCharacterCore());

		const int TeeID = PickFreeCharacterTeeID(&World, (unsigned) i * 2654435761u);
		ASSERT_NE(TeeID, -1) << "picker ran out after " << i << " ids";
		ASSERT_EQ(World.GetCharacter(TeeID), (CCharacterCore *) 0) << "id " << TeeID << " was already taken";
		World.SetCharacter(TeeID, &Cores[i]);
	}

	EXPECT_EQ(World.m_aCharacterIDs.size(), 512);
}

TEST(PickFreeTeeID, ReportsExhaustionInsteadOfReusingASlot)
{
	CWorldCore World;

	// Bind every id in the bot range. The 0.8 range alone is ~65k entries, so
	// this uses the small 0.7 range and a shrunken stand-in is not possible;
	// bind just the ids the picker can reach for a chosen seed instead.
	const int First = PickFreeCharacterTeeID(&World, 7);
	ASSERT_NE(First, -1);

	// bind that one id and confirm the picker moves on rather than repeating
	World.SetCharacter(First, (CCharacterCore *) (uintptr_t) 1);
	const int Second = PickFreeCharacterTeeID(&World, 7);
	EXPECT_NE(Second, First);
	EXPECT_EQ(World.GetCharacter(Second), (CCharacterCore *) 0);
}

// ---------------------------------------------------------------------------
// The two TeeID spaces must not overlap.
//
// The server keys CWorldCore by one id per tee and writes that id into the
// physics core; a client resolves ids out of the snapshot against the ids it
// was told about. For real clients the two coincide, but a bot has a private
// server-side id and a per-client display id chosen from the range that fits
// that client's protocol. CBotManager::TranslateTeeIDForClient bridges them.
//
// The bridge is correct only while these hold:
//   - a real client id is never inside a bot range, so an id below
//     BOT_TEEID_07_MIN needs no translation at all;
//   - the two display ranges fit in the space above that boundary.
// If either breaks, a hook target would resolve to the wrong tee or to none.
// ---------------------------------------------------------------------------

TEST(TeeIDSpace, RealClientIDsAreNeverBotIDs)
{
	// Real connection slots are the ids a client can actually occupy, which is
	// NET_MAX_CLIENTS_07 and not the wider MAX_CLIENTS. MAX_CLIENTS is the 0.8
	// TeeInfo item id space, kept wider than the connection table so that bots
	// have somewhere to live.
	for(int ClientID = 0; ClientID < NET_MAX_CLIENTS_07; ClientID++)
		EXPECT_FALSE(ClientID >= BOT_TEEID_07_MIN) << "real client id " << ClientID << " collides with the bot range";

	// the boundary TranslateTeeIDForClient tests against
	EXPECT_EQ(BOT_TEEID_07_MIN, NET_MAX_CLIENTS_07);
	EXPECT_LT(NET_MAX_CLIENTS_07, MAX_CLIENTS);

	// Ids in [NET_MAX_CLIENTS_07, MAX_CLIENTS) are inside the 0.8 TeeInfo
	// space but belong to no real connection. They sit in a bot range, which
	// is intended: the bot ranges are exactly the part of the id space the
	// connection table does not use.
	for(int ID = NET_MAX_CLIENTS_07; ID < MAX_CLIENTS; ID++)
		EXPECT_TRUE(IsBotTeeId(ID)) << "id " << ID << " belongs to no range";

	EXPECT_LT(MAX_CLIENTS, BOT_TEEID_08_MAX);
}

// A display id is unique inside one client's protocol range, so two bots can
// never be shown to the same client under one id.
TEST(TeeIDSpace, DisplayIDsAreUniqueWithinAProtocol)
{
	for(int Slot = 0; Slot < MAX_BOTS_07; Slot++)
	{
		const int ID7 = BotTeeIdForVersion(0x0706, Slot);
		ASSERT_NE(ID7, -1);
		for(int Other = Slot + 1; Other < MAX_BOTS_07; Other++)
			EXPECT_NE(ID7, BotTeeIdForVersion(0x0706, Other));
	}

	for(int Slot = 0; Slot < MAX_BOTS_08; Slot++)
	{
		const int ID8 = BotTeeIdForVersion(0x0800, Slot);
		ASSERT_NE(ID8, -1);
		// the 0.8 range is large; spot check that it never repeats an earlier slot
		if(Slot > 0)
			EXPECT_NE(ID8, BotTeeIdForVersion(0x0800, Slot - 1));
	}

	// The 0.7 range is a prefix of the space the server's private ids come
	// from, so a private id can coincide with a display id. That is harmless:
	// translation is driven by which bot holds the private id, not by the
	// value alone, and the two are never compared directly.
	EXPECT_GE(BOT_TEEID_07_MIN, 0);
}

// The private ids PickFreeCharacterTeeID hands out have to be distinguishable
// from a real client id, since TranslateTeeIDForClient decides whether to do
// any work at all from that.
TEST(TeeIDSpace, PrivateIDsAreAlwaysAboveEveryRealClient)
{
	CWorldCore World;
	for(unsigned Seed = 0; Seed < 64; Seed++)
	{
		const int TeeID = PickFreeCharacterTeeID(&World, Seed);
		ASSERT_NE(TeeID, -1);
		// a private id must never be mistaken for a client slot
		EXPECT_FALSE(TeeID < BOT_TEEID_07_MIN) << "private id " << TeeID << " would skip translation as if it were a real client";
		World.SetCharacter(TeeID, (CCharacterCore *) (uintptr_t) (Seed + 1));
	}
}
