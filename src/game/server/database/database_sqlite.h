#ifndef GAME_SERVER_DATABASE_SQLITE_H
#define GAME_SERVER_DATABASE_SQLITE_H

#include <game/server/database/database.h>

// forward declaration so the header does not pull in sqlite3.h
struct sqlite3;

class CDatabaseSQLite : public CDatabase
{
	struct sqlite3 *m_pDB;
	char m_aPath[256];

public:
	explicit CDatabaseSQLite(const SConfig &Config);
	virtual ~CDatabaseSQLite();

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

#endif // GAME_SERVER_DATABASE_SQLITE_H
