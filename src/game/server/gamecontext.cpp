/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <engine/engine.h>
#include <engine/map.h>
#include <engine/server/mapgen.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/memheap.h>
#include <engine/storage.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/localization.h>
#include <game/localization_format.h>
#include <game/server/database/database.h>
#include <game/server/database/database_util.h>
#include <game/version.h>
#include <generated/server_data.h>

#include "botmanager.h"
#include "container.h"
#include "entities/botentity.h"
#include "entities/character.h"
#include "entities/container.h"
#include "entities/drill.h"
#include "entities/dropped_pickup.h"
#include "entities/laser.h"
#include "entities/projectile.h"
#include "entities/resource.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "item.h"
#include "mobdata.h"
#include "npcdata.h"
#include "player.h"
#include "scripting/buff_system.h"
#include "scripting/script_commands.h"
#include "scripting/script_runtime.h"
#include "scripting/scripted_entity.h"
#include "team.h"
#include "trade.h"
#include "weapon.h"
#include "weaponmanager.h"

enum
{
	RESET,
	NO_RESET
};

// how far from the pointed-at position a resource still counts as hovered;
// the node's own proximity radius is added on top of this
constexpr float CursorResourceRadius = 20.0f;

// a container is a bigger prop, so its cursor radius is a little more generous
constexpr float CursorContainerRadius = 28.0f;

// how far a player may stand from the container they open
constexpr float ContainerReach = 4.0f * 32.0f;

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_apPlayers[i] = 0;

	m_pController = 0;
	m_pBotManager = 0;
	m_pDatabase = 0;
	m_pItemSystem = 0;
	m_pContainerSystem = 0;
	m_pTradeSystem = 0;
	m_pNpcSystem = 0;
	m_pMobSystem = 0;
	m_pTeamSystem = 0;
	m_pBuffSystem = 0;
	m_pScriptedEntities = 0;
	m_MainWorldID = 0;
	m_LockTeams = 0;
	m_TipTimer = 0;
	m_LastTip = -1;

	for(int i = 0; i < MAX_CLIENTS; i++)
		ResetBroadcastState(i);

	if(Resetting == NO_RESET)
	{
		m_pGameMenu = new CGameMenu(this);

		OnGameMenuInit();
	}
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		delete m_apPlayers[i];
	if(!m_Resetting)
	{
		delete m_pGameMenu;
		m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
		{
			delete pWorld;
		}, this);
		m_pWorlds.clear();
		// the manager owns the worlds' bots, so it must outlive them
		delete m_pBotManager;
		m_pBotManager = 0;
	}
}

void CGameContext::Clear()
{
	CTuningParams Tuning = m_Tuning;
	CGameMenu *pGameMenu = m_pGameMenu;
	CDatabase *pDatabase = m_pDatabase;
	CItemSystem *pItemSystem = m_pItemSystem;
	CContainerSystem *pContainerSystem = m_pContainerSystem;
	CTradeSystem *pTradeSystem = m_pTradeSystem;
	CNpcSystem *pNpcSystem = m_pNpcSystem;
	CMobSystem *pMobSystem = m_pMobSystem;
	CTeamSystem *pTeamSystem = m_pTeamSystem;
	CBuffSystem *pBuffSystem = m_pBuffSystem;
	CScriptedEntitySystem *pScriptedEntities = m_pScriptedEntities;
	// the engine services (IServer & co) outlive the game context, so preserve
	// them: the world teardown in OnShutdown and a following OnInit on a forced
	// reload both rely on Server() being valid.
	IServer *pServer = m_pServer;
	CConfig *pConfig = m_pConfig;
	IConsole *pConsole = m_pConsole;
	IStorage *pStorage = m_pStorage;
	// Deep-copy the world table, then release the original storage before the
	// object is zeroed: mem_zero alone would forget its allocations and leak
	// every world. The copy shares the CGameWorld pointers, which is exactly
	// the state that should survive a clear.
	hash_table<unsigned, CGameWorld *, 4> Worlds = m_pWorlds;
	unsigned MainWorldID = m_MainWorldID;
	m_pWorlds.clear();

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new(this) CGameContext(RESET);

	m_Tuning = Tuning;
	m_pGameMenu = pGameMenu;
	m_pDatabase = pDatabase;
	m_pItemSystem = pItemSystem;
	m_pContainerSystem = pContainerSystem;
	m_pTradeSystem = pTradeSystem;
	m_pNpcSystem = pNpcSystem;
	m_pMobSystem = pMobSystem;
	m_pTeamSystem = pTeamSystem;
	m_pBuffSystem = pBuffSystem;
	m_pScriptedEntities = pScriptedEntities;
	m_pServer = pServer;
	m_pConfig = pConfig;
	m_pConsole = pConsole;
	m_pStorage = pStorage;
	m_pWorlds = Worlds;
	m_MainWorldID = MainWorldID;
}

class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;
	return m_apPlayers[ClientID]->GetCharacter();
}

int CGameContext::LanguageOf(int ClientID) const
{
	if(ClientID >= 0 && ClientID < MAX_CLIENTS && m_apPlayers[ClientID])
		return m_apPlayers[ClientID]->Language();
	return g_Localization.DefaultLanguage();
}

// ----- send functions -----
void CGameContext::SendChat(int ChatterClientID, int Mode, int To, const char *pText)
{
	char aBuf[256];
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
	{
		if(Mode == CHAT_TEAM)
		{
			int TeamID = m_apPlayers[ChatterClientID]->GetTeam();
			str_format(aBuf, sizeof(aBuf), "%d:%d:%d:%s: %s", Mode, TeamID, ChatterClientID, Server()->ClientName(ChatterClientID), pText);
		}
		else
			str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", Mode, ChatterClientID, Server()->ClientName(ChatterClientID), pText);
	}
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", pText);

	const char *pModeStr;
	if(Mode == CHAT_WHISPER)
		pModeStr = 0;
	else if(Mode == CHAT_TEAM)
		pModeStr = "teamchat";
	else
		pModeStr = "chat";

	if(pModeStr)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, pModeStr, aBuf);
	}

	CNetMsg_Sv_Chat Msg;
	Msg.m_Mode = Mode;
	Msg.m_ClientID = ChatterClientID;
	Msg.m_pMessage = pText;
	Msg.m_TargetID = -1;

	if(Mode == CHAT_ALL)
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ChatterClientID == -1 ? To : -1);
	else if(Mode == CHAT_TEAM)
	{
		// pack one for the recording only
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

		// team chat always comes from a real player; without one there is no
		// team to deliver to, and the recording already has the message
		if(ChatterClientID < 0 || ChatterClientID >= MAX_CLIENTS || !m_apPlayers[ChatterClientID])
			return;

		To = m_apPlayers[ChatterClientID]->GetTeam();

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() == To)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
		}
	}
	else // Mode == CHAT_WHISPER
	{
		if(ChatterClientID == -1)
			Msg.m_Mode = CHAT_ALL;

		// send to the clients
		Msg.m_TargetID = To;
		if(ChatterClientID != -1)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ChatterClientID);
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::ResetBroadcastState(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	CBroadcastState &State = m_aBroadcastStates[ClientID];
	State.m_NoChangeTick = 0;
	State.m_Priority = BROADCAST_PRIORITY_LOWEST;
	State.m_aPrevMessage[0] = '\0';
	State.m_aNextMessage[0] = '\0';
	State.m_LifeSpanTick = 0;
	State.m_TimedPriority = BROADCAST_PRIORITY_LOWEST;
	State.m_aTimedMessage[0] = '\0';
}

void CGameContext::AddBroadcast(int ClientID, const char *pText, int Priority, int LifeSpan)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	CBroadcastState &State = m_aBroadcastStates[ClientID];
	if(LifeSpan > 0)
	{
		// a timed message only replaces an equal or lower priority one
		if(State.m_TimedPriority > Priority)
			return;
		str_copy(State.m_aTimedMessage, pText, sizeof(State.m_aTimedMessage));
		State.m_LifeSpanTick = LifeSpan;
		State.m_TimedPriority = Priority;
	}
	else
	{
		// a realtime message is submitted every tick; the highest one wins
		if(State.m_Priority > Priority)
			return;
		str_copy(State.m_aNextMessage, pText, sizeof(State.m_aNextMessage));
		State.m_Priority = Priority;
	}
}

void CGameContext::SendBroadcast(int ClientID, const char *pText, int Priority, int LifeSpan)
{
	if(ClientID < -1 || ClientID >= MAX_CLIENTS)
		return;

	const int Start = ClientID < 0 ? 0 : ClientID;
	const int End = ClientID < 0 ? MAX_CLIENTS : ClientID + 1;

	// record a global announcement once; the per-client messages below are
	// transient (and a realtime message repeats) and must not bloat the demo
	if(ClientID < 0)
	{
		CNetMsg_Sv_Broadcast Msg;
		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);
	}

	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
			AddBroadcast(i, pText, Priority, LifeSpan);
	}
}

void CGameContext::ClearBroadcast(int ClientID, int Priority)
{
	SendBroadcast(ClientID, "", Priority, BROADCAST_DURATION_REALTIME);
}

void CGameContext::FlushBroadcasts()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_apPlayers[i])
		{
			ResetBroadcastState(i);
			continue;
		}

		CBroadcastState &State = m_aBroadcastStates[i];

		// a timed announcement wins over the realtime hint of this tick
		if(State.m_LifeSpanTick > 0 && State.m_TimedPriority > State.m_Priority)
			str_copy(State.m_aNextMessage, State.m_aTimedMessage, sizeof(State.m_aNextMessage));

		// Send only on change, or once a second to fight the client's auto fade
		if(str_comp(State.m_aPrevMessage, State.m_aNextMessage) != 0 ||
			State.m_NoChangeTick > Server()->TickSpeed())
		{
			CNetMsg_Sv_Broadcast Msg;
			Msg.m_pMessage = State.m_aNextMessage;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);

			str_copy(State.m_aPrevMessage, State.m_aNextMessage, sizeof(State.m_aPrevMessage));
			State.m_NoChangeTick = 0;
		}
		else
			State.m_NoChangeTick++;

		if(State.m_LifeSpanTick > 0)
			State.m_LifeSpanTick--;
		if(State.m_LifeSpanTick <= 0)
		{
			State.m_aTimedMessage[0] = '\0';
			State.m_TimedPriority = BROADCAST_PRIORITY_LOWEST;
		}
		State.m_aNextMessage[0] = '\0';
		State.m_Priority = BROADCAST_PRIORITY_LOWEST;
	}
}

void CGameContext::LoadTips()
{
	m_lTips.clear();

	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile("tips.json", Storage(), IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("tips", "failed to load tips.json: %s", Parser.Error());
		return;
	}

	const json_value &Tips = (*pJson)["tips"];
	if(Tips.type != json_array)
	{
		dbg_msg("tips", "tips.json has no 'tips' array");
		return;
	}

	for(unsigned i = 0; i < Tips.u.array.length; i++)
	{
		if(Tips[i].type != json_string)
			continue;
		const char *pText = (const char *) Tips[i];
		if(!pText[0])
			continue;
		// a chat line is not a place for an essay; keep the message bounded
		if(str_length(pText) > 512)
		{
			dbg_msg("tips", "tip %u is too long, skipping", i);
			continue;
		}
		m_lTips.add(pText);
	}

	dbg_msg("tips", "loaded %d tip(s) from tips.json", m_lTips.size());
}

void CGameContext::TickTips()
{
	const int Interval = maximum(1, Config()->m_SvTipInterval) * Server()->TickSpeed();
	if(!Config()->m_SvTips || m_lTips.size() == 0)
	{
		m_TipTimer = Interval;
		return;
	}

	// bots keep the server busy but they do not need to be told anything
	bool HasPlayer = false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->IsDummy())
		{
			HasPlayer = true;
			break;
		}
	}
	if(!HasPlayer)
	{
		// start the countdown when somebody joins, not while the server is empty
		m_TipTimer = Interval;
		return;
	}

	if(m_TipTimer > 0)
	{
		m_TipTimer--;
		if(m_TipTimer > 0)
			return;
	}

	int TipIndex = random_int() % m_lTips.size();
	if(m_lTips.size() > 1 && TipIndex == m_LastTip)
		TipIndex = (TipIndex + 1) % m_lTips.size();
	m_LastTip = TipIndex;
	const std::string &Tip = m_lTips[TipIndex];

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_apPlayers[i] || m_apPlayers[i]->IsDummy())
			continue;
		// a tip list can be localized like any other system message
		CLanguageScope LanguageScope(LanguageOf(i));
		SendChat(-1, CHAT_ALL, i, Localize(Tip.c_str(), "Tip"));
	}

	m_TipTimer = Interval;
}

void CGameContext::UpdateCursorResourceBroadcast(int ClientID)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->IsDummy() || pPlayer->m_MapLoading)
		return;
	CCharacter *pChar = pPlayer->GetCharacter();
	if(!pChar)
		return;
	CGameWorld *pWorld = pPlayer->GameWorld();
	if(!pWorld)
		return;

	// the cursor's world position, the same point the drill aims at
	const vec2 CursorPos = pChar->GetPos() + pChar->AimVector();
	CResourceEntity *pRes = pWorld->ClosestResource(CursorPos, CursorResourceRadius);
	CLanguageScope LanguageScope(LanguageOf(ClientID));
	char aBuf[256];
	if(pRes)
	{
		str_format(aBuf, sizeof(aBuf), "%s %d/%d",
			ItemIDDisplayName(pRes->Item()), pRes->RemainingHardness(), pRes->MaxHardness());
		AddBroadcast(ClientID, aBuf, BROADCAST_PRIORITY_RESOURCEINFO, BROADCAST_DURATION_REALTIME);
		return;
	}

	// a container under the cursor names itself and how many item kinds it can
	// hold. This is also where a player learns that the menu can open it.
	CContainerEntity *pContainer = pWorld->ClosestContainer(CursorPos, CursorContainerRadius);
	if(!pContainer)
		return;
	str_format(aBuf, sizeof(aBuf), "%s %d/%d", Localize(pContainer->Name(), "Container Name"),
		pContainer->UsedSlots(), pContainer->Capacity());
	AddBroadcast(ClientID, aBuf, BROADCAST_PRIORITY_RESOURCEINFO, BROADCAST_DURATION_REALTIME);
}

CContainerEntity *CGameContext::AimedContainer(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return nullptr;
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->IsDummy() || pPlayer->m_MapLoading)
		return nullptr;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !pChr->IsAlive())
		return nullptr;
	CGameWorld *pWorld = pPlayer->GameWorld();
	if(!pWorld)
		return nullptr;

	const vec2 CursorPos = pChr->GetPos() + pChr->AimVector();
	CContainerEntity *pContainer = pWorld->ClosestContainer(CursorPos, CursorContainerRadius);
	if(!pContainer)
		return nullptr;
	// the cursor can be far away from the tee; the tee itself has to be able to
	// reach the container it is opening
	if(distance(pContainer->GetPos(), pChr->GetPos()) > ContainerReach)
		return nullptr;
	return pContainer;
}

bool CGameContext::CanUseBuilding(const CBuildAccess &Access, const Uuid &Viewer) const
{
	if(Access.m_Owner == UUID_ZEROED || Access.m_LockMode == BUILD_LOCK_PUBLIC)
		return true;
	if(Viewer != UUID_ZEROED && Viewer == Access.m_Owner)
		return true;
	if(Access.m_LockMode == BUILD_LOCK_TEAM && m_pTeamSystem && Viewer != UUID_ZEROED)
		return m_pTeamSystem->SameTeam(Viewer, Access.m_Owner);
	return false;
}

bool CGameContext::CanUseBuilding(const CBuildAccess &Access, CPlayer *pPlayer) const
{
	return pPlayer && CanUseBuilding(Access, pPlayer->m_AccountUuid);
}

Uuid CGameContext::BuildUserAccount(CEntity *pFrom) const
{
	if(!pFrom)
		return UUID_ZEROED;
	if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
	{
		const int Owner = static_cast<CChildEntity *>(pFrom)->GetOwner();
		if(Owner >= 0 && Owner < MAX_CLIENTS && m_apPlayers[Owner])
			return m_apPlayers[Owner]->m_AccountUuid;
		return UUID_ZEROED;
	}
	if(pFrom->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
	{
		const int ClientID = static_cast<CCharacter *>(pFrom)->GetCID();
		if(ClientID >= 0 && ClientID < MAX_CLIENTS && m_apPlayers[ClientID])
			return m_apPlayers[ClientID]->m_AccountUuid;
	}
	return UUID_ZEROED;
}

bool CGameContext::CanUseBuildingAsSource(const CBuildAccess &Access, CEntity *pFrom) const
{
	// damage without a player source (world, script) is not access-controlled
	if(!pFrom)
		return true;
	return CanUseBuilding(Access, BuildUserAccount(pFrom));
}

CEntity *CGameContext::AimedBuildable(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return nullptr;
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->IsDummy() || pPlayer->m_MapLoading)
		return nullptr;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !pChr->IsAlive())
		return nullptr;
	CGameWorld *pWorld = pPlayer->GameWorld();
	if(!pWorld)
		return nullptr;

	const vec2 CursorPos = pChr->GetPos() + pChr->AimVector();
	CEntity *pBest = nullptr;
	float BestDist = 1e30f;
	const float Reach = 4.0f * 32.0f;

	auto Consider = [&](CEntity *pEntity)
	{
		if(!pEntity || pEntity->IsMarkedForDestroy())
			return;
		if(distance(pEntity->GetPos(), pChr->GetPos()) > Reach)
			return;
		const float CursorDist = distance(pEntity->GetPos(), CursorPos);
		const float Radius = maximum(28.0f, pEntity->GetProximityRadius());
		if(CursorDist > Radius)
			return;
		if(CursorDist < BestDist)
		{
			BestDist = CursorDist;
			pBest = pEntity;
		}
	};

	for(CGameWorld::TypeRange r = pWorld->DoTypeRange(CGameWorld::ENTTYPE_DRILL); !r.empty(); r.pop_front())
		Consider(r.front());
	for(CGameWorld::TypeRange r = pWorld->DoTypeRange(CGameWorld::ENTTYPE_SCRIPTED); !r.empty(); r.pop_front())
	{
		CScriptedEntity *pEntity = static_cast<CScriptedEntity *>(r.front());
		if(pEntity->Persistent())
			Consider(pEntity);
	}
	return pBest;
}

bool CGameContext::BuildableAccess(CEntity *pEntity, CBuildAccess *pAccess) const
{
	if(!pEntity || !pAccess)
		return false;
	if(pEntity->ObjType() == CGameWorld::ENTTYPE_DRILL)
	{
		*pAccess = static_cast<CDrillEntity *>(pEntity)->Access();
		return true;
	}
	if(pEntity->ObjType() == CGameWorld::ENTTYPE_SCRIPTED)
	{
		*pAccess = static_cast<CScriptedEntity *>(pEntity)->Access();
		return true;
	}
	return false;
}

const char *CGameContext::BuildableName(CEntity *pEntity, char *pBuf, int BufSize)
{
	if(!pBuf || BufSize <= 0)
		return "";
	pBuf[0] = '\0';
	if(!pEntity)
		return pBuf;
	if(pEntity->ObjType() == CGameWorld::ENTTYPE_DRILL)
	{
		const CDrillEntity *pDrill = static_cast<const CDrillEntity *>(pEntity);
		const char *pName = Item() ? Item()->GetName(pDrill->ItemId()) : nullptr;
		str_copy(pBuf, pName && pName[0] ? pName : "Drill", BufSize);
	}
	else if(pEntity->ObjType() == CGameWorld::ENTTYPE_SCRIPTED)
	{
		const CScriptedEntity *pScripted = static_cast<const CScriptedEntity *>(pEntity);
		if(str_comp(pScripted->Kind(), "health_jar") == 0)
			str_copy(pBuf, Localize("Health Jar", "Building"), BufSize);
		else if(str_comp(pScripted->Kind(), "campfire") == 0)
			str_copy(pBuf, Localize("Campfire", "Building"), BufSize);
		else
			str_copy(pBuf, pScripted->Kind(), BufSize);
	}
	return pBuf;
}

bool CGameContext::SetAimedBuildLock(int ClientID, int Mode)
{
	if(Mode < BUILD_LOCK_PUBLIC || Mode > BUILD_LOCK_TEAM)
		return false;
	CPlayer *pPlayer = ClientID >= 0 && ClientID < MAX_CLIENTS ? m_apPlayers[ClientID] : nullptr;
	if(!pPlayer)
		return false;
	CEntity *pEntity = AimedBuildable(ClientID);
	if(!pEntity)
	{
		CLanguageScope LanguageScope(LanguageOf(ClientID));
		SendChat(-1, CHAT_ALL, ClientID, Localize("Aim at a building you own first.", "Building"));
		return false;
	}
	CBuildAccess Access;
	if(!BuildableAccess(pEntity, &Access) || Access.m_Owner == UUID_ZEROED || Access.m_Owner != pPlayer->m_AccountUuid)
	{
		CLanguageScope LanguageScope(LanguageOf(ClientID));
		SendChat(-1, CHAT_ALL, ClientID, Localize("Only the owner can change this building's lock.", "Building"));
		return false;
	}

	if(pEntity->ObjType() == CGameWorld::ENTTYPE_DRILL)
		static_cast<CDrillEntity *>(pEntity)->SetLockMode(Mode);
	else if(pEntity->ObjType() == CGameWorld::ENTTYPE_SCRIPTED)
		static_cast<CScriptedEntity *>(pEntity)->SetLockMode(Mode);
	else
		return false;

	CLanguageScope LanguageScope(LanguageOf(ClientID));
	const char *pMode = Mode == BUILD_LOCK_PUBLIC ? Localize("everyone", "Building") :
							(Mode == BUILD_LOCK_TEAM ? Localize("team only", "Building") : Localize("owner only", "Building"));
	SendChat(-1, CHAT_ALL, ClientID, LocalizeFormat("Building", "Building lock: {}.", pMode).c_str());
	return true;
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendMotd(int ClientID)
{
	CLanguageScope LanguageScope(LanguageOf(ClientID));
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = Config()->m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSettings(int ClientID)
{
	CNetMsg_Sv_ServerSettings Msg;
	Msg.m_KickVote = Config()->m_SvVoteKick;
	Msg.m_KickMin = Config()->m_SvVoteKickMin;
	Msg.m_SpecVote = Config()->m_SvVoteSpectate;
	Msg.m_TeamLock = m_LockTeams != 0;
	Msg.m_TeamBalance = 0;
	Msg.m_PlayerSlots = GetMaxPlayerSlots();
	Msg.m_AllowSpecVoting = Config()->m_SvAllowSpecVoting;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSkinChange(int ClientID, int TargetID)
{
	// 0.8: there is no Sv_SkinChange message anymore. Skins are part of the
	// TeeInfo snapshot object, so a skin change is carried by the next snapshot
	// of that client's TeeInfo. Nothing has to be sent explicitly here; the
	// (ClientID, TargetID) signature is kept so callers stay unchanged.
	(void) ClientID;
	(void) TargetID;
}

void CGameContext::SendGameMsg(int GameMsgID, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ParaI2, int ParaI3, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Msg.AddInt(ParaI2);
	Msg.AddInt(ParaI3);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendChatCommand(const CCommandManager::CCommand *pCommand, int ClientID)
{
	CNetMsg_Sv_CommandInfo Msg;
	Msg.m_Name = pCommand->m_aName;
	Msg.m_HelpText = pCommand->m_aHelpText;
	Msg.m_ArgsFormat = pCommand->m_aArgsFormat;

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendChatCommands(int ClientID)
{
	for(int i = 0; i < CommandManager()->CommandCount(); i++)
	{
		SendChatCommand(CommandManager()->GetCommand(i), ClientID);
	}
}

void CGameContext::SendRemoveChatCommand(const CCommandManager::CCommand *pCommand, int ClientID)
{
	CNetMsg_Sv_CommandInfoRemove Msg;
	Msg.m_Name = pCommand->m_aName;

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SwitchPlayerWorld(CPlayer *pPlayer, unsigned MapID, const vec2 &ArrivalPos, bool HasArrival, bool TrainArrival)
{
	if(pPlayer->GameWorld()->WorldID() == MapID)
	{
		vec2 SpawnPos;
		if(m_pController->CanSpawn(pPlayer->GameWorld(), pPlayer->GetTeam(), &SpawnPos))
		{
			// a same-world respawn is a fresh placement, not a continuation
			pPlayer->GetCharacter()->MoveTo(pPlayer->GameWorld(), SpawnPos, false);
		}
		if(TrainArrival)
		{
			pPlayer->QueueTrainProtection();
			pPlayer->StartQueuedTrainProtection();
		}
		return;
	}

	// The engine must be able to create the game world before the client is
	// told to switch: announcing a map whose world is missing would leave the
	// player in the old world while the client loads the new one.
	CGameWorld *pTargetWorld = EnsureWorld(MapID);
	if(!pTargetWorld)
	{
		pPlayer->m_HasPendingArrival = false;
		dbg_msg("game", "failed to switch client %d to map %u: missing world", pPlayer->GetCID(), MapID);
		return;
	}

	// A ring crossing keeps the mapped column; every other switch falls back to
	// a spawn once the new map has loaded.
	pPlayer->m_HasPendingArrival = HasArrival;
	pPlayer->m_PendingArrivalPos = ArrivalPos;

	bool Silent = false;
	if(Server()->SwitchClientMap(pPlayer->GetCID(), MapID, &Silent))
	{
		pPlayer->SwitchWorld(pTargetWorld);
		if(TrainArrival)
			pPlayer->QueueTrainProtection();

		if(Silent)
		{
			// No READY follows a silent switch, so the regular migration in
			// OnClientConnected never runs. Move the character into the new
			// world now and clear the loading flag, otherwise OnSnap would
			// skip this player and its world (empty snapshots).
			if(CCharacter *pChr = pPlayer->GetCharacter())
			{
				// A ring crossing supplies the mapped arrival column; every
				// other switch still lands on a regular spawn like the old
				// READY path did instead of falling through to (0, 0).
				vec2 Pos = ArrivalPos;
				if(!HasArrival)
				{
					vec2 SpawnPos;
					if(m_pController->CanSpawn(pPlayer->GameWorld(), pPlayer->GetTeam(), &SpawnPos))
						Pos = SpawnPos;
				}
				// only a ring seam continues the motion; a spawn arrival drops it
				pChr->MoveTo(pPlayer->GameWorld(), Pos, HasArrival);
			}
			pPlayer->m_HasPendingArrival = false;
			pPlayer->m_MapLoading = false;
			if(TrainArrival)
				pPlayer->StartQueuedTrainProtection();
		}
	}
	else
	{
		pPlayer->m_HasPendingArrival = false;
		dbg_msg("game", "failed to switch client %d to map %u", pPlayer->GetCID(), MapID);
	}
}

bool CGameContext::IsOnTrainEntrance(CPlayer *pPlayer) const
{
	if(!pPlayer)
		return false;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
		return false;
	CGameWorld *pWorld = pChr->GameWorld();
	if(!pWorld)
		return false;

	// OnExtraTile installs COLFLAG_TRAIN_ENTRANCE on the marker tiles, so the
	// same collision box checks used for movement can answer this. Sample the
	// body, its centre and the feet: a one-tile platform can sit just under the
	// feet and the four box corners alone would miss it.
	const vec2 Pos = pChr->GetPos();
	const vec2 Box(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE);
	CCollision *pCollision = pWorld->Collision();
	return pCollision->TestBox(Pos, Box, CCollision::COLFLAG_TRAIN_ENTRANCE) ||
	       pCollision->CheckPoint(Pos, CCollision::COLFLAG_TRAIN_ENTRANCE) ||
	       pCollision->CheckPoint(Pos + vec2(0.0f, CCharacterCore::PHYS_SIZE * 0.5f - 2.0f), CCollision::COLFLAG_TRAIN_ENTRANCE);
}

bool CGameContext::DropItem(int ClientID, const char *pItemId, int Count, vec2 Direction)
{
	CLanguageScope LanguageScope(LanguageOf(ClientID));
	CPlayer *pPlayer = m_apPlayers[ClientID];
	CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
	if(!pChr)
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("You need to be in the game to drop items.", "Item Drop"));
		return false;
	}

	CItemID Id;
	if(!CItemID::Parse(pItemId, &Id) || Id.IsZero())
		return false;
	if(Count <= 0)
		Count = 1;

	CItemSystem *pItems = Item();
	if(!pItems->IsKnownItem(Id))
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("Unknown item.", "Item Drop"));
		return false;
	}
	const int Owned = pItems->GetItemCount(ClientID, Id);
	if(Owned <= 0)
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("You do not have this item.", "Item Drop"));
		return false;
	}
	if(Owned < Count)
		Count = Owned;

	if(!pItems->RemoveItem(ClientID, Id, Count))
		return false;

	CGameWorld *pWorld = pChr->GameWorld();
	// no database write here: the world's drops are persisted when the
	// world is unloaded or the server shuts down
	new CDroppedPickup(pWorld, pChr->GetPos(), Direction, Id, Count);

	const char *pName = pItems->GetName(Id);
	const std::string Msg = LocalizeFormat("Item Drop", "You dropped: {} x{}", Localize(pName, "Item Name"), Count);
	SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
	return true;
}

void CGameContext::SendTuningParams(int ClientID)
{
	// 0.8: tuning is no longer a message (Sv_TuneParams). It is a singleton
	// snapshot object (NETOBJTYPE_TUNING, item id 0) emitted in OnSnap(), so
	// there is nothing to send here. Callers are kept for timing compatibility.
	(void) ClientID;
}

void CGameContext::SendReadyToEnter(CPlayer *pPlayer)
{
	pPlayer->m_IsReadyToEnter = true;
	CNetMsg_Sv_ReadyToEnter m;
	Server()->SendPackMsg(&m, MSGFLAG_VITAL | MSGFLAG_FLUSH, pPlayer->GetCID());
}

void CGameContext::SendSoundTarget(int ClientID, int SoundID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	if(!m_apPlayers[ClientID])
		return;
	m_apPlayers[ClientID]->GameWorld()->CreateSound(m_apPlayers[ClientID]->m_ViewPos, SoundID, CmaskOne(ClientID));
}

void CGameContext::OnTick()
{
	// free the queued database writes that finished since the last tick
	if(m_pDatabase)
		m_pDatabase->ReapJobs();

	// top up any NPC shelf whose restock time passed; open shop pages then need
	// to be regenerated so their stock and countdown stay live
	if(m_pNpcSystem && m_pNpcSystem->RestockDue())
		m_pGameMenu->RefreshShop(-1);

	// a player in a ring instance needs both neighbours live: the shared strip is
	// single owned, so half of what they see and can mine lives next door
	EnsureOccupiedRingNeighbours();

	// entrance target worlds are switched to synchronously; keep them loaded so
	// the switch never has to generate a world inside the movement tick
	EnsureOccupiedEntrances();

	// bind the neighbours' players as proxy cores before anything moves, so a
	// hook or a player collision this tick sees them where they are
	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		(void) pUser;
		pWorld->RefreshRingProxies();
	}, nullptr);

	// the one manager tops up every configured world before its entities
	// advance, so a bot respawned this tick takes part in it
	if(m_pBotManager)
		m_pBotManager->Tick();

	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		pWorld->m_Core.m_Tuning = *static_cast<CGameContext *>(pUser)->Tuning();
		pWorld->Tick();
	}, this);

	// pass on the drag a hook applied to a proxy, so the real character is pulled
	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		(void) pUser;
		pWorld->ForwardRingProxyDrag();
	}, nullptr);

	// if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
		}
	}

	// The cursor hint is a realtime broadcast: it is submitted again every tick
	// while the cursor is over a resource, and gone the tick it moves away.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			UpdateCursorResourceBroadcast(i);
	}
	FlushBroadcasts();

	// a short chat hint every sv_tip_interval seconds while somebody is online
	TickTips();

	// update voting
	GameMenu()->TickVote();

#ifdef CONF_DEBUG
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->IsDummy())
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i & 1) ? -1 : 1;
			m_apPlayers[i]->OnPredictedInput(&Input);
		}
	}
#endif

	// periodically unload worlds that no player is in and no client is still
	// attached to, so the server frees memory while idle (every ~30 seconds).
	// World content (dropped items etc.) is only persisted when a world is
	// unloaded or the server shuts down, never while it is loaded.
	if(Server()->Tick() % (Server()->TickSpeed() * 30) == 0)
	{
		UnloadIdleWorlds();
	}
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	int NumFailures = m_NetObjHandler.NumObjFailures();
	if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
	{
		if(Config()->m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT failed on '%s'", m_NetObjHandler.FailedObjOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
	}
	else
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *) pInput);
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	int NumFailures = m_NetObjHandler.NumObjFailures();
	if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
	{
		if(Config()->m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT corrected on '%s'", m_NetObjHandler.FailedObjOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
	}
	else
		m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *) pInput);
}

void CGameContext::OnClientEnter(int ClientID)
{
	CLanguageScope LanguageScope(LanguageOf(ClientID));
	// a reused slot must not inherit the previous occupant's broadcast queue
	ResetBroadcastState(ClientID);
	// send chat commands
	SendChatCommands(ClientID);

	// a (re)entering client knows no bot identity yet: forget every bot tee id
	// this slot was handed before, so it does not inherit the previous
	// occupant's bots
	if(m_pBotManager)
		m_pBotManager->OnClientRefresh(ClientID);

	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	GameMenu()->OnClientEntered(ClientID);

	// 0.8: identity (name, clan, country, skin, team, score) now travels in the
	// per-client TeeInfo snapshot object, so joining is just announced with the
	// Sv_ClientEnter message carrying the client id.
	CNetMsg_Sv_ClientEnter Msg;
	Msg.m_ClientID = ClientID;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// Guests are first-class players now: SetupGuestPlayer bound a
	// nickname-derived profile before this point, so no login is required to
	// enter the game.
	Server()->ExpireServerInfo();
}

void CGameContext::OnClientMapDataRequest(int ClientID)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->m_MapLoading)
		return;

	CGameWorld *pWorld = WorldByID(Server()->GetClientMapID(ClientID));
	if(!pWorld || pPlayer->GameWorld() != pWorld)
		return;

	// The silent switch already moved the character into the new world. Keep
	// that position for the READY migration, which would otherwise send the
	// character to a spawn.
	pPlayer->m_HasPendingArrival = false;
	if(CCharacter *pChr = pPlayer->GetCharacter())
	{
		pPlayer->m_HasPendingArrival = true;
		// The core is authoritative here: the entity position is only copied
		// from it in TickDefered, and this packet can be handled in the window
		// right after a silent switch, before that copy has run. Reading the
		// stale entity position would anchor the tee to its old-world column,
		// which in the destination instance can be solid ground.
		pPlayer->m_PendingArrivalPos = pChr->CorePos();
	}
	pPlayer->m_MapLoading = true;
}

void CGameContext::OnClientConnected(int ClientID, bool Dummy, bool AsSpec)
{
	// The engine only calls this with a real slot, but a stale or duplicated
	// callback must not index the player array out of bounds.
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
	{
		dbg_msg("game", "client %d connected outside the player slots, ignoring", ClientID);
		return;
	}

	// The slot must be free, or belong to a player that is still loading its
	// new map. A stale or duplicated callback is ignored instead of trapping.
	if(m_apPlayers[ClientID] && !m_apPlayers[ClientID]->m_MapLoading)
	{
		// A silent switch already migrated this player. A READY that follows
		// its map-data request is therefore not a busy slot; there is nothing
		// left to migrate and the join UI was already sent.
		CGameWorld *pCurrentWorld = m_apPlayers[ClientID]->GameWorld();
		if(pCurrentWorld && pCurrentWorld->WorldID() == Server()->GetClientMapID(ClientID))
			return;
		dbg_msg("game", "client %d connected on a busy slot, ignoring", ClientID);
		return;
	}
	// A world can be missing if it was unloaded before the client attached to
	// it; recreate it instead of leaving the client waiting forever.
	CGameWorld *pWorld = EnsureWorld(Server()->GetClientMapID(ClientID));
	if(!pWorld)
	{
		dbg_msg("game", "client %d connected to a missing world, ignoring", ClientID);
		return;
	}

	if(!m_apPlayers[ClientID])
	{
		// a recycled slot must not inherit the previous occupant's inventory
		Item()->GetInventory(ClientID).Reset();
		m_apPlayers[ClientID] = new(ClientID) CPlayer(pWorld, ClientID, Dummy, AsSpec);
		if(!Dummy && !AsSpec)
			SetupGuestPlayer(ClientID);
	}
	else if(GetPlayerChar(ClientID))
	{
		CPlayer *pPlayer = m_apPlayers[ClientID];
		if(pPlayer->m_HasPendingArrival)
		{
			// a ring crossing: land on the column the character left, which the
			// shared field gives the same terrain in both instances
			pPlayer->m_HasPendingArrival = false;
			pPlayer->GetCharacter()->MoveTo(pPlayer->GameWorld(), pPlayer->m_PendingArrivalPos, true);
		}
		else
		{
			vec2 SpawnPos;
			if(m_pController->CanSpawn(pPlayer->GameWorld(), pPlayer->GetTeam(), &SpawnPos))
			{
				// a spawn after a map change must not inherit the old momentum
				pPlayer->GetCharacter()->MoveTo(pPlayer->GameWorld(), SpawnPos, false);
			}
		}
	}
	m_apPlayers[ClientID]->m_MapLoading = false;
	// the destination map is loaded and the player has entered it: this is
	// where the short train-arrival invincibility starts
	m_apPlayers[ClientID]->StartQueuedTrainProtection();

	if(Dummy)
		return;

	// send active vote
	GameMenu()->SendActiveVote(ClientID);

	// send motd
	SendMotd(ClientID);

	// send settings
	SendSettings(ClientID);
}

void CGameContext::OnClientTeamChange(int ClientID)
{
	if(m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
		GameMenu()->AbortVoteOnTeamChange(ClientID);

	/* as spectator is not allowed, this won't happen
	// mark client's projectile has team projectile
	for(CGameWorld::TypeRange r = m_World.DoTypeRange(CGameWorld::ENTTYPE_PROJECTILE); !r.empty(); r.pop_front())
	{
		CProjectile *p = static_cast<CProjectile *>(r.front());
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}
	*/

	Server()->ExpireServerInfo();
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	// since the player might disconnect in the map loading, we do a check here to make sure the player exists
	// as the CServer will call this function when player is in the connecting state.
	if(!m_apPlayers[ClientID])
		return;

	// the player's shop listings persist with their escrowed goods, so only the
	// client slot is detached; the goods come back when the account logs in
	m_pTradeSystem->DetachSeller(ClientID);

	// persist the player's status before dropping
	SavePlayerData(m_apPlayers[ClientID]);
	if(m_pTeamSystem && m_apPlayers[ClientID]->m_AccountUuid != UUID_ZEROED)
		m_pTeamSystem->OnPlayerDisconnected(m_apPlayers[ClientID]->m_AccountUuid);

	GameMenu()->AbortVoteOnDisconnect(ClientID);
	m_pController->OnPlayerDisconnect(m_apPlayers[ClientID]);

	// update clients on drop
	// 0.8: the De_ClientLeave demo message is gone; Sv_ClientDrop is recorded
	// into demos by the engine like any other vital message.
	if(Server()->ClientIngame(ClientID) || IsClientBot(ClientID))
	{
		CNetMsg_Sv_ClientDrop Msg;
		Msg.m_ClientID = ClientID;
		Msg.m_pReason = pReason;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);
	}

	// mark client's projectile has team projectile
	for(CGameWorld::TypeRange r = m_apPlayers[ClientID]->GameWorld()->DoTypeRange(CGameWorld::ENTTYPE_PROJECTILE); !r.empty(); r.pop_front())
	{
		CProjectile *p = static_cast<CProjectile *>(r.front());
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}

	// a laser keeps a client id, not the character pointer; clear it so a slot
	// reused by the next player is not mistaken for the departed owner
	for(CGameWorld::TypeRange r = m_apPlayers[ClientID]->GameWorld()->DoTypeRange(CGameWorld::ENTTYPE_LASER); !r.empty(); r.pop_front())
	{
		CLaser *p = static_cast<CLaser *>(r.front());
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}

	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;
	ResetBroadcastState(ClientID);

	GameMenu()->SetVoteUpdate();

	Server()->ExpireServerInfo();
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(!pRawMsg)
	{
		if(Config()->m_Debug)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dropped weird message '%s' (%d), failed on '%s'", m_NetObjHandler.GetMsgName(MsgID), MsgID, m_NetObjHandler.FailedMsgOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
		return;
	}

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			if(Config()->m_SvSpamprotection && pPlayer->m_LastChatTeamTick && pPlayer->m_LastChatTeamTick + Server()->TickSpeed() > Server()->Tick())
				return;

			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *) pRawMsg;

			// trim right and set maximum length to 128 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = 0;
			while(*p)
			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(!str_utf8_is_whitespace(Code))
				{
					pEnd = 0;
				}
				else if(pEnd == 0)
					pEnd = pStrOld;

				if(++Length >= 127)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
			}
			if(pEnd != 0)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 20 characters per second)
			if(Length == 0 || (Config()->m_SvSpamprotection && pPlayer->m_LastChatTeamTick && pPlayer->m_LastChatTeamTick + Server()->TickSpeed() * (Length / 20) > Server()->Tick()))
				return;

			pPlayer->m_LastChatTeamTick = Server()->Tick();

			// don't allow spectators to disturb players during a running game in tournament mode
			if(pMsg->m_Mode != CHAT_NONE)
			{
				if(pMsg->m_pMessage[0] == '/')
				{
					const char *pCommandStr = pMsg->m_pMessage;
					char aCommand[16];
					str_format(aCommand, sizeof(aCommand), "%.*s", str_span(pCommandStr + 1, " "), pCommandStr + 1);
					const CCommandManager::CCommand *pCommand = m_CommandManager.GetCommand(aCommand);
					if(!pCommand)
					{
						return;
					}

					// execute command
					CommandManager()->OnCommand(pCommand->m_aName, str_skip_whitespaces_const(str_skip_to_whitespace_const(pCommandStr)), ClientID);
				}
				else
					SendChat(ClientID, pMsg->m_Mode, pMsg->m_Target, pMsg->m_pMessage);
			}
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			GameMenu()->OnCallVote(ClientID, (CNetMsg_Cl_CallVote *) pRawMsg);
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{
			GameMenu()->OnVote(ClientID, (CNetMsg_Cl_Vote *) pRawMsg);
		}
		else if(MsgID == NETMSGTYPE_CL_SETTEAM)
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *) pRawMsg;

			if(pPlayer->GetTeam() == pMsg->m_Team || pMsg->m_Team == TEAM_SPECTATORS ||
				(Config()->m_SvSpamprotection && pPlayer->m_LastSetTeamTick && pPlayer->m_LastSetTeamTick + Server()->TickSpeed() * 3 > Server()->Tick()) ||
				(pMsg->m_Team != TEAM_SPECTATORS && m_LockTeams) || pPlayer->m_TeamChangeTick > Server()->Tick())
				return;

			pPlayer->m_LastSetTeamTick = Server()->Tick();

			// Switch team on given client and kill/respawn him
			if(m_pController->CanJoinTeam(pMsg->m_Team, ClientID) && m_pController->CanChangeTeam(pPlayer, pMsg->m_Team))
			{
				if(pPlayer->GetTeam() == TEAM_SPECTATORS || pMsg->m_Team == TEAM_SPECTATORS)
					GameMenu()->SetVoteUpdate();
				pPlayer->m_TeamChangeTick = Server()->Tick() + Server()->TickSpeed() * 3;
				m_pController->DoTeamChange(pPlayer, pMsg->m_Team);
			}
		}
		else if(MsgID == NETMSGTYPE_CL_SETSPECTATORMODE)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *) pRawMsg;

			if(Config()->m_SvSpamprotection && pPlayer->m_LastSetSpectatorModeTick && pPlayer->m_LastSetSpectatorModeTick + Server()->TickSpeed() > Server()->Tick())
				return;

			pPlayer->m_LastSetSpectatorModeTick = Server()->Tick();
			if(!pPlayer->SetSpectatorID(pMsg->m_SpecMode, pMsg->m_SpectatorID))
				SendGameMsg(GAMEMSG_SPEC_INVALID_ID, ClientID);
		}
		else if(MsgID == NETMSGTYPE_CL_EMOTICON)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *) pRawMsg;

			if(Config()->m_SvSpamprotection && pPlayer->m_LastEmoteTick && pPlayer->m_LastEmoteTick + Server()->TickSpeed() * 3 > Server()->Tick())
				return;

			pPlayer->m_LastEmoteTick = Server()->Tick();

			SendEmoticon(ClientID, pMsg->m_Emoticon);
		}
		else if(MsgID == NETMSGTYPE_CL_KILL)
		{
			if(pPlayer->m_LastKillTick && pPlayer->m_LastKillTick + Server()->TickSpeed() * 3 > Server()->Tick())
				return;

			pPlayer->m_LastKillTick = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF, false);
		}
		else if(MsgID == NETMSGTYPE_CL_READYCHANGE)
		{
			if(pPlayer->m_LastReadyChangeTick && pPlayer->m_LastReadyChangeTick + Server()->TickSpeed() * 1 > Server()->Tick())
				return;

			pPlayer->m_LastReadyChangeTick = Server()->Tick();
			m_pController->OnPlayerReadyChange(pPlayer);
		}
		else if(MsgID == NETMSGTYPE_CL_SKINCHANGE)
		{
			if(pPlayer->m_LastChangeInfoTick && pPlayer->m_LastChangeInfoTick + Server()->TickSpeed() * 5 > Server()->Tick())
				return;

			pPlayer->m_LastChangeInfoTick = Server()->Tick();
			CNetMsg_Cl_SkinChange *pMsg = (CNetMsg_Cl_SkinChange *) pRawMsg;

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_utf8_copy_num(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], sizeof(pPlayer->m_TeeInfos.m_aaSkinPartNames[p]), MAX_SKIN_LENGTH);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			m_pController->OnPlayerInfoChange(pPlayer);
			// 0.8: skins live in the TeeInfo snapshot object, so the next
			// snapshot already carries the update to every client. No explicit
			// re-send is needed (the old Sv_SkinChange loop is gone).
		}
		else if(MsgID == NETMSGTYPE_CL_COMMAND)
		{
			CNetMsg_Cl_Command *pMsg = (CNetMsg_Cl_Command *) pRawMsg;
			CommandManager()->OnCommand(pMsg->m_Name, pMsg->m_Arguments, ClientID);
		}
	}
	else
	{
		if(MsgID == NETMSGTYPE_CL_STARTINFO)
		{
			if(pPlayer->m_IsReadyToEnter)
				return;

			CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *) pRawMsg;
			pPlayer->m_LastChangeInfoTick = Server()->Tick();

			// set start infos
			Server()->SetClientName(ClientID, pMsg->m_pName);
			Server()->SetClientClan(ClientID, pMsg->m_pClan);
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_utf8_copy_num(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], sizeof(pPlayer->m_TeeInfos.m_aaSkinPartNames[p]), MAX_SKIN_LENGTH);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			m_pController->OnPlayerInfoChange(pPlayer);

			GameMenu()->SendVoteClearOptions(ClientID);
			// SendVoteOptions(ClientID);
			SendTuningParams(ClientID);
			SendReadyToEnter(pPlayer);
		}
	}
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	const char *pParamName = pResult->GetString(0);

	char aBuf[256];
	if(pResult->NumArguments() == 2)
	{
		float NewValue = pResult->GetFloat(1);
		if(pSelf->Tuning()->Set(pParamName, NewValue) && pSelf->Tuning()->Get(pParamName, &NewValue))
		{
			str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
			pSelf->SendTuningParams(-1);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "No such tuning parameter: %s", pParamName);
		}
	}
	else
	{
		float Value;
		if(pSelf->Tuning()->Get(pParamName, &Value))
		{
			str_format(aBuf, sizeof(aBuf), "%s %.2f", pParamName, Value);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "No such tuning parameter: %s", pParamName);
		}
	}
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	CTuningParams TuningParams;

	if(pResult->NumArguments())
	{
		const char *pParamName = pResult->GetString(0);
		float DefaultValue = 0.0f;
		char aBuf[256];
		if(TuningParams.Get(pParamName, &DefaultValue) && pSelf->Tuning()->Set(pParamName, DefaultValue))
		{
			str_format(aBuf, sizeof(aBuf), "%s reset to %.2f", pParamName, DefaultValue);
			pSelf->SendTuningParams(-1);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "No such tuning parameter: %s", pParamName);
		}
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
	else
	{
		*pSelf->Tuning() = TuningParams;
		pSelf->SendTuningParams(-1);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
	}
}

void CGameContext::ConTunes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float Value;
		pSelf->Tuning()->Get(i, &Value);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->GetName(i), Value);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->SendChat(-1, CHAT_ALL, -1, pResult->GetString(0));
}

void CGameContext::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->SendBroadcast(-1, pResult->GetString(0), BROADCAST_PRIORITY_SERVERANNOUNCE, pSelf->Server()->TickSpeed() * 3);
}

void CGameContext::ConSetTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	int ClientID = clamp(pResult->GetInteger(0), 0, (int) MAX_CLIENTS - 1);
	int Team = clamp(pResult->GetInteger(1), -1, 1);
	int Delay = pResult->NumArguments() > 2 ? pResult->GetInteger(2) : 0;
	if(!pSelf->m_apPlayers[ClientID] || !pSelf->m_pController->CanJoinTeam(Team, ClientID))
		return;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved client %d to team %d", ClientID, Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	pSelf->m_apPlayers[ClientID]->m_TeamChangeTick = pSelf->Server()->Tick() + pSelf->Server()->TickSpeed() * Delay * 60;
	pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[ClientID], Team);
}

void CGameContext::ConSetTeamAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	int Team = clamp(pResult->GetInteger(0), -1, 1);

	pSelf->SendGameMsg(GAMEMSG_TEAM_ALL, Team, -1);

	for(int i = 0; i < MAX_CLIENTS; ++i)
		if(pSelf->m_apPlayers[i] && pSelf->m_pController->CanJoinTeam(Team, i))
			pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[i], Team, false);
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->GameMenu()->AddVoteOption(pResult->GetString(0), pResult->GetString(1));
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->GameMenu()->RemoveVoteOption(pResult->GetString(0));
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->GameMenu()->ClearVoteOptions();
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *) pUserData;
	pSelf->GameMenu()->ForceVote(pResult->GetString(0));
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *) pUserData;
		pSelf->SendMotd(-1);
	}
}

void CGameContext::ConchainSettingUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *) pUserData;
		pSelf->SendSettings(-1);
	}
}

void CGameContext::ConchainLanguageUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *) pUserData;
		g_Localization.LoadAll(pSelf->Config()->m_SvLanguagefile, pSelf->Storage(), pSelf->Console(), true);
	}
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("tune", "s[tuning] ?i[value]", CFGFLAG_SERVER, ConTuneParam, this, "Tune variable to value or show current value");
	Console()->Register("tune_reset", "?s[tuning]", CFGFLAG_SERVER, ConTuneReset, this, "Reset all or one tuning variable to default");
	Console()->Register("tunes", "", CFGFLAG_SERVER, ConTunes, this, "List all tuning variables and their values");

	Console()->Register("say", "r[text]", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Register("broadcast", "r[text]", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("set_team", "i[id] i[team] ?i[delay]", CFGFLAG_SERVER, ConSetTeam, this, "Set team of player to team");
	Console()->Register("set_team_all", "i[team]", CFGFLAG_SERVER, ConSetTeamAll, this, "Set team of all players to team");

	Console()->Register("add_vote", "s[option] r[command]", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "s[option]", CFGFLAG_SERVER, ConRemoveVote, this, "remove a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("vote", "r['yes'|'no']", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");

	RegisterScriptCommands(this);
}

void CGameContext::NewCommandHook(const CCommandManager::CCommand *pCommand, void *pContext)
{
	CGameContext *pSelf = (CGameContext *) pContext;
	pSelf->SendChatCommand(pCommand, -1);
}

void CGameContext::RemoveCommandHook(const CCommandManager::CCommand *pCommand, void *pContext)
{
	CGameContext *pSelf = (CGameContext *) pContext;
	pSelf->SendRemoveChatCommand(pCommand, -1);
}

void CGameContext::OnGameMenuInit()
{
}

void CGameContext::OnInit()
{
	// init everything
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	// the one bot manager for the whole game: it owns every world's bots and
	// the per-client tee id mapping, so ring neighbours cannot collide
	m_pBotManager = new CBotManager(this);

	m_CommandManager.Init(m_pConsole, this, NewCommandHook, RemoveCommandHook);

	g_Localization.LoadAll(Config()->m_SvLanguagefile, Storage(), Console(), true);

	// HACK: only set static size for items, which were available in the first 0.7 release
	// so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	// select gametype
	m_pController = new CGameController(this);

	m_pController->RegisterChatCommands(CommandManager());

	// collect the statically registered weapon classes (prints the list)
	WeaponManager()->OutputRegisteredWeapons();

	// initialize the database (runtime schema creation)
	{
		CDatabase::SConfig DbConfig;
		DbConfig.m_pBackend = Config()->m_SvDbBackend;
		DbConfig.m_pPath = Config()->m_SvDbPath;
		DbConfig.m_pConnString = Config()->m_SvDbConnstring;
		m_pDatabase = CreateDatabase(DbConfig);
		if(m_pDatabase && !m_pDatabase->Init())
		{
			dbg_msg("database", "failed to initialize the database, running without persistence");
			delete m_pDatabase;
			m_pDatabase = 0;
		}
		// the shared thread pool runs the queued writes
		if(m_pDatabase)
			m_pDatabase->SetJobPool(Kernel()->RequestInterface<IEngine>());
	}

	// The PocketPy VM is process-wide: initialize it before any system that
	// can load a script (items, buffs, weapons, entities, bot AI).
	CScriptRuntime::Get()->Init(this, Storage());

	// initialize the item system (loads datasrc/items/*.json)
	m_pItemSystem = new CItemSystem();
	m_pItemSystem->Init(this, Storage());

	// container kinds (data/containers.json) placed by map structures
	m_pContainerSystem = new CContainerSystem();
	m_pContainerSystem->Init(this, Storage());

	// buffs: data/buffs/*.json + per-tick Python modules
	m_pBuffSystem = new CBuffSystem();
	m_pBuffSystem->Init(this, Storage());

	// generic scripted props: data/entities/*.json + scripts/entities/*.py
	m_pScriptedEntities = new CScriptedEntitySystem();
	m_pScriptedEntities->Init(this, Storage());

	// data/weapons/*.json overrides the native weapon class of the same name
	WeaponManager()->LoadScriptedWeapons(Storage());

	// the player owned shop (in-memory listings for this session)
	m_pTradeSystem = new CTradeSystem();
	m_pTradeSystem->Init(this);

	// content-defined mobs (datasrc/mobs/*.json): factions, abilities and
	// fixed spawns, loaded before any world can ask the bot manager for one
	m_pMobSystem = new CMobSystem();
	m_pMobSystem->Init(this, Storage());

	// guild-style teams; loaded from the database so a restart keeps them
	m_pTeamSystem = new CTeamSystem();
	m_pTeamSystem->Init(this);

	// the configured NPCs (datasrc/bots/npc/*.json)
	m_pNpcSystem = new CNpcSystem();
	m_pNpcSystem->Init(this, Storage());

	// periodic chat tips (datasrc/tips.json)
	LoadTips();

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);

	Console()->Chain("sv_vote_kick", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_kick_min", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_spectate", ConchainSettingUpdate, this);
	Console()->Chain("sv_max_clients", ConchainSettingUpdate, this);
	Console()->Chain("sv_allow_spec_voting", ConchainSettingUpdate, this);
	Console()->Chain("sv_languagefile", ConchainLanguageUpdate, this);
}

void CGameContext::SavePlayerData(CPlayer *pPlayer)
{
	if(!m_pDatabase || !pPlayer || pPlayer->IsDummy() || pPlayer->m_AccountUuid == UUID_ZEROED)
		return;
	// A guest has a nickname-derived UUID and a persisted profile even though
	// m_LoggedIn is false. Anything else without a bound identity is skipped.
	if(!pPlayer->m_LoggedIn && !pPlayer->m_Guest)
		return;

	// snapshot the player state on this thread and let the pool write it
	CDbJob *pJob = new CDbJob();
	pJob->m_Type = CDbJob::TYPE_PLAYER;
	pJob->m_pDB = m_pDatabase;
	pJob->m_Uuid = pPlayer->m_AccountUuid;
	pPlayer->CaptureStatus(*pJob);
	m_pDatabase->QueueJob(pJob);
}

void CGameContext::GiveStartingKit(int ClientID)
{
	CItemSystem *pItem = Item();
	if(!pItem || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	pItem->AddItem(ClientID, "hammer", 1, true);
	pItem->AddItem(ClientID, "gun", 1, true);
	pItem->AddItem(ClientID, "pistol_bullet", 100, true);
	pItem->AddItem(ClientID, "bandage", 3, true);
}

void CGameContext::SetupGuestPlayer(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->IsDummy() || pPlayer->m_LoggedIn || pPlayer->m_AccountUuid != UUID_ZEROED)
		return;

	const char *pName = Server()->ClientName(ClientID);
	char aIdentity[192];
	str_format(aIdentity, sizeof(aIdentity), "lastday:guest:%s", pName);
	const Uuid GuestUuid = calculate_uuid(aIdentity);

	char aUsername[96];
	str_format(aUsername, sizeof(aUsername), "@guest:%s", pName);

	pPlayer->m_AccountUuid = GuestUuid;
	pPlayer->m_Guest = true;

	bool Created = true;
	if(m_pDatabase)
	{
		Created = m_pDatabase->EnsureGuestPlayer(GuestUuid, aUsername);
		pPlayer->LoadStatus(m_pDatabase, false);
	}
	if(Created)
		GiveStartingKit(ClientID);

	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		m_pController->DoTeamChange(pPlayer, TEAM_RED, false);

	CLanguageScope LanguageScope(LanguageOf(ClientID));
	SendChat(-1, CHAT_ALL, ClientID, Localize("Playing as a guest. Use /register <username> <password> to keep your progress on an account.", "Account"));
}

void CGameContext::NpcAddAggro(const Uuid &Npc, const Uuid &User, int Delta)
{
	if(!m_pDatabase || Delta == 0 || Npc == UUID_ZEROED || User == UUID_ZEROED)
		return;
	m_pDatabase->AddNpcAggro(Npc, User, Delta);
}

int CGameContext::NpcGetAggro(const Uuid &Npc, const Uuid &User)
{
	int Aggro = 0;
	if(m_pDatabase && Npc != UUID_ZEROED && User != UUID_ZEROED)
		m_pDatabase->GetNpcAggro(Npc, User, &Aggro);
	return Aggro;
}

void CGameContext::EnsureNpcAggroFor(CPlayer *pPlayer)
{
	if(!m_pDatabase || !m_pNpcSystem || !pPlayer || pPlayer->m_AccountUuid == UUID_ZEROED)
		return;
	for(int i = 0; i < m_pNpcSystem->NumDefs(); i++)
	{
		const SNpcDef *pDef = m_pNpcSystem->DefAt(i);
		if(pDef)
			m_pDatabase->EnsureNpcAggro(pDef->m_Id.Value(), pPlayer->m_AccountUuid);
	}
}

bool CGameContext::BuyFromNpc(int ClientID, int DefIndex, int SellIndex)
{
	if(!m_pNpcSystem || ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return false;
	CLanguageScope LanguageScope(LanguageOf(ClientID));
	const SNpcDef *pDef = m_pNpcSystem->DefAt(DefIndex);
	if(!pDef || SellIndex < 0 || SellIndex >= pDef->m_lSells.size())
		return false;
	if(!NpcInRange(ClientID, DefIndex))
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("You must be near the merchant.", "Trade"));
		return false;
	}

	const SNpcSell &Sell = pDef->m_lSells[SellIndex];
	CItemSystem *pItems = Item();
	if(!pItems->IsKnownItem(Sell.m_Item))
		return false;
	if(Sell.m_Stock < Sell.m_Count)
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("Out of stock.", "Trade"));
		return false;
	}

	const bool HasPrice = Sell.m_PriceCount > 0 && !Sell.m_PriceItem.IsZero();
	if(HasPrice && pItems->GetItemCount(ClientID, Sell.m_PriceItem) < Sell.m_PriceCount)
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("You cannot afford this.", "Trade"));
		return false;
	}

	// hand over the goods first: if the buyer has no room nothing has changed
	if(!pItems->AddItem(ClientID, Sell.m_Item, Sell.m_Count))
	{
		SendChat(-1, CHAT_WHISPER, ClientID, Localize("You have no inventory space.", "Trade"));
		return false;
	}
	if(HasPrice && !pItems->RemoveItem(ClientID, Sell.m_PriceItem, Sell.m_PriceCount))
	{
		pItems->RemoveItem(ClientID, Sell.m_Item, Sell.m_Count);
		return false;
	}
	// take the stock out of the shelf; the checkout above already checked it
	if(!m_pNpcSystem->ConsumeStock(DefIndex, SellIndex, Sell.m_Count))
	{
		pItems->RemoveItem(ClientID, Sell.m_Item, Sell.m_Count);
		if(HasPrice)
			pItems->AddItem(ClientID, Sell.m_PriceItem, Sell.m_PriceCount, true);
		return false;
	}

	const std::string Msg = LocalizeFormat("Trade", "Bought {} x{} from {}", pItems->GetName(Sell.m_Item), Sell.m_Count, NpcIDDisplayName(pDef->m_Id));
	SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());

	// other players looking at the shop must see the stock drop too; the caller
	// regenerates this buyer's own page after the callback
	GameMenu()->RefreshShop(ClientID);
	return true;
}

bool CGameContext::NpcInRange(int ClientID, int DefIndex)
{
	if(!m_pNpcSystem || ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return false;
	const SNpcDef *pDef = m_pNpcSystem->DefAt(DefIndex);
	if(!pDef)
		return false;

	CPlayer *pPlayer = m_apPlayers[ClientID];
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !pChr->IsAlive())
		return false;

	if(!m_pBotManager)
		return false;
	IBotEntity *pBot = m_pBotManager->BotByID(pDef->m_Id.Value());
	if(!pBot || !pBot->IsAlive())
		return false;
	return distance(pBot->GetPos(), pChr->GetPos()) <= NPC_INTERACT_RADIUS;
}

void CGameContext::OnShutdown()
{
	// persist the world content (dropped items etc.) of every loaded world
	// before tearing down
	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		(void) pUser;
		if(pWorld)
			pWorld->SaveToDatabase();
	}, nullptr);

	// flush every connected player to the database before tearing down
	if(m_pDatabase)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
			if(m_apPlayers[i])
				SavePlayerData(m_apPlayers[i]);

		// the writes are queued on the pool; let them finish before the
		// connection goes away. On a timeout the connection object is left
		// alive (jobs still point at it) but forgotten, not freed under them.
		if(m_pDatabase->DrainJobs())
			delete m_pDatabase;
		m_pDatabase = 0;
	}

	// Destroy players before the worlds: a player owns its character entity,
	// so the world must not delete it first.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		delete m_apPlayers[i];
		m_apPlayers[i] = 0;
	}

	// Destroy the worlds here, while the engine server (whose snap id pool
	// CEntity::~CEntity uses) is still alive. The final destructor runs after
	// the engine has already freed IServer, so no entity may outlive this
	// point: otherwise the teardown dereferences a freed server.
	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		(void) pUser;
		delete pWorld;
		pWorld = nullptr;
	}, nullptr);
	m_pWorlds.clear();
	m_MainWorldID = 0;

	// after the worlds: the manager owns their bots as entities
	delete m_pBotManager;
	m_pBotManager = 0;

	delete m_pItemSystem;
	m_pItemSystem = 0;

	delete m_pContainerSystem;
	m_pContainerSystem = 0;

	delete m_pTradeSystem;
	m_pTradeSystem = 0;

	delete m_pNpcSystem;
	m_pNpcSystem = 0;

	delete m_pMobSystem;
	m_pMobSystem = 0;

	delete m_pTeamSystem;
	m_pTeamSystem = 0;

	delete m_pBuffSystem;
	m_pBuffSystem = 0;

	// entity definitions reference loaded script modules, so release them while
	// the VM is still alive
	delete m_pScriptedEntities;
	m_pScriptedEntities = 0;

	// Scripts no longer have any live game object at this point. Tear the VM
	// down while the engine is still valid so module destruction is orderly.
	CScriptRuntime::Get()->Shutdown();

	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::OnSnap(int ClientID)
{
	// 0.8: tuning is a singleton snapshot object (NETOBJTYPE_TUNING, item id 0)
	// instead of the Sv_TuneParams message / the De_TuneParams demo object. The
	// delta makes it free in the common case and demos record it for free.
	CNetObj_Tuning *pTuning = static_cast<CNetObj_Tuning *>(Server()->SnapNewItem(NETOBJTYPE_TUNING, 0, sizeof(CNetObj_Tuning)));
	if(pTuning)
	{
		// The client predicts its own movement with these tuning values, so a
		// character with an accessory speed bonus must receive a matching
		// control speed. The server applies the same scale in
		// CCharacterCore::Tick, so prediction and simulation stay in step.
		CTuningParams Tuning = m_Tuning;
		CPlayer *pPlayer = (ClientID >= 0 && ClientID < MAX_CLIENTS) ? m_apPlayers[ClientID] : nullptr;
		CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
		if(pChr && pChr->IsDrillControl())
		{
			// steering a drill: the client's local prediction must keep the tee
			// completely still while the server pins it in place
			Tuning.m_GroundControlSpeed = 0.0f;
			Tuning.m_AirControlSpeed = 0.0f;
			Tuning.m_GroundJumpImpulse = 0.0f;
			Tuning.m_AirJumpImpulse = 0.0f;
			Tuning.m_PlayerHooking = 0.0f;
			Tuning.m_Gravity = 0.0f;
		}
		else if(pChr)
		{
			const float SpeedScale = pChr->SpeedScale();
			if(SpeedScale > 0.0f && SpeedScale != 1.0f)
			{
				Tuning.m_GroundControlSpeed = (float) Tuning.m_GroundControlSpeed * SpeedScale;
				Tuning.m_AirControlSpeed = (float) Tuning.m_AirControlSpeed * SpeedScale;
			}
		}
		mem_copy(pTuning->m_aTuneParams, &Tuning, sizeof(pTuning->m_aTuneParams));
	}

	m_pController->Snap(ClientID);
	if(m_apPlayers[ClientID] && !m_apPlayers[ClientID]->m_MapLoading)
		m_apPlayers[ClientID]->GameWorld()->Snap(ClientID);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && !m_apPlayers[i]->m_MapLoading)
		{
			m_apPlayers[i]->Snap(ClientID);
		}
	}
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	// refresh the global per-client bot mapping once, after the entities of
	// every world emitted their snapshots
	if(m_pBotManager)
		m_pBotManager->PostSnap();

	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		pWorld->PostSnap();
	}, nullptr);
}

bool CGameContext::IsClientBot(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->IsDummy();
}

bool CGameContext::IsClientReady(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReadyToEnter;
}

bool CGameContext::IsClientPlayer(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() != TEAM_SPECTATORS;
}

bool CGameContext::IsClientSpectator(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS;
}

const char *CGameContext::GameType() const { return MOD_NAME; }
const char *CGameContext::Version() const { return GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }
const char *CGameContext::NetVersionHashUsed() const { return GAME_NETVERSION_HASH_FORCED; }
const char *CGameContext::NetVersionHashReal() const { return GAME_NETVERSION_HASH; }
bool CGameContext::TimeScore() const { return false; }

void CGameContext::OnUpdatePlayerServerInfo(CJsonWriter *pJsonWriter, int ClientID)
{
	if(!m_apPlayers[ClientID])
		return;

	CTeeInfos &TeeInfo = m_apPlayers[ClientID]->m_TeeInfos;

	pJsonWriter->WriteAttribute("skin");
	pJsonWriter->BeginObject();

	const char *apPartNames[NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};

	for(int i = 0; i < NUM_SKINPARTS; ++i)
	{
		pJsonWriter->WriteAttribute(apPartNames[i]);
		pJsonWriter->BeginObject();

		pJsonWriter->WriteAttribute("name");
		pJsonWriter->WriteStrValue(TeeInfo.m_aaSkinPartNames[i]);

		if(TeeInfo.m_aUseCustomColors[i])
		{
			pJsonWriter->WriteAttribute("color");
			pJsonWriter->WriteIntValue(TeeInfo.m_aSkinPartColors[i]);
		}

		pJsonWriter->EndObject();
	}

	pJsonWriter->EndObject();

	pJsonWriter->WriteAttribute("afk");
	pJsonWriter->WriteBoolValue(false);

	pJsonWriter->WriteAttribute("team");
	pJsonWriter->WriteIntValue(m_apPlayers[ClientID]->GetTeam());
}

int CGameContext::GetMaxPlayerSlots() { return Config()->m_SvMaxClients; }

CGameWorld *CGameContext::MainWorld()
{
	CGameWorld **ppWorld = m_pWorlds[m_MainWorldID];
	return ppWorld ? *ppWorld : nullptr;
}

CGameWorld *CGameContext::WorldByID(unsigned MapID)
{
	CGameWorld **ppWorld = m_pWorlds[MapID];
	return ppWorld ? *ppWorld : nullptr;
}

CGameWorld *CGameContext::EnsureWorld(unsigned MapID)
{
	CGameWorld *pWorld = WorldByID(MapID);
	if(pWorld)
		return pWorld;
	if(!Server()->LoadMapForWorld(MapID))
		return nullptr;
	return WorldByID(MapID);
}

bool CGameContext::HasRingNeighbour(unsigned MapID) const
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_apPlayers[i];
		if(!pPlayer)
			continue;
		CGameWorld *pWorld = pPlayer->GameWorld();
		if(!pWorld || !pWorld->IsRing())
			continue;
		for(int Dir = -1; Dir <= 1; Dir += 2)
		{
			char aName[64];
			float ArrivalX = 0.0f;
			if(pWorld->RingNeighbour(Dir, aName, sizeof(aName), 0.0f, &ArrivalX) &&
				str_quickhash(aName) == MapID)
				return true;
		}
	}
	return false;
}

void CGameContext::EnsureOccupiedRingNeighbours()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_apPlayers[i];
		if(!pPlayer)
			continue;
		CGameWorld *pWorld = pPlayer->GameWorld();
		if(!pWorld || !pWorld->IsRing())
			continue;
		for(int Dir = -1; Dir <= 1; Dir += 2)
		{
			char aName[64];
			if(!pWorld->RingNeighbour(Dir, aName, sizeof(aName), 0.0f, nullptr))
				continue;
			EnsureWorld(str_quickhash(aName));
		}
	}
}

bool CGameContext::HasEntranceTarget(unsigned MapID) const
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const CPlayer *pPlayer = m_apPlayers[i];
		if(!pPlayer)
			continue;
		const CGameWorld *pWorld = pPlayer->GameWorld();
		if(!pWorld)
			continue;
		for(int e = 0; e < pWorld->m_lEntrances.size(); e++)
		{
			const char *pTarget = pWorld->m_lEntrances[e].m_aTargetMap;
			if(pTarget[0] && str_quickhash(pTarget) == MapID)
				return true;
		}
	}
	return false;
}

void CGameContext::EnsureOccupiedEntrances()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_apPlayers[i];
		if(!pPlayer)
			continue;
		CGameWorld *pWorld = pPlayer->GameWorld();
		if(!pWorld || pWorld->m_lEntrances.size() == 0)
			continue;

		for(int e = 0; e < pWorld->m_lEntrances.size(); e++)
		{
			const CGameWorld::CEntranceInfo &Entrance = pWorld->m_lEntrances[e];
			if(!Entrance.m_aTargetMap[0])
				continue;

			// The engine has to know the map name before LoadMapForWorld can
			// find it; registration is idempotent.
			Server()->RegisterMapName(Entrance.m_aTargetMap);
			EnsureWorld(str_quickhash(Entrance.m_aTargetMap));
		}
	}
}

void CGameContext::RequestLoadWorld(unsigned MapID)
{
	// Loading is idempotent: a duplicate request (a delayed map load callback,
	// a reload) must not create a second world for the same map or trap.
	if(m_pWorlds.get(MapID) != nullptr)
		return;
	if(!m_pWorlds.size())
		m_MainWorldID = MapID;

	CGameWorld *pNewWorld = new CGameWorld(this);
	pNewWorld->m_WorldID = MapID;
	// The canonical map name is needed before InitCollision: it is both the
	// persistence key of the world saves and the key of the generated world
	// metadata sidecar.
	str_copy(pNewWorld->m_aMapName, Server()->GetMapName(MapID), sizeof(pNewWorld->m_aMapName));
	pNewWorld->InitCollision(Kernel()->RequestInterface<IMap>());

	// Bots are allowed only in euclidean worlds with sv_bots enabled. A
	// non-euclidean space stays empty, and the hub spawns peaceful NPCs only
	// (never hostile bots). An unknown kind is treated as non-euclidean.
	const int WorldKind = Server()->GetMapWorldKind(MapID);
	const bool BotWorld = Config()->m_SvBots > 0 && (WorldKind == WORLDKIND_EUCLID || WorldKind == WORLDKIND_HUB);

	// Configured NPCs are content, not the random bot population, so they spawn
	// even with sv_bots 0. A world that owns them never rolls random bots.
	const char *pMapName = Server()->GetMapName(MapID);
	array<const SNpcDef *> lpNpcs;
	if(m_pNpcSystem)
	{
		for(int i = 0; i < m_pNpcSystem->NumDefs(); i++)
		{
			const SNpcDef *pDef = m_pNpcSystem->DefAt(i);
			if(pDef && str_comp(pDef->m_aWorld, pMapName) == 0)
				lpNpcs.add(pDef);
		}
	}

	// Fixed mob spawns are content too: a friendly ranger in the hub or a
	// named guard in a dungeon spawns even with sv_bots 0, and is respawned by
	// the bot manager after death.
	array<const SMobSpawn *> lpMobs;
	if(m_pMobSystem)
	{
		for(int i = 0; i < m_pMobSystem->NumSpawns(); i++)
		{
			const SMobSpawn *pSpawn = m_pMobSystem->SpawnAt(i);
			if(pSpawn && str_comp(pSpawn->m_aWorld, pMapName) == 0)
				lpMobs.add(pSpawn);
		}
	}

	pNewWorld->m_BotConfigured = BotWorld || lpNpcs.size() > 0 || lpMobs.size() > 0;
	pNewWorld->m_BotNpcRole = WorldKind == WORLDKIND_HUB || lpNpcs.size() > 0;
	if(m_pBotManager)
	{
		for(int i = 0; i < lpNpcs.size(); i++)
			m_pBotManager->SpawnNpc(pNewWorld, lpNpcs[i]);
		for(int i = 0; i < lpMobs.size(); i++)
		{
			const SMobSpawn *pSpawn = lpMobs[i];
			bool Known = false;
			for(int k = 0; k < pNewWorld->m_lBotDefinedMobs.size() && !Known; k++)
				Known = pNewWorld->m_lBotDefinedMobs[k] == pSpawn;
			if(!Known)
				pNewWorld->m_lBotDefinedMobs.add(pSpawn);
			m_pBotManager->SpawnMob(pNewWorld, pSpawn->m_pDef, pSpawn->m_Pos, &pSpawn->m_Id, pSpawn->m_aName);
		}
	}

	// A ring crossing names the neighbouring instance at runtime, and the engine
	// only switches to or loads a map it already knows, so register the two
	// neighbours now. Loading one of them registers its own further neighbours
	// in turn, which is how the ring is discovered lazily.
	if(pNewWorld->IsRing())
	{
		for(int Dir = -1; Dir <= 1; Dir += 2)
		{
			char aName[64];
			if(pNewWorld->RingNeighbour(Dir, aName, sizeof(aName), 0.0f, nullptr))
				Server()->RegisterMapName(aName);
		}
	}
	// restore the saved state (dropped items etc.) of this world
	pNewWorld->RestoreFromDatabase();
	// containers that had no saved entry are freshly generated map content:
	// roll their initial loot now, after the restore had a chance to claim the
	// ones with saved (possibly empty) contents
	pNewWorld->InitContainerLoot();

	m_pWorlds.set(MapID, pNewWorld);
}

bool CGameContext::UnloadWorld(unsigned MapID)
{
	CGameWorld **ppWorld = m_pWorlds[MapID];
	if(!ppWorld || !*ppWorld)
		return false;

	CGameWorld *pWorld = *ppWorld;

	// never unload the main world
	if(MapID == m_MainWorldID)
		return false;

	// keep the world while any client is still attached to it. A client can
	// be downloading the map or waiting for READY before its player has been
	// moved into the world, and OnClientConnected still needs that world.
	if(Server()->IsMapInUse(MapID))
		return false;

	// keep the world while a player is still inside it
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->GameWorld() == pWorld)
			return false;
	}

	// persist the world state (dropped items etc.) before destroying it
	pWorld->SaveToDatabase();

	delete pWorld;
	m_pWorlds.remove(MapID);

	// release the cached map data on the engine side
	Server()->UnloadMap(MapID);
	return true;
}

void CGameContext::UnloadIdleWorlds()
{
	struct SUnloadCollector
	{
		CGameContext *m_pSelf;
		array<unsigned> m_lToUnload;
	};
	SUnloadCollector Collector = {this, {}};

	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		SUnloadCollector *pCollector = static_cast<SUnloadCollector *>(pUser);
		CGameContext *pSelf = pCollector->m_pSelf;
		if(!pWorld)
			return;
		if(pWorld->WorldID() == pSelf->m_MainWorldID)
			return;

		// check if any player is still in this world
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(pSelf->m_apPlayers[i] && pSelf->m_apPlayers[i]->GameWorld() == pWorld)
				return;
		}
		// keep the live neighbours of a ring world, so a crossing entity has
		// somewhere to go even when nobody is in the neighbour yet
		if(pSelf->HasRingNeighbour(pWorld->WorldID()))
			return;
		// same for an entrance target: it is preloaded so the synchronous
		// switch does not have to generate it; unloading it after 30s would
		// only make the next tick regenerate it again
		if(pSelf->HasEntranceTarget(pWorld->WorldID()))
			return;
		pCollector->m_lToUnload.add(pWorld->WorldID());
	}, &Collector);

	for(int i = 0; i < Collector.m_lToUnload.size(); i++)
		UnloadWorld(Collector.m_lToUnload[i]);
}

IGameServer *CreateGameServer() { return new CGameContext; }
