/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_GAMECONTEXT_H
#define GAME_SERVER_GAMECONTEXT_H

#include <base/tl/hashtable.h>
#include <base/tl/sorted_array.h>
#include <base/uuid.h>

#include <engine/console.h>
#include <engine/server.h>

#include <game/commands.h>
#include <game/voting.h>

#include "gamemenu.h"
#include "gameworld.h"

class CDatabase;
class CItemSystem;
class CNpcSystem;
class CTradeSystem;

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
	unsigned m_MainWorldID;
	hash_table<unsigned, CGameWorld *, 4> m_pWorlds;

	CGameMenu *m_pGameMenu;

	CDatabase *m_pDatabase;

	CItemSystem *m_pItemSystem;

	CTradeSystem *m_pTradeSystem;

	CNpcSystem *m_pNpcSystem;

	CGameMenu *GameMenu() const { return m_pGameMenu; }
	CDatabase *Database() const { return m_pDatabase; }
	CItemSystem *Item() const { return m_pItemSystem; }
	CTradeSystem *Trade() const { return m_pTradeSystem; }
	CNpcSystem *Npcs() const { return m_pNpcSystem; }

	// the always-loaded main world (Connector) and a lookup by map id
	CGameWorld *MainWorld();
	CGameWorld *WorldByID(unsigned MapID);

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

	// helper functions
	class CCharacter *GetPlayerChar(int ClientID);

	int m_LockTeams;

	enum
	{
		MIN_SKINCHANGE_CLIENTVERSION = 0x0703,
		MIN_RACE_CLIENTVERSION = 0x0704,
	};

	// ----- send functions -----
	void SendChat(int ChatterClientID, int Mode, int To, const char *pText);
	void SendBroadcast(int ClientID, const char *pText);
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
	// character on the column it left instead of at a spawn.
	void SwitchPlayerWorld(class CPlayer *pPlayer, unsigned MapID, const vec2 &ArrivalPos = vec2(0.0f, 0.0f), bool HasArrival = false);

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
