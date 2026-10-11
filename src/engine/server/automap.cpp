/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>

#include <engine/shared/jsonparser.h>

#include "automap.h"

// see automap.h for the semantics, which mirror the Teeworlds 0.7 editor

CAutoMapper::CAutoMapper()
{
	m_pStorage = 0;
	m_pConsole = 0;
}

CAutoMapper::~CAutoMapper()
{
}

const CAutoMapper::CRuleSet *CAutoMapper::FindRuleSet(const char *pName) const
{
	for(int i = 0; i < m_lRuleSets.size(); i++)
	{
		if(str_comp(m_lRuleSets[i].m_aName, pName) == 0)
			return &m_lRuleSets[i];
	}
	return 0;
}

// reads one "rules" array into the rule set
static void ParseRules(const json_value &rRules, CAutoMapper::CRuleSet &RuleSet)
{
	for(unsigned j = 0; j < rRules.u.array.length; j++)
	{
		const json_value &rRuleNode = rRules[j];
		if(rRuleNode.type != json_object)
			continue;

		CAutoMapper::CRule &Rule = RuleSet.m_aRules.emplace();

		const json_value &rIndex = rRuleNode["index"];
		Rule.m_Index = rIndex.type == json_integer ? clamp((int) rIndex.u.integer, 0, 255) : 1;

		// "random" is a 1 in N chance; 0 and 1 both mean "always"
		const json_value &rRandom = rRuleNode["random"];
		Rule.m_Random = rRandom.type == json_integer ? clamp((int) rRandom.u.integer, 0, 99999) : 0;

		const json_value &rRotation = rRuleNode["rotate"];
		Rule.m_Rotation = rRotation.type == json_integer ? clamp((int) rRotation.u.integer, 0, 270) : 0;

		const json_value &rHFlip = rRuleNode["hflip"];
		Rule.m_HFlip = rHFlip.type == json_integer ? clamp((int) rHFlip.u.integer, 0, 1) : 0;

		const json_value &rVFlip = rRuleNode["vflip"];
		Rule.m_VFlip = rVFlip.type == json_integer ? clamp((int) rVFlip.u.integer, 0, 1) : 0;

		const json_value &rCondition = rRuleNode["condition"];
		if(rCondition.type != json_array)
			continue;

		for(unsigned k = 0; k < rCondition.u.array.length; k++)
		{
			CAutoMapper::CRuleCondition &Condition = Rule.m_aConditions.emplace();
			Condition.m_X = (int) (json_int_t) rCondition[k]["x"];
			Condition.m_Y = (int) (json_int_t) rCondition[k]["y"];

			const json_value &rValue = rCondition[k]["value"];
			if(rValue.type == json_string)
			{
				// not an index: either "full" or "empty"
				if(str_comp(rValue.u.string.ptr, "full") == 0)
					Condition.m_Value = CAutoMapper::CRuleCondition::FULL;
				else
					Condition.m_Value = CAutoMapper::CRuleCondition::EMPTY;
			}
			else if(rValue.type == json_integer)
				Condition.m_Value = clamp((int) rValue.u.integer, (int) CAutoMapper::CRuleCondition::EMPTY, 255);
			else
				Condition.m_Value = CAutoMapper::CRuleCondition::EMPTY;
		}
	}
}

bool CAutoMapper::LoadFile(IStorage *pStorage, const char *pPath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pPath, pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("automap", "failed to parse '%s'", pPath);
		return false;
	}

	const json_value &rTileset = (*pJson)["tileset"];
	if(rTileset.type != json_array)
	{
		// doodad rule sets are not part of the tile automapper
		return false;
	}

	for(unsigned i = 0; i < rTileset.u.array.length; i++)
	{
		const json_value &rEntry = rTileset[i];
		if(rEntry.type != json_object || rEntry.u.object.length != 1)
			continue;

		// each entry is { "<ruleset name>": { "basetile": N, "rules": [ ... ] } }
		const char *pName = rEntry.u.object.values[0].name;
		const json_value &rBody = *rEntry.u.object.values[0].value;
		if(rBody.type != json_object)
			continue;

		CRuleSet &RuleSet = m_lRuleSets.emplace();
		str_copy(RuleSet.m_aName, pName, sizeof(RuleSet.m_aName));

		const json_value &rBaseTile = rBody["basetile"];
		RuleSet.m_BaseTile = rBaseTile.type == json_integer ? clamp((int) rBaseTile.u.integer, 0, 255) : 1;

		ParseRules(rBody["rules"], RuleSet);
		dbg_msg("automap", "loaded rule set '%s' (%d rule(s), basetile %d)", RuleSet.m_aName, RuleSet.m_aRules.size(), RuleSet.m_BaseTile);
	}
	return true;
}

int CAutoMapper::ListFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CAutoMapper *pSelf = static_cast<CAutoMapper *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "automap/%s", pFilename);
	pSelf->LoadFile(pSelf->m_pStorage, aPath);
	return 0;
}

int CAutoMapper::LoadAll(IStorage *pStorage, IConsole *pConsole)
{
	m_pStorage = pStorage;
	m_pConsole = pConsole;

	const int Before = m_lRuleSets.size();
	pStorage->ListDirectory(IStorage::TYPE_ALL, "automap", ListFilesCallback, this);
	return m_lRuleSets.size() - Before;
}

// Deterministic 1-in-N roll. The editor uses random_float(); the generator must
// be reproducible, so it derives the roll from the seed and the position.
static bool RollRandom(unsigned Seed, int X, int Y, int Chance)
{
	if(Chance <= 1)
		return true;

	unsigned h = Seed * 0x9E3779B1u;
	h ^= (unsigned) X * 0x85EBCA6Bu;
	h ^= (unsigned) Y * 0xC2B2AE35u;
	h ^= h >> 15;
	h *= 0x2545F491u;
	h ^= h >> 13;

	return (int) (h % (unsigned) Chance) == 0;
}

// Resolves the x a rule condition actually reads.
//
// Interior tiles wrap, so a rule on the left edge sees the terrain on the right
// edge; that keeps the cylindrical seam free of a wall. The outermost Margin
// columns are held out of the wrap and clamp to the map instead, so the border
// does not get an outer corner tile stamped onto an otherwise straight edge.
static int ResolveSampleX(int CenterX, int X, int Width, int Margin)
{
	if(Margin > 0 && (CenterX < Margin || CenterX >= Width - Margin))
		return clamp(X, 0, Width - 1);

	return ((X % Width) + Width) % Width;
}

// rewrites an index through the caller's remap, if it gave one
static int RemapIndex(int Index, const array<CAutoMapper::CRemap> *pRemap)
{
	if(!pRemap)
		return Index;
	for(int i = 0; i < pRemap->size(); i++)
		if((*pRemap)[i].m_From == Index)
			return (*pRemap)[i].m_To;
	return Index;
}

bool CAutoMapper::Proceed(const char *pRuleSetName, CTile *pTiles, int Width, int Height, unsigned Seed,
	bool WrapX, int MarginX, const array<CRemap> *pRemap) const
{
	const CRuleSet *pConf = FindRuleSet(pRuleSetName);
	if(!pConf)
		return false;
	if(!pConf->m_aRules.size())
		return true;

	const int MaxIndex = Width * Height;
	const int BaseTile = pConf->m_BaseTile;

	// snapshot the source: rules are all evaluated against the state before the
	// run, not against each other's output
	CTile *pSource = new CTile[MaxIndex];
	mem_copy(pSource, pTiles, sizeof(CTile) * MaxIndex);

	for(int y = 0; y < Height; y++)
	{
		for(int x = 0; x < Width; x++)
		{
			const int Index = y * Width + x;
			CTile *pTile = &pTiles[Index];

			// empty tiles are left alone; the mapper only decorates rock
			if(pSource[Index].m_Index == 0)
				continue;

			pTile->m_Index = RemapIndex(BaseTile, pRemap);
			pTile->m_Flags = 0;

			for(int i = 0; i < pConf->m_aRules.size(); i++)
			{
				const CRule &Rule = pConf->m_aRules[i];
				bool RespectRules = true;

				for(int j = 0; j < Rule.m_aConditions.size() && RespectRules; j++)
				{
					const CRuleCondition &Condition = Rule.m_aConditions[j];

					// the original clamps to the layer bounds, so a condition
					// outside the map reads the border tile; a cylindrical world
					// wraps the horizontal axis instead
					const int CheckY = clamp(y + Condition.m_Y, 0, Height - 1);
					const int CheckX = WrapX
						? ResolveSampleX(x, x + Condition.m_X, Width, MarginX)
						: clamp(x + Condition.m_X, 0, Width - 1);
					const int CheckIndex = CheckY * Width + CheckX;

					if(CheckIndex < 0 || CheckIndex >= MaxIndex)
					{
						RespectRules = false;
						continue;
					}

					if(Condition.m_Value == CRuleCondition::EMPTY || Condition.m_Value == CRuleCondition::FULL)
					{
						if(pSource[CheckIndex].m_Index > 0 && Condition.m_Value == CRuleCondition::EMPTY)
							RespectRules = false;
						if(pSource[CheckIndex].m_Index == 0 && Condition.m_Value == CRuleCondition::FULL)
							RespectRules = false;
					}
					else if(pSource[CheckIndex].m_Index != Condition.m_Value)
						RespectRules = false;
				}

				if(!RespectRules || !RollRandom(Seed + (unsigned) i * 7919u, x, y, Rule.m_Random))
					continue;

				pTile->m_Index = RemapIndex(Rule.m_Index, pRemap);
				pTile->m_Flags = 0;

				// rotate
				if(Rule.m_Rotation == 90)
					pTile->m_Flags ^= TILEFLAG_ROTATE;
				else if(Rule.m_Rotation == 180)
					pTile->m_Flags ^= (TILEFLAG_HFLIP | TILEFLAG_VFLIP);
				else if(Rule.m_Rotation == 270)
					pTile->m_Flags ^= (TILEFLAG_HFLIP | TILEFLAG_VFLIP | TILEFLAG_ROTATE);

				// flip; the original swaps the axis when the tile is rotated
				if(Rule.m_HFlip)
					pTile->m_Flags ^= pTile->m_Flags & TILEFLAG_ROTATE ? TILEFLAG_HFLIP : TILEFLAG_VFLIP;
				if(Rule.m_VFlip)
					pTile->m_Flags ^= pTile->m_Flags & TILEFLAG_ROTATE ? TILEFLAG_VFLIP : TILEFLAG_HFLIP;
			}
		}
	}

	delete[] pSource;
	return true;
}
