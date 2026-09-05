#ifndef GAME_SERVER_DATABASE_POSTGRES_H
#define GAME_SERVER_DATABASE_POSTGRES_H

#include <game/server/database/database.h>

// forward declaration so the header does not pull in libpq-fe.h
typedef struct pg_conn PGconn;

class CDatabasePostgres : public CDatabase
{
	PGconn *m_pConn;
	char m_aConnString[512];

public:
	explicit CDatabasePostgres(const SConfig &Config);
	virtual ~CDatabasePostgres();

	virtual bool Init() override;
	virtual bool FindByName(const char *pUsername, SPlayerData &out) override;
	virtual bool InsertPlayer(const SPlayerData &Player) override;
	virtual bool LoadByUuid(const Uuid &Uuid, SPlayerData &out) override;
	virtual bool GetJson(const Uuid &Uuid, const CJsonPath &Path, char *pOut, int Size) override;
	virtual bool SetJson(const Uuid &Uuid, const CJsonPath &Path, const char *pValue) override;
	virtual bool DelJson(const Uuid &Uuid, const CJsonPath &Path) override;
	virtual bool GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength) override;
	virtual bool SaveWorldSave(const char *pMap, const char *pJsonData) override;
	virtual bool GetWorldSaveData(const char *pMap, char *pOut, int Size) override;
	virtual bool DeleteWorldSave(const char *pMap) override;
};

#endif // GAME_SERVER_DATABASE_POSTGRES_H
