/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* SQLite (embedded) backend for the database. */
#include <base/system.h>
#include <game/server/currency.h>
#include <game/server/database/database_sqlite.h>

#include <sqlite3.h>

// convert a dot-notation json path ("inventory.0.count") into a SQLite
// json path ("$.inventory[0].count")
static bool BuildJsonPath(const CJsonPath &Path, char *pOut, int Size)
{
	int Offset = 0;
	if(Offset + 1 < Size)
		pOut[Offset++] = '$';
	else
		return false;

	const char *p = Path.c_str();
	while(*p)
	{
		const char *pStart = p;
		while(*p && *p != '.')
			p++;
		const int SegLen = (int) (p - pStart);
		if(SegLen == 0)
			return false;

		bool Numeric = true;
		for(int i = 0; i < SegLen; i++)
			if(pStart[i] < '0' || pStart[i] > '9')
			{
				Numeric = false;
				break;
			}

		if(Offset + 1 + SegLen + (Numeric ? 1 : 0) >= Size)
			return false;

		pOut[Offset++] = Numeric ? '[' : '.';
		mem_copy(pOut + Offset, pStart, SegLen);
		Offset += SegLen;
		if(Numeric)
			pOut[Offset++] = ']';

		if(*p == '.')
			p++;
	}
	pOut[Offset] = '\0';
	return true;
}

// whether an existing table has a column; used to recognise the old barter
// shop schema and migrate it once instead of silently misreading it
static bool SqliteTableHasColumn(sqlite3 *pDB, const char *pTable, const char *pColumn)
{
	char aSql[128];
	str_format(aSql, sizeof(aSql), "PRAGMA table_info(\"%s\");", pTable);
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(pDB, aSql, -1, &pStmt, 0) != SQLITE_OK)
		return false;

	bool Found = false;
	while(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		const unsigned char *pName = sqlite3_column_text(pStmt, 1);
		if(pName && str_comp((const char *) pName, pColumn) == 0)
		{
			Found = true;
			break;
		}
	}
	sqlite3_finalize(pStmt);
	return Found;
}

// The shop was a barter market before currency: shop_listings carried
// price_item/price_count and the npc tables keyed their stock by the price
// item. None of those rows can be converted to G, so the old listing table is
// preserved under a different name for manual recovery and the derived npc
// tables are dropped (they are rebuilt from the npc json on every start).
static void SqliteMigrateLegacyShopSchema(sqlite3 *pDB)
{
	if(SqliteTableHasColumn(pDB, "shop_listings", "price_item") && !SqliteTableHasColumn(pDB, "shop_listings", "price"))
	{
		if(sqlite3_exec(pDB,
			   "DROP TABLE IF EXISTS shop_listings_barter_legacy;"
			   "ALTER TABLE shop_listings RENAME TO shop_listings_barter_legacy;",
			   0, 0, 0) == SQLITE_OK)
			dbg_msg("database", "legacy barter shop listings moved to shop_listings_barter_legacy");
		else
			dbg_msg("database", "failed to archive legacy barter shop listings");
	}

	if(SqliteTableHasColumn(pDB, "npc_shop", "price_item") || SqliteTableHasColumn(pDB, "npc_stock", "price_item"))
	{
		if(sqlite3_exec(pDB, "DROP TABLE IF EXISTS npc_shop; DROP TABLE IF EXISTS npc_stock;", 0, 0, 0) == SQLITE_OK)
			dbg_msg("database", "dropped the legacy npc shop/stock tables; they are rebuilt from the npc json");
	}
}

CDatabaseSQLite::CDatabaseSQLite(const SConfig &Config)
{
	m_pDB = 0;
	str_copy(m_aPath, Config.m_pPath && Config.m_pPath[0] ? Config.m_pPath : "database/lastday.db", sizeof(m_aPath));
}

CDatabaseSQLite::~CDatabaseSQLite()
{
	if(m_pDB)
		sqlite3_close(m_pDB);
}

bool CDatabaseSQLite::Init()
{
	CDatabaseLock Guard(m_Lock);
	// make sure the parent directory of the database file exists
	char aDir[256];
	str_copy(aDir, m_aPath, sizeof(aDir));
	char *pSlash = 0;
	for(char *p = aDir; *p; p++)
		if(*p == '/')
			pSlash = p;
	if(pSlash && pSlash != aDir)
	{
		*pSlash = '\0';
		fs_makedir_recursive(aDir);
	}

	if(sqlite3_open(m_aPath, &m_pDB) != SQLITE_OK)
	{
		dbg_msg("database", "failed to open sqlite database '%s': %s", m_aPath, m_pDB ? sqlite3_errmsg(m_pDB) : "unknown error");
		if(m_pDB)
		{
			sqlite3_close(m_pDB);
			m_pDB = 0;
		}
		return false;
	}

	SqliteMigrateLegacyShopSchema(m_pDB);

	// create the schema at runtime (idempotent); the "data" column is JSON.
	// world_saves rows carry the auto-increment cell "id" as primary key
	const char *pCreate =
		"CREATE TABLE IF NOT EXISTS players ("
		" uuid TEXT PRIMARY KEY,"
		" username TEXT NOT NULL UNIQUE,"
		" password TEXT NOT NULL,"
		" data JSON NOT NULL DEFAULT '{}');"
		// guild-style teams: unlimited members, one account per team
		"CREATE TABLE IF NOT EXISTS teams ("
		" \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
		" name TEXT NOT NULL UNIQUE,"
		" leader TEXT NOT NULL,"
		" friendly_fire INTEGER NOT NULL DEFAULT 0,"
		" created_at INTEGER NOT NULL DEFAULT 0);"
		"CREATE TABLE IF NOT EXISTS team_members ("
		" account TEXT PRIMARY KEY,"
		" team_id INTEGER NOT NULL);"
		"CREATE INDEX IF NOT EXISTS team_members_team ON team_members(team_id);"
		"CREATE TABLE IF NOT EXISTS world_saves ("
		" \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
		" map TEXT NOT NULL UNIQUE,"
		" data JSON NOT NULL DEFAULT '{}');"
		// the npc aggro ledger: one dated signed delta per row, summed on read
		"CREATE TABLE IF NOT EXISTS npc_aggro ("
		" \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
		" npc_id TEXT NOT NULL,"
		" userid TEXT NOT NULL,"
		" delta INTEGER NOT NULL,"
		" created_at INTEGER NOT NULL);"
		"CREATE INDEX IF NOT EXISTS npc_aggro_pair ON npc_aggro(npc_id, userid);"
		// the currency ledger: one signed int64 delta per row and the reason it
		// was written. The balance is always SUM(delta); there is no balance
		// column that could drift.
		"CREATE TABLE IF NOT EXISTS currency_ledger ("
		" \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
		" account TEXT NOT NULL,"
		" delta INTEGER NOT NULL,"
		" reason TEXT NOT NULL,"
		" created_at INTEGER NOT NULL);"
		"CREATE INDEX IF NOT EXISTS currency_ledger_account ON currency_ledger(account);"
		// persisted shop listings: the escrowed goods outlive the session. The
		// price is currency (G), not another item.
		"CREATE TABLE IF NOT EXISTS shop_listings ("
		" id INTEGER PRIMARY KEY,"
		" seller TEXT NOT NULL,"
		" offer_item TEXT NOT NULL,"
		" offer_count INTEGER NOT NULL,"
		" price INTEGER NOT NULL);"
		// the shelf of each configured NPC, mirrored from its json
		"CREATE TABLE IF NOT EXISTS npc_shop ("
		" npc_id TEXT NOT NULL,"
		" item TEXT NOT NULL,"
		" \"count\" INTEGER NOT NULL,"
		" price INTEGER NOT NULL,"
		" PRIMARY KEY (npc_id, item, price));"
		// runtime stock of each shelf entry, so a purchase/restock survives a restart
		"CREATE TABLE IF NOT EXISTS npc_stock ("
		" npc_id TEXT NOT NULL,"
		" item TEXT NOT NULL,"
		" price INTEGER NOT NULL,"
		" stock INTEGER NOT NULL,"
		" restock_at INTEGER NOT NULL,"
		" PRIMARY KEY (npc_id, item, price));"
		// NPC buy orders (what the merchant wants from players), mirrored
		// from the NPC json just like npc_shop. "limit" is the number of
		// packages the merchant will take per restock period.
		"CREATE TABLE IF NOT EXISTS npc_buy_orders ("
		" npc_id TEXT NOT NULL,"
		" item TEXT NOT NULL,"
		" \"count\" INTEGER NOT NULL,"
		" price INTEGER NOT NULL,"
		" \"limit\" INTEGER NOT NULL,"
		" restock_seconds INTEGER NOT NULL,"
		" PRIMARY KEY (npc_id, item, price));"
		// runtime demand of one buy order, persisted like npc_stock
		"CREATE TABLE IF NOT EXISTS npc_buy_stock ("
		" npc_id TEXT NOT NULL,"
		" item TEXT NOT NULL,"
		" price INTEGER NOT NULL,"
		" demand INTEGER NOT NULL,"
		" restock_at INTEGER NOT NULL,"
		" PRIMARY KEY (npc_id, item, price));";

	char *pErr = 0;
	if(sqlite3_exec(m_pDB, pCreate, 0, 0, &pErr) != SQLITE_OK)
	{
		dbg_msg("database", "failed to create schema: %s", pErr ? pErr : "unknown error");
		sqlite3_free(pErr);
		return false;
	}

	dbg_msg("database", "sqlite database ready at '%s'", m_aPath);
	return true;
}

bool CDatabaseSQLite::FindByName(const char *pUsername, SPlayerData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT uuid, username, password FROM players WHERE username = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pUsername, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		Found = true;
		mem_zero(&out, sizeof(out));
		const unsigned char *pUuid = sqlite3_column_text(pStmt, 0);
		const unsigned char *pName = sqlite3_column_text(pStmt, 1);
		const unsigned char *pPassword = sqlite3_column_text(pStmt, 2);
		out.m_Uuid = UUID_ZEROED;
		if(pUuid)
			parse_uuid(&out.m_Uuid, (const char *) pUuid);
		str_copy(out.m_aUsername, pName ? (const char *) pName : "", sizeof(out.m_aUsername));
		str_copy(out.m_aPasswordHash, pPassword ? (const char *) pPassword : "", sizeof(out.m_aPasswordHash));
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::InsertPlayer(const SPlayerData &Player)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Player.m_Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT INTO players (uuid, username, password) VALUES (?1, ?2, ?3);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, Player.m_aUsername, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 3, Player.m_aPasswordHash, -1, SQLITE_TRANSIENT);

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to insert Player '%s': %s", Player.m_aUsername, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::EnsureGuestPlayer(const Uuid &Uuid, const char *pUsername)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pUsername || !pUsername[0])
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR IGNORE INTO players (uuid, username, password) VALUES (?1, ?2, '!guest');",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pUsername, -1, SQLITE_TRANSIENT);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	const int Changes = Ok ? sqlite3_changes(m_pDB) : 0;
	if(!Ok)
		dbg_msg("database", "failed to ensure guest '%s': %s", pUsername, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok && Changes > 0;
}

bool CDatabaseSQLite::CreateTeam(const char *pName, const Uuid &Leader, bool FriendlyFire, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pName || !pName[0])
		return false;

	char aLeader[UUID_MAXSTRSIZE];
	format_uuid(Leader, aLeader, sizeof(aLeader));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT INTO teams (name, leader, friendly_fire, created_at) VALUES (?1, ?2, ?3, 0);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pName, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aLeader, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 3, FriendlyFire ? 1 : 0);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	if(!Ok)
		return false;

	const int Id = (int) sqlite3_last_insert_rowid(m_pDB);
	// Add the leader without taking the database lock again (the public helper
	// would deadlock on the non-recursive mutex).
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO team_members (account, team_id) VALUES (?1, ?2);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aLeader, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 2, Id);
	const bool MemberOk = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	if(!MemberOk)
	{
		char aDelete[128];
		str_format(aDelete, sizeof(aDelete), "DELETE FROM teams WHERE id = %d;", Id);
		sqlite3_exec(m_pDB, aDelete, nullptr, nullptr, nullptr);
		return false;
	}

	mem_zero(&out, sizeof(out));
	out.m_Id = Id;
	str_copy(out.m_aName, pName, sizeof(out.m_aName));
	out.m_Leader = Leader;
	out.m_FriendlyFire = FriendlyFire;
	return true;
}

bool CDatabaseSQLite::FindTeamByName(const char *pName, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pName)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT id, name, leader, friendly_fire FROM teams WHERE name = ?1 LIMIT 1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pName, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		Found = true;
		mem_zero(&out, sizeof(out));
		out.m_Id = sqlite3_column_int(pStmt, 0);
		str_copy(out.m_aName, (const char *) sqlite3_column_text(pStmt, 1), sizeof(out.m_aName));
		parse_uuid(&out.m_Leader, (const char *) sqlite3_column_text(pStmt, 2));
		out.m_FriendlyFire = sqlite3_column_int(pStmt, 3) != 0;
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::FindTeamForMember(const Uuid &Account, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT t.id, t.name, t.leader, t.friendly_fire FROM teams t "
		   "JOIN team_members m ON m.team_id = t.id WHERE m.account = ?1 LIMIT 1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aAccount, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		Found = true;
		mem_zero(&out, sizeof(out));
		out.m_Id = sqlite3_column_int(pStmt, 0);
		str_copy(out.m_aName, (const char *) sqlite3_column_text(pStmt, 1), sizeof(out.m_aName));
		parse_uuid(&out.m_Leader, (const char *) sqlite3_column_text(pStmt, 2));
		out.m_FriendlyFire = sqlite3_column_int(pStmt, 3) != 0;
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::ListTeams(array<CDatabase::STeamData> &out)
{
	CDatabaseLock Guard(m_Lock);
	out.clear();
	if(!m_pDB)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT id, name, leader, friendly_fire FROM teams ORDER BY id;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	while(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		CDatabase::STeamData &Team = out.emplace();
		Team.m_Id = sqlite3_column_int(pStmt, 0);
		str_copy(Team.m_aName, (const char *) sqlite3_column_text(pStmt, 1), sizeof(Team.m_aName));
		parse_uuid(&Team.m_Leader, (const char *) sqlite3_column_text(pStmt, 2));
		Team.m_FriendlyFire = sqlite3_column_int(pStmt, 3) != 0;
	}
	sqlite3_finalize(pStmt);
	return true;
}

bool CDatabaseSQLite::LoadTeamMembers(int TeamId, array<Uuid> &out)
{
	CDatabaseLock Guard(m_Lock);
	out.clear();
	if(!m_pDB)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT account FROM team_members WHERE team_id = ?1 ORDER BY account;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_int(pStmt, 1, TeamId);
	while(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		Uuid Account = UUID_ZEROED;
		parse_uuid(&Account, (const char *) sqlite3_column_text(pStmt, 0));
		if(Account != UUID_ZEROED)
			out.add(Account);
	}
	sqlite3_finalize(pStmt);
	return true;
}

bool CDatabaseSQLite::AddTeamMember(int TeamId, const Uuid &Account)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || TeamId <= 0)
		return false;

	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO team_members (account, team_id) VALUES (?1, ?2);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aAccount, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 2, TeamId);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::RemoveTeamMember(int TeamId, const Uuid &Account)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "DELETE FROM team_members WHERE account = ?1 AND team_id = ?2;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aAccount, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 2, TeamId);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::DeleteTeam(int TeamId)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || TeamId <= 0)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "DELETE FROM team_members WHERE team_id = ?1;", -1, &pStmt, 0) == SQLITE_OK)
	{
		sqlite3_bind_int(pStmt, 1, TeamId);
		sqlite3_step(pStmt);
		sqlite3_finalize(pStmt);
	}
	if(sqlite3_prepare_v2(m_pDB, "DELETE FROM teams WHERE id = ?1;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_int(pStmt, 1, TeamId);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::SetTeamLeader(int TeamId, const Uuid &Leader)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;
	char aLeader[UUID_MAXSTRSIZE];
	format_uuid(Leader, aLeader, sizeof(aLeader));
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "UPDATE teams SET leader = ?1 WHERE id = ?2;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aLeader, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 2, TeamId);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::SetTeamFriendlyFire(int TeamId, bool FriendlyFire)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "UPDATE teams SET friendly_fire = ?1 WHERE id = ?2;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_int(pStmt, 1, FriendlyFire ? 1 : 0);
	sqlite3_bind_int(pStmt, 2, TeamId);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::LoadByUuid(const Uuid &Uuid, SPlayerData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT uuid, username, password FROM players WHERE uuid = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		Found = true;
		mem_zero(&out, sizeof(out));
		const unsigned char *pUuid = sqlite3_column_text(pStmt, 0);
		out.m_Uuid = UUID_ZEROED;
		if(pUuid)
			parse_uuid(&out.m_Uuid, (const char *) pUuid);
		str_copy(out.m_aUsername, (const char *) sqlite3_column_text(pStmt, 1), sizeof(out.m_aUsername));
		str_copy(out.m_aPasswordHash, (const char *) sqlite3_column_text(pStmt, 2), sizeof(out.m_aPasswordHash));
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::GetJson(const Uuid &Uuid, const CJsonPath &Path, char *pOut, int Size)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT json_extract(data, ?2) FROM players WHERE uuid = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aJsonPath, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW && sqlite3_column_type(pStmt, 0) != SQLITE_NULL)
	{
		Found = true;
		str_copy(pOut, (const char *) sqlite3_column_text(pStmt, 0), Size);
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::SetJson(const Uuid &Uuid, const CJsonPath &Path, const char *pValue)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "UPDATE players SET data = json_set(data, ?2, json(?3)) WHERE uuid = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aJsonPath, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 3, pValue, -1, SQLITE_TRANSIENT);

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to set json field '%s' of '%s': %s", Path.c_str(), aUuid, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::DelJson(const Uuid &Uuid, const CJsonPath &Path)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "UPDATE players SET data = json_remove(data, ?2) WHERE uuid = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aJsonPath, -1, SQLITE_TRANSIENT);

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to remove json field '%s' of '%s': %s", Path.c_str(), aUuid, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT json_array_length(data, ?2) FROM players WHERE uuid = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aUuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aJsonPath, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW && sqlite3_column_type(pStmt, 0) != SQLITE_NULL)
	{
		Found = true;
		*pLength = sqlite3_column_int(pStmt, 0);
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::GetNpcAggro(const Uuid &Npc, const Uuid &User, int *pOut)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT COALESCE(SUM(delta), 0) FROM npc_aggro WHERE npc_id = ?1 AND userid = ?2;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aUser, -1, SQLITE_TRANSIENT);

	bool Ok = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		*pOut = sqlite3_column_int(pStmt, 0);
		Ok = true;
	}
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::AddNpcAggro(const Uuid &Npc, const Uuid &User, int Delta)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT INTO npc_aggro (npc_id, userid, delta, created_at) VALUES (?1, ?2, ?3, ?4);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aUser, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 3, Delta);
	sqlite3_bind_int64(pStmt, 4, (sqlite3_int64) time_timestamp());

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to add npc aggro '%s'/'%s': %s", aNpc, aUser, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::EnsureNpcAggro(const Uuid &Npc, const Uuid &User)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT INTO npc_aggro (npc_id, userid, delta, created_at) "
		   "SELECT ?1, ?2, 0, ?3 WHERE NOT EXISTS "
		   "(SELECT 1 FROM npc_aggro WHERE npc_id = ?1 AND userid = ?2);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, aUser, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 3, (sqlite3_int64) time_timestamp());

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to ensure npc aggro '%s'/'%s': %s", aNpc, aUser, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::LoadShopListings(array<SShopListing> &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT id, seller, offer_item, offer_count, price FROM shop_listings ORDER BY id;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;

	const unsigned char *p;
	while(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		SShopListing &Listing = out.emplace();
		Listing.m_Id = sqlite3_column_int(pStmt, 0);
		Listing.m_Seller = UUID_ZEROED;
		p = sqlite3_column_text(pStmt, 1);
		if(p)
			parse_uuid(&Listing.m_Seller, (const char *) p);
		p = sqlite3_column_text(pStmt, 2);
		str_copy(Listing.m_aOfferItem, p ? (const char *) p : "", sizeof(Listing.m_aOfferItem));
		Listing.m_OfferCount = sqlite3_column_int(pStmt, 3);
		Listing.m_Price = (int64) sqlite3_column_int64(pStmt, 4);
	}
	sqlite3_finalize(pStmt);
	return true;
}

bool CDatabaseSQLite::SaveShopListing(const SShopListing &Listing)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aSeller[UUID_MAXSTRSIZE];
	format_uuid(Listing.m_Seller, aSeller, sizeof(aSeller));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO shop_listings (id, seller, offer_item, offer_count, price) "
		   "VALUES (?1, ?2, ?3, ?4, ?5);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_int(pStmt, 1, Listing.m_Id);
	sqlite3_bind_text(pStmt, 2, aSeller, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 3, Listing.m_aOfferItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 4, Listing.m_OfferCount);
	sqlite3_bind_int64(pStmt, 5, (sqlite3_int64) Listing.m_Price);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save shop listing #%d: %s", Listing.m_Id, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::DeleteShopListing(int Id)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "DELETE FROM shop_listings WHERE id = ?1;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_int(pStmt, 1, Id);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::ClearNpcSells(const Uuid &Npc)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "DELETE FROM npc_shop WHERE npc_id = ?1;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::SaveNpcSell(const Uuid &Npc, const char *pItem, int Count, int64 Price)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO npc_shop (npc_id, item, \"count\", price) VALUES (?1, ?2, ?3, ?4);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 3, Count);
	sqlite3_bind_int64(pStmt, 4, (sqlite3_int64) Price);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save npc sell '%s': %s", aNpc, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::GetNpcStock(const Uuid &Npc, const char *pItem, int64 Price, int *pStock, int *pRestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT stock, restock_at FROM npc_stock WHERE npc_id = ?1 AND item = ?2 AND price = ?3;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 3, (sqlite3_int64) Price);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		*pStock = sqlite3_column_int(pStmt, 0);
		*pRestockAt = sqlite3_column_int(pStmt, 1);
		Found = true;
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::SaveNpcStock(const Uuid &Npc, const char *pItem, int64 Price, int Stock, int RestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO npc_stock (npc_id, item, price, stock, restock_at) VALUES (?1, ?2, ?3, ?4, ?5);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 3, (sqlite3_int64) Price);
	sqlite3_bind_int(pStmt, 4, Stock);
	sqlite3_bind_int(pStmt, 5, RestockAt);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save npc stock '%s': %s", aNpc, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::ClearNpcBuys(const Uuid &Npc)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB, "DELETE FROM npc_buy_orders WHERE npc_id = ?1;", -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::SaveNpcBuy(const Uuid &Npc, const char *pItem, int Count, int64 Price, int Limit, int RestockSeconds)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO npc_buy_orders (npc_id, item, \"count\", price, \"limit\", restock_seconds) "
		   "VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(pStmt, 3, Count);
	sqlite3_bind_int64(pStmt, 4, (sqlite3_int64) Price);
	sqlite3_bind_int(pStmt, 5, Limit);
	sqlite3_bind_int(pStmt, 6, RestockSeconds);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save npc buy '%s': %s", aNpc, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::GetNpcBuyDemand(const Uuid &Npc, const char *pItem, int64 Price, int *pDemand, int *pRestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT demand, restock_at FROM npc_buy_stock WHERE npc_id = ?1 AND item = ?2 AND price = ?3;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 3, (sqlite3_int64) Price);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		*pDemand = sqlite3_column_int(pStmt, 0);
		*pRestockAt = sqlite3_column_int(pStmt, 1);
		Found = true;
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::SaveNpcBuyDemand(const Uuid &Npc, const char *pItem, int64 Price, int Demand, int RestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB)
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO npc_buy_stock (npc_id, item, price, demand, restock_at) VALUES (?1, ?2, ?3, ?4, ?5);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aNpc, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pItem, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 3, (sqlite3_int64) Price);
	sqlite3_bind_int(pStmt, 4, Demand);
	sqlite3_bind_int(pStmt, 5, RestockAt);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save npc buy demand '%s': %s", aNpc, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

// currency helpers assume the caller already holds m_Lock
static bool SqliteReadCurrencyBalance(sqlite3 *pDB, const Uuid &Account, int64 *pOut)
{
	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(pDB,
		   "SELECT COALESCE(SUM(delta), 0) FROM currency_ledger WHERE account = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aAccount, -1, SQLITE_TRANSIENT);

	bool Ok = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW)
	{
		*pOut = (int64) sqlite3_column_int64(pStmt, 0);
		Ok = true;
	}
	sqlite3_finalize(pStmt);
	return Ok;
}

static bool SqliteInsertCurrency(sqlite3 *pDB, const Uuid &Account, int64 Delta, const char *pReason, int CreatedAt)
{
	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(pDB,
		   "INSERT INTO currency_ledger (account, delta, reason, created_at) VALUES (?1, ?2, ?3, ?4);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aAccount, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 2, (sqlite3_int64) Delta);
	sqlite3_bind_text(pStmt, 3, pReason ? pReason : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 4, (sqlite3_int64) CreatedAt);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to append currency ledger row for '%s': %s", aAccount, sqlite3_errmsg(pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::GetCurrencyBalance(const Uuid &Account, int64 *pOut)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pOut || Account == UUID_ZEROED)
		return false;
	return SqliteReadCurrencyBalance(m_pDB, Account, pOut);
}

bool CDatabaseSQLite::AddCurrency(const Uuid &Account, int64 Delta, const char *pReason)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || Account == UUID_ZEROED || Delta == 0 || Delta > CURRENCY_MAX_AMOUNT || Delta < -CURRENCY_MAX_AMOUNT)
		return false;
	return SqliteInsertCurrency(m_pDB, Account, Delta, pReason, time_timestamp());
}

bool CDatabaseSQLite::SpendCurrency(const Uuid &Account, int64 Amount, const char *pReason)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || Account == UUID_ZEROED || Amount <= 0 || Amount > CURRENCY_MAX_AMOUNT)
		return false;

	int64 Balance = 0;
	if(!SqliteReadCurrencyBalance(m_pDB, Account, &Balance))
		return false;
	if(Balance < Amount)
		return false;
	return SqliteInsertCurrency(m_pDB, Account, -Amount, pReason, time_timestamp());
}

bool CDatabaseSQLite::TransferCurrency(const Uuid &From, const Uuid &To, int64 Amount,
	const char *pFromReason, const char *pToReason)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || From == UUID_ZEROED || To == UUID_ZEROED || From == To || Amount <= 0 || Amount > CURRENCY_MAX_AMOUNT)
		return false;

	int64 Balance = 0;
	if(!SqliteReadCurrencyBalance(m_pDB, From, &Balance))
		return false;
	if(Balance < Amount)
		return false;

	char aFrom[UUID_MAXSTRSIZE];
	char aTo[UUID_MAXSTRSIZE];
	format_uuid(From, aFrom, sizeof(aFrom));
	format_uuid(To, aTo, sizeof(aTo));
	const int CreatedAt = time_timestamp();

	// one INSERT keeps the debit and the credit together
	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT INTO currency_ledger (account, delta, reason, created_at) "
		   "VALUES (?1, ?2, ?3, ?4), (?5, ?6, ?7, ?4);",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, aFrom, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 2, (sqlite3_int64) -Amount);
	sqlite3_bind_text(pStmt, 3, pFromReason ? pFromReason : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 4, (sqlite3_int64) CreatedAt);
	sqlite3_bind_text(pStmt, 5, aTo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(pStmt, 6, (sqlite3_int64) Amount);
	sqlite3_bind_text(pStmt, 7, pToReason ? pToReason : "", -1, SQLITE_TRANSIENT);

	const bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to transfer currency '%s' -> '%s': %s", aFrom, aTo, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::SaveWorldSave(const char *pMap, const char *pJsonData)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pMap || !pJsonData)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "INSERT OR REPLACE INTO world_saves (map, data) VALUES (?1, json(?2));",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pMap, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(pStmt, 2, pJsonData, -1, SQLITE_TRANSIENT);

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to save world save on '%s': %s", pMap, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}

bool CDatabaseSQLite::GetWorldSaveSize(const char *pMap, int *pSize)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pMap || !pSize)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT length(data) FROM world_saves WHERE map = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pMap, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW && sqlite3_column_type(pStmt, 0) != SQLITE_NULL)
	{
		Found = true;
		*pSize = sqlite3_column_int(pStmt, 0);
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::GetWorldSaveData(const char *pMap, char *pOut, int Size)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pMap || !pOut)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "SELECT data FROM world_saves WHERE map = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pMap, -1, SQLITE_TRANSIENT);

	bool Found = false;
	if(sqlite3_step(pStmt) == SQLITE_ROW && sqlite3_column_type(pStmt, 0) != SQLITE_NULL)
	{
		Found = true;
		str_copy(pOut, (const char *) sqlite3_column_text(pStmt, 0), Size);
	}
	sqlite3_finalize(pStmt);
	return Found;
}

bool CDatabaseSQLite::DeleteWorldSave(const char *pMap)
{
	CDatabaseLock Guard(m_Lock);
	if(!m_pDB || !pMap)
		return false;

	sqlite3_stmt *pStmt = 0;
	if(sqlite3_prepare_v2(m_pDB,
		   "DELETE FROM world_saves WHERE map = ?1;",
		   -1, &pStmt, 0) != SQLITE_OK)
		return false;
	sqlite3_bind_text(pStmt, 1, pMap, -1, SQLITE_TRANSIENT);

	bool Ok = sqlite3_step(pStmt) == SQLITE_DONE;
	if(!Ok)
		dbg_msg("database", "failed to delete world save on '%s': %s", pMap, sqlite3_errmsg(m_pDB));
	sqlite3_finalize(pStmt);
	return Ok;
}
