/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_COLLISION_H
#define GAME_COLLISION_H

#include <base/tl/array.h>
#include <base/vmath.h>

class CCollision
{
	array<int> m_lTiles;
	int m_Width;
	int m_Height;
	// Whether the world is a cylinder: the left and right edges are the same
	// place, so a lookup past either edge reads the opposite one. Generated
	// floors are cylinders, hand drawn maps are not.
	bool m_WrapX;
	class CLayers *m_pLayers;

	bool IsTile(int x, int y, int Flag = COLFLAG_SOLID) const;
	int GetTile(int x, int y) const;

public:
	enum
	{
		COLFLAG_SOLID = 1,
		COLFLAG_DEATH = 2,
		COLFLAG_UNHOOKABLE = 4,
		// water (TILE_WATER): lets entities pass but is detectable, so
		// swimming and laser reflection/refraction can react to it.
		COLFLAG_WATER = 1 << 4,
		// a train platform (TILE_TRAIN_ENTRANCE). The tile is a marker: the flag
		// makes it queryable through the normal collision box checks without
		// giving it solidity or a teleport of its own.
		COLFLAG_TRAIN_ENTRANCE = 1 << 16,
	};

	CCollision();
	void Init(class CLayers *pLayers);
	bool CheckPoint(float x, float y, int Flag = COLFLAG_SOLID) const { return IsTile(round_to_int(x), round_to_int(y), Flag); }
	bool CheckPoint(vec2 Pos, int Flag = COLFLAG_SOLID) const { return CheckPoint(Pos.x, Pos.y, Flag); }
	int GetCollisionAt(float x, float y) const { return GetTile(round_to_int(x), round_to_int(y)); }
	int GetWidth() const { return m_Width; }
	int GetHeight() const { return m_Height; }
	bool WrapsX() const { return m_WrapX; }
	void SetWrapX(bool Wrap) { m_WrapX = Wrap; }

	// Installs a tile grid directly, bypassing the map loader. Tests only: the
	// wrap logic is pure array lookup and needs no map file.
	void InitForTesting(int Width, int Height, const int *pTiles, bool Wrap)
	{
		m_Width = Width;
		m_Height = Height;
		m_WrapX = Wrap;
		m_lTiles.set_size(Width * Height);
		for(int i = 0; i < Width * Height; i++)
			m_lTiles[i] = pTiles[i];
	}

	// Folds a world x back into [0, Width) on a cylinder. Everything that must
	// agree on where a position is goes through this, so the seam cannot
	// disagree with itself.
	float WrapX(float x) const
	{
		if(!m_WrapX || m_Width <= 0)
			return x;
		const float W = (float) m_Width * 32.0f;
		x = fmodf(x, W);
		if(x < 0.0f)
			x += W;
		return x;
	}
	int IntersectLine(vec2 Pos0, vec2 Pos1, vec2 *pOutCollision, vec2 *pOutBeforeCollision) const;
	void MovePoint(vec2 *pInoutPos, vec2 *pInoutVel, float Elasticity, int *pBounces) const;
	void MoveBox(vec2 *pInoutPos, vec2 *pInoutVel, vec2 Size, float Elasticity, bool *pDeath = 0) const;
	bool TestBox(vec2 Pos, vec2 Size, int Flag = COLFLAG_SOLID) const;
	int TestBoxAt(vec2 Pos, vec2 Size) const;
	int TestBoxMoveAt(vec2 LastPos, vec2 NewPos, vec2 Size) const;

	// hooks for gamecontroller
	void SetFlagFor(float x, float y, int Flag);
	void SetFlagFor(vec2 Pos, int Flag) { SetFlagFor(Pos.x, Pos.y, Flag); }
};

#endif
