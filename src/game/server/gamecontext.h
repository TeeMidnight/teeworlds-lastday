/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_GAMECONTEXT_H
#define GAME_SERVER_GAMECONTEXT_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/tl/sorted_array.h>
#include <base/uuid.h>

#include <engine/console.h>
#include <engine/server.h>

#include <game/commands.h>
#include <game/voting.h>

#include <string>

#include "gamemenu.h"
#include "gameworld.h"

class CBotManager;
class CDatabase;
class CItemSystem;
class CContainerSystem;
class CContainerEntity;
class CNpcSystem;
class CMobSystem;
class CTradeSystem;
class CScriptedEntitySystem;

// Broadcast life span in ticks. A timed message stays on screen for the whole
// span; a realtime message (life span 0) is only visible while it is submitted
// again, which is how a constantly refreshed hint is kept alive.
#define BROADCAST_DURATION_REALTIME (0)
#define BROADCAST_DURATION_GAMEANNOUNCE (Server()->TickSpeed() * 2)

// Higher priorities win while their message is active. A realtime message is
// replaced by a timed message of a higher priority and comes back when that
// one expires.
enum
{
	BROADCAST_PRIORITY_LOWEST = 0,
	BROADCAST_PRIORITY_RESOURCEINFO,
	BROADCAST_PRIORITY_WEAPONSTATE,
	BROADCAST_PRIORITY_EFFECTSTATE,
	BROADCAST_PRIORITY_GAMEANNOUNCE,
	BROADCAST_PRIORITY_SERVERANNOUNCE,
	BROADCAST_PRIORITY_INTERFACE,
};

/*
	Tick
		Game Context (CGameContext::tick)
			Game World (GAMEWORLD::tick)
				Reset world if requested (GAMEWORLD::reset)
				All entities in the world (ENTITY::tick)
				All entities in the world (ENTITY::tick_defered)
				Remove entities marked for deletion (GAMEWORLD::remove_entities)
			Game Controller (GAMECONTROLLER::tick)
			All players (CPlayer::tick)


	Snap
		Game Context (CGameContext::snap)
			Game World (GAMEWORLD::snap)
				All entities in the world (ENTITY::snap)
			Game Controller (GAMECONTROLLER::snap)
			Events handler (EVENT_HANDLER::snap)
			All players (CPlayer::snap)

*/
class CGameContext : public IGameServer
{
	IServer *m_pServer;
	class CConfig *m_pConfig;
	class IConsole *m_pConsole;
	class IStorage *m_pStorage;
	CNetObjHandler m_NetObjHandler;
	CTuningParams m_Tuning;

	static void ConTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneReset(IConsole::IResult *pResult, void *pUserData);
	static void ConTunes(IConsole::IResult *pResult, void *pUserData);
	static void ConSay(IConsole::IResult *pResult, void *pUserData);
	static void ConBroadcast(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeam(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeamAll(IConsole::IResult *pResult, void *pUserData);
	static void ConAddVote(IConsole::IResult *pResult, void *pUserData);
	static void ConRemoveVote(IConsole::IResult *pResult, void *pUserData);
	static void ConClearVotes(IConsole::IResult *pResult, void *pUserData);
	static void ConVote(IConsole::IResult *pResult, void *pUserData);
	static void ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSettingUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainLanguageUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	static void NewCommandHook(const CCommandManager::CCommand *pCommand, void *pContext);
	static void RemoveCommandHook(const CCommandManager::CCommand *pCommand, void *pContext);

	// One client's broadcast queue. The message currently on screen is kept so
	// a repeated realtime message is only resent when it changes (or once a
	// second, to fight the client's auto fade); a timed message has a life span
	// and is restored when it expires.
	class CBroadcastState
	{
	public:
		int m_NoChangeTick;
		char m_aPrevMessage[1024];

		int m_Priority;
		char m_aNextMessage[1024];

		int m_LifeSpanTick;
		int m_TimedPriority;
		char m_aTimedMessage[1024];
	};
	CBroadcastState m_aBroadcastStates[MAX_CLIENTS];

	// loaded from data/tips.json; the strings are localization keys
	array<std::string> m_lTips;
	int m_TipTimer;
	int m_LastTip;

	void LoadTips();
	void TickTips();

	void AddBroadcast(int ClientID, const char *pText, int Priority, int LifeSpan);
	void ResetBroadcastState(int ClientID);
	void FlushBroadcasts();
	// submit the resource or container under this client's cursor as a
	// realtime broadcast
	void UpdateCursorResourceBroadcast(int ClientID);

	CGameContext(int Resetting);
	void Construct(int Resetting);

	bool m_Resetting;

public:
	IServer *Server() const { return m_pServer; }
	class CConfig *Config() { return m_pConfig; }
	class IConsole *Console() { return m_pConsole; }
	class IStorage *Storage() { return m_pStorage; }
	CTuningParams *Tuning() { return &m_Tuning; }

	CGameContext();
	~CGameContext();

	void Clear();

	class CPlayer *m_apPlayers[MAX_CLIENTS];

	class CGameController *m_pController;
	// The one bot manager of the whole game. It owns every world's bots and
	// the per-client tee id mapping, so two live ring neighbours can never
	// hand a client the same bot slot.
	class CBotManager *m_pBotManager;
	unsigned m_MainWorldID;
	hash_table<unsigned, CGameWorld *, 4> m_pWorlds;

	CGameMenu *m_pGameMenu;

	CDatabase *m_pDatabase;

	CItemSystem *m_pItemSystem;

	CContainerSystem *m_pContainerSystem;

	CTradeSystem *m_pTradeSystem;

	CNpcSystem *m_pNpcSystem;
	// data/mobs/*.json: content-defined factions, abilities and fixed spawns
	CMobSystem *m_pMobSystem;

	class CBuffSystem *m_pBuffSystem;

	// generic scripted props (data/entities/*.json + scripts/entities/*.py)
	class CScriptedEntitySystem *m_pScriptedEntities;

	CGameMenu *GameMenu() const { return m_pGameMenu; }
	CDatabase *Database() const { return m_pDatabase; }
	CItemSystem *Item() const { return m_pItemSystem; }
	CContainerSystem *Containers() const { return m_pContainerSystem; }
	CTradeSystem *Trade() const { return m_pTradeSystem; }
	CNpcSystem *Npcs() const { return m_pNpcSystem; }
	CMobSystem *Mobs() const { return m_pMobSystem; }
	class CBuffSystem *Buffs() const { return m_pBuffSystem; }
	class CScriptedEntitySystem *ScriptedEntities() const { return m_pScriptedEntities; }

	// the global bot manager (null before OnInit)
	class CBotManager *BotManager() const { return m_pBotManager; }

	// the always-loaded main world (Connector) and a lookup by map id
	CGameWorld *MainWorld();
	CGameWorld *WorldByID(unsigned MapID);

	// The world of a map, loading (and generating) its map without switching any
	// client's map when it does not exist yet. Null when the map cannot be
	// loaded. Used to bring a ring neighbour in.
	CGameWorld *EnsureWorld(unsigned MapID);

	// whether a ring world with a player has the given map as a live neighbour
	bool HasRingNeighbour(unsigned MapID) const;

	// whether an occupied world has an entrance pointing at the given map
	bool HasEntranceTarget(unsigned MapID) const;

	// Make sure the live neighbours of every occupied ring world exist, so the
	// single owner nodes and entities in the shared strip can be reached.
	void EnsureOccupiedRingNeighbours();

	// Preload the target world of every entrance in an occupied world. Entrance
	// switches are synchronous like ring crossings, so a world that is only
	// discovered at the moment the player walks into the entrance would stall
	// that tick and, if generation fails, drop the switch entirely.
	void EnsureOccupiedEntrances();

	// apply a signed aggro delta to the (npc, account) ledger
	void NpcAddAggro(const Uuid &Npc, const Uuid &User, int Delta);
	// sum the ledger of one (npc, account) pair; 0 when absent or no database
	int NpcGetAggro(const Uuid &Npc, const Uuid &User);
	// record the default 0 aggro of one player with every configured NPC
	void EnsureNpcAggroFor(class CPlayer *pPlayer);

	// buy one entry of a configured NPC's shelf for a client (the NPC shop is
	// part of the same menu as the player listings)
	bool BuyFromNpc(int ClientID, int DefIndex, int SellIndex);
	// whether the configured NPC (by definition index) and its entity are close
	// enough to the client to trade with
	bool NpcInRange(int ClientID, int DefIndex);

	// persist a player's status to the database (only when the player is
	// logged in to an account); the write is queued on the database job pool
	void SavePlayerData(class CPlayer *pPlayer);

	// remove Count copies of pItemId from the player's inventory and spawn
	// a CDroppedPickup at the player, thrown with the given initial velocity
	// (units per tick). Returns false (with a chat message) on failure.
	bool DropItem(int ClientID, const char *pItemId, int Count, vec2 Direction);

	CCommandManager m_CommandManager;

	CCommandManager *CommandManager() { return &m_CommandManager; }

	// The container the player is aiming at and is close enough to use, or null.
	// Both the main menu entry and the /container command resolve their target
	// through this, so they can never disagree.
	CContainerEntity *AimedContainer(int ClientID);

	// helper functions
	class CCharacter *GetPlayerChar(int ClientID);
	// localization language of a client, or the server default
	int LanguageOf(int ClientID) const;

	int m_LockTeams;

	enum
	{
		MIN_SKINCHANGE_CLIENTVERSION = 0x0703,
		MIN_RACE_CLIENTVERSION = 0x0704,
	};

	// ----- send functions -----
	void SendChat(int ChatterClientID, int Mode, int To, const char *pText);
	// ClientID == -1 sends to every player. A timed message (LifeSpan > 0)
	// expires on its own; a realtime message (LifeSpan == 0) lasts until it is
	// replaced, so it must be submitted again when it should stay on screen.
	void SendBroadcast(int ClientID, const char *pText, int Priority, int LifeSpan);
	void ClearBroadcast(int ClientID, int Priority);
	void SendEmoticon(int ClientID, int Emoticon);
	void SendWeaponPickup(int ClientID, int Weapon);
	void SendMotd(int ClientID);
	void SendSettings(int ClientID);
	void SendSkinChange(int ClientID, int TargetID);
	void SendTuningParams(int ClientID);
	void SendReadyToEnter(CPlayer *pPlayer);
	void SendSoundTarget(int ClientID, int SoundID);

	void SendGameMsg(int GameMsgID, int ClientID);
	void SendGameMsg(int GameMsgID, int ParaI1, int ClientID);
	void SendGameMsg(int GameMsgID, int ParaI1, int ParaI2, int ParaI3, int ClientID);

	void SendChatCommand(const CCommandManager::CCommand *pCommand, int ClientID);
	void SendChatCommands(int ClientID);
	void SendRemoveChatCommand(const CCommandManager::CCommand *pCommand, int ClientID);

	//
	// MapID == the current world moves the character to a spawn. ArrivalPos is
	// only meaningful with HasArrival, which a ring crossing uses to land the
	// character on the column it left instead of at a spawn. TrainArrival arms
	// the short invincibility window that starts once the new map has loaded.
	void SwitchPlayerWorld(class CPlayer *pPlayer, unsigned MapID, const vec2 &ArrivalPos = vec2(0.0f, 0.0f), bool HasArrival = false, bool TrainArrival = false);

	// Whether the player is standing on a TILE_TRAIN_ENTRANCE platform. The
	// travel menu is only usable there; the tile itself never teleports.
	bool IsOnTrainEntrance(class CPlayer *pPlayer) const;

	// engine events
	virtual void OnInit();
	virtual void OnConsoleInit();
	virtual void OnShutdown();

	virtual void OnTick();
	virtual void OnPreSnap();
	virtual void OnSnap(int ClientID);
	virtual void OnPostSnap();

	virtual void OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID);

	virtual void OnClientConnected(int ClientID, bool AsSpec) { OnClientConnected(ClientID, false, AsSpec); }
	void OnClientConnected(int ClientID, bool Dummy, bool AsSpec);
	virtual void OnClientMapDataRequest(int ClientID);
	void OnClientTeamChange(int ClientID);
	virtual void OnClientEnter(int ClientID);
	virtual void OnClientDrop(int ClientID, const char *pReason);
	virtual void OnClientDirectInput(int ClientID, void *pInput);
	virtual void OnClientPredictedInput(int ClientID, void *pInput);

	virtual bool IsClientBot(int ClientID) const;
	virtual bool IsClientReady(int ClientID) const;
	virtual bool IsClientPlayer(int ClientID) const;
	virtual bool IsClientSpectator(int ClientID) const;

	virtual const char *GameType() const;
	virtual const char *Version() const;
	virtual const char *NetVersion() const;
	virtual const char *NetVersionHashUsed() const;
	virtual const char *NetVersionHashReal() const;

	virtual bool TimeScore() const;
	virtual void OnUpdatePlayerServerInfo(CJsonWriter *pJsonWriter, int ClientID);

	virtual int GetMaxPlayerSlots();
	virtual void RequestLoadWorld(unsigned MapID);

	// unloads worlds that no player is currently in, to free up memory
	// when the server is idle (called periodically from OnTick)
	void UnloadIdleWorlds();
	bool UnloadWorld(unsigned MapID);

protected:
	virtual void OnGameMenuInit();
};

inline int64 CmaskAll() { return -1; }
inline int64 CmaskOne(int ClientID) { return (int64) 1 << ClientID; }
inline int64 CmaskAllExceptOne(int ClientID) { return CmaskAll() ^ CmaskOne(ClientID); }
inline bool CmaskIsSet(int64 Mask, int ClientID) { return (Mask & CmaskOne(ClientID)) != 0; }

const char *Localize(const char *pStr, const char *pContext = "")
	GNUC_ATTRIBUTE((format_arg(1)));

#endif
