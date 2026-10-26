/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* PostgreSQL (libpq) backend for the database. */
#include <game/server/database/database_postgres.h>

#include <base/system.h>

#include <libpq-fe.h>

namespace {
// convert a dot-notation json path ("inventory.0.count") into a PostgreSQL
// jsonb text[] path ("{inventory,0,count}")
bool BuildJsonPath(const CJsonPath &Path, char *pOut, int Size)
{
	int Offset = 0;
	if(Offset + 1 >= Size)
		return false;
	pOut[Offset++] = '{';

	const char *p = Path.c_str();
	while(*p)
	{
		const char *pStart = p;
		while(*p && *p != '.')
			p++;
		const int SegLen = (int) (p - pStart);
		if(SegLen == 0 || Offset + SegLen + 1 >= Size)
			return false;

		mem_copy(pOut + Offset, pStart, SegLen);
		Offset += SegLen;
		pOut[Offset++] = ',';

		if(*p == '.')
			p++;
	}

	pOut[Offset - 1] = '}'; // replace the trailing comma
	pOut[Offset] = '\0';
	return true;
}
} // anonymous namespace

CDatabasePostgres::CDatabasePostgres(const SConfig &Config)
{
	m_pConn = 0;
	str_copy(m_aConnString, Config.m_pConnString && Config.m_pConnString[0] ? Config.m_pConnString : "", sizeof(m_aConnString));
}

CDatabasePostgres::~CDatabasePostgres()
{
	if(m_pConn)
		PQfinish(m_pConn);
}

bool CDatabasePostgres::EnsureConnected()
{
	if(!m_pConn)
		return false;
	if(PQstatus(m_pConn) == CONNECTION_OK)
		return true;

	dbg_msg("database", "postgres connection lost, reconnecting: %s", PQerrorMessage(m_pConn));
	PQreset(m_pConn);
	if(PQstatus(m_pConn) != CONNECTION_OK)
	{
		dbg_msg("database", "postgres reconnect failed: %s", PQerrorMessage(m_pConn));
		return false;
	}
	return true;
}

bool CDatabasePostgres::Init()
{
	CDatabaseLock Guard(m_Lock);
	m_pConn = PQconnectdb(m_aConnString);
	if(PQstatus(m_pConn) != CONNECTION_OK)
	{
		dbg_msg("database", "failed to connect to postgres: %s", PQerrorMessage(m_pConn));
		PQfinish(m_pConn);
		m_pConn = 0;
		return false;
	}

	// create the schema at runtime (idempotent); the "data" column is JSONB
	// and the primary key uses PostgreSQL's native UUID type
	const char *pCreate =
		"CREATE TABLE IF NOT EXISTS players ("
		" uuid UUID PRIMARY KEY,"
		" username TEXT NOT NULL UNIQUE,"
		" password TEXT NOT NULL,"
		" data JSONB NOT NULL DEFAULT '{}');"
		// guild-style teams: unlimited members, one account per team
		"CREATE TABLE IF NOT EXISTS teams ("
		" id SERIAL PRIMARY KEY,"
		" name TEXT NOT NULL UNIQUE,"
		" leader TEXT NOT NULL,"
		" friendly_fire INTEGER NOT NULL DEFAULT 0,"
		" created_at BIGINT NOT NULL DEFAULT 0);"
		"CREATE TABLE IF NOT EXISTS team_members ("
		" account TEXT PRIMARY KEY,"
		" team_id INTEGER NOT NULL REFERENCES teams(id) ON DELETE CASCADE);"
		"CREATE INDEX IF NOT EXISTS team_members_team ON team_members(team_id);";

	PGresult *pRes = PQexec(m_pConn, pCreate);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to create schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	// world-persisted saves with a database-assigned auto-increment cell id
	const char *pCreateSaves =
		"CREATE TABLE IF NOT EXISTS world_saves ("
		" id SERIAL PRIMARY KEY,"
		" map TEXT NOT NULL UNIQUE,"
		" data JSONB NOT NULL DEFAULT '{}');";

	pRes = PQexec(m_pConn, pCreateSaves);
	const bool SavesOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!SavesOk)
		dbg_msg("database", "failed to create world_saves schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	// the npc aggro ledger: one dated signed delta per row, summed on read. The
	// identity columns use PostgreSQL's native UUID type.
	const char *pCreateAggro =
		"CREATE TABLE IF NOT EXISTS npc_aggro ("
		" id SERIAL PRIMARY KEY,"
		" npc_id UUID NOT NULL,"
		" userid UUID NOT NULL,"
		" delta INTEGER NOT NULL,"
		" created_at BIGINT NOT NULL);"
		"CREATE INDEX IF NOT EXISTS npc_aggro_pair ON npc_aggro(npc_id, userid);";

	pRes = PQexec(m_pConn, pCreateAggro);
	const bool AggroOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!AggroOk)
		dbg_msg("database", "failed to create npc_aggro schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	// persisted shop listings; the seller is an account, so the escrowed goods
	// outlive the session that posted them
	const char *pCreateShop =
		"CREATE TABLE IF NOT EXISTS shop_listings ("
		" id INTEGER PRIMARY KEY,"
		" seller UUID NOT NULL,"
		" offer_item TEXT NOT NULL,"
		" offer_count INTEGER NOT NULL,"
		" price_item TEXT NOT NULL,"
		" price_count INTEGER NOT NULL);";

	pRes = PQexec(m_pConn, pCreateShop);
	const bool ShopOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!ShopOk)
		dbg_msg("database", "failed to create shop_listings schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	// the shelf of each configured NPC, mirrored from its json
	const char *pCreateNpcShop =
		"CREATE TABLE IF NOT EXISTS npc_shop ("
		" npc_id UUID NOT NULL,"
		" item TEXT NOT NULL,"
		" \"count\" INTEGER NOT NULL,"
		" price_item TEXT NOT NULL,"
		" price_count INTEGER NOT NULL,"
		" PRIMARY KEY (npc_id, item, price_item));";

	pRes = PQexec(m_pConn, pCreateNpcShop);
	const bool NpcShopOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!NpcShopOk)
		dbg_msg("database", "failed to create npc_shop schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	// runtime stock of each shelf entry, so a purchase/restock survives a restart
	const char *pCreateNpcStock =
		"CREATE TABLE IF NOT EXISTS npc_stock ("
		" npc_id UUID NOT NULL,"
		" item TEXT NOT NULL,"
		" price_item TEXT NOT NULL,"
		" stock INTEGER NOT NULL,"
		" restock_at BIGINT NOT NULL,"
		" PRIMARY KEY (npc_id, item, price_item));";

	pRes = PQexec(m_pConn, pCreateNpcStock);
	const bool NpcStockOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!NpcStockOk)
		dbg_msg("database", "failed to create npc_stock schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	if(Ok && SavesOk && AggroOk && ShopOk && NpcShopOk && NpcStockOk)
		dbg_msg("database", "postgres database ready");
	return Ok && SavesOk && AggroOk && ShopOk && NpcShopOk && NpcStockOk;
}

bool CDatabasePostgres::FindByName(const char *pUsername, SPlayerData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	const char *apParams[1] = {pUsername};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT uuid, username, password FROM players WHERE username = $1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		dbg_msg("database", "FindByName failed: %s", PQresultErrorMessage(pRes));
		PQclear(pRes);
		return false;
	}

	bool Found = PQntuples(pRes) > 0;
	if(Found)
	{
		mem_zero(&out, sizeof(out));
		out.m_Uuid = UUID_ZEROED;
		if(!PQgetisnull(pRes, 0, 0))
			parse_uuid(&out.m_Uuid, PQgetvalue(pRes, 0, 0));
		str_copy(out.m_aUsername, PQgetvalue(pRes, 0, 1), sizeof(out.m_aUsername));
		str_copy(out.m_aPasswordHash, PQgetvalue(pRes, 0, 2), sizeof(out.m_aPasswordHash));
	}
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::InsertPlayer(const SPlayerData &Player)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Player.m_Uuid, aUuid, sizeof(aUuid));

	const char *apParams[3] = {aUuid, Player.m_aUsername, Player.m_aPasswordHash};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO players (uuid, username, password) VALUES ($1::uuid, $2, $3);",
		3, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to insert Player '%s': %s", Player.m_aUsername, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::EnsureGuestPlayer(const Uuid &Uuid, const char *pUsername)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pUsername || !pUsername[0])
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[2] = {aUuid, pUsername};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO players (uuid, username, password) VALUES ($1::uuid, $2, '!guest') "
		"ON CONFLICT DO NOTHING;",
		2, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	const bool Created = Ok && str_comp(PQcmdTuples(pRes), "1") == 0;
	if(!Ok)
		dbg_msg("database", "failed to ensure guest '%s': %s", pUsername, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Created;
}

bool CDatabasePostgres::CreateTeam(const char *pName, const Uuid &Leader, bool FriendlyFire, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pName || !pName[0])
		return false;

	char aLeader[UUID_MAXSTRSIZE];
	format_uuid(Leader, aLeader, sizeof(aLeader));
	const char *apParams[3] = {pName, aLeader, FriendlyFire ? "1" : "0"};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO teams (name, leader, friendly_fire, created_at) VALUES ($1, $2, $3::int, 0) RETURNING id;",
		3, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK || PQntuples(pRes) != 1)
	{
		dbg_msg("database", "failed to create team '%s': %s", pName, PQresultErrorMessage(pRes));
		PQclear(pRes);
		return false;
	}
	const int Id = str_toint(PQgetvalue(pRes, 0, 0));
	PQclear(pRes);

	char aId[16];
	str_format(aId, sizeof(aId), "%d", Id);
	const char *apMember[2] = {aLeader, aId};
	pRes = PQexecParams(m_pConn,
		"INSERT INTO team_members (account, team_id) VALUES ($1, $2::int);",
		2, 0, apMember, 0, 0, 0);
	const bool MemberOk = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!MemberOk)
		dbg_msg("database", "failed to add team leader: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);
	if(!MemberOk)
	{
		char aDelete[96];
		str_format(aDelete, sizeof(aDelete), "DELETE FROM teams WHERE id = %d;", Id);
		PQexec(m_pConn, aDelete);
		return false;
	}

	mem_zero(&out, sizeof(out));
	out.m_Id = Id;
	str_copy(out.m_aName, pName, sizeof(out.m_aName));
	out.m_Leader = Leader;
	out.m_FriendlyFire = FriendlyFire;
	return true;
}

bool CDatabasePostgres::FindTeamByName(const char *pName, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pName)
		return false;
	const char *apParams[1] = {pName};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT id, name, leader, friendly_fire FROM teams WHERE name = $1 LIMIT 1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK || PQntuples(pRes) <= 0)
	{
		PQclear(pRes);
		return false;
	}
	mem_zero(&out, sizeof(out));
	out.m_Id = str_toint(PQgetvalue(pRes, 0, 0));
	str_copy(out.m_aName, PQgetvalue(pRes, 0, 1), sizeof(out.m_aName));
	parse_uuid(&out.m_Leader, PQgetvalue(pRes, 0, 2));
	out.m_FriendlyFire = str_toint(PQgetvalue(pRes, 0, 3)) != 0;
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::FindTeamForMember(const Uuid &Account, CDatabase::STeamData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	const char *apParams[1] = {aAccount};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT t.id, t.name, t.leader, t.friendly_fire FROM teams t "
		"JOIN team_members m ON m.team_id = t.id WHERE m.account = $1 LIMIT 1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK || PQntuples(pRes) <= 0)
	{
		PQclear(pRes);
		return false;
	}
	mem_zero(&out, sizeof(out));
	out.m_Id = str_toint(PQgetvalue(pRes, 0, 0));
	str_copy(out.m_aName, PQgetvalue(pRes, 0, 1), sizeof(out.m_aName));
	parse_uuid(&out.m_Leader, PQgetvalue(pRes, 0, 2));
	out.m_FriendlyFire = str_toint(PQgetvalue(pRes, 0, 3)) != 0;
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::ListTeams(array<CDatabase::STeamData> &out)
{
	CDatabaseLock Guard(m_Lock);
	out.clear();
	if(!EnsureConnected())
		return false;
	PGresult *pRes = PQexec(m_pConn, "SELECT id, name, leader, friendly_fire FROM teams ORDER BY id;");
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}
	for(int i = 0; i < PQntuples(pRes); i++)
	{
		CDatabase::STeamData &Team = out.emplace();
		Team.m_Id = str_toint(PQgetvalue(pRes, i, 0));
		str_copy(Team.m_aName, PQgetvalue(pRes, i, 1), sizeof(Team.m_aName));
		parse_uuid(&Team.m_Leader, PQgetvalue(pRes, i, 2));
		Team.m_FriendlyFire = str_toint(PQgetvalue(pRes, i, 3)) != 0;
	}
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::LoadTeamMembers(int TeamId, array<Uuid> &out)
{
	CDatabaseLock Guard(m_Lock);
	out.clear();
	if(!EnsureConnected())
		return false;
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[1] = {aId};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT account FROM team_members WHERE team_id = $1::int ORDER BY account;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}
	for(int i = 0; i < PQntuples(pRes); i++)
	{
		Uuid Account = UUID_ZEROED;
		parse_uuid(&Account, PQgetvalue(pRes, i, 0));
		if(Account != UUID_ZEROED)
			out.add(Account);
	}
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::AddTeamMember(int TeamId, const Uuid &Account)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[2] = {aAccount, aId};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO team_members (account, team_id) VALUES ($1, $2::int) "
		"ON CONFLICT (account) DO UPDATE SET team_id = EXCLUDED.team_id;",
		2, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::RemoveTeamMember(int TeamId, const Uuid &Account)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aAccount[UUID_MAXSTRSIZE];
	format_uuid(Account, aAccount, sizeof(aAccount));
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[2] = {aAccount, aId};
	PGresult *pRes = PQexecParams(m_pConn,
		"DELETE FROM team_members WHERE account = $1 AND team_id = $2::int;",
		2, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::DeleteTeam(int TeamId)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[1] = {aId};
	PGresult *pRes = PQexecParams(m_pConn, "DELETE FROM teams WHERE id = $1::int;", 1, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::SetTeamLeader(int TeamId, const Uuid &Leader)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aLeader[UUID_MAXSTRSIZE];
	format_uuid(Leader, aLeader, sizeof(aLeader));
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[2] = {aLeader, aId};
	PGresult *pRes = PQexecParams(m_pConn,
		"UPDATE teams SET leader = $1 WHERE id = $2::int;", 2, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::SetTeamFriendlyFire(int TeamId, bool FriendlyFire)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;
	char aId[16];
	str_format(aId, sizeof(aId), "%d", TeamId);
	const char *apParams[2] = {FriendlyFire ? "1" : "0", aId};
	PGresult *pRes = PQexecParams(m_pConn,
		"UPDATE teams SET friendly_fire = $1::int WHERE id = $2::int;", 2, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::LoadByUuid(const Uuid &Uuid, SPlayerData &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[1] = {aUuid};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT uuid, username, password FROM players WHERE uuid = $1::uuid;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	bool Found = PQntuples(pRes) > 0;
	if(Found)
	{
		mem_zero(&out, sizeof(out));
		out.m_Uuid = UUID_ZEROED;
		if(!PQgetisnull(pRes, 0, 0))
			parse_uuid(&out.m_Uuid, PQgetvalue(pRes, 0, 0));
		str_copy(out.m_aUsername, PQgetvalue(pRes, 0, 1), sizeof(out.m_aUsername));
		str_copy(out.m_aPasswordHash, PQgetvalue(pRes, 0, 2), sizeof(out.m_aPasswordHash));
	}
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::GetJson(const Uuid &Uuid, const CJsonPath &Path, char *pOut, int Size)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[2] = {aUuid, aJsonPath};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT data #>> $2::text[] FROM players WHERE uuid = $1::uuid;",
		2, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	const bool Found = PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0);
	if(Found)
		str_copy(pOut, PQgetvalue(pRes, 0, 0), Size);
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::SetJson(const Uuid &Uuid, const CJsonPath &Path, const char *pValue)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[3] = {aUuid, aJsonPath, pValue};
	PGresult *pRes = PQexecParams(m_pConn,
		"UPDATE players SET data = jsonb_set(data, $2::text[], $3::jsonb, true) WHERE uuid = $1::uuid;",
		3, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to set json field '%s' of '%s': %s", Path.c_str(), aUuid, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::DelJson(const Uuid &Uuid, const CJsonPath &Path)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[2] = {aUuid, aJsonPath};
	PGresult *pRes = PQexecParams(m_pConn,
		"UPDATE players SET data = data #- $2::text[] WHERE uuid = $1::uuid;",
		2, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to remove json field '%s' of '%s': %s", Path.c_str(), aUuid, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aJsonPath[128];
	if(!BuildJsonPath(Path, aJsonPath, sizeof(aJsonPath)))
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Uuid, aUuid, sizeof(aUuid));

	const char *apParams[2] = {aUuid, aJsonPath};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT jsonb_array_length(data #> $2::text[]) FROM players WHERE uuid = $1::uuid;",
		2, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	const bool Found = PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0);
	if(Found)
		*pLength = str_toint(PQgetvalue(pRes, 0, 0));
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::GetNpcAggro(const Uuid &Npc, const Uuid &User, int *pOut)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));

	const char *apParams[2] = {aNpc, aUser};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT COALESCE(SUM(delta), 0) FROM npc_aggro WHERE npc_id = $1::uuid AND userid = $2::uuid;",
		2, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	if(PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0))
		*pOut = str_toint(PQgetvalue(pRes, 0, 0));
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::AddNpcAggro(const Uuid &Npc, const Uuid &User, int Delta)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	char aDelta[32];
	char aCreated[32];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));
	str_format(aDelta, sizeof(aDelta), "%d", Delta);
	str_format(aCreated, sizeof(aCreated), "%d", time_timestamp());

	const char *apParams[4] = {aNpc, aUser, aDelta, aCreated};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO npc_aggro (npc_id, userid, delta, created_at) VALUES ($1::uuid, $2::uuid, $3::int, $4::bigint);",
		4, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to add npc aggro '%s'/'%s': %s", aNpc, aUser, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::EnsureNpcAggro(const Uuid &Npc, const Uuid &User)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aUser[UUID_MAXSTRSIZE];
	char aCreated[32];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	format_uuid(User, aUser, sizeof(aUser));
	str_format(aCreated, sizeof(aCreated), "%d", time_timestamp());

	const char *apParams[3] = {aNpc, aUser, aCreated};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO npc_aggro (npc_id, userid, delta, created_at) "
		"SELECT $1::uuid, $2::uuid, 0, $3::bigint WHERE NOT EXISTS "
		"(SELECT 1 FROM npc_aggro WHERE npc_id = $1::uuid AND userid = $2::uuid);",
		3, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to ensure npc aggro '%s'/'%s': %s", aNpc, aUser, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::LoadShopListings(array<SShopListing> &out)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	PGresult *pRes = PQexec(m_pConn,
		"SELECT id, seller, offer_item, offer_count, price_item, price_count FROM shop_listings ORDER BY id;");
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	for(int i = 0; i < PQntuples(pRes); i++)
	{
		SShopListing &Listing = out.emplace();
		Listing.m_Id = str_toint(PQgetvalue(pRes, i, 0));
		Listing.m_Seller = UUID_ZEROED;
		parse_uuid(&Listing.m_Seller, PQgetvalue(pRes, i, 1));
		str_copy(Listing.m_aOfferItem, PQgetvalue(pRes, i, 2), sizeof(Listing.m_aOfferItem));
		Listing.m_OfferCount = str_toint(PQgetvalue(pRes, i, 3));
		str_copy(Listing.m_aPriceItem, PQgetvalue(pRes, i, 4), sizeof(Listing.m_aPriceItem));
		Listing.m_PriceCount = str_toint(PQgetvalue(pRes, i, 5));
	}
	PQclear(pRes);
	return true;
}

bool CDatabasePostgres::SaveShopListing(const SShopListing &Listing)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aSeller[UUID_MAXSTRSIZE];
	char aId[32];
	char aOfferCount[32];
	char aPriceCount[32];
	format_uuid(Listing.m_Seller, aSeller, sizeof(aSeller));
	str_format(aId, sizeof(aId), "%d", Listing.m_Id);
	str_format(aOfferCount, sizeof(aOfferCount), "%d", Listing.m_OfferCount);
	str_format(aPriceCount, sizeof(aPriceCount), "%d", Listing.m_PriceCount);

	const char *apParams[6] = {aId, aSeller, Listing.m_aOfferItem, aOfferCount, Listing.m_aPriceItem, aPriceCount};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO shop_listings (id, seller, offer_item, offer_count, price_item, price_count) "
		"VALUES ($1::int, $2::uuid, $3::text, $4::int, $5::text, $6::int) "
		"ON CONFLICT (id) DO UPDATE SET seller = EXCLUDED.seller, offer_item = EXCLUDED.offer_item, "
		"offer_count = EXCLUDED.offer_count, price_item = EXCLUDED.price_item, price_count = EXCLUDED.price_count;",
		6, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to save shop listing #%d: %s", Listing.m_Id, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::DeleteShopListing(int Id)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aId[32];
	str_format(aId, sizeof(aId), "%d", Id);
	const char *apParams[1] = {aId};
	PGresult *pRes = PQexecParams(m_pConn, "DELETE FROM shop_listings WHERE id = $1::int;", 1, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::ClearNpcSells(const Uuid &Npc)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	const char *apParams[1] = {aNpc};
	PGresult *pRes = PQexecParams(m_pConn, "DELETE FROM npc_shop WHERE npc_id = $1::uuid;", 1, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::SaveNpcSell(const Uuid &Npc, const char *pItem, int Count, const char *pPriceItem, int PriceCount)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aCount[32];
	char aPriceCount[32];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	str_format(aCount, sizeof(aCount), "%d", Count);
	str_format(aPriceCount, sizeof(aPriceCount), "%d", PriceCount);

	const char *apParams[5] = {aNpc, pItem, aCount, pPriceItem, aPriceCount};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO npc_shop (npc_id, item, \"count\", price_item, price_count) VALUES ($1::uuid, $2::text, $3::int, $4::text, $5::int) "
		"ON CONFLICT (npc_id, item, price_item) DO UPDATE SET \"count\" = EXCLUDED.\"count\", price_count = EXCLUDED.price_count;",
		5, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to save npc sell '%s': %s", aNpc, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::GetNpcStock(const Uuid &Npc, const char *pItem, const char *pPriceItem, int *pStock, int *pRestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	format_uuid(Npc, aNpc, sizeof(aNpc));

	const char *apParams[3] = {aNpc, pItem, pPriceItem};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT stock, restock_at FROM npc_stock WHERE npc_id = $1::uuid AND item = $2::text AND price_item = $3::text;",
		3, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	bool Found = false;
	if(PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0))
	{
		*pStock = str_toint(PQgetvalue(pRes, 0, 0));
		*pRestockAt = str_toint(PQgetvalue(pRes, 0, 1));
		Found = true;
	}
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::SaveNpcStock(const Uuid &Npc, const char *pItem, const char *pPriceItem, int Stock, int RestockAt)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected())
		return false;

	char aNpc[UUID_MAXSTRSIZE];
	char aStock[32];
	char aRestock[32];
	format_uuid(Npc, aNpc, sizeof(aNpc));
	str_format(aStock, sizeof(aStock), "%d", Stock);
	str_format(aRestock, sizeof(aRestock), "%d", RestockAt);

	const char *apParams[5] = {aNpc, pItem, pPriceItem, aStock, aRestock};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO npc_stock (npc_id, item, price_item, stock, restock_at) VALUES ($1::uuid, $2::text, $3::text, $4::int, $5::bigint) "
		"ON CONFLICT (npc_id, item, price_item) DO UPDATE SET stock = EXCLUDED.stock, restock_at = EXCLUDED.restock_at;",
		5, 0, apParams, 0, 0, 0);
	const bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to save npc stock '%s': %s", aNpc, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::SaveWorldSave(const char *pMap, const char *pJsonData)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pMap || !pJsonData)
		return false;

	const char *apParams[2] = {pMap, pJsonData};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO world_saves (map, data) VALUES ($1, $2::jsonb)"
		" ON CONFLICT (map) DO UPDATE SET data = EXCLUDED.data;",
		2, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to save world save on '%s': %s", pMap, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::GetWorldSaveSize(const char *pMap, int *pSize)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pMap || !pSize)
		return false;

	const char *apParams[1] = {pMap};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT length(data::text) FROM world_saves WHERE map = $1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	const bool Found = PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0);
	if(Found)
		*pSize = str_toint(PQgetvalue(pRes, 0, 0));
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::GetWorldSaveData(const char *pMap, char *pOut, int Size)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pMap || !pOut)
		return false;

	const char *apParams[1] = {pMap};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT data::text FROM world_saves WHERE map = $1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		PQclear(pRes);
		return false;
	}

	const bool Found = PQntuples(pRes) > 0 && !PQgetisnull(pRes, 0, 0);
	if(Found)
		str_copy(pOut, PQgetvalue(pRes, 0, 0), Size);
	PQclear(pRes);
	return Found;
}

bool CDatabasePostgres::DeleteWorldSave(const char *pMap)
{
	CDatabaseLock Guard(m_Lock);
	if(!EnsureConnected() || !pMap)
		return false;

	const char *apParams[1] = {pMap};
	PGresult *pRes = PQexecParams(m_pConn,
		"DELETE FROM world_saves WHERE map = $1;",
		1, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("database", "failed to delete world save on '%s': %s", pMap, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}
