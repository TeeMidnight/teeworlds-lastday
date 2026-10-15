/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#include <engine/shared/config.h>

#include <game/server/entities/botentity.h>
#include <game/server/entities/character.h>
#include <game/server/entities/npcentity.h>
#include <game/server/npcdata.h>

#include "botmanager.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gameworld.h"
#include "player.h"

#include <algorithm>
#include <vector>

CConfig *CBotManager::Config() const { return GameServer()->Config(); }
CGameContext *CBotManager::GameServer() const { return GameWorld()->GameServer(); }
CGameController *CBotManager::GameController() const { return GameServer()->m_pController; }
IServer *CBotManager::Server() const { return GameServer()->Server(); }

// The kind every manager spawns unless a different creator was registered.
static IBotEntity *DefaultBotCreator(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team, void *pUser)
{
	(void) pUser;
	return new CBotEntity(pWorld, Pos, BotID, TeeInfo, Team);
}

// The peaceful variant: a wandering NPC that never fights.
static IBotEntity *NpcBotCreator(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team, void *pUser)
{
	(void) pUser;
	return new CNpcEntity(pWorld, Pos, BotID, TeeInfo, Team);
}

CBotManager::CBotManager(CGameWorld *pGameWorld)
{
	m_pGameWorld = pGameWorld;
	m_lBotOrder.hint_size(MAX_BOTS_07);

	m_pfnBotCreator = DefaultBotCreator;
	m_pBotCreatorUser = nullptr;
	str_copy(m_aBotType, "default", sizeof(m_aBotType));
	m_Role = BOTROLE_HOSTILE;

	ClearPlayerMap(-1);

	dbg_msg("bot", "manager ready: type='%s' role=%s wanted=%d (0.7 tee ids %d..%d, 0.8 tee ids %d..%d)",
		m_aBotType, m_Role == BOTROLE_NPC ? "npc" : "hostile",
		DesiredBotCount(), BOT_TEEID_07_MIN, BOT_TEEID_07_MAX, BOT_TEEID_08_MIN, BOT_TEEID_08_MAX);
}

CBotManager::~CBotManager()
{
	ClearBots();
}

IBotEntity *CBotManager::BotByID(Uuid BotID) const
{
	IBotEntity *const *ppBot = m_Bots.get(BotID);
	return ppBot ? *ppBot : nullptr;
}

void CBotManager::SetBotTypeCreator(const char *pTypeName, FBotCreator pfnCreator, void *pUser)
{
	m_pfnBotCreator = pfnCreator ? pfnCreator : DefaultBotCreator;
	m_pBotCreatorUser = pUser;
	str_copy(m_aBotType, pTypeName ? pTypeName : "default", sizeof(m_aBotType));
}

void CBotManager::SetBotRole(EBotRole Role)
{
	m_Role = Role;
	// the role owns the default kind: a peaceful world spawns NPCs, a contested
	// one spawns the hostile default fighter
	if(Role == BOTROLE_NPC)
		SetBotTypeCreator("npc", NpcBotCreator);
	else
		SetBotTypeCreator("default", DefaultBotCreator);
}

void CBotManager::ClearPlayerMap(int ClientID)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
			ClearPlayerMap(i);
		return;
	}

	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;

	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
		m_aaBotIDMaps[ClientID][i] = UUID_ZEROED;
}

void CBotManager::ClearBots()
{
	for(int i = 0; i < m_lBotOrder.size(); i++)
	{
		IBotEntity *pBot = BotByID(m_lBotOrder[i]);
		// forget the bot before deleting it: its destructor may run through
		// game world code that looks the bot up again
		if(pBot && pBot->TeeID() != -1)
			m_BotsByTeeID.remove(pBot->TeeID());
		m_Bots.remove(m_lBotOrder[i]);
		delete pBot;
	}
	m_lBotOrder.clear();
	m_lDefinedNpcs.clear();
	ClearPlayerMap(-1);
}

bool DistanceCompare(const std::pair<float, Uuid> &a, const std::pair<float, Uuid> &b)
{
	return a.first < b.first;
}

int CBotManager::FindClientID(int SnappingClient, Uuid BotID) const
{
	// demos are built with SnappingClient == -1; a bot then keeps a stable,
	// protocol independent id so the recording stays self consistent
	if(SnappingClient == -1)
	{
		int Index = 0;
		for(int i = 0; i < m_lBotOrder.size(); i++)
		{
			if(m_lBotOrder[i] == BotID)
				return BotTeeIdForVersion(0x0800, Index);
			Index++;
		}
		return -1;
	}

	if(SnappingClient < 0 || SnappingClient >= MAX_CLIENTS || !GameServer()->m_apPlayers[SnappingClient])
		return -1;

	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(m_aaBotIDMaps[SnappingClient][i] == BotID)
			return BotTeeIdForVersion(Server()->GetClientVersion(SnappingClient), i);
	}

	return -1;
}

int CBotManager::TranslateTeeIDForClient(int SnappingClient, int TeeID) const
{
	// -1 (no hook), -2 (world) and friends are not tees
	if(TeeID < 0)
		return TeeID;

	// A real client's slot is its TeeID on the wire too, and the bot ranges
	// start above every real client id, so anything below the first bot id is
	// already in the client's own space.
	if(TeeID < BOT_TEEID_07_MIN)
		return TeeID;

	// The bot holding this internal id. It keeps that id for life, so the id
	// alone identifies it, whatever client we are snapping for.
	IBotEntity *pBot = 0;
	IBotEntity *const *ppBot = m_BotsByTeeID.get(TeeID);
	if(ppBot)
		pBot = *ppBot;

	// A vanished bot, or a stray id in the bot range with nothing behind it,
	// cannot be named to the client.
	if(!pBot)
		return -1;

	return FindClientID(SnappingClient, pBot->GetBotID());
}

IBotEntity *CBotManager::BotForSlot(int ClientID, int Slot) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Slot < 0 || Slot >= BOT_MAX_PER_CLIENT)
		return nullptr;

	const Uuid BotID = m_aaBotIDMaps[ClientID][Slot];
	if(BotID == UUID_ZEROED)
		return nullptr;

	return BotByID(BotID);
}

int CBotManager::NumVisibleBots(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;

	int Count = 0;
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
		if(m_aaBotIDMaps[ClientID][i] != UUID_ZEROED)
			Count++;
	return Count;
}

void CBotManager::ReleaseBot(Uuid BotID)
{
	IBotEntity *pBot = BotByID(BotID);
	if(!pBot)
		return;

	if(pBot->TeeID() != -1)
		m_BotsByTeeID.remove(pBot->TeeID());
	m_Bots.remove(BotID);
	m_lBotOrder.remove(BotID);

	// the bot is not in this world anymore; forget every client slot pointing
	// at it so a recycled tee id is not resolved to it
	for(int c = 0; c < MAX_CLIENTS; c++)
		for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
			if(m_aaBotIDMaps[c][i] == BotID)
				m_aaBotIDMaps[c][i] = UUID_ZEROED;
}

void CBotManager::AdoptBot(IBotEntity *pBot)
{
	if(!pBot)
		return;
	const Uuid BotID = pBot->GetBotID();
	if(BotID == UUID_ZEROED || BotByID(BotID))
		return;

	m_Bots.set(BotID, pBot);
	m_lBotOrder.add(BotID);
	if(pBot->TeeID() != -1)
		m_BotsByTeeID.set(pBot->TeeID(), pBot);
}

void CBotManager::SendBotEmoticon(Uuid BotID, int Emoticon)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!GameServer()->m_apPlayers[i])
			continue;
		const int TeeID = FindClientID(i, BotID);
		if(TeeID == -1)
			continue;

		CNetMsg_Sv_Emoticon Msg;
		Msg.m_ClientID = TeeID;
		Msg.m_Emoticon = Emoticon;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}
}

bool CBotManager::ClientIsLegacy(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	// Anything below the 0.8 version is a 0.7 peer. Same split as
	// BotTeeIdForVersion() uses to pick a bot's tee id range.
	return Server()->GetClientVersion(ClientID) < 0x0800;
}

void CBotManager::SendBotInfo(int ClientID, int TeeID, IBotEntity *pBot)
{
	(void) pBot;

	// A 0.8 client reads a bot's identity straight from the TeeInfo snapshot
	// object the bot emits. 0.8 also removed the "silent" flag, so
	// Sv_ClientEnter would announce the bot as a joining player: send nothing.
	if(!ClientIsLegacy(ClientID))
		return;

	// A 0.7 client has no TeeInfo and only learns of a tee from Sv_ClientEnter.
	// The translator caches the TeeInfo from the snapshot and expands this back
	// into a 0.7 Sv_ClientInfo with silent set, so no join notice appears.
	CNetMsg_Sv_ClientEnter Msg;
	Msg.m_ClientID = TeeID;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
}

void CBotManager::SendBotDrop(int ClientID, int TeeID)
{
	// Mirror of SendBotInfo: a 0.8 client retires the bot once its TeeInfo stops
	// being snapped, and has no silent flag to quiet the leave notice.
	if(!ClientIsLegacy(ClientID))
		return;

	// A 0.7 client would otherwise keep a stale tee forever.
	CNetMsg_Sv_ClientDrop Msg;
	Msg.m_ClientID = TeeID;
	Msg.m_pReason = "";
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
}

void CBotManager::UpdatePlayerMap(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !GameServer()->m_apPlayers[ClientID])
		return;

	Uuid aLastMap[BOT_MAX_PER_CLIENT];
	mem_copy(aLastMap, m_aaBotIDMaps[ClientID], sizeof(m_aaBotIDMaps[ClientID]));

	Uuid *pMap = m_aaBotIDMaps[ClientID];

	// drop entries whose bot is gone (died or was removed)
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(pMap[i] == UUID_ZEROED)
			continue;
		IBotEntity *pBot = BotByID(pMap[i]);
		if(!pBot || !pBot->IsAlive())
			pMap[i] = UUID_ZEROED;
	}

	// rank every live bot by distance to this client's view position
	std::vector<std::pair<float, Uuid>> Distances;
	Distances.reserve(m_lBotOrder.size());
	for(int i = 0; i < m_lBotOrder.size(); i++)
	{
		IBotEntity *pBot = BotByID(m_lBotOrder[i]);
		if(!pBot || !pBot->IsAlive())
			continue;

		Distances.emplace_back(distance(GameServer()->m_apPlayers[ClientID]->m_ViewPos, pBot->GetPos()), m_lBotOrder[i]);
	}
	std::sort(Distances.begin(), Distances.end(), DistanceCompare);
	Distances.resize(minimum((int) Distances.size(), (int) BOT_MAX_PER_CLIENT));

	// keep the slots that are still among the nearest bots; free the rest
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(pMap[i] == UUID_ZEROED)
			continue;

		bool Found = false;
		for(int j = 0; j < (int) Distances.size(); j++)
		{
			if(Distances[j].second == pMap[i])
			{
				Found = true;
				Distances.erase(Distances.begin() + j);
				break;
			}
		}

		if(!Found)
			pMap[i] = UUID_ZEROED;
	}

	// fill the free slots with the nearest remaining bots, nearest first
	int Index = 0;
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(Index >= (int) Distances.size())
			break;

		if(pMap[i] == UUID_ZEROED)
			pMap[i] = Distances[Index++].second;
	}

	// tell the client about every slot that changed. The identity itself is
	// carried by TeeInfo, which the bot snaps, so this only keeps the engine's
	// per-tee string table in sync for names and clans.
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(aLastMap[i] == pMap[i])
			continue;

		const int TeeID = BotTeeIdForVersion(Server()->GetClientVersion(ClientID), i);
		if(TeeID == -1)
			continue;

		if(pMap[i] != UUID_ZEROED)
		{
			IBotEntity *pBot = BotByID(pMap[i]);
			if(pBot)
				SendBotInfo(ClientID, TeeID, pBot);
		}
		else
			SendBotDrop(ClientID, TeeID);
	}
}

IBotEntity *CBotManager::CreateBot()
{
	return CreateBot(random_uuid(), vec2(0.0f, 0.0f));
}

IBotEntity *CBotManager::CreateBot(Uuid BotID, vec2 Pos)
{
	// never reuse a BotID: it is the identity, and a client may still hold it
	// in a snapshot delta from a previous life
	if(BotID == UUID_ZEROED || BotByID(BotID))
		return nullptr;

	vec2 SpawnPos = Pos;
	if(SpawnPos == vec2(0.0f, 0.0f))
	{
		if(!GameController()->CanSpawn(GameWorld(), TEAM_BLUE, &SpawnPos))
			return nullptr;
	}

	const CBotTeeInfo TeeInfo = BotMakeRandomTeeInfo();
	IBotEntity *pBot = m_pfnBotCreator(GameWorld(), SpawnPos, BotID, TeeInfo, TEAM_BLUE, m_pBotCreatorUser);
	if(!pBot)
		return nullptr;

	m_Bots.set(BotID, pBot);
	m_lBotOrder.add(BotID);
	if(pBot->TeeID() != -1)
		m_BotsByTeeID.set(pBot->TeeID(), pBot);
	return pBot;
}

IBotEntity *CBotManager::SpawnNpc(const SNpcDef *pDef)
{
	if(!pDef)
		return nullptr;
	// the definition carries a fixed identity, so a second copy of the same npc
	// (or the same world reloaded) never spawns twice
	if(BotByID(pDef->m_BotId))
		return nullptr;

	IBotEntity *pBot = new CNpcEntity(m_pGameWorld, pDef, TEAM_BLUE);
	m_Bots.set(pDef->m_BotId, pBot);
	m_lBotOrder.add(pDef->m_BotId);
	if(pBot->TeeID() != -1)
		m_BotsByTeeID.set(pBot->TeeID(), pBot);

	bool Known = false;
	for(int i = 0; i < m_lDefinedNpcs.size() && !Known; i++)
		Known = m_lDefinedNpcs[i] == pDef;
	if(!Known)
		m_lDefinedNpcs.add(pDef);
	return pBot;
}

int CBotManager::DesiredBotCount() const
{
	// sv_bots capped by what one client can be shown: it resolves at most
	// BOT_MAX_PER_CLIENT bots, so any extra would exist but stay invisible.
	int Count = Config()->m_SvBots;
	if(Count < 0)
		Count = 0;
	return minimum(Count, (int) BOT_MAX_PER_CLIENT);
}

void CBotManager::Tick()
{
	// a world that owns configured NPCs respawns those instead of rolling
	// random bots, so the population is exactly the definitions
	if(m_lDefinedNpcs.size())
		return;

	// respawn up to the desired population
	int Guard = 0;
	while(m_lBotOrder.size() < DesiredBotCount() && Guard++ < MAX_BOTS_07)
	{
		if(!CreateBot())
			break;
	}
}

void CBotManager::PostSnap()
{
	// A bot manager belongs to one world, so only a client currently inside it
	// needs a refresh; a client in another world gets one when it enters (see
	// OnClientRefresh). Updating every client for every world was the bulk of
	// the per-snapshot cost.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(pPlayer && pPlayer->GameWorld() == GameWorld() && Server()->ClientIngame(i))
			UpdatePlayerMap(i);
	}
}

void CBotManager::CreateDamage(vec2 Pos, Uuid BotID, vec2 Source, int HealthAmount, int ArmorAmount, bool Self)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!GameServer()->m_apPlayers[i])
			continue;

		const int ClientID = FindClientID(i, BotID);
		if(ClientID == -1)
			continue;

		GameWorld()->CreateDamage(Pos, ClientID, Source, HealthAmount, ArmorAmount, Self);
	}
}

void CBotManager::CreateDeath(vec2 Pos, Uuid BotID)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!GameServer()->m_apPlayers[i])
			continue;

		const int ClientID = FindClientID(i, BotID);
		if(ClientID == -1)
			continue;

		GameWorld()->CreateDeath(Pos, ClientID);
	}
}

void CBotManager::OnBotDeath(Uuid BotID)
{
	// Unlink right away. The world deletes the entity in RemoveEntities, long
	// before PostSnap, so keeping the pointer would leave a dangling entry that
	// any lookup in between (snap, damage events) would follow.
	IBotEntity *pBot = BotByID(BotID);
	if(pBot && pBot->TeeID() != -1)
		m_BotsByTeeID.remove(pBot->TeeID());
	m_Bots.remove(BotID);
	// remove, not remove_fast: the order is what makes the per-client slot
	// assignment (and the demo tee ids) deterministic
	m_lBotOrder.remove(BotID);
}

void CBotManager::OnClientRefresh(int ClientID)
{
	if(ClientID == -1)
		return;

	ClearPlayerMap(ClientID);
}
