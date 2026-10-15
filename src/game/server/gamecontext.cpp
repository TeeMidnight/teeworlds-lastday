/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>

#include <engine/engine.h>
#include <engine/map.h>
#include <engine/server/mapgen.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/memheap.h>
#include <engine/storage.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/localization.h>
#include <game/version.h>
#include <generated/server_data.h>

#include "botmanager.h"
#include "entities/botentity.h"
#include "entities/character.h"
#include "entities/dropped_pickup.h"
#include "entities/laser.h"
#include "entities/projectile.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "item.h"
#include "npcdata.h"
#include "player.h"
#include "trade.h"
#include "weapon.h"
#include "weaponmanager.h"

#include <game/server/database/database.h>
#include <game/server/database/database_util.h>

enum
{
	RESET,
	NO_RESET
};

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_apPlayers[i] = 0;

	m_pController = 0;
	m_pDatabase = 0;
	m_pItemSystem = 0;
	m_pTradeSystem = 0;
	m_pNpcSystem = 0;
	m_MainWorldID = 0;
	m_LockTeams = 0;

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
	}
}

void CGameContext::Clear()
{
	CTuningParams Tuning = m_Tuning;
	CGameMenu *pGameMenu = m_pGameMenu;
	CDatabase *pDatabase = m_pDatabase;
	CItemSystem *pItemSystem = m_pItemSystem;
	CTradeSystem *pTradeSystem = m_pTradeSystem;
	CNpcSystem *pNpcSystem = m_pNpcSystem;
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
	m_pTradeSystem = pTradeSystem;
	m_pNpcSystem = pNpcSystem;
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

void CGameContext::SendBroadcast(int ClientID, const char *pText)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
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
	(void)ClientID;
	(void)TargetID;
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

void CGameContext::SwitchPlayerWorld(CPlayer *pPlayer, unsigned MapID)
{
	if(pPlayer->GameWorld()->WorldID() == MapID)
	{
		vec2 SpawnPos;
		if(m_pController->CanSpawn(pPlayer->GameWorld(), pPlayer->GetTeam(), &SpawnPos))
		{
			pPlayer->GetCharacter()->MoveTo(pPlayer->GameWorld(), SpawnPos);
		}
		return;
	}

	if(Server()->SwitchClientMap(pPlayer->GetCID(), MapID))
	{
		CGameWorld **ppWorld = m_pWorlds[MapID];
		CGameWorld *pWorld = ppWorld ? *ppWorld : nullptr;
		if(pWorld)
			pPlayer->SwitchWorld(pWorld);
	}
}

bool CGameContext::DropItem(int ClientID, const char *pItemId, int Count, vec2 Direction)
{
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
	char aMsg[128];
	str_format(aMsg, sizeof(aMsg), Localize("You dropped: %s x%d", "Item Drop"), Localize(pName, "Item Name"), Count);
	SendChat(-1, CHAT_WHISPER, ClientID, aMsg);
	return true;
}

void CGameContext::SendTuningParams(int ClientID)
{
	// 0.8: tuning is no longer a message (Sv_TuneParams). It is a singleton
	// snapshot object (NETOBJTYPE_TUNING, item id 0) emitted in OnSnap(), so
	// there is nothing to send here. Callers are kept for timing compatibility.
	(void)ClientID;
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

	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		pWorld->m_Core.m_Tuning = *static_cast<CGameContext *>(pUser)->Tuning();
		pWorld->Tick();
	}, this);

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

	// periodically unload worlds that no player is in anymore, so the
	// server frees memory while idle (every ~30 seconds). World content
	// (dropped items etc.) is only persisted when a world is unloaded or
	// the server shuts down, never while it is loaded.
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
	// send chat commands
	SendChatCommands(ClientID);

	// a (re)entering client knows no bot identity yet: forget every bot tee id
	// this slot was handed before, so it does not inherit the previous
	// occupant's bots
	struct SRefreshBots
	{
		int m_ClientID;
	};
	SRefreshBots Refresh = {ClientID};
	m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		SRefreshBots *pRefresh = static_cast<SRefreshBots *>(pUser);
		if(pWorld && pWorld->HasBotManager())
			pWorld->GetBotManager()->OnClientRefresh(pRefresh->m_ClientID);
	}, &Refresh);

	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	GameMenu()->OnClientEntered(ClientID);

	// 0.8: identity (name, clan, country, skin, team, score) now travels in the
	// per-client TeeInfo snapshot object, so joining is just announced with the
	// Sv_ClientEnter message carrying the client id.
	CNetMsg_Sv_ClientEnter Msg;
	Msg.m_ClientID = ClientID;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// a player that is not logged in may only spectate; they must log in (or
	// register) before they are allowed to join the game
	if(!m_apPlayers[ClientID]->m_LoggedIn)
	{
		m_apPlayers[ClientID]->SetTeam(TEAM_SPECTATORS);
		SendBroadcast(ClientID, Localize("You are not logged in. Use /login <username> <password> or /register <username> <password> to join the game.", "Account"));
	}

	// the player must log in or register to bind an account; guest players
	// start with a fresh status
	Server()->ExpireServerInfo();
}

void CGameContext::OnClientConnected(int ClientID, bool Dummy, bool AsSpec)
{
	// The slot must be free, or belong to a player that is still loading its
	// new map. A stale or duplicated callback is ignored instead of trapping.
	if(m_apPlayers[ClientID] && !m_apPlayers[ClientID]->m_MapLoading)
	{
		dbg_msg("game", "client %d connected on a busy slot, ignoring", ClientID);
		return;
	}
	CGameWorld **ppWorld = m_pWorlds.get(Server()->GetClientMapID(ClientID));
	if(!ppWorld || !*ppWorld)
	{
		dbg_msg("game", "client %d connected to a missing world, ignoring", ClientID);
		return;
	}
	CGameWorld *pWorld = *ppWorld;

	if(!m_apPlayers[ClientID])
	{
		// a recycled slot must not inherit the previous occupant's inventory
		Item()->GetInventory(ClientID).Reset();
		m_apPlayers[ClientID] = new(ClientID) CPlayer(pWorld, ClientID, Dummy, AsSpec);
	}
	else if(GetPlayerChar(ClientID))
	{
		vec2 SpawnPos;
		if(m_pController->CanSpawn(m_apPlayers[ClientID]->GameWorld(), m_apPlayers[ClientID]->GetTeam(), &SpawnPos))
		{
			m_apPlayers[ClientID]->GetCharacter()->MoveTo(m_apPlayers[ClientID]->GameWorld(), SpawnPos);
		}
	}
	m_apPlayers[ClientID]->m_MapLoading = false;

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
			else if(pMsg->m_Team != TEAM_SPECTATORS && !pPlayer->m_LoggedIn)
			{
				SendChat(-1, CHAT_ALL, ClientID, Localize("You must be logged in to join the game. Use /login <username> <password>.", "Account"));
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
	pSelf->SendBroadcast(-1, pResult->GetString(0));
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
		g_Localization.Load(pSelf->Config()->m_SvLanguagefile, pSelf->Storage(), pSelf->Console(), true);
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
	m_CommandManager.Init(m_pConsole, this, NewCommandHook, RemoveCommandHook);

	g_Localization.Load(Config()->m_SvLanguagefile, Storage(), Console(), true);

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

	// initialize the item system (loads datasrc/items/*.json)
	m_pItemSystem = new CItemSystem();
	m_pItemSystem->Init(this, Storage());

	// the player owned shop (in-memory listings for this session)
	m_pTradeSystem = new CTradeSystem();
	m_pTradeSystem->Init(this);

	// the configured NPCs (datasrc/bots/npc/*.json)
	m_pNpcSystem = new CNpcSystem();
	m_pNpcSystem->Init(this, Storage());

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
	if(!m_pDatabase || !pPlayer || pPlayer->IsDummy() || !pPlayer->m_LoggedIn || pPlayer->m_AccountUuid == UUID_ZEROED)
		return;

	// snapshot the player state on this thread and let the pool write it
	CDbJob *pJob = new CDbJob();
	pJob->m_Type = CDbJob::TYPE_PLAYER;
	pJob->m_pDB = m_pDatabase;
	pJob->m_Uuid = pPlayer->m_AccountUuid;
	pPlayer->CaptureStatus(*pJob);
	m_pDatabase->QueueJob(pJob);
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
			m_pDatabase->EnsureNpcAggro(pDef->m_BotId, pPlayer->m_AccountUuid);
	}
}

bool CGameContext::BuyFromNpc(int ClientID, int DefIndex, int SellIndex)
{
	if(!m_pNpcSystem || ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return false;
	const SNpcDef *pDef = m_pNpcSystem->DefAt(DefIndex);
	if(!pDef || SellIndex < 0 || SellIndex >= pDef->m_lSells.size())
		return false;

	const SNpcSell &Sell = pDef->m_lSells[SellIndex];
	CItemSystem *pItems = Item();
	if(!pItems->IsKnownItem(Sell.m_Item))
		return false;

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

	char aBuf[192];
	str_format(aBuf, sizeof(aBuf), "Bought %s x%d from %s", pItems->GetName(Sell.m_Item), Sell.m_Count, pDef->m_aName);
	SendChat(-1, CHAT_WHISPER, ClientID, aBuf);
	return true;
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

	delete m_pItemSystem;
	m_pItemSystem = 0;

	delete m_pTradeSystem;
	m_pTradeSystem = 0;

	delete m_pNpcSystem;
	m_pNpcSystem = 0;

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
		mem_copy(pTuning->m_aTuneParams, &m_Tuning, sizeof(pTuning->m_aTuneParams));

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

	if(BotWorld || lpNpcs.size())
	{
		CBotManager *pBots = pNewWorld->BotManager();
		const bool Peaceful = WorldKind == WORLDKIND_HUB || lpNpcs.size();
		pBots->SetBotRole(Peaceful ? CBotManager::BOTROLE_NPC : CBotManager::BOTROLE_HOSTILE);
		for(int i = 0; i < lpNpcs.size(); i++)
			pBots->SpawnNpc(lpNpcs[i]);
	}

	// remember the canonical map name: it is the persistence key of the
	// world saves stored in the database
	str_copy(pNewWorld->m_aMapName, Server()->GetMapName(MapID), sizeof(pNewWorld->m_aMapName));
	// restore the saved state (dropped items etc.) of this world
	pNewWorld->RestoreFromDatabase();

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
		pCollector->m_lToUnload.add(pWorld->WorldID());
	}, &Collector);

	for(int i = 0; i < Collector.m_lToUnload.size(); i++)
		UnloadWorld(Collector.m_lToUnload[i]);
}

IGameServer *CreateGameServer() { return new CGameContext; }
