/* LastDay database abstraction.
 *
 * "players" stores one row per player. Only the identity columns (uuid,
 * username, password) are fixed; every other piece of player state
 * (inventory, sanity, and any future survival stat) is serialized into a
 * single JSON column ("data") and accessed through json paths. This keeps
 * the schema stable while the survival systems keep evolving.
 *
 * "world_saves" stores one row per world instance (unique map name). The
 * row's "data" is an evolving JSON document that can hold arbitrary world
 * content: an array of dropped items under "drop", and whatever other
 * persistent world state appears later, each under its own field and
 * accessed through the same json path helpers as the player data.
 */
#ifndef GAME_SERVER_DATABASE_H
#define GAME_SERVER_DATABASE_H

#include <base/system.h>
#include <base/uuid.h>

#include <game/server/database/jsonpath.h>

class CDatabase
{
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

	// configuration selected at startup from the sv_db_* cvars
	struct SConfig
	{
		const char *m_pBackend; // "sqlite" or "postgres"
		const char *m_pPath; // sqlite database file path
		const char *m_pConnString; // postgres connection string
	};

	CDatabase() {}
	virtual ~CDatabase() {}

	// open the connection and create the schema if it does not exist yet.
	// returns true when the database is ready to use.
	virtual bool Init() = 0;

	// find a player by username (used by the login flow). returns true and
	// fills `out` when the player exists.
	virtual bool FindByName(const char *pUsername, SPlayerData &out) = 0;

	// register a brand-new player row (data column starts as '{}').
	virtual bool InsertPlayer(const SPlayerData &Player) = 0;

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
	// read the whole "data" json of the row of the map; returns false when
	// the row does not exist
	virtual bool GetWorldSaveData(const char *pMap, char *pOut, int Size) = 0;
	// remove the whole row of the map; a no-op when it does not exist
	virtual bool DeleteWorldSave(const char *pMap) = 0;
};

// create the backend selected by `config`; returns nullptr when the backend
// name is unknown.
CDatabase *CreateDatabase(const CDatabase::SConfig &Config);

#endif
