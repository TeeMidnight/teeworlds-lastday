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

CConfig *CBotManager::Config() const { return m_pGameServer->Config(); }
CGameController *CBotManager::GameController() const { return m_pGameServer->m_pController; }
IServer *CBotManager::Server() const { return m_pGameServer->Server(); }

// The kind every world spawns unless it is a peaceful NPC world.
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

CBotManager::CBotManager(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_lBotOrder.hint_size(MAX_BOTS_07);

	ClearPlayerMap(-1);

	dbg_msg("bot", "global manager ready, wanted=%d per world (0.7 tee ids %d..%d, 0.8 tee ids %d..%d)",
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

void CBotManager::RemoveFromPlayerMaps(Uuid BotID)
{
	for(int c = 0; c < MAX_CLIENTS; c++)
		for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
			if(m_aaBotIDMaps[c][i] == BotID)
				m_aaBotIDMaps[c][i] = UUID_ZEROED;
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
	ClearPlayerMap(-1);
}

void CBotManager::ClearBots(CGameWorld *pWorld)
{
	if(!pWorld)
		return;

	for(int i = 0; i < m_lBotOrder.size();)
	{
		IBotEntity *pBot = BotByID(m_lBotOrder[i]);
		if(!pBot || pBot->GameWorld() != pWorld)
		{
			i++;
			continue;
		}

		const Uuid BotID = m_lBotOrder[i];
		if(pBot->TeeID() != -1)
			m_BotsByTeeID.remove(pBot->TeeID());
		m_Bots.remove(BotID);
		RemoveFromPlayerMaps(BotID);
		delete pBot;
		m_lBotOrder.remove_index(i);
	}
}

bool DistanceCompare(const std::pair<float, Uuid> &a, const std::pair<float, Uuid> &b)
{
	return a.first < b.first;
}

int CBotManager::PickTeeID(Uuid BotID) const
{
	// Hash the BotID into the seed so a bot keeps its slot across a respawn and
	// bots spread across the range instead of filling it from the bottom.
	unsigned Seed = 0;
	for(int i = 0; i < (int) sizeof(BotID.m_aData); i++)
		Seed = Seed * 31u + BotID.m_aData[i];

	const int Range = BOT_TEEID_08_MAX - BOT_TEEID_07_MIN + 1;
	const int Start = (int) (Seed % (unsigned) Range);
	for(int i = 0; i < Range; i++)
	{
		const int Id = BOT_TEEID_07_MIN + (Start + i) % Range;
		// ids are global, so the table of live bots is the whole allocation map
		if(!m_BotsByTeeID.get(Id))
			return Id;
	}
	return -1;
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

	if(SnappingClient < 0 || SnappingClient >= MAX_CLIENTS || !m_pGameServer->m_apPlayers[SnappingClient])
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

	// The bot holding this internal id. PickTeeID keeps the id globally unique
	// for the bot's whole life, so the id alone identifies it, whatever world
	// it currently lives in and whatever client we are snapping for.
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
	RemoveFromPlayerMaps(BotID);
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
		if(!m_pGameServer->m_apPlayers[i])
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
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_pGameServer->m_apPlayers[ClientID])
		return;

	CPlayer *pPlayer = m_pGameServer->m_apPlayers[ClientID];
	CGameWorld *pView = pPlayer->GameWorld();

	// The worlds whose bots this client can be shown: its own, plus the live
	// ring neighbours the snapshot mirrors. Collecting their offsets once keeps
	// the distance ranking in the client's own frame.
	CGameWorld *apWorlds[3];
	vec2 aOffsets[3];
	int NumWorlds = 0;
	if(pView)
	{
		apWorlds[NumWorlds] = pView;
		aOffsets[NumWorlds] = vec2(0.0f, 0.0f);
		NumWorlds++;
		CGameWorld *apRing[2];
		vec2 aRingOffsets[2];
		const int Neighbours = pView->RingNeighbours(apRing, aRingOffsets, 2);
		for(int i = 0; i < Neighbours && NumWorlds < 3; i++)
		{
			apWorlds[NumWorlds] = apRing[i];
			aOffsets[NumWorlds] = aRingOffsets[i];
			NumWorlds++;
		}
	}

	Uuid aLastMap[BOT_MAX_PER_CLIENT];
	mem_copy(aLastMap, m_aaBotIDMaps[ClientID], sizeof(m_aaBotIDMaps[ClientID]));

	Uuid *pMap = m_aaBotIDMaps[ClientID];

	// drop entries whose bot is gone, dead or no longer in one of the worlds
	// this client is being shown
	for(int i = 0; i < BOT_MAX_PER_CLIENT; i++)
	{
		if(pMap[i] == UUID_ZEROED)
			continue;
		IBotEntity *pBot = BotByID(pMap[i]);
		bool Visible = pBot && pBot->IsAlive();
		if(Visible)
		{
			Visible = false;
			for(int w = 0; w < NumWorlds; w++)
				if(pBot->GameWorld() == apWorlds[w])
				{
					Visible = true;
					break;
				}
		}
		if(!Visible)
			pMap[i] = UUID_ZEROED;
	}

	// rank every live visible bot by distance to this client's view position
	std::vector<std::pair<float, Uuid>> Distances;
	Distances.reserve(m_lBotOrder.size());
	for(int i = 0; i < m_lBotOrder.size(); i++)
	{
		IBotEntity *pBot = BotByID(m_lBotOrder[i]);
		if(!pBot || !pBot->IsAlive())
			continue;

		int World = -1;
		for(int w = 0; w < NumWorlds; w++)
			if(pBot->GameWorld() == apWorlds[w])
			{
				World = w;
				break;
			}
		if(World < 0)
			continue;

		Distances.emplace_back(distance(pPlayer->m_ViewPos, pBot->GetPos() + aOffsets[World]), m_lBotOrder[i]);
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

IBotEntity *CBotManager::CreateBot(CGameWorld *pWorld)
{
	return CreateBot(pWorld, random_uuid(), vec2(0.0f, 0.0f));
}

IBotEntity *CBotManager::CreateBot(CGameWorld *pWorld, Uuid BotID, vec2 Pos)
{
	if(!pWorld)
		return nullptr;
	// never reuse a BotID: it is the identity, and a client may still hold it
	// in a snapshot delta from a previous life
	if(BotID == UUID_ZEROED || BotByID(BotID))
		return nullptr;

	vec2 SpawnPos = Pos;
	if(SpawnPos == vec2(0.0f, 0.0f))
	{
		if(!GameController()->CanSpawn(pWorld, TEAM_BLUE, &SpawnPos))
			return nullptr;
	}

	const CBotTeeInfo TeeInfo = BotMakeRandomTeeInfo();
	IBotEntity *pBot = pWorld->IsNpcWorld()
		? NpcBotCreator(pWorld, SpawnPos, BotID, TeeInfo, TEAM_BLUE, nullptr)
		: DefaultBotCreator(pWorld, SpawnPos, BotID, TeeInfo, TEAM_BLUE, nullptr);
	if(!pBot)
		return nullptr;

	m_Bots.set(BotID, pBot);
	m_lBotOrder.add(BotID);
	if(pBot->TeeID() != -1)
		m_BotsByTeeID.set(pBot->TeeID(), pBot);
	return pBot;
}

IBotEntity *CBotManager::SpawnNpc(CGameWorld *pWorld, const SNpcDef *pDef)
{
	if(!pWorld || !pDef)
		return nullptr;
	// the definition carries a fixed identity, so a second copy of the same npc
	// (or the same world reloaded) never spawns twice
	if(BotByID(pDef->m_Id.Value()))
		return nullptr;

	IBotEntity *pBot = new CNpcEntity(pWorld, pDef, TEAM_BLUE);
	m_Bots.set(pDef->m_Id.Value(), pBot);
	m_lBotOrder.add(pDef->m_Id.Value());
	if(pBot->TeeID() != -1)
		m_BotsByTeeID.set(pBot->TeeID(), pBot);

	bool Known = false;
	for(int i = 0; i < pWorld->m_lBotDefinedNpcs.size() && !Known; i++)
		Known = pWorld->m_lBotDefinedNpcs[i] == pDef;
	if(!Known)
		pWorld->m_lBotDefinedNpcs.add(pDef);
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

int CBotManager::LiveBotsInWorld(CGameWorld *pWorld) const
{
	int Count = 0;
	for(int i = 0; i < m_lBotOrder.size(); i++)
	{
		IBotEntity *const *ppBot = m_Bots.get(m_lBotOrder[i]);
		if(ppBot && *ppBot && (*ppBot)->GameWorld() == pWorld)
			Count++;
	}
	return Count;
}

void CBotManager::TickWorld(CGameWorld *pWorld)
{
	if(!pWorld || !pWorld->m_BotConfigured)
		return;

	// a world that owns configured NPCs respawns those instead of rolling
	// random bots, so the population is exactly the definitions
	if(pWorld->m_lBotDefinedNpcs.size())
	{
		for(int i = 0; i < pWorld->m_lBotDefinedNpcs.size(); i++)
		{
			const SNpcDef *pDef = pWorld->m_lBotDefinedNpcs[i];
			if(pDef && !BotByID(pDef->m_Id.Value()))
				SpawnNpc(pWorld, pDef);
		}
		return;
	}

	// respawn up to the desired population
	int Guard = 0;
	while(LiveBotsInWorld(pWorld) < DesiredBotCount() && Guard++ < MAX_BOTS_07)
	{
		if(!CreateBot(pWorld))
			break;
	}
}

void CBotManager::Tick()
{
	struct SBotTick
	{
		CBotManager *m_pSelf;
	};
	SBotTick Data = {this};
	m_pGameServer->m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		SBotTick *pData = static_cast<SBotTick *>(pUser);
		if(pWorld)
			pData->m_pSelf->TickWorld(pWorld);
	}, &Data);
}

void CBotManager::PostSnap()
{
	// One manager covers every world, so refresh each client exactly once no
	// matter how many worlds it can see.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_pGameServer->m_apPlayers[i];
		if(pPlayer && !pPlayer->m_MapLoading && Server()->ClientIngame(i))
			UpdatePlayerMap(i);
	}
}

void CBotManager::CreateDamage(vec2 Pos, Uuid BotID, vec2 Source, int HealthAmount, int ArmorAmount, bool Self)
{
	IBotEntity *pBot = BotByID(BotID);
	if(!pBot)
		return;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_pGameServer->m_apPlayers[i])
			continue;

		const int ClientID = FindClientID(i, BotID);
		if(ClientID == -1)
			continue;

		pBot->GameWorld()->CreateDamage(Pos, ClientID, Source, HealthAmount, ArmorAmount, Self);
	}
}

void CBotManager::CreateDeath(vec2 Pos, Uuid BotID)
{
	IBotEntity *pBot = BotByID(BotID);
	if(!pBot)
		return;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_pGameServer->m_apPlayers[i])
			continue;

		const int ClientID = FindClientID(i, BotID);
		if(ClientID == -1)
			continue;

		pBot->GameWorld()->CreateDeath(Pos, ClientID);
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
	RemoveFromPlayerMaps(BotID);
}

void CBotManager::OnClientRefresh(int ClientID)
{
	if(ClientID == -1)
		return;

	ClearPlayerMap(ClientID);
}
