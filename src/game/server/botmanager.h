/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#ifndef GAME_SERVER_BOTMANAGER_H
#define GAME_SERVER_BOTMANAGER_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/uuid.h>
#include <base/vmath.h>

#include <engine/shared/protocol.h>
#include <generated/protocol.h>

#include <game/server/bot.h>

class CGameContext;
class CGameController;
class CGameWorld;
class CBotEntity;
class IBotEntity;
struct CBotTeeInfo;

/*
	Class: CBotManager
		Owns one world's bots and maps their stable BotIDs (Uuid) to the
		integer tee ids clients are shown.

		The mapping is per receiving client, because the id range depends on
		that client's protocol and because a client is only shown the bots
		nearest to it, letting a server run far more bots than any one client
		could be told about. The BotID is the identity; the mapped tee id is a
		display detail that may change.

		MAX_CLIENTS is untouched: a bot never owns a client slot or a CPlayer.
*/
class CBotManager
{
public:
	// Factory for bot variants: builds one bot and returns it, or null to
	// refuse. pUser is the value passed to SetBotTypeCreator.
	typedef IBotEntity *(*FBotCreator)(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team, void *pUser);

private:
	CGameWorld *m_pGameWorld;

	// Every live bot by BotID. A bot unlinks itself the moment it dies, so
	// this never holds a destroyed entity.
	hash_table<Uuid, IBotEntity *, 8, uuid_table_function> m_Bots;
	// The same bots in spawn order. hash_table has no indexed iteration, and
	// this keeps the per-client id assignment (and demo tee ids) deterministic
	// rather than dependent on bucket layout.
	array<Uuid> m_lBotOrder;

	// Per receiving client, the bot in each of that client's bot slots. Index
	// 0 is BOT_TEEID_07_MIN, or BOT_TEEID_08_MIN for a 0.8 client, so one
	// index resolves to a different tee id per protocol.
	Uuid m_aaBotIDMaps[MAX_CLIENTS][BOT_MAX_PER_CLIENT];

	// The bot kind spawned by Tick(); see RegisterBotType.
	FBotCreator m_pfnBotCreator;
	void *m_pBotCreatorUser;
	char m_aBotType[32];

	// How many bots this world wants to keep alive.
	int DesiredBotCount() const;

	void ClearPlayerMap(int ClientID);
	// Recomputes one client's bot slots and emits the identity messages for
	// every slot that changed.
	void UpdatePlayerMap(int ClientID);

	// Emits the 0.7 identity of a bot to one client, or its removal. Both are
	// no-ops for a 0.8 client; see the definitions.
	void SendBotInfo(int ClientID, int TeeID, IBotEntity *pBot);
	void SendBotDrop(int ClientID, int TeeID);

	// Whether this client speaks 0.7. A 0.7 peer learns about a tee only from
	// Sv_ClientEnter/Sv_ClientDrop; a 0.8 peer reads TeeInfo directly.
	bool ClientIsLegacy(int ClientID) const;

public:
	class CConfig *Config() const;
	CGameContext *GameServer() const;
	CGameWorld *GameWorld() const { return m_pGameWorld; }
	CGameController *GameController() const;
	class IServer *Server() const;

	CBotManager(CGameWorld *pGameWorld);
	~CBotManager();

	void Tick();
	void PostSnap();

	// --- spawning ---------------------------------------------------------
	// Spawns one bot of the configured kind at a valid spawn point, or null
	// when no spawn point is free.
	IBotEntity *CreateBot();
	// Spawns one bot with an explicit BotID and position.
	IBotEntity *CreateBot(Uuid BotID, vec2 Pos);
	// Removes every bot (world reset / shutdown).
	void ClearBots();

	// --- bot kind registry ------------------------------------------------
	// Installs the factory used by CreateBot(). pTypeName must outlive the
	// manager (a string literal).
	void SetBotTypeCreator(const char *pTypeName, FBotCreator pfnCreator, void *pUser = nullptr);
	const char *BotTypeName() const { return m_aBotType; }

	// --- lookup -----------------------------------------------------------
	// The tee id the given client sees this bot as, or -1 when it is not
	// being shown the bot at all.
	int FindClientID(int SnappingClient, Uuid BotID) const;
	// The bot in one of a client's slots, or null. Slot is 0-based within the
	// range that fits the client's protocol.
	IBotEntity *BotForSlot(int ClientID, int Slot) const;
	// The bot behind a BotID, or null.
	IBotEntity *BotByID(Uuid BotID) const;
	// Number of bots a client can currently be shown.
	int NumVisibleBots(int ClientID) const;
	int NumBots() const { return m_lBotOrder.size(); }

	// --- events -----------------------------------------------------------
	// Emits a damage indicator to every client that can see the bot.
	void CreateDamage(vec2 Pos, Uuid BotID, vec2 Source, int HealthAmount, int ArmorAmount, bool Self);
	// Emits a death event to every client that can see the bot.
	void CreateDeath(vec2 Pos, Uuid BotID);

	// Called by a bot right before it is destroyed: it unlinks immediately,
	// because the world deletes the entity at the end of the tick and no later
	// lookup may see a dangling pointer.
	void OnBotDeath(Uuid BotID);
	// Called when a client enters the game, so its stale mapping is dropped
	// and the bot identities are re-sent from scratch.
	void OnClientRefresh(int ClientID);

	/*
		Function: TranslateTeeIDForClient
			Rewrites a server-internal TeeID into the id the given client knows
			that tee by.

		The server keys CWorldCore by a bot's own fixed id (IBotEntity::TeeID),
		while the client is told an id from the range that fits its protocol.
		Every field written into a snapshot is therefore in the server's space
		and has to come through here. m_HookedPlayer is the one that matters: a
		client silently drops a hook whose target it cannot resolve.

		Real clients use the same slot in both spaces and pass through
		unchanged, as does a value with no tee behind it (-1, -2, ...).

		Returns:
			The id to snap for SnappingClient, or -1 when the tee is not visible
			to it, which drops the reference rather than naming the wrong tee.
	*/
	int TranslateTeeIDForClient(int SnappingClient, int TeeID) const;
};

#endif // GAME_SERVER_BOTMANAGER_H
