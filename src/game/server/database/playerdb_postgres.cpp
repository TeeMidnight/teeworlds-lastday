/* PostgreSQL (libpq) backend for the Player database. */
#include <game/server/database/playerdb_postgres.h>

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

bool CDatabasePostgres::Init()
{
	m_pConn = PQconnectdb(m_aConnString);
	if(PQstatus(m_pConn) != CONNECTION_OK)
	{
		dbg_msg("playerdb", "failed to connect to postgres: %s", PQerrorMessage(m_pConn));
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
		" data JSONB NOT NULL DEFAULT '{}');";

	PGresult *pRes = PQexec(m_pConn, pCreate);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("playerdb", "failed to create schema: %s", PQresultErrorMessage(pRes));
	PQclear(pRes);

	if(Ok)
		dbg_msg("playerdb", "postgres database ready");
	return Ok;
}

bool CDatabasePostgres::FindByName(const char *pUsername, SPlayerData &out)
{
	if(!m_pConn)
		return false;

	const char *apParams[1] = {pUsername};
	PGresult *pRes = PQexecParams(m_pConn,
		"SELECT uuid, username, password FROM players WHERE username = $1;",
		1, 0, apParams, 0, 0, 0);
	if(PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		dbg_msg("playerdb", "FindByName failed: %s", PQresultErrorMessage(pRes));
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
	if(!m_pConn)
		return false;

	char aUuid[UUID_MAXSTRSIZE];
	format_uuid(Player.m_Uuid, aUuid, sizeof(aUuid));

	const char *apParams[3] = {aUuid, Player.m_aUsername, Player.m_aPasswordHash};
	PGresult *pRes = PQexecParams(m_pConn,
		"INSERT INTO players (uuid, username, password) VALUES ($1::uuid, $2, $3);",
		3, 0, apParams, 0, 0, 0);
	bool Ok = PQresultStatus(pRes) == PGRES_COMMAND_OK;
	if(!Ok)
		dbg_msg("playerdb", "failed to insert Player '%s': %s", Player.m_aUsername, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::LoadByUuid(const Uuid &Uuid, SPlayerData &out)
{
	if(!m_pConn)
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
	if(!m_pConn)
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
	if(!m_pConn)
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
		dbg_msg("playerdb", "failed to set json field '%s' of '%s': %s", Path.c_str(), aUuid, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::DelJson(const Uuid &Uuid, const CJsonPath &Path)
{
	if(!m_pConn)
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
		dbg_msg("playerdb", "failed to remove json field '%s' of '%s': %s", Path.c_str(), aUuid, PQresultErrorMessage(pRes));
	PQclear(pRes);
	return Ok;
}

bool CDatabasePostgres::GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength)
{
	if(!m_pConn)
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
