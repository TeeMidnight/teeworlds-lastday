/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <engine/shared/config.h>
#include <game/collision.h>
#include <game/server/botmanager.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/scripting/buff_system.h>
#include <game/server/weapon.h>
#include <game/server/weaponmanager.h>
#include <generated/server_data.h>

#include "character.h"
/*
#include "laser.h"
*/
#include "projectile.h"

// input count
CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;

	while(i != Cur)
	{
		i = (i + 1) & INPUT_STATE_MASK;
		if(i & 1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}

MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld) : CHitableEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER, 0, vec2(0, 0), ms_PhysSize)
{
	m_Health = 0;
	m_Armor = 0;
	m_Oxygen = 10;
	m_OxygenTick = 0;
	m_TriggeredEvents = 0;
	m_MaxHealth = MAX_HEALTH;
	m_RangeScale = 1.0f;
	m_SpeedScale = 1.0f;
	m_DrillControl = false;
	m_DrillExitRequest = false;
	m_Ninja.m_Active = false;
	mem_zero(m_aAccessories, sizeof(m_aAccessories));
	m_Tuning = *GameServer()->Tuning();
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_LastNoAmmoSound = -1;
	m_ActiveWeapon = WEAPON_HAMMER; // slot 0
	m_QueuedWeapon = -1;

	// oxygen starts full on every life
	m_Oxygen = 10;
	m_OxygenTick = 0;

	// the loadout is (re)filled by the game controller from the weapons the
	// player owns as items; clear any stale state from a previous life
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_aWeapons[i] = CItemID();
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
		m_aAccessories[i] = CItemID();
	// no accessories are worn yet; the game controller applies the player's
	// saved layout later in OnCharacterSpawn (once m_pPlayer is set)
	m_MaxHealth = MAX_HEALTH;
	m_RangeScale = 1.0f;
	m_SpeedScale = 1.0f;
	m_Core.m_SpeedScale = 1.0f;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	m_Core.Reset();
	m_Core.Init(&GameWorld()->m_Core, GameWorld()->Collision());
	m_Core.m_Pos = m_Pos;
	GameWorld()->m_Core.SetCharacter(m_pPlayer->GetCID(), &m_Core);

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));
	m_LegacyReckoningTick = 0;
	mem_zero(&m_LegacySendCore, sizeof(m_LegacySendCore));
	mem_zero(&m_LegacyReckoningCore, sizeof(m_LegacyReckoningCore));

	GameWorld()->InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);
	UpdateTuning();

	return true;
}

void CCharacter::Destroy()
{
	GameWorld()->m_Core.SetCharacter(m_pPlayer->GetCID(), 0);
	m_Alive = false;
}

// Slot is a loadout slot index (0..NUM_WEAPONS-1)
void CCharacter::SetWeapon(int Slot)
{
	if(Slot == m_ActiveWeapon)
		return;

	m_QueuedWeapon = -1;
	m_ActiveWeapon = Slot;
	GameWorld()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH);

	if(m_ActiveWeapon < 0 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = 0;
	WeaponManager()->OnWeaponSwitch(this);
}

bool CCharacter::IsGrounded()
{
	if(GameWorld()->Collision()->CheckPoint(m_Pos.x + GetProximityRadius() / 2, m_Pos.y + GetProximityRadius() / 2 + 5))
		return true;
	if(GameWorld()->Collision()->CheckPoint(m_Pos.x - GetProximityRadius() / 2, m_Pos.y + GetProximityRadius() / 2 + 5))
		return true;
	return false;
}

void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(!m_ReloadTimer.expired(Server()->Tick()) || m_QueuedWeapon == -1)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon + 1) % NUM_WEAPONS;
			Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon - 1) < 0 ? NUM_WEAPONS - 1 : WantedWeapon - 1;
			Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon - 1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(!m_ReloadTimer.expired(Server()->Tick()))
		return;

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	// the weapon the player may actually use: the slot's weapon when its item
	// is owned, otherwise the fallback hand (punch, no item needed)
	const unsigned ActiveWeaponID = GetUsableWeapon();

	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(WeaponManager()->IsFullAuto(ActiveWeaponID) && (m_LatestInput.m_Fire & 1))
		WillFire = true;

	if(!WillFire)
		return;

	IWeaponInterface *pWeapon = WeaponManager()->GetWeapon(ActiveWeaponID);
	if(!pWeapon)
		return;

	// ammo comes from the player's inventory (item system). a weapon that has
	// no ammo item defined (e.g. hammer, hand) is unlimited. several different
	// ammo items may feed one weapon, each with its own effect.
	const char *pWeaponName = pWeapon->Name();
	const bool NeedsAmmo = GameServer()->Item()->WeaponNeedsAmmo(pWeaponName);
	if(NeedsAmmo &&
		GameServer()->Item()->GetAmmoCountForWeapon(GetCID(), pWeaponName) <= 0)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer.start(Server()->Tick(), 125 * Server()->TickSpeed() / 1000 + 1);
		if(m_LastNoAmmoSound + Server()->TickSpeed() <= Server()->Tick())
		{
			GameWorld()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO);
			m_LastNoAmmoSound = Server()->Tick();
		}
		return;
	}

	if(Config()->m_Debug)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "shot player='%d:%s' team=%d weapon='%s'", m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), m_pPlayer->GetTeam(), pWeaponName);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	m_AttackTick = Server()->Tick();

	// consume one ammo item from the inventory and use its effect
	SAmmoEffect Ammo;
	if(NeedsAmmo)
		GameServer()->Item()->ConsumeAmmoForWeapon(GetCID(), pWeaponName, &Ammo);

	const int ReloadTicks = WeaponManager()->FireWeapon(this, Direction, ActiveWeaponID, Ammo);
	// the old countdown blocked the next ReloadTicks ticks; the deadline makes
	// the same wait survive a world that was frozen in between
	m_ReloadTimer.start(Server()->Tick(), ReloadTicks + 1);
}

void CCharacter::HandleWeapons()
{
	WeaponManager()->TickWeapon(this);

	// check reload timer
	if(!m_ReloadTimer.expired(Server()->Tick()))
		return;

	// fire Weapon, if wanted
	FireWeapon();
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

int CCharacter::GetCID()
{
	return m_pPlayer->GetCID();
}

// loadout (equipment menu) helpers; Slot is 0-based (0..NUM_WEAPONS-1)
// the slot holds an item (hash of its res_id); for weapon items the hash
// equals the weapon id. any owned item can be placed: only weapon items
// actually fire, everything else resolves to the hand fallback.
bool CCharacter::EquipWeaponSlot(int Slot, const CItemID &Id)
{
	if(Slot < 0 || Slot >= NUM_WEAPONS)
		return false;
	if(!Id.IsZero())
	{
		// the item must exist in the player's inventory
		if(!GameServer()->Item()->HasItem(GetCID(), Id))
			return false;
	}

	// remove the item from its previous slot so it is not duplicated
	for(int i = 0; i < NUM_WEAPONS; i++)
		if(i != Slot && m_aWeapons[i] == Id)
			m_aWeapons[i] = CItemID();

	m_aWeapons[Slot] = Id;

	// if the active slot became empty (its weapon was removed), switch to
	// another equipped weapon
	if(m_aWeapons[m_ActiveWeapon].IsZero())
	{
		for(int i = 0; i < NUM_WEAPONS; i++)
		{
			if(!m_aWeapons[i].IsZero())
			{
				m_ActiveWeapon = i;
				break;
			}
		}
	}
	return true;
}

CItemID CCharacter::WeaponAtSlot(int Slot) const
{
	if(Slot < 0 || Slot >= NUM_WEAPONS)
		return CItemID();
	return m_aWeapons[Slot];
}

int CCharacter::NumWeaponsHeld() const
{
	int Count = 0;
	for(int i = 0; i < NUM_WEAPONS; i++)
		if(!m_aWeapons[i].IsZero())
			Count++;
	return Count;
}

unsigned CCharacter::GetUsableWeapon()
{
	const CItemID SlotItem = WeaponAtSlot(m_ActiveWeapon);
	if(!SlotItem.IsZero())
	{
		// The item must still exist in the inventory. Do not clear the slot
		// here: this runs from Snap(), and a read path must not mutate the
		// character. UnequipMissingWeapons() clears it on the menu/tick path.
		if(!GameServer()->Item()->HasItem(GetCID(), SlotItem))
			return WeaponID("hand");

		// map the item to the weapon it equips as (by name)
		const char *pWeaponName = GameServer()->Item()->GetWeaponName(SlotItem);
		if(pWeaponName[0] && WeaponManager()->GetWeaponByName(pWeaponName))
			return WeaponID(pWeaponName);
		// else: an item without a corresponding weapon (e.g. a resource)
		// stays equipped but falls back to the hand
	}
	// empty slot, non-weapon item, or missing item: hand (no item needed)
	return WeaponID("hand");
}

bool CCharacter::UnequipMissingWeapons()
{
	bool Changed = false;
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		const CItemID &SlotItem = m_aWeapons[i];
		if(SlotItem.IsZero())
			continue;
		// the slot holds an item that no longer exists: clear it
		if(!GameServer()->Item()->HasItem(GetCID(), SlotItem))
		{
			m_aWeapons[i] = CItemID();
			Changed = true;
		}
	}

	// if the active slot was cleared, switch to another occupied slot
	if(m_aWeapons[m_ActiveWeapon].IsZero())
	{
		for(int i = 0; i < NUM_WEAPONS; i++)
		{
			if(!m_aWeapons[i].IsZero())
			{
				m_ActiveWeapon = i;
				break;
			}
		}
	}
	return Changed;
}

// ---- accessory loadout ----------------------------------------------------

bool CCharacter::EquipAccessorySlot(int Slot, const CItemID &Id)
{
	if(Slot < 0 || Slot >= NUM_ACCESSORY_SLOTS)
		return false;

	if(!Id.IsZero())
	{
		// only a declared accessory can be worn, and it must still be owned
		if(!GameServer()->Item()->IsAccessory(Id))
			return false;
		if(!GameServer()->Item()->HasItem(GetCID(), Id))
			return false;

		// one physical item cannot be worn in two slots at once
		for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
			if(i != Slot && m_aAccessories[i] == Id)
				m_aAccessories[i] = CItemID();
	}

	m_aAccessories[Slot] = Id;
	const int OldMaxHealth = m_MaxHealth;
	RefreshAccessoryEffects();
	// wearing a health accessory immediately grants the extra capacity, so the
	// HUD does not show a hole that the player has to heal into
	if(m_MaxHealth > OldMaxHealth)
		m_Health = clamp(m_Health + (m_MaxHealth - OldMaxHealth), 0, m_MaxHealth);
	return true;
}

CItemID CCharacter::AccessoryAtSlot(int Slot) const
{
	if(Slot < 0 || Slot >= NUM_ACCESSORY_SLOTS)
		return CItemID();
	return m_aAccessories[Slot];
}

void CCharacter::ClearAccessories()
{
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
		m_aAccessories[i] = CItemID();
	RefreshAccessoryEffects();
}

void CCharacter::SetAccessoryLayout(const CItemID *pIds)
{
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
		m_aAccessories[i] = pIds ? pIds[i] : CItemID();
	RefreshAccessoryEffects();
}

bool CCharacter::UnequipMissingAccessories()
{
	bool Changed = false;
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
	{
		const CItemID &Id = m_aAccessories[i];
		if(Id.IsZero())
			continue;
		if(!GameServer()->Item()->HasItem(GetCID(), Id) || !GameServer()->Item()->IsAccessory(Id))
		{
			m_aAccessories[i] = CItemID();
			Changed = true;
		}
	}
	if(Changed)
		RefreshAccessoryEffects();
	return Changed;
}

void CCharacter::RefreshAccessoryEffects()
{
	int Health = MAX_HEALTH;
	float Range = 1.0f;
	float Speed = 1.0f;

	CItemSystem *pItem = GameServer()->Item();
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
	{
		const CItemID &Id = m_aAccessories[i];
		if(Id.IsZero() || !pItem->HasItem(GetCID(), Id))
			continue;
		const CItemSystem::SAccessory Acc = pItem->GetAccessory(Id);
		if(!Acc.m_HasAccessory)
			continue;
		Health += Acc.m_Health;
		Range += Acc.m_Range;
		Speed += Acc.m_Speed;
	}

	// active buffs add on top of the worn accessories
	int BuffHealth = 0;
	float BuffRange = 0.0f;
	float BuffSpeed = 0.0f;
	if(GameServer()->Buffs())
		GameServer()->Buffs()->GetModifiers(GetCID(), &BuffHealth, &BuffRange, &BuffSpeed);
	Health += BuffHealth;
	Range += BuffRange;
	Speed += BuffSpeed;

	// clamp the combined bonuses so content cannot unboundedly break the game
	m_MaxHealth = clamp(Health, MAX_HEALTH, MAX_HEALTH * 2);
	m_RangeScale = clamp(Range, 1.0f, 2.0f);
	m_SpeedScale = clamp(Speed, 1.0f, 1.5f);

	// losing a health accessory can drop the player's maximum below the
	// current value; never leave them above it
	if(m_Health > m_MaxHealth)
		m_Health = m_MaxHealth;
	m_Core.m_SpeedScale = m_SpeedScale;
	UpdateTuning();
}

// The client and the server both use this tuning, so a drill-controlled tee is
// pinned on either side.
static void ApplyDrillTuning(CTuningParams &Tuning)
{
	Tuning.m_GroundControlSpeed = 0.0f;
	Tuning.m_AirControlSpeed = 0.0f;
	Tuning.m_GroundJumpImpulse = 0.0f;
	Tuning.m_AirJumpImpulse = 0.0f;
	Tuning.m_PlayerHooking = 0.0f;
	Tuning.m_Gravity = 0.0f;
}

void CCharacter::UpdateTuning()
{
	// The same water query the old core used: the upper half of the physics
	// box touching a water tile.
	const bool InWater = GameWorld()->Collision()->TestBox(m_Core.m_Pos,
		vec2(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE / 2), CCollision::COLFLAG_WATER);
	m_Tuning = BuildCharacterTuning(*GameServer()->Tuning(), m_SpeedScale, m_Core.m_SilentStep,
		InWater, m_Core.m_Input.m_Jump != 0, m_Core.m_Vel.y);

	if(m_DrillControl)
		ApplyDrillTuning(m_Tuning);
	m_Core.SetTuning(&m_Tuning);
}

CTuningParams CCharacter::LegacyTuning(int ClientVersion)
{
	// The 0.7 translation layer has one tuning and a gamecore without water or
	// silent-step handling. Reproduce the old Tick(false, false) path: keep the
	// speed bonus (and the drill lock), but leave water and silent step out.
	// Older 0.7 clients do not understand tuning at all and keep the defaults.
	CTuningParams Base = ClientVersion >= MIN_CORRECTTUNING_CLIENTVERSION ? *GameServer()->Tuning() : CTuningParams();
	CTuningParams Tuning = BuildCharacterTuning(Base, m_SpeedScale, false, false, false, 0.0f);
	if(m_DrillControl)
		ApplyDrillTuning(Tuning);
	return Tuning;
}

void CCharacter::SetDrillControl(bool Control)
{
	m_DrillControl = Control;
	m_DrillExitRequest = false;
	UpdateTuning();
	if(!Control)
		return;

	// pin the tee where it stands and drop any hook it still holds; the
	// client's prediction is frozen through the per-client tuning
	m_Core.m_Vel = vec2(0.0f, 0.0f);
	m_Core.m_HookState = HOOK_IDLE;
	m_Core.m_HookedPlayer = -1;
	m_Core.m_HookDragVel = vec2(0.0f, 0.0f);
	m_Core.m_HookPos = m_Core.m_Pos;
	m_Core.m_TriggeredEvents = 0;
	m_Input.m_Direction = 0;
	m_Input.m_Jump = 0;
	m_Input.m_Hook = 0;
}

bool CCharacter::ConsumeDrillExitRequest()
{
	const bool Request = m_DrillExitRequest;
	m_DrillExitRequest = false;
	return Request;
}

void CCharacter::TeleTo(vec2 Pos, bool KeepSpeed)
{
	if(!KeepSpeed)
		m_Core.m_Vel = vec2(0.0f, 0.0f);
	m_Pos = Pos;
	m_Core.m_Pos = Pos;
}

vec2 CCharacter::AimVector() const
{
	return vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY);
}

vec2 CCharacter::AimDirection() const
{
	vec2 Dir = AimVector();
	if(length(Dir) < 0.0001f)
		Dir = vec2(1.0f, 0.0f);
	return normalize(Dir);
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	// steering a drill: the aim and fire button still update (the drill reads
	// them), but the player must not also swing or fire their weapon. A jump
	// press is the exit request, handled by the owning CPlayer.
	if(m_DrillControl)
	{
		if(CountInput(m_LatestPrevInput.m_Jump, m_LatestInput.m_Jump).m_Presses)
			m_DrillExitRequest = true;
		mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
		return;
	}

	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	m_Input.m_Hook = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire & 1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::Tick()
{
	// the player's ready state is the silent-step toggle: while it is clear
	// the character creeps at half speed and its jumps make no sound
	m_Core.m_SilentStep = !m_pPlayer->m_IsReadyToPlay;
	// accessory speed bonus; the client is sent a matching tuning so its local
	// prediction agrees with the server simulation
	m_Core.m_SpeedScale = m_SpeedScale;

	m_Core.m_Input = m_Input;
	// A client that is loading a map sends no more input, so the character
	// would otherwise keep the last held direction and walk on across the new
	// world (and past its edge) for the whole download. Only the movement keys
	// are dropped; aiming stays.
	if(m_pPlayer->m_MapLoading)
	{
		m_Core.m_Input.m_Direction = 0;
		m_Core.m_Input.m_Jump = 0;
		m_Core.m_Input.m_Hook = 0;
	}
	UpdateTuning();
	if(m_DrillControl)
	{
		// steering a drill: freeze the core in place. The client receives zeroed
		// control speeds in its tuning, so its local prediction stays still too.
		m_Core.m_Input.m_Direction = 0;
		m_Core.m_Input.m_Jump = 0;
		m_Core.m_Input.m_Hook = 0;
		m_Core.m_Vel = vec2(0.0f, 0.0f);
		m_Core.m_SpeedScale = 0.0f;
		m_Core.m_TriggeredEvents = 0;
		m_Core.m_Angle = (int) (angle(vec2(m_Input.m_TargetX, m_Input.m_TargetY)) * 256.0f);
	}
	else
	{
		m_Core.Tick(true, true);
	}

	// handle leaving gamelayer
	if(GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	// oxygen: while underwater the player breathes from a 10-tank supply,
	// one tank every 2 seconds. when the air runs out the player drowns
	// (one health per second)
	if(InWater())
	{
		if(m_OxygenTick == 0)
			m_OxygenTick = Server()->Tick() + Server()->TickSpeed() * 2;
		else if(Server()->Tick() >= m_OxygenTick)
		{
			if(m_Oxygen > 0)
			{
				m_Oxygen--;
				m_OxygenTick += Server()->TickSpeed() * 2;
			}
			else
			{
				m_OxygenTick += Server()->TickSpeed();
				TakeDamage(vec2(0.f, 0.f), m_Pos, 1, m_pPlayer->GetCID(), WEAPON_WORLD);
			}
		}
	}
	else
	{
		// fresh air refills the lungs: the next dive starts with a full tank
		m_Oxygen = 10;
		m_OxygenTick = 0;
	}

	// handle Weapons
	if(!m_DrillControl)
		HandleWeapons();
}

bool CCharacter::InWater()
{
	// same notion as the physics core: the character is "in water" when its
	// upper half touches a water tile (wading with wet feet does not count)
	return GameWorld()->Collision()->TestBox(m_Pos - vec2(0.0f, ms_PhysSize / 2.0f), vec2(ms_PhysSize, ms_PhysSize), CCollision::COLFLAG_WATER);
}

void CCharacter::TickDefered()
{
	static const vec2 ColBox(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE);
	// advance the dummy used for 0.8 clients (full per-Tee tuning)
	{
		CWorldCore TempWorld;
		TempWorld.m_Tuning = m_Tuning;
		m_ReckoningCore.Init(&TempWorld, GameWorld()->Collision());
		m_ReckoningCore.Tick(false, false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}
	// advance the legacy dummy used by the 0.7 translation layer (old gamecore
	// calculation: speed bonus only, no water/silent per-Tee values)
	{
		CWorldCore TempWorld;
		TempWorld.m_Tuning = LegacyTuning(MIN_CORRECTTUNING_CLIENTVERSION);
		m_LegacyReckoningCore.Init(&TempWorld, GameWorld()->Collision());
		m_LegacyReckoningCore.Tick(false, false);
		m_LegacyReckoningCore.Move();
		m_LegacyReckoningCore.Quantize();
	}

	// apply drag velocity when the player is not firing ninja
	// and set it back to 0 for the next tick
	if(GetUsableWeapon() != WeaponID("ninja") || !m_Ninja.m_Active)
		m_Core.AddDragVelocity();
	m_Core.ResetDragVelocity();

	// lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);

	m_Core.Move();

	bool StuckAfterMove = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);
	m_Core.Quantize();
	bool StuckAfterQuant = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		} StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	m_TriggeredEvents |= m_Core.m_TriggeredEvents;

	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}
	else if(m_Core.m_Death)
	{
		// handle death-tiles and ensure no core dumped issue.
		// also, since dead characters are removed from the game world and won't be snapped,
		// so we can skip the 'update the m_SendCore' process.
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
		return;
	}

	// manage emote here instead of in the snap function
	if(m_EmoteStop < Server()->Tick())
	{
		SetEmote(EMOTE_NORMAL, -1);
	}

	if(!m_pPlayer->m_MapLoading)
		GameServer()->m_pController->HandleCharacterTiles(this, StartPos, m_Pos);

	// update the send cores if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reckoning for a top of 3 seconds
		if(m_ReckoningTick + Server()->TickSpeed() * 3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}

		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_LegacyReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);
		if(m_LegacyReckoningTick + Server()->TickSpeed() * 3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_LegacyReckoningTick = Server()->Tick();
			m_LegacySendCore = m_Core;
			m_LegacyReckoningCore = m_Core;
		}
	}
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	++m_ReckoningTick;
	++m_LegacyReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= m_MaxHealth)
		return false;
	m_Health = clamp(m_Health + Amount, 0, m_MaxHealth);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor + Amount, 0, 10);
	return true;
}

void CCharacter::Die(int Killer, int Weapon)
{
	// we got to wait 0.5 secs before respawning
	m_Alive = false;
	m_pPlayer->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() / 2;
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, (Killer < 0) ? 0 : GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	if(Killer < 0)
	{
		str_format(aBuf, sizeof(aBuf), "kill killer='%d:%d:' victim='%d:%d:%s' weapon=%d special=%d",
			Killer, -1 - Killer,
			m_pPlayer->GetCID(), m_pPlayer->GetTeam(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "kill killer='%d:%d:%s' victim='%d:%d:%s' weapon=%d special=%d",
			Killer, GameServer()->m_apPlayers[Killer]->GetTeam(), Server()->ClientName(Killer),
			m_pPlayer->GetCID(), m_pPlayer->GetTeam(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	}
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_ModeSpecial = ModeSpecial;
	Msg.m_Assist = -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Server()->ClientIngame(i))
			continue;

		if(Killer < 0 && Server()->GetClientVersion(i) < MIN_KILLMESSAGE_CLIENTVERSION)
		{
			Msg.m_Killer = 0;
			Msg.m_Weapon = WEAPON_WORLD;
		}
		else
		{
			Msg.m_Killer = Killer;
			Msg.m_Weapon = Weapon;
		}
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}

	// a nice sound
	GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();

	Remove();
	GameWorld()->CreateDeath(m_Pos, m_pPlayer->GetCID());
	if(Weapon >= WEAPON_SELF)
	{
		m_Health = 1;
		m_Alive = true;
		m_Pos = vec2(0, 0);
		GameServer()->SwitchPlayerWorld(m_pPlayer, GameServer()->m_MainWorldID);
	}
}

void CCharacter::Remove()
{
	GameWorld()->RemoveEntity(this);
	GameWorld()->m_Core.SetCharacter(m_pPlayer->GetCID(), 0);
}

void CCharacter::RebindWorld(CGameWorld *pWorld)
{
	// Repoint a detached character at the world it is about to enter. It is not
	// inserted here (MoveTo does that once the client finished loading), but it
	// must not keep a pointer to the old world: that world can be unloaded as
	// soon as this player no longer references it, and destroying the character
	// in the meantime would then remove it from freed memory.
	SetGameWorld(pWorld);
	m_Core.Init(&GameWorld()->m_Core, GameWorld()->Collision());
}

void CCharacter::MoveTo(CGameWorld *pWorld, vec2 Pos, bool KeepMomentum)
{
	// MoveTo is reached both with a detached character (SwitchWorld removed it and
	// RebindWorld only repointed it) and with one that is still inserted. The
	// second case is real: a silent map switch already moves the character here,
	// and when the client answers NEED_DOWNLOAD its READY fallback runs
	// OnClientConnected, which moves it again. A same-world respawn
	// (SwitchPlayerWorld) does the same. Inserting twice leaves two entries for
	// one object, so the character is ticked twice per frame and the snapshot
	// carries two items with the same type/id key; the client's delta decoder
	// keeps only one of them, which makes every snapshot fail its crc check
	// ("snapshot crc error"). Detach unconditionally first: remove_fast tolerates
	// an entity that is not in the list at all, so this is safe when SwitchWorld
	// already detached it.
	Remove();

	SetGameWorld(pWorld);
	m_Core.Init(&GameWorld()->m_Core, GameWorld()->Collision());
	m_Core.m_Pos = Pos;
	m_Core.m_HookState = HOOK_IDLE;
	if(!KeepMomentum)
	{
		// A spawn arrival is not a continuation of the old world. Without this
		// the tee keeps the velocity it had when it left (running, falling or
		// being dragged), drives straight into the destination terrain and can
		// end up stuck in it. A ring seam crossing passes KeepMomentum and is
		// the one switch where the motion has to carry over.
		m_Core.m_Vel = vec2(0.0f, 0.0f);
		m_Core.m_HookDragVel = vec2(0.0f, 0.0f);
		m_Core.m_HookPos = Pos;
		m_Core.m_HookedPlayer = -1;
		m_Core.m_Jumped = 0;
	}
	// Keep the entity position in step with the core: GetPos, the view position
	// and NetworkClipped read m_Pos, and a stale one points into the old world
	// until the next tick.
	m_Pos = Pos;
	GameWorld()->m_Core.SetCharacter(m_pPlayer->GetCID(), &m_Core);
	GameWorld()->InsertEntity(this);
	UpdateTuning();
}

bool CCharacter::TakeDamage(vec2 Force, vec2 Source, int Dmg, int From, int Weapon)
{
	// a player that just arrived by train is invincible for three seconds
	// after entering the destination world
	if(m_pPlayer && m_pPlayer->IsTrainProtected())
		return false;

	// an allied NPC's shot can pass through a player without hurting them
	if(From == PLAYER_TEAM_FRIENDLY)
		return false;

	m_Core.m_Vel += Force;

	if(From >= 0)
	{
		if(GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From, Dmg))
			return false;
	}
	else if(From != PLAYER_TEAM_HOSTILE)
	{
		int Team = TEAM_RED;
		if(From == PLAYER_TEAM_BLUE)
			Team = TEAM_BLUE;
		if(GameServer()->m_pController->IsFriendlyTeamFire(m_pPlayer->GetTeam(), Team, Dmg))
			return false;
	}

	// m_pPlayer only inflicts half damage on self
	if(From == m_pPlayer->GetCID())
		Dmg = maximum(1, Dmg / 2);

	int OldHealth = m_Health, OldArmor = m_Armor;
	if(Dmg)
	{
		if(m_Armor)
		{
			if(Dmg > 1)
			{
				m_Health--;
				Dmg--;
			}

			if(Dmg > m_Armor)
			{
				Dmg -= m_Armor;
				m_Armor = 0;
			}
			else
			{
				m_Armor -= Dmg;
				Dmg = 0;
			}
		}

		m_Health -= Dmg;
	}

	// create healthmod indicator
	GameWorld()->CreateDamage(m_Pos, m_pPlayer->GetCID(), Source, OldHealth - m_Health, OldArmor - m_Armor, From == m_pPlayer->GetCID());

	// do damage Hit sound
	if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
	{
		int64 Mask = CmaskOne(From);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && (GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || GameServer()->m_apPlayers[i]->m_DeadSpecMode) &&
				GameServer()->m_apPlayers[i]->GetSpectatorID() == From)
				Mask |= CmaskOne(i);
		}
		GameWorld()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, Mask);
	}

	// check for death
	if(m_Health <= 0)
	{
		// set attacker's face to happy (taunt!)
		if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
		{
			CCharacter *pChr = GameServer()->m_apPlayers[From]->GetCharacter();
			if(pChr)
			{
				pChr->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
			}
		}
		// move the die function below so that we can ensure no core dumped issue here
		Die(From, Weapon);
		return false;
	}

	if(Dmg > 2)
		GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
	else
		GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

	SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);

	return true;
}

int CCharacter::GetHitAttacker()
{
	return GetCID();
}

bool CCharacter::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	int From = -1;
	if(pFrom)
	{
		// a carrier (projectile, explosion) reports the client that fired it;
		// every other hittable reports itself, which is a client id for a real
		// character and a PLAYER_TEAM_* sentinel for a bot
		if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
			From = static_cast<CChildEntity *>(pFrom)->GetOwner();
		else
			From = pFrom->GetHitAttacker();
	}
	return TakeDamage(Force, Source, Dmg, From, Weapon);
}

void CCharacter::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
	{
		if(m_Core.m_HookState == HOOK_IDLE)
			return;
		if(NetworkClippedLine(SnappingClient, m_Pos, m_Core.m_HookPos))
			return;
	}

	if(!GameServer()->m_pController->IsCharacterSnapable(SnappingClient, GetCID()))
		return;

	// A remote observer predicts this tee, so it needs this tee's own tuning.
	// The local client's tuning is emitted by CGameContext::OnSnap under the
	// same TeeInfoID; emitting it here as well would duplicate the item.
	const int TuningID = m_pPlayer->GetCID();
	if(SnappingClient != TuningID)
	{
		CNetObj_Tuning *pTuning = static_cast<CNetObj_Tuning *>(Server()->SnapNewItem(NETOBJTYPE_TUNING, TuningID, sizeof(CNetObj_Tuning)));
		if(pTuning)
			mem_copy(pTuning->m_aTuneParams, &m_Tuning, sizeof(pTuning->m_aTuneParams));
	}

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, TuningID, sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	// 0.8 clients get the full per-Tee dead reckoning; the 0.7 translation
	// layer gets the old gamecore calculation without water/silent tuning.
	const bool LegacyClient = SnappingClient >= 0 && Server()->GetClientVersion(SnappingClient) < MIN_PERTEE_TUNING_CLIENTVERSION;
	const int ReckoningTick = LegacyClient ? m_LegacyReckoningTick : m_ReckoningTick;
	const CCharacterCore &SendCore = LegacyClient ? m_LegacySendCore : m_SendCore;
	if(!ReckoningTick)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = ReckoningTick;
		SendCore.Write(pCharacter);
	}

	// The core holds the hook target as a server-internal TeeID, but the client
	// resolves m_HookedPlayer against the ids it was told. A bot that hooked
	// this player is shown under a per-client id, so translate it here too.
	if(GameWorld()->HasBotManager())
		pCharacter->m_HookedPlayer = GameWorld()->GetBotManager()->TranslateTeeIDForClient(SnappingClient, pCharacter->m_HookedPlayer);

	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;
	// 0.8: the maximum values are public and always sent, so that the client can
	// scale the health/armor bars without knowing the server's tuning. m_MaxHealth
	// includes the accessory health bonus, so a 0.8 HUD scales correctly. Legacy
	// 0.7 clients get their value normalised to 10 by the snapshot translator.
	pCharacter->m_MaxHealth = m_MaxHealth;
	pCharacter->m_MaxArmor = MAX_ARMOR;
	// a silent step leaves no trace: its jumps are not announced to anyone
	int TriggeredEvents = m_TriggeredEvents;
	if(m_Core.m_SilentStep)
		TriggeredEvents &= ~(COREEVENTFLAG_GROUND_JUMP | COREEVENTFLAG_AIR_JUMP);
	pCharacter->m_TriggeredEvents = TriggeredEvents;

	// snap the weapon the player may actually use: an empty slot or an unowned
	// weapon snaps as the hand (-1, no weapon shown)
	const unsigned ActiveSnapWeaponID = GetUsableWeapon();
	pCharacter->m_Weapon = WeaponManager()->SnapStyle(ActiveSnapWeaponID);
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == -1 ||
		(!Config()->m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->GetSpectatorID()))
	{
		pCharacter->m_Health = m_Health;
		// underwater the armor bar shows the remaining oxygen (the real
		// armor still absorbs damage but is hidden while diving)
		pCharacter->m_Armor = InWater() ? m_Oxygen : m_Armor;
		if(ActiveSnapWeaponID != 0)
		{
			IWeaponInterface *pWeapon = WeaponManager()->GetWeapon(ActiveSnapWeaponID);
			if(pWeapon)
			{
				// ammo comes from the player's inventory; weapons without an
				// ammo item (e.g. hammer, ninja, hand) show a full bar instead
				if(GameServer()->Item()->WeaponNeedsAmmo(pWeapon->Name()))
					pCharacter->m_AmmoCount = GameServer()->Item()->GetAmmoCountForWeapon(GetCID(), pWeapon->Name());
				else
					pCharacter->m_AmmoCount = pWeapon->MaxAmmo();
			}
		}
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL)
	{
		if(5 * Server()->TickSpeed() - ((Server()->Tick() - m_LastAction) % (5 * Server()->TickSpeed())) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
	}
}

void CCharacter::ShiftForSnap(vec2 Delta)
{
	m_Pos += Delta;
	m_Core.m_Pos += Delta;
	m_Core.m_HookPos += Delta;
	m_SendCore.m_Pos += Delta;
	m_SendCore.m_HookPos += Delta;
	m_LegacySendCore.m_Pos += Delta;
	m_LegacySendCore.m_HookPos += Delta;
}

void CCharacter::PostSnap()
{
	m_TriggeredEvents = 0;
}
