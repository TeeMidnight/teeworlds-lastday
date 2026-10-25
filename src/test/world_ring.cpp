/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// The ring: the instances of one euclidean floor are consecutive windows of a
// single shared noise field, so two neighbours generate the strip they share
// identically and the whole ring closes after the last instance.
//
// These cover the coordinate mapping and the tile/resource continuity the
// boundary transfer relies on.
#include <gtest/gtest.h>

#include <engine/server/mapgen.h>
#include <engine/server/worldgen.h>

#include <base/tl/array.h>

#include <game/collision.h>
#include <game/gamecore.h>

namespace {

const int W = 300; // instance width
const int H = 80;  // instance height
const int M = 75;  // shared margin
const int S = W - M; // stride between neighbouring instances

void MakeInstruction(CInstruction &Inst, bool Ring)
{
	Inst.m_Kind = WORLDKIND_EUCLID;
	str_copy(Inst.m_aFloorName, "RingTest", sizeof(Inst.m_aFloorName));
	Inst.m_aBaseMap[0] = '\0';

	Inst.m_Generation.m_Enabled = true;
	Inst.m_Generation.m_Width = W;
	Inst.m_Generation.m_Height = H;
	Inst.m_Generation.m_SurfaceLevel = H / 2;
	Inst.m_Generation.m_SurfaceAmplitude = 22.0f;
	Inst.m_Generation.m_EdgeWall = 2;
	Inst.m_Generation.m_Surface.m_Octaves = 4;
	Inst.m_Generation.m_Surface.m_Frequency = 0.008f;
	Inst.m_Generation.m_Surface.m_Lacunarity = 2.0f;
	Inst.m_Generation.m_Surface.m_Persistence = 0.5f;
	Inst.m_Generation.m_Surface.m_Amplitude = 0.0f;

	Inst.m_Generation.m_Ring.m_Enabled = Ring;
	Inst.m_Generation.m_Ring.m_Size = 1024;
	Inst.m_Generation.m_Ring.m_SharedMargin = M;
	Inst.m_Generation.m_Ring.m_BaseSeed = 1234;
	Inst.m_Generation.m_RingBackgroundGroundY = -1;

	Inst.m_Generation.m_Caves.m_Enabled = true;
	Inst.m_Generation.m_Caves.m_Noise.m_Octaves = 3;
	Inst.m_Generation.m_Caves.m_Noise.m_Frequency = 0.045f;
	Inst.m_Generation.m_Caves.m_Noise.m_Lacunarity = 2.0f;
	Inst.m_Generation.m_Caves.m_Noise.m_Persistence = 0.5f;
	Inst.m_Generation.m_Caves.m_Noise.m_SeedOffset = 2000;
	Inst.m_Generation.m_Caves.m_Threshold = 0.60f;
	Inst.m_Generation.m_Caves.m_MinDepth = 6;
	Inst.m_Generation.m_Caves.m_SurfaceMargin = 3;

	CInstruction::COreConfig &Ore = Inst.m_Generation.m_Ores.emplace();
	Ore.m_Noise.m_Octaves = 3;
	Ore.m_Noise.m_Frequency = 0.05f;
	Ore.m_Noise.m_Lacunarity = 2.0f;
	Ore.m_Noise.m_Persistence = 0.5f;
	Ore.m_Noise.m_SeedOffset = 3000;
	Ore.m_Threshold = 0.68f;
	Ore.m_MinDepth = 12;
	Ore.m_Probes = 2;
	Ore.m_Proba = 1.0f;
	Ore.m_HardnessMin = 1;
	Ore.m_HardnessMax = 5;
	Ore.m_RespawnTimeMin = 60;
	Ore.m_RespawnTimeMax = 120;
	Ore.m_SurfaceProba = 0.30f;
	str_copy(Ore.m_aResId, "iron_ore", sizeof(Ore.m_aResId));
	str_copy(Ore.m_aDisplay, "armor", sizeof(Ore.m_aDisplay));

	Inst.m_Generation.m_SurfaceEntrances.m_Enabled = false;
	Inst.m_Generation.m_SurfaceEntrances.m_Count = 0;
}

// Cave cleanup reads its neighbours, and the outermost columns of a generated
// buffer have no real neighbour (a ring instance does not carry the next
// instance's cave field). The generator confines the difference to those
// columns, which the transfer region keeps players away from.
const int EDGE = 2;

struct SRes
{
	int m_X, m_Y, m_Hardness, m_RespawnTime;
};

void CollectResources(const CGeneratedWorld *pWorld, int X0, int X1, array<SRes> &lOut)
{
	for(int i = 0; i < pWorld->m_lResources.size(); i++)
	{
		const CGeneratedWorld::CResource &R = pWorld->m_lResources[i];
		if(R.m_X < X0 || R.m_X >= X1)
			continue;
		SRes Out = {R.m_X - X0, R.m_Y, R.m_Hardness, R.m_RespawnTime};
		lOut.add(Out);
	}
}

} // namespace

TEST(WorldRing, MapsInstanceOntoTheSharedField)
{
	CInstruction Inst;
	MakeInstruction(Inst, true);

	const CWorldRing First = WorldRing(&Inst, 1);
	EXPECT_TRUE(First.m_Enabled);
	EXPECT_EQ(First.m_OffsetX, 0);
	EXPECT_EQ(First.m_Stride, S);
	EXPECT_EQ(First.m_Period, 1024 * S);

	EXPECT_EQ(WorldRing(&Inst, 2).m_OffsetX, S);
	EXPECT_EQ(WorldRing(&Inst, 1024).m_OffsetX, 1023 * S);
	// the last instance's shared strip starts exactly at the ring seam, which is
	// the first instance's offset
	EXPECT_EQ(WorldRing(&Inst, 1024).m_OffsetX + S, WorldRing(&Inst, 1024).m_Period);
}

TEST(WorldRing, DisabledKeepsTheSelfWrappingCylinder)
{
	CInstruction Inst;
	MakeInstruction(Inst, false);
	const CWorldRing Ring = WorldRing(&Inst, 7);
	EXPECT_FALSE(Ring.m_Enabled);
	EXPECT_EQ(Ring.m_OffsetX, 0);
	EXPECT_EQ(Ring.m_Period, W);
	EXPECT_EQ(Ring.m_NoiseSeed, 7u);
}

TEST(WorldRing, NeighboursShareTheirTerrainAndResources)
{
	CInstruction Inst;
	MakeInstruction(Inst, true);
	CGeneratedWorld *pA = GenerateEuclideanWorld(&Inst, 1);
	CGeneratedWorld *pB = GenerateEuclideanWorld(&Inst, 2);
	ASSERT_NE(pA, nullptr);
	ASSERT_NE(pB, nullptr);
	ASSERT_FALSE(pA->m_lLayers.size() < 1);
	ASSERT_FALSE(pB->m_lLayers.size() < 1);

	// whole surface of the shared strip: a pure function of the global column
	for(int i = 0; i < M; i++)
		EXPECT_EQ(pA->m_Surface[S + i], pB->m_Surface[i]) << "surface column " << i;

	const CGeneratedWorld::CLayer &LA = pA->m_lLayers[0];
	const CGeneratedWorld::CLayer &LB = pB->m_lLayers[0];
	for(int i = EDGE; i < M - EDGE; i++)
	{
		for(int y = 0; y < H; y++)
		{
			const CTile &TA = LA.m_pTiles[y * W + (S + i)];
			const CTile &TB = LB.m_pTiles[y * W + i];
			ASSERT_EQ(TA.m_Index, TB.m_Index) << "tile " << i << "," << y;
			ASSERT_EQ(TA.m_Flags, TB.m_Flags) << "tile " << i << "," << y;
		}
	}

	// The shared strip has a single resource owner: an instance only creates a
	// node where its local column is at least one shared margin in, so its left
	// strip is empty and its right strip carries the nodes the next instance
	// skips. A physical node therefore exists exactly once across the ring.
	array<SRes> lOwned, lSkipped;
	CollectResources(pA, S + EDGE, W - EDGE, lOwned);
	CollectResources(pB, EDGE, M - EDGE, lSkipped);
	EXPECT_FALSE(lOwned.size() == 0);
	EXPECT_TRUE(lSkipped.size() == 0);
	for(int i = 0; i < pA->m_lResources.size(); i++)
		EXPECT_GE(pA->m_lResources[i].m_X, M) << "slice 1 node in its left strip";
	for(int i = 0; i < pB->m_lResources.size(); i++)
		EXPECT_GE(pB->m_lResources[i].m_X, M) << "slice 2 node in its left strip";

	delete pA;
	delete pB;
}

TEST(WorldRing, BackgroundAnchorsToTheWholeRing)
{
	// Every instance of a ring has to anchor its clouds and mountains to the
	// same ground line, otherwise the scenery jumps vertically on a crossing.
	// The anchor is the highest ground of the whole ring, so it also has to sit
	// at or above (a smaller y than) the highest ground of each instance.
	CInstruction Inst;
	MakeInstruction(Inst, true);
	CGeneratedWorld *pA = GenerateEuclideanWorld(&Inst, 1);
	CGeneratedWorld *pB = GenerateEuclideanWorld(&Inst, 2);
	ASSERT_NE(pA, nullptr);
	ASSERT_NE(pB, nullptr);

	const auto HighestGround = [](const CGeneratedWorld *pWorld) {
		int Y = pWorld->m_Surface[0];
		for(int i = 1; i < pWorld->m_Surface.size(); i++)
			Y = minimum(Y, pWorld->m_Surface[i]);
		return Y;
	};
	const int HighestA = HighestGround(pA);
	const int HighestB = HighestGround(pB);
	EXPECT_EQ(pA->m_BackgroundGroundY, pB->m_BackgroundGroundY);
	EXPECT_LE(pA->m_BackgroundGroundY, HighestA);
	EXPECT_LE(pA->m_BackgroundGroundY, HighestB);

	delete pA;
	delete pB;
}

TEST(WorldRing, ClosesAfterTheLastInstance)
{
	CInstruction Inst;
	MakeInstruction(Inst, true);
	CGeneratedWorld *pLast = GenerateEuclideanWorld(&Inst, 1024);
	CGeneratedWorld *pFirst = GenerateEuclideanWorld(&Inst, 1);
	ASSERT_NE(pLast, nullptr);
	ASSERT_NE(pFirst, nullptr);

	// instance 1024's right strip is local [S, W) at global [P, P+M); instance
	// 1's left strip is local [0, M) at global [0, M). The ring closes, so the
	// two are the same place and the shared field gives them the same terrain.
	for(int i = 0; i < M; i++)
		EXPECT_EQ(pLast->m_Surface[S + i], pFirst->m_Surface[i]) << "closing surface " << i;

	const CGeneratedWorld::CLayer &LL = pLast->m_lLayers[0];
	const CGeneratedWorld::CLayer &LF = pFirst->m_lLayers[0];
	for(int i = EDGE; i < M - EDGE; i++)
		for(int y = 0; y < H; y++)
			ASSERT_EQ(LL.m_pTiles[y * W + (S + i)].m_Index, LF.m_pTiles[y * W + i].m_Index)
				<< "closing tile " << i << "," << y;

	delete pLast;
	delete pFirst;
}

TEST(WorldRing, TransferHysteresis)
{
	// the worked example: 300 tiles wide, 75 shared, stride 225
	EXPECT_FLOAT_EQ(RingTriggerX(300, 75, 1), 275.0f * 32.0f);
	EXPECT_FLOAT_EQ(RingTriggerX(300, 75, -1), 25.0f * 32.0f);

	// crossing right at the trigger lands 50 tiles into the next instance;
	// crossing left at the trigger lands at 250
	EXPECT_FLOAT_EQ(RingArrivalX(225, 1, RingTriggerX(300, 75, 1)), 50.0f * 32.0f);
	EXPECT_FLOAT_EQ(RingArrivalX(225, -1, RingTriggerX(300, 75, -1)), 250.0f * 32.0f);

	// the landing is strictly inside both triggers, so a crossing cannot bounce
	const float Right = RingTriggerX(300, 75, 1);
	const float Left = RingTriggerX(300, 75, -1);
	EXPECT_GT(RingArrivalX(225, 1, Right), Left);
	EXPECT_LT(RingArrivalX(225, 1, Right), Right);
	EXPECT_GT(RingArrivalX(225, -1, Left), Left);
	EXPECT_LT(RingArrivalX(225, -1, Left), Right);
}

// The crossing is detected after the move, and a switch can also be delayed
// (the neighbour may still be loading). Either way the source column can end up
// outside the strip both instances share; only inside that strip is the terrain
// guaranteed to match. Mapping such an overshoot straight through lands the
// character on the neighbour's own terrain, which is how a tee ends up inside a
// hill after a crossing.
TEST(WorldRing, OvershotArrivalIsClampedIntoTheSharedStrip)
{
	const int W = 1000, M = 75;
	const int S = W - M;

	// a normal crossing at the trigger is unchanged
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, 1, RingTriggerX(W, M, 1)), 50.0f * 32.0f);
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, -1, RingTriggerX(W, M, -1)), 950.0f * 32.0f);

	// drifting past the left edge: the source clamps to column 0, so the
	// arrival is the first column of the destination's shared strip
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, -1, -500.0f), (float) S * 32.0f);
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, -1, -50.0f * 32.0f), (float) S * 32.0f);

	// drifting past the right edge: the source clamps to the last column, so
	// the arrival is the last column of the strip
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, 1, (float) W * 32.0f + 500.0f), (float) (M - 1) * 32.0f + 31.0f);
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, 1, (float) W * 32.0f), (float) (M - 1) * 32.0f + 31.0f);

	// a source inside the strip keeps its exact offset
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, 1, (float) (W - 1) * 32.0f + 16.0f), (float) (M - 1) * 32.0f + 16.0f);
	EXPECT_FLOAT_EQ(RingArrivalXClamped(S, W, M, -1, 16.0f), (float) S * 32.0f + 16.0f);
}

TEST(WorldRing, CrossDirectionMatchesTheTransferTriggers)
{
	const float Right = RingTriggerX(300, 75, 1);
	const float Left = RingTriggerX(300, 75, -1);

	// inside the instance: neither trigger
	EXPECT_EQ(RingCrossDirection((Left + Right) * 0.5f, Right, Left), 0);
	EXPECT_EQ(RingCrossDirection(Left + 1.0f, Right, Left), 0);
	EXPECT_EQ(RingCrossDirection(Right - 1.0f, Right, Left), 0);

	// at or past a trigger: crosses that way. The boundary is inclusive, so an
	// entity standing exactly on it is transferred rather than left behind.
	EXPECT_EQ(RingCrossDirection(Right, Right, Left), 1);
	EXPECT_EQ(RingCrossDirection(Right + 1.0f, Right, Left), 1);
	EXPECT_EQ(RingCrossDirection(Left, Right, Left), -1);
	EXPECT_EQ(RingCrossDirection(Left - 1.0f, Right, Left), -1);
}

TEST(WorldRing, MirroredNeighbourTakesTheShortWayRound)
{
	const int Period = 1024 * 225;
	// 1 -> 2 and back
	EXPECT_EQ(RingNeighbourShift(0, 225, Period), 225);
	EXPECT_EQ(RingNeighbourShift(225, 0, Period), -225);
	// instance 1's left neighbour is the last instance, one stride back, not a
	// whole circumference forward
	EXPECT_EQ(RingNeighbourShift(0, 1023 * 225, Period), -225);
	EXPECT_EQ(RingNeighbourShift(1023 * 225, 0, Period), 225);
}

TEST(WorldRing, NeighbourSeedWrapsAroundTheRing)
{
	EXPECT_EQ(RingNeighbourSeed(1, 1, 1024), 2);
	EXPECT_EQ(RingNeighbourSeed(1, -1, 1024), 1024);
	EXPECT_EQ(RingNeighbourSeed(1024, 1, 1024), 1);
	EXPECT_EQ(RingNeighbourSeed(1024, -1, 1024), 1023);
}

// A hook flying at a core bound at a translated column grabs it: this is the
// mechanism the ring proxy relies on to reach a player in the next instance.
TEST(RingProxy, HookGrabsACoreBoundAtATranslatedColumn)
{
	const int CW = 64, CH = 16;
	array<int> aTiles;
	aTiles.set_size(CW * CH);
	mem_zero(aTiles.base_ptr(), sizeof(int) * CW * CH);
	CCollision Coll;
	Coll.InitForTesting(CW, CH, aTiles.base_ptr(), false);

	CWorldCore World;
	CCharacterCore Hooker, Proxy;
	Hooker.Init(&World, &Coll);
	Hooker.Reset();
	Proxy.Init(&World, &Coll);
	Proxy.Reset();

	// the next instance's player, bound under its real client id and standing at
	// the column it would occupy in this map
	World.SetCharacter(5, &Proxy);
	Proxy.m_Pos = vec2(90.0f, 0.0f);

	Hooker.m_Pos = vec2(0.0f, 0.0f);
	Hooker.m_HookState = HOOK_FLYING;
	Hooker.m_HookPos = Hooker.m_Pos;
	Hooker.m_HookDir = vec2(1.0f, 0.0f);
	Hooker.m_HookedPlayer = -1;
	Hooker.m_Input.m_Hook = 1;

	Hooker.Tick(true, true);
	EXPECT_EQ(Hooker.m_HookedPlayer, 5);
}

// A bot is borrowed into a neighbour's core under its private tee id, which
// lies in the bot range above every real client id. The hook has to report that
// id: TranslateTeeIDForClient then maps it to the id the receiving client knows.
TEST(RingProxy, HookGrabsABotCoreBoundUnderItsPrivateID)
{
	const int CW = 64, CH = 16;
	array<int> aTiles;
	aTiles.set_size(CW * CH);
	mem_zero(aTiles.base_ptr(), sizeof(int) * CW * CH);
	CCollision Coll;
	Coll.InitForTesting(CW, CH, aTiles.base_ptr(), false);

	CWorldCore World;
	CCharacterCore Hooker, Proxy;
	Hooker.Init(&World, &Coll);
	Hooker.Reset();
	Proxy.Init(&World, &Coll);
	Proxy.Reset();

	const int BotTeeID = BOT_TEEID_07_MIN + 17;
	World.SetCharacter(BotTeeID, &Proxy);
	Proxy.m_Pos = vec2(90.0f, 0.0f);

	Hooker.m_Pos = vec2(0.0f, 0.0f);
	Hooker.m_HookState = HOOK_FLYING;
	Hooker.m_HookPos = Hooker.m_Pos;
	Hooker.m_HookDir = vec2(1.0f, 0.0f);
	Hooker.m_HookedPlayer = -1;
	Hooker.m_Input.m_Hook = 1;

	Hooker.Tick(true, true);
	EXPECT_EQ(Hooker.m_HookedPlayer, BotTeeID);
}
