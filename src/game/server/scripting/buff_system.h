/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_SCRIPTING_BUFF_SYSTEM_H
#define GAME_SERVER_SCRIPTING_BUFF_SYSTEM_H

#include <base/system.h>
#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/tl/timer.h>
#include <engine/shared/protocol.h>

#include <string>

class CGameContext;
class CPlayer;
class IStorage;

// A buff definition is data plus an optional Python module:
//
//   data/buffs/<id>.json
//   data/scripts/buffs/<id>.py
//
// The C++ side owns duration, stacking and the stat modifiers. The script is
// called every tick for every active instance (on_tick), and on apply/remove.
struct SBuffDef
{
	char m_aId[64] = "";
	char m_aName[64] = "";
	char m_aDesc[256] = "";
	char m_aScript[128] = "";
	int m_Duration = 0; // seconds, -1 = permanent
	int m_MaxStacks = 1;
	bool m_StackDuration = true;
	int m_Health = 0; // per stack
	float m_Range = 0.0f; // per stack
	float m_Speed = 0.0f; // per stack
	int m_Module = -1;
};

struct SBuffInstance
{
	int m_DefIndex = -1;
	int m_Stacks = 1;
	tick_timer m_Remaining; // infinite = permanent
	int m_Source = -1;
	std::string m_State = "{}";
};

class CBuffSystem
{
public:
	void Init(CGameContext *pGameServer, IStorage *pStorage);
	void Load();
	void ReloadScripts();

	bool Add(int ClientID, const char *pBuffId, int DurationTicks = -1, int Stacks = 1, int Source = -1);
	bool Remove(int ClientID, const char *pBuffId);
	bool Has(int ClientID, const char *pBuffId) const;
	int Stacks(int ClientID, const char *pBuffId) const;
	void ClearPlayer(int ClientID);

	// called once per game tick for every client with at least one buff
	void TickPlayer(int ClientID);

	// summed modifiers from all active instances; the character clamps them
	void GetModifiers(int ClientID, int *pHealth, float *pRange, float *pSpeed) const;

	const SBuffDef *GetDef(const char *pBuffId) const;
	int NumDefs() const { return m_lDefs.size(); }

	// script / console helpers
	bool AddById(int ClientID, const char *pBuffId, int DurationTicks, int Stacks, int Source);
	bool RemoveById(int ClientID, const char *pBuffId);

private:
	CGameContext *m_pGameServer = nullptr;
	IStorage *m_pStorage = nullptr;

	array<SBuffDef *> m_lDefs;
	hash_table<unsigned, SBuffDef *, 8> m_Defs;

	array<SBuffInstance> m_aBuffs[MAX_CLIENTS];

	static int ListDirsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	static int ListFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadBuff(const char *pDefaultId, const char *pFilePath);

	void CallEvent(SBuffInstance &Buff, int ClientID, const char *pEvent);
	void RefreshCharacter(int ClientID);
	int FindInstance(int ClientID, const char *pBuffId) const;
};

#endif
