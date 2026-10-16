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

class CConfig;
class CGameContext;
class CGameController;
class CGameWorld;
class CBotEntity;
class IBotEntity;
struct CBotTeeInfo;
struct SNpcDef;

/*
	Class: CBotManager
		Owns every world's bots and maps their stable BotIDs (Uuid) to the
		integer tee ids clients are shown.

		There is exactly one manager for the whole game, owned by CGameContext.
		A world used to own one, which meant two live ring neighbours each
		numbered their bots from the same per-client slot range: a client shown
		both worlds could not be told their bots apart. A single manager keeps a
		global per-client table, so a bot of any live world resolves to a
		distinct, stable id and the neighbour snapshot mirror can find it.

		The mapping is per receiving client, because the id range depends on
		that client's protocol and because a client is only shown the bots
		nearest to it, letting a server run far more bots than any one client
		could be told about. The BotID is the identity; the mapped tee id is a
		display detail that may change.

		A bot also needs a private id for the CWorldCore it registers its
		physics core in. PickTeeID hands those out from the same range, kept
		globally unique across worlds, so the internal id alone identifies a bot
		wherever it currently lives.

		MAX_CLIENTS is untouched: a bot never owns a client slot or a CPlayer.
*/
class CBotManager
{
public:
	CBotManager(CGameContext *pGameServer);
	~CBotManager();

	CConfig *Config() const;
	CGameContext *GameServer() const { return m_pGameServer; }
	CGameController *GameController() const;
	class IServer *Server() const;

	// Advance every configured world: respawn its defined NPCs or top up its
	// random hostile population.
	void Tick();
	// Refresh the per-client slot tables after the tick's snapshots.
	void PostSnap();

	// --- spawning ---------------------------------------------------------
	// Spawns one bot of the world's configured kind at a valid spawn point, or
	// null when no spawn point is free.
	IBotEntity *CreateBot(CGameWorld *pWorld);
	// Spawns one bot with an explicit BotID and position.
	IBotEntity *CreateBot(CGameWorld *pWorld, Uuid BotID, vec2 Pos);
	// Spawns one configured NPC from its definition, at its own position and
	// with its own fixed identity. A world that owns defined NPCs does not top
	// up with random bots.
	IBotEntity *SpawnNpc(CGameWorld *pWorld, const SNpcDef *pDef);
	// Puts a defined NPC back into the world after its death sequence.
	IBotEntity *RespawnNpc(CGameWorld *pWorld, const SNpcDef *pDef) { return SpawnNpc(pWorld, pDef); }
	// Removes every bot living in one world (world unload).
	void ClearBots(CGameWorld *pWorld);
	// Removes every bot of every world (shutdown).
	void ClearBots();

	// --- identity ---------------------------------------------------------
	// A free private tee id for a new bot, derived from BotID so a bot keeps
	// its slot across a respawn. Globally unique among the live bots of every
	// world, so the id alone names the bot.
	int PickTeeID(Uuid BotID) const;

	// --- world handover ---------------------------------------------------
	// Detach a live bot without destroying it, so it can be adopted after a
	// move to another world (NPC respawn teleport, ring crossing).
	void ReleaseBot(Uuid BotID);
	// Register a live bot that was released from another world.
	void AdoptBot(IBotEntity *pBot);
	// Emit an Sv_Emoticon for a bot to every client that can see it.
	void SendBotEmoticon(Uuid BotID, int Emoticon);

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

private:
	CGameContext *m_pGameServer;

	// Every live bot of every world, by BotID. A bot unlinks itself the moment
	// it dies, so this never holds a destroyed entity.
	hash_table<Uuid, IBotEntity *, 8, uuid_table_function> m_Bots;
	// The same bots in spawn order. hash_table has no indexed iteration, and
	// this keeps the per-client id assignment (and demo tee ids) deterministic
	// rather than dependent on bucket layout.
	array<Uuid> m_lBotOrder;
	// The same bots by their globally unique internal tee id, so translating a
	// snapshot tee id does not scan every bot for every item of every client.
	hash_table<int, IBotEntity *, 8> m_BotsByTeeID;

	// Per receiving client, the bot in each of that client's bot slots. Index
	// 0 is BOT_TEEID_07_MIN, or BOT_TEEID_08_MIN for a 0.8 client, so one
	// index resolves to a different tee id per protocol. A slot only ever
	// holds a bot of the client's own world or of a live ring neighbour.
	Uuid m_aaBotIDMaps[MAX_CLIENTS][BOT_MAX_PER_CLIENT];

	// Bots this world wants, capped by what one client can be shown.
	int DesiredBotCount() const;
	// Top up one world's population.
	void TickWorld(CGameWorld *pWorld);
	int LiveBotsInWorld(CGameWorld *pWorld) const;

	void ClearPlayerMap(int ClientID);
	// Drop every slot pointing at one bot.
	void RemoveFromPlayerMaps(Uuid BotID);
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
};

#endif // GAME_SERVER_BOTMANAGER_H
