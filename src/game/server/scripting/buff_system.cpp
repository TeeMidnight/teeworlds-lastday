/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/scripting/buff_system.h>
#include <game/server/scripting/script_runtime.h>

struct CBuffListData
{
	CBuffSystem *m_pSelf;
	char m_aPath[IO_MAX_PATH_LENGTH];
};

static bool JsonIdFromFilename(const char *pFilename, char *pId, int Size)
{
	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return false;
	if(Len - 5 >= Size)
		return false;
	str_copy(pId, pFilename, Size);
	pId[Len - 5] = '\0';
	return true;
}

static int ReadInt(const json_value &Value, int Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (int) (json_int_t) Value;
	return Default;
}

static float ReadFloat(const json_value &Value, float Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (float) (double) Value;
	return Default;
}

void CBuffSystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	m_pStorage = pStorage;
	Load();
}

int CBuffSystem::ListDirsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	CBuffSystem *pSelf = static_cast<CBuffSystem *>(pUser);
	if(!pFilename[0] || str_comp(pFilename, ".") == 0 || str_comp(pFilename, "..") == 0)
		return 0;
	if(IsDir)
		return 0;

	char aId[64];
	if(!JsonIdFromFilename(pFilename, aId, sizeof(aId)))
		return 0;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "buffs/%s", pFilename);
	pSelf->LoadBuff(aId, aPath);
	return 0;
}

int CBuffSystem::ListFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	CBuffSystem *pSelf = static_cast<CBuffSystem *>(pUser);
	if(IsDir)
		return 0;

	char aId[64];
	if(!JsonIdFromFilename(pFilename, aId, sizeof(aId)))
		return 0;

	// The callback path is the full path of the directory in the existing
	// item loader; here ListDirectory is called twice, so rebuild it from the
	// directory argument. The directory is encoded in pUser by the caller.
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "buffs/%s", pFilename);
	pSelf->LoadBuff(aId, aPath);
	return 0;
}

void CBuffSystem::LoadBuff(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
		return;

	SBuffDef *pDef = new SBuffDef();
	const char *pId = (*pJson)["id"];
	str_copy(pDef->m_aId, pId[0] ? pId : pDefaultId, sizeof(pDef->m_aId));
	str_copy(pDef->m_aName, (*pJson)["name"], sizeof(pDef->m_aName));
	str_copy(pDef->m_aDesc, (*pJson)["desc"], sizeof(pDef->m_aDesc));
	str_copy(pDef->m_aScript, (*pJson)["script"], sizeof(pDef->m_aScript));
	pDef->m_Duration = ReadInt((*pJson)["duration"], 0);
	pDef->m_MaxStacks = maximum(1, ReadInt((*pJson)["max_stacks"], 1));
	const json_value &StackDuration = (*pJson)["stack_duration"];
	pDef->m_StackDuration = StackDuration.type != json_boolean || StackDuration.u.boolean != 0;

	const json_value &Modifiers = (*pJson)["modifiers"];
	if(Modifiers.type == json_object)
	{
		pDef->m_Health = ReadInt(Modifiers["health"], 0);
		pDef->m_Range = ReadFloat(Modifiers["range"], 0.0f);
		pDef->m_Speed = ReadFloat(Modifiers["speed"], 0.0f);
	}

	if(pDef->m_aScript[0])
	{
		pDef->m_Module = CScriptRuntime::Get()->LoadModule(pDef->m_aScript);
		if(pDef->m_Module < 0)
			dbg_msg("buff", "script '%s' for buff '%s' failed to load", pDef->m_aScript, pDef->m_aId);
	}

	const unsigned Hash = str_quickhash(pDef->m_aId);
	if(m_Defs.get(Hash))
	{
		dbg_msg("buff", "duplicate buff id '%s', keeping the first", pDef->m_aId);
		delete pDef;
		return;
	}

	m_Defs.set(Hash, pDef);
	m_lDefs.add(pDef);
	dbg_msg("buff", "loaded buff '%s' (%d stack(s), duration %ds)", pDef->m_aId, pDef->m_MaxStacks, pDef->m_Duration);
}

void CBuffSystem::Load()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();
	for(int i = 0; i < MAX_CLIENTS; i++)
		m_aBuffs[i].clear();

	if(!m_pStorage)
		return;
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "buffs", ListDirsCallback, this);
}

void CBuffSystem::ReloadScripts()
{
	for(int i = 0; i < m_lDefs.size(); i++)
	{
		SBuffDef *pDef = m_lDefs[i];
		if(pDef->m_aScript[0])
			pDef->m_Module = CScriptRuntime::Get()->LoadModule(pDef->m_aScript);
	}
}

const SBuffDef *CBuffSystem::GetDef(const char *pBuffId) const
{
	if(!pBuffId || !pBuffId[0])
		return nullptr;
	SBuffDef *const *ppDef = m_Defs.get(str_quickhash(pBuffId));
	return ppDef ? *ppDef : nullptr;
}

int CBuffSystem::FindInstance(int ClientID, const char *pBuffId) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !pBuffId)
		return -1;
	for(int i = 0; i < m_aBuffs[ClientID].size(); i++)
		if(str_comp(m_lDefs[m_aBuffs[ClientID][i].m_DefIndex]->m_aId, pBuffId) == 0)
			return i;
	return -1;
}

void CBuffSystem::CallEvent(SBuffInstance &Buff, int ClientID, const char *pEvent)
{
	if(Buff.m_DefIndex < 0 || Buff.m_DefIndex >= m_lDefs.size())
		return;
	SBuffDef *pDef = m_lDefs[Buff.m_DefIndex];
	if(pDef->m_Module < 0)
		return;

	SScriptContext Ctx;
	Ctx.m_pGameServer = m_pGameServer;
	Ctx.m_ClientID = ClientID;
	CPlayer *pPlayer = m_pGameServer->m_apPlayers[ClientID];
	if(pPlayer && pPlayer->GetCharacter())
		Ctx.m_WorldID = pPlayer->GetCharacter()->GameWorld()->WorldID();
	Ctx.m_Now = m_pGameServer->Server()->Tick();
	Ctx.m_TickSpeed = m_pGameServer->Server()->TickSpeed();
	str_copy(Ctx.m_aEvent, pEvent, sizeof(Ctx.m_aEvent));
	str_copy(Ctx.m_aBuff, pDef->m_aId, sizeof(Ctx.m_aBuff));
	Ctx.m_State = Buff.m_State;

	if(pPlayer && pPlayer->GetCharacter())
		Ctx.m_Pos = pPlayer->GetCharacter()->GetPos();

	if(!CScriptRuntime::Get()->Call(pDef->m_Module, pEvent, Ctx))
	{
		dbg_msg("buff", "script error for '%s': %s", pDef->m_aId, CScriptRuntime::Get()->LastError());
		return;
	}
	Buff.m_State = Ctx.m_State;
}

bool CBuffSystem::Add(int ClientID, const char *pBuffId, int DurationTicks, int Stacks, int Source)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_pGameServer->m_apPlayers[ClientID])
		return false;

	const SBuffDef *pDef = GetDef(pBuffId);
	if(!pDef)
		return false;

	int DefIndex = -1;
	for(int i = 0; i < m_lDefs.size(); i++)
	{
		if(m_lDefs[i] == pDef)
		{
			DefIndex = i;
			break;
		}
	}
	if(DefIndex < 0)
		return false;
	const int TickSpeed = m_pGameServer->Server()->TickSpeed();
	const int Duration = DurationTicks >= 0 ? DurationTicks : (pDef->m_Duration < 0 ? -1 : pDef->m_Duration * TickSpeed);

	int Index = FindInstance(ClientID, pBuffId);
	if(Index >= 0)
	{
		SBuffInstance &Buff = m_aBuffs[ClientID][Index];
		Buff.m_Stacks = minimum(pDef->m_MaxStacks, Buff.m_Stacks + maximum(1, Stacks));
		if(Duration < 0)
			Buff.m_Remaining = -1;
		else if(pDef->m_StackDuration)
			Buff.m_Remaining = maximum(Buff.m_Remaining, Duration);
		else
			Buff.m_Remaining = maximum(Buff.m_Remaining, Duration);
		Buff.m_Source = Source;
		CallEvent(Buff, ClientID, "on_apply");
		RefreshCharacter(ClientID);
		return true;
	}

	SBuffInstance Buff;
	Buff.m_DefIndex = DefIndex;
	Buff.m_Stacks = maximum(1, Stacks);
	Buff.m_Remaining = Duration;
	Buff.m_Source = Source;
	Buff.m_State = "{}";
	m_aBuffs[ClientID].add(Buff);
	CallEvent(m_aBuffs[ClientID][m_aBuffs[ClientID].size() - 1], ClientID, "on_apply");
	RefreshCharacter(ClientID);
	return true;
}

bool CBuffSystem::AddById(int ClientID, const char *pBuffId, int DurationTicks, int Stacks, int Source)
{
	return Add(ClientID, pBuffId, DurationTicks, Stacks, Source);
}

bool CBuffSystem::Remove(int ClientID, const char *pBuffId)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;
	const int Index = FindInstance(ClientID, pBuffId);
	if(Index < 0)
		return false;

	CallEvent(m_aBuffs[ClientID][Index], ClientID, "on_remove");
	m_aBuffs[ClientID].remove_index(Index);
	RefreshCharacter(ClientID);
	return true;
}

bool CBuffSystem::RemoveById(int ClientID, const char *pBuffId)
{
	return Remove(ClientID, pBuffId);
}

bool CBuffSystem::Has(int ClientID, const char *pBuffId) const
{
	return FindInstance(ClientID, pBuffId) >= 0;
}

int CBuffSystem::Stacks(int ClientID, const char *pBuffId) const
{
	const int Index = FindInstance(ClientID, pBuffId);
	return Index >= 0 ? m_aBuffs[ClientID][Index].m_Stacks : 0;
}

void CBuffSystem::ClearPlayer(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	while(m_aBuffs[ClientID].size() > 0)
	{
		CallEvent(m_aBuffs[ClientID][m_aBuffs[ClientID].size() - 1], ClientID, "on_remove");
		m_aBuffs[ClientID].remove_index(m_aBuffs[ClientID].size() - 1);
	}
	RefreshCharacter(ClientID);
}

void CBuffSystem::RefreshCharacter(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_pGameServer)
		return;
	CCharacter *pChr = m_pGameServer->GetPlayerChar(ClientID);
	if(pChr)
		pChr->RefreshAccessoryEffects();
}

void CBuffSystem::TickPlayer(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;

	for(int i = m_aBuffs[ClientID].size() - 1; i >= 0; i--)
	{
		SBuffInstance &Buff = m_aBuffs[ClientID][i];
		if(Buff.m_DefIndex < 0 || Buff.m_DefIndex >= m_lDefs.size())
		{
			m_aBuffs[ClientID].remove_index(i);
			continue;
		}

		CallEvent(Buff, ClientID, "on_tick");

		bool Remove = false;
		if(Buff.m_Remaining > 0)
		{
			Buff.m_Remaining--;
			if(Buff.m_Remaining == 0)
				Remove = true;
		}
		if(Remove)
		{
			CallEvent(Buff, ClientID, "on_remove");
			m_aBuffs[ClientID].remove_index(i);
		}
	}
}

void CBuffSystem::GetModifiers(int ClientID, int *pHealth, float *pRange, float *pSpeed) const
{
	if(pHealth)
		*pHealth = 0;
	if(pRange)
		*pRange = 0.0f;
	if(pSpeed)
		*pSpeed = 0.0f;
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;

	for(int i = 0; i < m_aBuffs[ClientID].size(); i++)
	{
		const SBuffInstance &Buff = m_aBuffs[ClientID][i];
		if(Buff.m_DefIndex < 0 || Buff.m_DefIndex >= m_lDefs.size())
			continue;
		const SBuffDef *pDef = m_lDefs[Buff.m_DefIndex];
		if(pHealth)
			*pHealth += pDef->m_Health * Buff.m_Stacks;
		if(pRange)
			*pRange += pDef->m_Range * Buff.m_Stacks;
		if(pSpeed)
			*pSpeed += pDef->m_Speed * Buff.m_Stacks;
	}
}
