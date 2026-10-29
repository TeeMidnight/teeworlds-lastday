/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_DATABASE_DATABASE_SQLITE_H
#define GAME_SERVER_DATABASE_DATABASE_SQLITE_H

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
	virtual bool EnsureGuestPlayer(const Uuid &Uuid, const char *pUsername) override;
	virtual bool CreateTeam(const char *pName, const Uuid &Leader, bool FriendlyFire, CDatabase::STeamData &out) override;
	virtual bool FindTeamByName(const char *pName, CDatabase::STeamData &out) override;
	virtual bool FindTeamForMember(const Uuid &Account, CDatabase::STeamData &out) override;
	virtual bool ListTeams(array<CDatabase::STeamData> &out) override;
	virtual bool LoadTeamMembers(int TeamId, array<Uuid> &out) override;
	virtual bool AddTeamMember(int TeamId, const Uuid &Account) override;
	virtual bool RemoveTeamMember(int TeamId, const Uuid &Account) override;
	virtual bool DeleteTeam(int TeamId) override;
	virtual bool SetTeamLeader(int TeamId, const Uuid &Leader) override;
	virtual bool SetTeamFriendlyFire(int TeamId, bool FriendlyFire) override;
	virtual bool LoadByUuid(const Uuid &Uuid, SPlayerData &out) override;
	virtual bool GetJson(const Uuid &Uuid, const CJsonPath &Path, char *pOut, int Size) override;
	virtual bool SetJson(const Uuid &Uuid, const CJsonPath &Path, const char *pValue) override;
	virtual bool DelJson(const Uuid &Uuid, const CJsonPath &Path) override;
	virtual bool GetJsonLength(const Uuid &Uuid, const CJsonPath &Path, int *pLength) override;
	virtual bool GetNpcAggro(const Uuid &Npc, const Uuid &User, int *pOut) override;
	virtual bool AddNpcAggro(const Uuid &Npc, const Uuid &User, int Delta) override;
	virtual bool EnsureNpcAggro(const Uuid &Npc, const Uuid &User) override;
	virtual bool LoadShopListings(array<SShopListing> &out) override;
	virtual bool SaveShopListing(const SShopListing &Listing) override;
	virtual bool DeleteShopListing(int Id) override;
	virtual bool ClearNpcSells(const Uuid &Npc) override;
	virtual bool SaveNpcSell(const Uuid &Npc, const char *pItem, int Count, int64 Price) override;
	virtual bool GetNpcStock(const Uuid &Npc, const char *pItem, int64 Price, int *pStock, int *pRestockAt) override;
	virtual bool SaveNpcStock(const Uuid &Npc, const char *pItem, int64 Price, int Stock, int RestockAt) override;
	virtual bool GetCurrencyBalance(const Uuid &Account, int64 *pOut) override;
	virtual bool AddCurrency(const Uuid &Account, int64 Delta, const char *pReason) override;
	virtual bool SpendCurrency(const Uuid &Account, int64 Amount, const char *pReason) override;
	virtual bool TransferCurrency(const Uuid &From, const Uuid &To, int64 Amount,
		const char *pFromReason, const char *pToReason) override;
	virtual bool SaveWorldSave(const char *pMap, const char *pJsonData) override;
	virtual bool GetWorldSaveSize(const char *pMap, int *pSize) override;
	virtual bool GetWorldSaveData(const char *pMap, char *pOut, int Size) override;
	virtual bool DeleteWorldSave(const char *pMap) override;
};

#endif // GAME_SERVER_DATABASE_DATABASE_SQLITE_H
