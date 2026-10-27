/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#ifndef GAME_SERVER_BOT_H
#define GAME_SERVER_BOT_H

#include <base/system.h>
#include <base/uuid.h>
#include <engine/shared/protocol.h>
#include <generated/protocol.h>

// A bot's BotID is a Uuid and never changes, but the tee id a *client* sees
// depends on that client's protocol: 0.7 peers have a 7 bit space, 0.8 peers
// the full 16 bit one. The mapped id is therefore a property of the
// (client, bot) pair, and CBotManager keeps one table per receiving client.
// Bots never own a connection slot or a CPlayer, so MAX_CLIENTS is untouched.

// Bot tee ids available to one client. The 0.7 range is the tighter one, so it
// is the effective ceiling wherever the count has to be protocol independent.
enum
{
	BOT_MAX_PER_CLIENT = MAX_BOTS_07,
	// Natural monster density: one monster per this many tiles of world width.
	// Fixed content, NPCs, bosses and summons are not part of this budget.
	BOT_NATURAL_TILES_PER_MONSTER = 48,
};

// A bot's social stance. The value is independent of the protocol team: two
// hostile monsters can belong to different warbands and fight each other, a
// neutral monster only fights back, and a friendly unit fights for the players.
enum EBotFaction
{
	BOTFACTION_HOSTILE = 0,
	BOTFACTION_NEUTRAL,
	BOTFACTION_FRIENDLY,
};

inline const char *BotFactionName(int Faction)
{
	switch(Faction)
	{
		case BOTFACTION_HOSTILE: return "hostile";
		case BOTFACTION_NEUTRAL: return "neutral";
		case BOTFACTION_FRIENDLY: return "friendly";
	}
	return "unknown";
}

inline int BotFactionFromString(const char *pName, int Default = BOTFACTION_HOSTILE)
{
	if(!pName || !pName[0])
		return Default;
	if(str_comp_nocase(pName, "hostile") == 0 || str_comp_nocase(pName, "enemy") == 0)
		return BOTFACTION_HOSTILE;
	if(str_comp_nocase(pName, "neutral") == 0)
		return BOTFACTION_NEUTRAL;
	if(str_comp_nocase(pName, "friendly") == 0 || str_comp_nocase(pName, "ally") == 0)
		return BOTFACTION_FRIENDLY;
	return Default;
}

// Same layout as player.h's CTeeInfos, repeated so that spawning a bot does not
// drag the whole player header (and IServer / CGameWorld) into every call site.
struct CBotSkin
{
	char m_aaSkinPartNames[NUM_SKINPARTS][MAX_SKIN_ARRAY_SIZE];
	int m_aUseCustomColors[NUM_SKINPARTS];
	int m_aSkinPartColors[NUM_SKINPARTS];
};

// Resolves a bot tee id in the range that fits the given client version. Slot
// is a 0-based index into that range, so callers stay protocol agnostic.
inline int BotTeeIdForVersion(int ClientVersion, int Slot)
{
	// anything below 0x0800 is a legacy peer with only the 7 bit tee id space
	if(ClientVersion < 0x0800)
	{
		if(Slot < 0 || Slot >= MAX_BOTS_07)
			return -1;
		return BOT_TEEID_07_MIN + Slot;
	}

	if(Slot < 0 || Slot >= MAX_BOTS_08)
		return -1;
	return BOT_TEEID_08_MIN + Slot;
}

// Whether the id lies in either bot range, i.e. above every real client id.
inline bool IsBotTeeId(int TeeId)
{
	return TeeId >= BOT_TEEID_07_MIN && TeeId <= BOT_TEEID_08_MAX;
}

/*
	Function: PickBotTeeID
		Chooses a bot's private CWorldCore tee id from its stable BotID.

	The BotID hash is the seed, so a respawn lands on the same slot, and the
	probe walks the whole range so the result is -1 only when every id is taken.
	Used(TeeID) decides which ids are taken; the global CBotManager checks every
	live bot of every world, which is what makes the id unique across the ring.

	Returns:
		A free id in [BOT_TEEID_07_MIN, BOT_TEEID_08_MAX], or -1.
*/
template<class TUsed>
inline int PickBotTeeID(Uuid BotID, TUsed Used)
{
	const int Range = BOT_TEEID_08_MAX - BOT_TEEID_07_MIN + 1;

	unsigned Seed = 0;
	for(int i = 0; i < (int) sizeof(BotID.m_aData); i++)
		Seed = Seed * 31u + BotID.m_aData[i];

	const int Start = (int) (Seed % (unsigned) Range);
	for(int i = 0; i < Range; i++)
	{
		const int Id = BOT_TEEID_07_MIN + (Start + i) % Range;
		if(!Used(Id))
			return Id;
	}
	return -1;
}

// The identity a bot presents to clients. Mirrors what CPlayer::Snap writes
// into TeeInfo, so a bot renders exactly like a real tee.
struct CBotTeeInfo
{
	char m_aName[MAX_NAME_ARRAY_SIZE];
	char m_aClan[MAX_CLAN_ARRAY_SIZE];
	CBotSkin m_Skin;
};

// Builds a tee info with a random standard skin, so bots never look blank.
CBotTeeInfo BotMakeRandomTeeInfo(const char *pName = "");

#endif // GAME_SERVER_BOT_H
