#include <game/server/database/database.h>

#include <base/system.h>

#include <game/server/database/database_postgres.h>
#include <game/server/database/database_sqlite.h>

CDatabase *CreateDatabase(const CDatabase::SConfig &Config)
{
	const char *pBackend = Config.m_pBackend && Config.m_pBackend[0] ? Config.m_pBackend : "sqlite";

	if(str_comp_nocase(pBackend, "sqlite") == 0)
		return new CDatabaseSQLite(Config);

	if(str_comp_nocase(pBackend, "postgres") == 0)
		return new CDatabasePostgres(Config);

	dbg_msg("database", "unknown database backend '%s'", pBackend);
	return 0;
}
