#include <game/server/weapon.h>
#include <game/server/weaponmanager.h>

#include <base/system.h>
#include <engine/server.h>

#include <game/server/entities/character.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <generated/server_data.h>

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

int CWeaponManager::FireWeapon(CCharacter *pChr, vec2 Direction, unsigned WeaponID, int AmmoDamage)
{
	IWeaponInterface *pWeapon = GetWeapon(WeaponID);
	if(!pWeapon)
		return 0;

	int ReloadTicks = pWeapon->OnFire(pChr, Direction, AmmoDamage);
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
