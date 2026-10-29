/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_CHARACTER_H
#define GAME_SERVER_ENTITIES_CHARACTER_H

#include <base/tl/array.h>
#include <base/tl/timer.h>
#include <game/gamecore.h>
#include <game/server/entity.h>
#include <game/server/player.h>
#include <generated/protocol.h>

class CWeaponNinja;

class CCharacter : public CHitableEntity
{
	MACRO_ALLOC_POOL_ID()

public:
	// character's size
	static const int ms_PhysSize = 28;

	// lastday caps health and armor at these values; they are also what the 0.8
	// CNetObj_Character::m_MaxHealth/m_MaxArmor fields report, so the client can
	// scale its health and armor bars correctly.
	static const int MAX_HEALTH = 10;
	static const int MAX_ARMOR = 10;

	enum
	{
		MIN_KILLMESSAGE_CLIENTVERSION = 0x0704, // todo 0.8: remove me
		MIN_CORRECTTUNING_CLIENTVERSION = 0x0706, // todo 0.8: remove me
		// 0.8 clients understand one Tuning object per Tee. Older clients get
		// the single legacy tuning and its vanilla physics below.
		MIN_PERTEE_TUNING_CLIENTVERSION = 0x0800,
	};

	CCharacter(CGameWorld *pWorld);

	virtual void Reset();
	virtual void Destroy();
	virtual void Tick();
	virtual void TickDefered();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);
	virtual void ShiftForSnap(vec2 Delta) override;
	virtual void PostSnap();

	bool IsGrounded();

	void SetWeapon(int W);
	void HandleWeaponSwitch();
	void DoWeaponSwitch();

	void HandleWeapons();

	void OnPredictedInput(CNetObj_PlayerInput *pNewInput);
	void OnDirectInput(CNetObj_PlayerInput *pNewInput);
	void ResetInput();
	void FireWeapon();

	void Die(int Killer, int Weapon);
	void Remove();
	void RebindWorld(CGameWorld *pWorld);
	// Put the character into pWorld at Pos. KeepMomentum is true for a ring
	// seam crossing, which continues the same motion into the neighbouring
	// instance; a spawn arrival (train, entrance, respawn) passes false so the
	// tee does not carry the old world's velocity into the new terrain.
	void MoveTo(CGameWorld *pWorld, vec2 Pos, bool KeepMomentum = true);
	bool TakeDamage(vec2 Force, vec2 Source, int Dmg, int From, int Weapon);
	virtual bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon);
	int GetHitAttacker() override;

	bool Spawn(class CPlayer *pPlayer, vec2 Pos);

	bool IncreaseHealth(int Amount);
	bool IncreaseArmor(int Amount);

	void SetEmote(int Emote, int Tick);

	bool IsAlive() const { return m_Alive; }
	class CPlayer *GetPlayer() { return m_pPlayer; }
	int GetCID();

	// current physics velocity (used by scripted callbacks)
	vec2 Velocity() const { return m_Core.m_Vel; }

	// ring proxies: the core position a neighbouring world mirrors, and the hook
	// drag a proxy received that belongs to this character
	vec2 CorePos() const { return m_Core.m_Pos; }
	void AddHookDragVel(vec2 Vel) { m_Core.m_HookDragVel += Vel; }

	// the item id in the active loadout slot (a zeroed id when the slot is
	// empty). Use GetUsableWeapon() to resolve the weapon it equips as.
	CItemID GetActiveWeapon() const { return WeaponAtSlot(m_ActiveWeapon); }

	// loadout (equipment menu) helpers; Slot is 0-based (0..NUM_WEAPONS-1),
	// matching the client weapon key minus one. the slot holds an item: any
	// owned item can be placed, only weapon items actually fire.
	bool EquipWeaponSlot(int Slot, const CItemID &Id);
	CItemID WeaponAtSlot(int Slot) const; // zeroed id = empty
	int NumWeaponsHeld() const;

	// the weapon the player may actually fire in the active slot: the slot's
	// weapon item when it exists, otherwise the fallback "hand" (needs no
	// item). items without a weapon and missing items also resolve to hand.
	unsigned GetUsableWeapon();
	// clear every slot whose item no longer exists; returns whether anything
	// was unequipped
	bool UnequipMissingWeapons();

	// accessory loadout helpers; Slot is 0-based (0..NUM_ACCESSORY_SLOTS-1).
	// an accessory stays in the inventory and grants a passive effect while
	// worn. EquipAccessorySlot(Slot, CItemID()) clears the slot.
	bool EquipAccessorySlot(int Slot, const CItemID &Id);
	CItemID AccessoryAtSlot(int Slot) const; // zeroed id = empty
	void ClearAccessories();
	// replace the whole accessory layout at once (validated by the caller).
	// this refreshes the cached effects only after every slot is set, so the
	// intermediate maximum never clamps the player's current health.
	void SetAccessoryLayout(const CItemID *pIds);
	bool UnequipMissingAccessories();
	// recompute the cached health/range/speed bonuses from the worn items
	void RefreshAccessoryEffects();

	// derived stats granted by the worn accessories
	int MaxHealth() const { return m_MaxHealth; }
	int Health() const { return m_Health; }
	int Armor() const { return m_Armor; }
	float RangeScale() const { return m_RangeScale; }
	float SpeedScale() const { return m_SpeedScale; }
	// Effective per-tee tuning (speed bonus, silent step, water) that the
	// server simulates and snaps for this Tee's prediction.
	const CTuningParams &Tuning() const { return m_Tuning; }
	// The tuning a pre-0.8 client can actually predict: its single legacy
	// tuning keeps the speed bonus and drill lock, but none of the LastDay
	// physics (water, silent step) the old gamecore did not know about.
	CTuningParams LegacyTuning(int ClientVersion);

	// drill control: while set, the character is pinned in place, ignores its
	// movement input and stays silent; the player steers a nearby drill instead.
	void SetDrillControl(bool Control);
	bool IsDrillControl() const { return m_DrillControl; }
	// a jump press while steering asks the player loop to release the drill;
	// returns true once and clears the request
	bool ConsumeDrillExitRequest();
	// whether the fire button is currently held (read by the drill)
	bool IsFiring() const { return (m_LatestInput.m_Fire & 1) != 0; }

	// need this hook for gamecontroller
	void TeleTo(vec2 Pos, bool KeepSpeed);

	// the raw cursor vector relative to the character (client target input)
	vec2 AimVector() const;
	// the normalized direction the player is currently aiming at
	// (e.g. used to throw dropped items)
	vec2 AimDirection() const;

private:
	// the ninja weapon drives its own state machine through OnTick and needs
	// direct access to the per-character ninja state
	friend class CWeaponNinja;

	// player controlling this character
	class CPlayer *m_pPlayer;

	bool m_Alive;
	// pinned while the player steers a drill (see SetDrillControl)
	bool m_DrillControl;
	// a jump press while steering, consumed by the owning CPlayer
	bool m_DrillExitRequest;

	// weapon info: entities already hit during the current ninja swing, by
	// their unique server id rather than a raw pointer so a destroyed entity
	// cannot be resurrected by an address reuse
	array<int> m_lpHitObjects;

	// loadout: NUM_WEAPONS fixed slots, each holds the item placed on it.
	// slot order matches the client's weapon selection keys (1..NUM_WEAPONS).
	CItemID m_aWeapons[NUM_WEAPONS]; // zeroed id = empty

	// accessory loadout: passive items, independent of the weapon slots
	CItemID m_aAccessories[NUM_ACCESSORY_SLOTS];
	// cached accessory effects: base max health plus the health bonus, and the
	// range/speed multipliers (1.0 = no bonus)
	int m_MaxHealth;
	float m_RangeScale;
	float m_SpeedScale;
	// Rebuilt every Tick from the server tuning plus the per-player state.
	CTuningParams m_Tuning;
	void UpdateTuning();

	int m_ActiveWeapon; // slot index of the currently held weapon (0..NUM_WEAPONS-1)
	int m_QueuedWeapon; // slot index to switch to, or -1

	tick_timer m_ReloadTimer;
	int m_AttackTick;

	int m_EmoteType;
	int m_EmoteStop;

	// last tick that the player took any action ie some input
	int m_LastAction;
	int m_LastNoAmmoSound;

	// these are non-heldback inputs
	CNetObj_PlayerInput m_LatestPrevInput;
	CNetObj_PlayerInput m_LatestInput;

	// input
	CNetObj_PlayerInput m_Input;
	int m_NumInputs;
	int m_Jumped;

	int m_Health;
	int m_Armor;
	// oxygen while underwater (0..10): the armor bar shows it instead of the
	// real armor; empty oxygen makes the player drown
	int m_Oxygen;
	// next tick the oxygen is consumed (or drowning damage dealt); 0 = not
	// counting down (on dry land)
	int m_OxygenTick;

	int m_TriggeredEvents;

	// ninja dash/swing state, driven by CWeaponNinja (permanent weapon: no
	// activation timer, the swing only runs for a short window after firing)
	struct
	{
		vec2 m_ActivationDir;
		// deadline of the current swing; m_Active carries the latch so the
		// velocity is restored exactly on the tick it expires
		tick_timer m_MoveTimer;
		bool m_Active;
		int m_OldVelAmount;
	} m_Ninja;

	// the player core for the physics
	CCharacterCore m_Core;

	// info for dead reckoning. The full core carries this Tee's tuning and is
	// what a 0.8 client extrapolates; the legacy core is the old gamecore
	// calculation (no water/silent per-Tee values) for the 0.7 translation
	// layer.
	int m_ReckoningTick; // tick that we are performing dead reckoning From
	CCharacterCore m_SendCore; // core that we should send
	CCharacterCore m_ReckoningCore; // the full dead reckoning core
	int m_LegacyReckoningTick;
	CCharacterCore m_LegacySendCore;
	CCharacterCore m_LegacyReckoningCore;

	// whether the character's physics box currently touches a water tile
	// (drives the oxygen system)
	bool InWater();

	friend void CPlayer::SaveStatus(class CDatabase *pDB);
	friend void CPlayer::CaptureStatus(struct CDbJob &Job);
	friend void CPlayer::LoadStatus(class CDatabase *pDB, bool RefreshMenu);
};

#endif
