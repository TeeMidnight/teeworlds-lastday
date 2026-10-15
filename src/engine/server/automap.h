/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_AUTOMAP_H
#define ENGINE_SERVER_AUTOMAP_H

#include <base/tl/array.h>

#include <game/mapitems.h>

// A port of the Teeworlds 0.7 editor automapper (src/game/editor/auto_map.cpp),
// reduced to the tileset rule sets, loaded from "automap/" instead of from the
// editor map.
//
// As in the original: every non empty tile is first reset to the rule set's
// basetile, rules are tested against a snapshot so they do not feed back into
// each other, all matching rules apply in order with the last one winning, and
// "random" rules only apply when a roll hits them. The layer must already carry
// tile indices before the run; the mapper replaces them with autotile variants.

class CAutoMapper
{
public:
	// a single condition of a rule, e.g. "the tile above is empty"
	struct CRuleCondition
	{
		enum
		{
			EMPTY = 0,
			FULL = 1,
		};

		int m_X;
		int m_Y;
		// EMPTY / FULL for the symbolic values, otherwise the exact tile index
		// that has to be present
		int m_Value;
	};

	struct CRule
	{
		int m_Index;
		int m_Random; // 0 or 1 = always, otherwise a 1 in N chance
		int m_Rotation; // 0, 90, 180 or 270
		int m_HFlip;
		int m_VFlip;
		array<CRuleCondition> m_aConditions;
	};

	struct CRuleSet
	{
		char m_aName[32];
		// the automap file the set came from, so a caller can tell whether its
		// tile indices belong to a given tileset before applying it
		char m_aSource[64];
		int m_BaseTile;
		array<CRule> m_aRules;
	};

	// A doodad is a rectangular stamp taken from a doodads sheet: m_StartId is
	// its top-left tile, m_EndId its bottom-right, both as 16-wide sheet indices
	// (the map format's own layout). The rule's m_Ry is the offset of that
	// top-left from the anchor, and m_Location says where the anchor is found.
	struct CDoodad
	{
		enum
		{
			LOCATION_FLOOR = 0,
			LOCATION_CEILING,
			LOCATION_WALL,
		};

		int m_StartId;
		int m_EndId;
		int m_Rx;
		int m_Ry;
		int m_Location;
		int m_Random; // 0 or 1 = always, otherwise a 1 in N chance
	};

	struct CDoodadSet
	{
		char m_aName[32];
		char m_aSource[64];
		array<CDoodad> m_aDoodads;
	};

	CAutoMapper();
	~CAutoMapper();

	// Loads every "<name>.json" of the "automap" directory and returns how many
	// rule sets were registered.
	int LoadAll(class IStorage *pStorage, class IConsole *pConsole);

	// Loads one automap file: a "tileset" array of single key objects, each
	// defining a named rule set.
	bool LoadFile(class IStorage *pStorage, const char *pPath);

	// Rewrites one index a rule set produces. Lets a layer keep a shared rule
	// set's shapes while swapping indices it does not want (say the soil
	// coloured variants of a stone/soil transition set) without forking the
	// json.
	struct CRemap
	{
		int m_From;
		int m_To;
	};

	// Applies the named rule set and returns false if it is unknown, so the
	// caller can report the typo.
	//
	// WrapX makes the horizontal axis cylindrical: neighbour lookups wrap around
	// the layer edge instead of clamping. A generated euclidean world needs it,
	// otherwise the mapper sees a wall at the seam and draws a line of edge tiles
	// down the world. MarginX keeps the outermost columns out of that wrap, so
	// the border does not end up as a lone corner tile; 0 wraps edge to edge,
	// which is what a hand drawn map wants. pRemap rewrites the produced indices.
	bool Proceed(const char *pRuleSetName, CTile *pTiles, int Width, int Height, unsigned Seed,
		bool WrapX = false, int MarginX = 0, const array<CRemap> *pRemap = 0) const;

	const CRuleSet *FindRuleSet(const char *pName) const;
	int NumRuleSets() const { return m_lRuleSets.size(); }
	const CRuleSet *RuleSet(int Index) const { return Index >= 0 && Index < m_lRuleSets.size() ? &m_lRuleSets[Index] : 0; }

	// the doodad sets of the "doodads" arrays in automap/*.json, kept apart from
	// the tile rule sets because placement, not per-tile scanning, drives them
	const CDoodadSet *FindDoodadSet(const char *pName) const;
	int NumDoodadSets() const { return m_lDoodadSets.size(); }
	const CDoodadSet *DoodadSet(int Index) const { return Index >= 0 && Index < m_lDoodadSets.size() ? &m_lDoodadSets[Index] : 0; }

private:
	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	array<CRuleSet> m_lRuleSets;
	array<CDoodadSet> m_lDoodadSets;

	static int ListFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
};

#endif // ENGINE_SERVER_AUTOMAP_H
