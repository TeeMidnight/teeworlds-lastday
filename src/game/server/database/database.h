/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_DATABASE_DATABASE_H
#define GAME_SERVER_DATABASE_DATABASE_H

// Player state is stored one row per player: only the identity columns (uuid,
// username, password) are fixed, everything else lives in a JSON "data" column
// so the schema stays stable as the survival systems evolve. World state is
// stored the same way, one row per map name.

#include <base/system.h>
#include <base/tl/array.h>
#include <base/uuid.h>
#include <game/server/database/jsonpath.h>

class IEngine;
struct CDbJob;

class CDatabase
{
protected:
	// Every backend keeps one connection and none of them is thread safe. All
	// public entry points take this lock, so a job running on the thread pool
	// can use the database while the game thread does too.
	LOCK m_Lock;

	// Queued writes run on the engine's job pool, one at a time in submission
	// order: a tick never blocks on the database, and two saves for the same
	// player cannot interleave. Reads stay synchronous because their result is
	// needed immediately.
	class IEngine *m_pEngine;
	array<struct CDbJob *> m_lDbJobs;

	// dispatch the front job unless one is already in flight
	void DispatchJob();

public:
	enum
	{
		// fixed-size buffers for the identity columns
		USERNAME_SIZE = 32,
		PASSWORD_SIZE = 128,
		INVENTORY_JSON_SIZE = 2048,
	};

	// a single player row; the primary key uses the engine's native Uuid type
	struct SPlayerData
	{
		Uuid m_Uuid;
		char m_aUsername[USERNAME_SIZE];
		char m_aPasswordHash[PASSWORD_SIZE];
	};

	// a guild-style team row. Members live in team_members, one account per
	// team; the list is unlimited.
	struct STeamData
	{
		int m_Id;
		char m_aName[64];
		Uuid m_Leader;
		bool m_FriendlyFire;
	};

	// one persisted shop listing. The seller is an account uuid, so a listing
	// outlives the session that posted it, and the item ids are stored by their
	// source name ("lastday:stone") so they survive an id rehash.
	struct SShopListing
	{
		int m_Id;
		Uuid m_Seller;
		char m_aOfferItem[64];
		int m_OfferCount;
		char m_aPriceItem[64];
		int m_PriceCount;
	};

	// configuration selected at startup from the sv_db_* cvars
	struct SConfig
	{
		const char *m_pBackend; // "sqlite" or "postgres"
		const char *m_pPath; // sqlite database file path
		const char *m_pConnString; // postgres connection string
	};

	CDatabase()
	{
		m_Lock = lock_create();
		m_pEngine = 0;
	}
	virtual ~CDatabase();

	// open the connection and create the schema if it does not exist yet.
	// returns true when the database is ready to use.
	virtual bool Init() = 0;

	// find a player by username (used by the login flow). returns true and
	// fills `out` when the player exists.
	virtual bool FindByName(const char *pUsername, SPlayerData &out) = 0;

	// register a brand-new player row (data column starts as '{}').
	virtual bool InsertPlayer(const SPlayerData &Player) = 0;

	// Ensure a guest profile row exists. Guests have no password and are keyed
	// by a UUID derived from their nickname, so their inventory and loadout
	// survive a reconnect without becoming a real account. Returns true when
	// the row was created by this call (i.e. the guest is new).
	virtual bool EnsureGuestPlayer(const Uuid &Uuid, const char *pUsername) = 0;

	// --- teams ------------------------------------------------------------
	// Create a team and add its leader as the first member, atomically enough
	// for the game thread's usage (a failure deletes the half-created row).
	virtual bool CreateTeam(const char *pName, const Uuid &Leader, bool FriendlyFire, STeamData &out) = 0;
	virtual bool FindTeamByName(const char *pName, STeamData &out) = 0;
	virtual bool FindTeamForMember(const Uuid &Account, STeamData &out) = 0;
	virtual bool ListTeams(array<STeamData> &out) = 0;
	virtual bool LoadTeamMembers(int TeamId, array<Uuid> &out) = 0;
	// Moves the account into this team (one account, one team).
	virtual bool AddTeamMember(int TeamId, const Uuid &Account) = 0;
	virtual bool RemoveTeamMember(int TeamId, const Uuid &Account) = 0;
	virtual bool DeleteTeam(int TeamId) = 0;
	virtual bool SetTeamLeader(int TeamId, const Uuid &Leader) = 0;
	virtual bool SetTeamFriendlyFire(int TeamId, bool FriendlyFire) = 0;

	// load a player row by its primary key.
	virtual bool LoadByUuid(const Uuid &Uuid, SPlayerData &out) = 0;

	// read a single field of the player's JSON "data" column. on success the
	// value (as plain text) is copied into `pOut`; returns false when the
	// field does not exist.
	virtual bool GetJson(const Uuid &Uuid, const CJsonPath &Path, char *pOut, int Size) = 0;

	// write a single field of the player's JSON "data" column. `pValue` is a
	// json value (e.g. "5", "\"text\"", "{\"stone\":5}"); the field is created
	// if it does not exist.
	virtual bool SetJson(const Uuid &Uuid, const CJsonPath &Path, const char *pValue) = 0;

	// remove a single field of the player's JSON "data" column. a no-op when
	// the field does not exist.
	virtual bool DelJson(const Uuid &Uuid, const CJsonPath &Path) = 0;

	// get the length of a json array at `Path` (e.g. "inventory"). returns
	// false when the field does not exist or is not an array.
	virtual bool GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength) = 0;

	// -------- npc aggro ledger --------
	// Aggro is kept as a dated ledger of signed deltas, so it can be audited
	// and reconstructed by summing. A pair with no rows sums to 0.
	// Sum every ledger row of one (npc, user) pair into *pOut. Returns false
	// only on a database failure.
	virtual bool GetNpcAggro(const Uuid &Npc, const Uuid &User, int *pOut) = 0;
	// Append one dated delta (positive adds aggro, negative decays it).
	virtual bool AddNpcAggro(const Uuid &Npc, const Uuid &User, int Delta) = 0;
	// Give the pair a 0 baseline row when it has none, so a freshly joined
	// player is recorded with the default 0 aggro.
	virtual bool EnsureNpcAggro(const Uuid &Npc, const Uuid &User) = 0;

	// -------- shop listings --------
	// The player listings of the unified shop survive a restart: the escrowed
	// goods were already taken out of the seller's inventory when the listing
	// was posted, so the row has to outlive the session too.
	virtual bool LoadShopListings(array<SShopListing> &out) = 0;
	virtual bool SaveShopListing(const SShopListing &Listing) = 0;
	virtual bool DeleteShopListing(int Id) = 0;

	// -------- npc shelves --------
	// A configured NPC's shelf is mirrored into SQL when its json is loaded, so
	// the shop data is queryable even though the json stays the authoring
	// source. Loading clears the NPC's rows first, so a removed entry is gone.
	virtual bool ClearNpcSells(const Uuid &Npc) = 0;
	virtual bool SaveNpcSell(const Uuid &Npc, const char *pItem, int Count, const char *pPriceItem, int PriceCount) = 0;

	// runtime stock of one shelf entry: how much is left and when it next
	// restocks (unix seconds). Get returns false when the entry has no row yet.
	virtual bool GetNpcStock(const Uuid &Npc, const char *pItem, const char *pPriceItem, int *pStock, int *pRestockAt) = 0;
	virtual bool SaveNpcStock(const Uuid &Npc, const char *pItem, const char *pPriceItem, int Stock, int RestockAt) = 0;

	// -------- world saves (world persistence) --------
	// one row per world instance (one map name, e.g. "<floor>_<seed>" for
	// generated worlds or the plain name for static maps), so a world's
	// whole saved state lives in one json "data" document that can hold
	// arbitrary content: dropped items under "drop" (an array of pickups),
	// and any future world state under its own field. The map name is
	// unique; "id" is only the database's internal auto-increment key.
	// Rows survive server restarts, so world content can be restored when a
	// world is loaded again.
	//
	// The document is written as a whole (typically delete the old row
	// first, then SaveWorldSave), so no per-field accessors are needed here.

	// write the whole world document of the map in one go (inserts the row,
	// or replaces the data of an existing one); pJsonData must be valid json
	virtual bool SaveWorldSave(const char *pMap, const char *pJsonData) = 0;
	// size in bytes of the map's json document; returns false when the row does
	// not exist. Used to size the read buffer instead of guessing a maximum.
	virtual bool GetWorldSaveSize(const char *pMap, int *pSize) = 0;
	// read the whole "data" json of the row of the map; returns false when
	// the row does not exist
	virtual bool GetWorldSaveData(const char *pMap, char *pOut, int Size) = 0;
	// remove the whole row of the map; a no-op when it does not exist
	virtual bool DeleteWorldSave(const char *pMap) = 0;

	// ----- queued writes -----
	// hand the engine's job pool to the database; without one the writes run
	// synchronously, which keeps a test harness correct
	void SetJobPool(class IEngine *pEngine) { m_pEngine = pEngine; }
	// take ownership of a prepared job and dispatch it when its turn comes
	void QueueJob(struct CDbJob *pJob);
	// snapshot a whole-world write and queue it
	void QueueWorldSave(const char *pMap, const char *pJson, bool HasJson);
	// free the queued jobs that finished; call once per tick
	void ReapJobs();
	// block until every queued job finished; false when they did not in time
	bool DrainJobs();
};

// scoped lock for the database; backends hold it for the duration of a call
class CDatabaseLock
{
	LOCK m_Lock;

public:
	explicit CDatabaseLock(LOCK Lock) : m_Lock(Lock) { lock_wait(m_Lock); }
	~CDatabaseLock() { lock_unlock(m_Lock); }
};

// create the backend selected by `config`; returns nullptr when the backend
// name is unknown.
CDatabase *CreateDatabase(const CDatabase::SConfig &Config);

#endif
