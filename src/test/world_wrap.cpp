/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
// The cylindrical world: a generated floor has no left or right edge, its two
// ends are the same place.
//
// These cover the tile lookup (which used to clamp at the edge, making the seam
// read as a wall) and the move across it (which used to measure as a jump the
// width of the world).
#include <gtest/gtest.h>

#include <game/collision.h>

namespace {

// a world 8 tiles wide, clear except the last column
const int W = 8;
const int H = 4;

void Fill(int *pTiles, bool SolidAtSeam)
{
	for(int i = 0; i < W * H; i++)
		pTiles[i] = 0;
	if(SolidAtSeam)
	{
		for(int y = 0; y < H; y++)
			pTiles[y * W + (W - 1)] = CCollision::COLFLAG_SOLID;
	}
}

} // namespace

// A lookup one tile left of the map must read the rightmost column, not clamp
// onto column 0; otherwise the seam is a wall.
TEST(WorldWrap, TileLookupWrapsInsteadOfClamping)
{
	int aTiles[W * H];
	Fill(aTiles, true);

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, true);

	// x = -1 tile reads the solid rightmost column
	EXPECT_EQ(Coll.GetCollisionAt(-16.0f, 16.0f), CCollision::COLFLAG_SOLID);
	// x = W tiles reads column 0, which is clear
	EXPECT_EQ(Coll.GetCollisionAt((float) W * 32.0f + 16.0f, 16.0f), 0);
	// and one more full lap lands back on the solid column
	EXPECT_EQ(Coll.GetCollisionAt((float) (2 * W - 1) * 32.0f + 16.0f, 16.0f), CCollision::COLFLAG_SOLID);
}

// With the wrap off the lookup clamps, which is what a hand drawn map wants.
TEST(WorldWrap, ClampingIsPreservedWithoutWrap)
{
	int aTiles[W * H];
	Fill(aTiles, true);

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, false);

	// clamped onto column 0, which is clear
	EXPECT_EQ(Coll.GetCollisionAt(-16.0f, 16.0f), 0);
	EXPECT_FALSE(Coll.WrapsX());
}

// Folding a position is what carries an entity across the seam.
TEST(WorldWrap, WrapXFoldsIntoRange)
{
	int aTiles[W * H];
	Fill(aTiles, false);

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, true);

	const float Width = (float) W * 32.0f;

	EXPECT_FLOAT_EQ(Coll.WrapX(0.0f), 0.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(Width - 1.0f), Width - 1.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(Width), 0.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(Width + 10.0f), 10.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(-10.0f), Width - 10.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(-Width - 10.0f), Width - 10.0f);

	// a non wrapping world leaves the value alone
	Coll.SetWrapX(false);
	EXPECT_FLOAT_EQ(Coll.WrapX(-10.0f), -10.0f);
	EXPECT_FLOAT_EQ(Coll.WrapX(Width + 10.0f), Width + 10.0f);
}

// Moving off the right edge must land just inside the left one, a short step.
TEST(WorldWrap, MovingAcrossTheSeamIsAShortStep)
{
	int aTiles[W * H];
	Fill(aTiles, false);

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, true);

	const float Width = (float) W * 32.0f;

	// one step off the right edge wraps to just inside the left edge
	const float Before = Width - 8.0f;
	const float After = Coll.WrapX(Before + 16.0f);
	EXPECT_FLOAT_EQ(After, 8.0f);

	// the delta across the seam is the short way round, which is what the
	// entrance walk and the stuck check both measure
	float Delta = After - Before;
	if(Delta > Width * 0.5f)
		Delta -= Width;
	if(Delta < -Width * 0.5f)
		Delta += Width;
	EXPECT_FLOAT_EQ(Delta, 16.0f);
}

// The whole point of a cylinder: stepping over the seam meets the ground that
// was on the other side.
TEST(WorldWrap, GroundContinuesAcrossTheSeam)
{
	int aTiles[W * H];
	for(int i = 0; i < W * H; i++)
		aTiles[i] = 0;
	// make the floor solid everywhere below the first row
	for(int y = 1; y < H; y++)
		for(int x = 0; x < W; x++)
			aTiles[y * W + x] = CCollision::COLFLAG_SOLID;

	CCollision Coll;
	Coll.InitForTesting(W, H, aTiles, true);

	const float Width = (float) W * 32.0f;

	// standing just inside the right edge and just inside the left edge see the
	// same floor
	const int RightSample = Coll.GetCollisionAt(Width - 16.0f, 40.0f);
	const int LeftSample = Coll.GetCollisionAt(16.0f, 40.0f);
	EXPECT_EQ(RightSample, LeftSample);
	EXPECT_EQ(LeftSample, CCollision::COLFLAG_SOLID);
}
