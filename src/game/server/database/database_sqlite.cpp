/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* SQLite (embedded) backend for the database. */
#include <game/server/database/database_sqlite.h>

#include <base/system.h>

#include <sqlite3.h>

namespace {
// convert a dot-notation json path ("inventory.0.count") into a SQLite
// json path ("$.inventory[0].count")
bool BuildJsonPath(const CJsonPath &Path, char *pOut, int Size)
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
} // anonymous namespace

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

	// create the schema at runtime (idempotent); the "data" column is JSON.
	// world_saves rows carry the auto-increment cell "id" as primary key
	const char *pCreate =
		"CREATE TABLE IF NOT EXISTS players ("
		" uuid TEXT PRIMARY KEY,"
		" username TEXT NOT NULL UNIQUE,"
		" password TEXT NOT NULL,"
		" data JSON NOT NULL DEFAULT '{}');"
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
		"CREATE INDEX IF NOT EXISTS npc_aggro_pair ON npc_aggro(npc_id, userid);";

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
