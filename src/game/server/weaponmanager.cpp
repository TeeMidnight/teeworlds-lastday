/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/server.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>
#include <game/server/entities/character.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <game/server/scripting/script_runtime.h>
#include <game/server/scripting/scripted_weapon.h>
#include <game/server/weapon.h>
#include <game/server/weaponmanager.h>
#include <generated/protocol.h>
#include <generated/server_data.h>

struct SScriptedWeaponListData
{
	CWeaponManager *m_pManager;
	IStorage *m_pStorage;
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

static int SnapStyleFromName(const char *pName)
{
	if(str_comp_nocase(pName, "hammer") == 0)
		return WEAPON_HAMMER;
	if(str_comp_nocase(pName, "gun") == 0)
		return WEAPON_GUN;
	if(str_comp_nocase(pName, "shotgun") == 0)
		return WEAPON_SHOTGUN;
	if(str_comp_nocase(pName, "grenade") == 0)
		return WEAPON_GRENADE;
	if(str_comp_nocase(pName, "laser") == 0)
		return WEAPON_LASER;
	if(str_comp_nocase(pName, "ninja") == 0)
		return WEAPON_NINJA;
	if(str_comp_nocase(pName, "sword") == 0)
		return WEAPON_NINJA;
	if(str_comp_nocase(pName, "hand") == 0)
		return -1;
	return -1;
}

// snap_style accepts either the protocol integer (preferred, unambiguous) or a
// protocol weapon name ("gun", "ninja", ...). "hand" maps to -1 (no weapon
// rendered by the client).
static int SnapStyleFromJson(const json_value &Value, const char *pName)
{
	if(Value.type == json_integer)
		return (int) (json_int_t) Value;
	if(Value.type == json_double)
		return (int) (double) Value;
	if(Value.type == json_string)
		return SnapStyleFromName(Value);
	return SnapStyleFromName(pName);
}

static int DefaultFireDelayFromName(const char *pName)
{
	if(str_comp_nocase(pName, "shotgun") == 0 || str_comp_nocase(pName, "grenade") == 0)
		return 500;
	if(str_comp_nocase(pName, "laser") == 0)
		return 800;
	if(str_comp_nocase(pName, "hand") == 0)
		return 500;
	return 125;
}

// Static registry: CWeaponRegistrar writes the weapon instances here during
// static initialization. A function-local static avoids cross-TU static
// initialization order problems.
static array<IWeaponInterface *> &WeaponRegistry()
{
	static array<IWeaponInterface *> s_lRegistry;
	return s_lRegistry;
}

void CWeaponManager::RegisterWeapon(IWeaponInterface *pWeapon)
{
	WeaponRegistry().add(pWeapon);
}

void CWeaponManager::Init()
{
	if(m_Initialized)
		return;
	m_Initialized = true;

	for(int i = 0; i < WeaponRegistry().size(); i++)
	{
		IWeaponInterface *pWeapon = WeaponRegistry()[i];

		// only one weapon per ID (hash collision / double registration guard)
		if(m_Weapons.get(pWeapon->ID()))
		{
			dbg_msg("weapon", "duplicate weapon id for '%s'", pWeapon->Name());
			continue;
		}

		m_Weapons.set(pWeapon->ID(), pWeapon);
		m_apWeaponList.add(pWeapon);
	}
}

IWeaponInterface *CWeaponManager::GetWeapon(unsigned WeaponID)
{
	Init();
	// a scripted definition takes precedence over a native class of the same name
	CScriptedWeapon **ppScripted = m_ScriptedWeapons.get(WeaponID);
	if(ppScripted && *ppScripted)
		return *ppScripted;
	IWeaponInterface **ppWeapon = m_Weapons.get(WeaponID);
	return ppWeapon ? *ppWeapon : nullptr;
}

IWeaponInterface *CWeaponManager::GetWeaponByName(const char *pName)
{
	return GetWeapon(WeaponID(pName));
}

IWeaponInterface *CWeaponManager::GetWeaponByIndex(int Index)
{
	Init();
	return Index >= 0 && Index < m_apWeaponList.size() ? m_apWeaponList[Index] : nullptr;
}

void CWeaponManager::OutputRegisteredWeapons()
{
	Init();
	dbg_msg("weapon", "%d weapon class(es) registered", m_apWeaponList.size());
	for(int i = 0; i < m_apWeaponList.size(); i++)
		dbg_msg("weapon", "  id=%08x name='%s' style=%d", m_apWeaponList[i]->ID(), m_apWeaponList[i]->Name(), m_apWeaponList[i]->SnapStyle());
}

void CWeaponManager::LoadScriptedWeapons(IStorage *pStorage)
{
	ClearScriptedWeapons();
	if(!pStorage)
	{
		dbg_msg("weapon", "no storage, scripted weapons unavailable");
		return;
	}

	SScriptedWeaponListData Data;
	Data.m_pManager = this;
	Data.m_pStorage = pStorage;
	pStorage->ListDirectory(IStorage::TYPE_ALL, "weapons", ListScriptedWeaponsCallback, &Data);

	dbg_msg("weapon", "%d scripted weapon(s) active", m_apScriptedWeaponList.size());
}

void CWeaponManager::ReloadScripts()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	LoadScriptedWeapons(pRuntime ? pRuntime->Storage() : nullptr);
}

void CWeaponManager::ClearScriptedWeapons()
{
	for(int i = 0; i < m_apScriptedWeaponList.size(); i++)
		delete m_apScriptedWeaponList[i];
	m_apScriptedWeaponList.clear();
	m_ScriptedWeapons.clear();
}

CScriptedWeapon *CWeaponManager::GetScriptedWeaponByIndex(int Index) const
{
	return Index >= 0 && Index < m_apScriptedWeaponList.size() ? m_apScriptedWeaponList[Index] : nullptr;
}

int CWeaponManager::ListScriptedWeaponsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	if(IsDir)
		return 0;
	if(!pFilename[0] || str_comp(pFilename, ".") == 0 || str_comp(pFilename, "..") == 0)
		return 0;

	SScriptedWeaponListData *pData = static_cast<SScriptedWeaponListData *>(pUser);
	char aName[64];
	if(!JsonIdFromFilename(pFilename, aName, sizeof(aName)))
		return 0;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "weapons/%s", pFilename);
	pData->m_pManager->LoadScriptedWeapon(pData->m_pStorage, aName, aPath);
	return 0;
}

void CWeaponManager::LoadScriptedWeapon(IStorage *pStorage, const char *pDefaultName, const char *pPath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pPath, pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("weapon", "failed to parse '%s': %s", pPath, Parser.Error());
		return;
	}

	const char *pName = (*pJson)["name"];
	if(!pName[0])
		pName = pDefaultName;

	const char *pScript = (*pJson)["script"];
	if(!pName[0] || !pScript[0])
	{
		dbg_msg("weapon", "scripted weapon '%s': missing name or script, ignored", pPath);
		return;
	}

	const int SnapStyle = SnapStyleFromJson((*pJson)["snap_style"], pName);
	const int FireDelay = ReadInt((*pJson)["fire_delay"], DefaultFireDelayFromName(pName));
	const bool FullAuto = static_cast<bool>((*pJson)["full_auto"]);
	const int DefaultAmmo = ReadInt((*pJson)["default_ammo"], -1);
	const int MaxAmmo = ReadInt((*pJson)["max_ammo"], DefaultAmmo);

	CScriptedWeapon *pWeapon = new CScriptedWeapon(pName, pScript, SnapStyle, FullAuto, FireDelay, DefaultAmmo, MaxAmmo);
	if(!pWeapon->LoadScript())
	{
		dbg_msg("weapon", "scripted weapon '%s' disabled: script '%s' failed", pName, pScript);
		delete pWeapon;
		return;
	}

	if(m_ScriptedWeapons.get(pWeapon->ID()))
	{
		dbg_msg("weapon", "duplicate scripted weapon '%s' in '%s', keeping the first", pName, pPath);
		delete pWeapon;
		return;
	}

	m_ScriptedWeapons.set(pWeapon->ID(), pWeapon);
	m_apScriptedWeaponList.add(pWeapon);
	dbg_msg("weapon", "loaded scripted weapon '%s' (style=%d, delay=%dms, auto=%d, ammo=%d/%d) script='%s'",
		pName, SnapStyle, FireDelay, FullAuto ? 1 : 0, DefaultAmmo, MaxAmmo, pScript);
}

int CWeaponManager::SnapStyle(unsigned WeaponID)
{
	IWeaponInterface *pWeapon = GetWeapon(WeaponID);
	return pWeapon ? pWeapon->SnapStyle() : WEAPON_GUN;
}

bool CWeaponManager::IsFullAuto(unsigned WeaponID)
{
	IWeaponInterface *pWeapon = GetWeapon(WeaponID);
	return pWeapon ? pWeapon->FullAuto() : false;
}

int CWeaponManager::FireWeapon(CCharacter *pChr, vec2 Direction, unsigned WeaponID, const SAmmoEffect &Ammo)
{
	IWeaponInterface *pWeapon = GetWeapon(WeaponID);
	if(!pWeapon)
		return 0;

	int ReloadTicks = pWeapon->OnFire(pChr, Direction, Ammo);
	if(ReloadTicks == 0)
		ReloadTicks = pWeapon->FireDelay() * pChr->Server()->TickSpeed() / 1000;
	return ReloadTicks;
}

void CWeaponManager::TickWeapon(CCharacter *pChr)
{
	IWeaponInterface *pWeapon = GetWeapon(pChr->GetUsableWeapon());
	if(pWeapon)
		pWeapon->OnTick(pChr);
}

void CWeaponManager::OnWeaponSwitch(CCharacter *pChr)
{
	IWeaponInterface *pWeapon = GetWeapon(pChr->GetUsableWeapon());
	if(pWeapon)
		pWeapon->OnWeaponSwitch(pChr);
}

CWeaponRegistrar::CWeaponRegistrar(IWeaponInterface *pWeapon)
{
	CWeaponManager::RegisterWeapon(pWeapon);
}

CWeaponManager *WeaponManager()
{
	static CWeaponManager s_WeaponManager;
	return &s_WeaponManager;
}
