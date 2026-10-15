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
		" data JSONB NOT NULL DEFAULT '{}');";

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

	if(Ok && SavesOk && AggroOk && ShopOk)
		dbg_msg("database", "postgres database ready");
	return Ok && SavesOk && AggroOk && ShopOk;
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
