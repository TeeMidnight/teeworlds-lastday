/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_CHARACTER_H
#define GAME_SERVER_ENTITIES_CHARACTER_H

#include <generated/protocol.h>

#include <base/tl/array.h>

#include <game/gamecore.h>
#include <game/server/entity.h>
#include <game/server/player.h>

class CWeaponNinja;

class CCharacter : public CHitableEntity
{
	MACRO_ALLOC_POOL_ID()

public:
	// character's size
	static const int ms_PhysSize = 28;

	enum
	{
		MIN_KILLMESSAGE_CLIENTVERSION = 0x0704, // todo 0.8: remove me
		MIN_CORRECTTUNING_CLIENTVERSION = 0x0706, // todo 0.8: remove me
	};

	CCharacter(CGameWorld *pWorld);

	virtual void Reset();
	virtual void Destroy();
	virtual void Tick();
	virtual void TickDefered();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);
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
	void MoveTo(CGameWorld *pWorld, vec2 Pos);
	bool TakeDamage(vec2 Force, vec2 Source, int Dmg, int From, int Weapon);
	virtual bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon);

	bool Spawn(class CPlayer *pPlayer, vec2 Pos);

	bool IncreaseHealth(int Amount);
	bool IncreaseArmor(int Amount);

	void SetEmote(int Emote, int Tick);

	bool IsAlive() const { return m_Alive; }
	class CPlayer *GetPlayer() { return m_pPlayer; }
	int GetCID();

	// the item hash (str_quickhash of the item res_id) in the active loadout
	// slot, or 0 when the slot is empty. for weapon items this equals the
	// weapon id; for non-weapon items it matches no registered weapon.
	unsigned GetActiveWeapon() const { return WeaponAtSlot(m_ActiveWeapon); }

	// loadout (equipment menu) helpers; Slot is 0-based (0..NUM_WEAPONS-1),
	// matching the client weapon key minus one. the slot holds an item: any
	// owned item can be placed, only weapon items actually fire.
	bool EquipWeaponSlot(int Slot, unsigned ItemHash);
	unsigned WeaponAtSlot(int Slot) const; // item hash, 0 = empty
	int NumWeaponsHeld() const;

	// the weapon the player may actually fire in the active slot: the slot's
	// weapon item when it exists, otherwise the fallback "hand" (needs no
	// item). items without a weapon and missing items also resolve to hand.
	unsigned GetUsableWeapon();
	// clear every slot whose item no longer exists; returns whether anything
	// was unequipped
	bool UnequipMissingWeapons();

	// need this hook for gamecontroller
	void TeleTo(vec2 Pos, bool KeepSpeed);

private:
	// the ninja weapon drives its own state machine through OnTick and needs
	// direct access to the per-character ninja state
	friend class CWeaponNinja;

	// player controlling this character
	class CPlayer *m_pPlayer;

	bool m_Alive;

	// weapon info
	array<CEntity *> m_lpHitObjects;

	// loadout: NUM_WEAPONS fixed slots, each holds the hash of the item placed
	// on it (weapon items: hash == weapon id). slot order matches the client's
	// weapon selection keys (1..NUM_WEAPONS).
	unsigned m_aWeapons[NUM_WEAPONS]; // str_quickhash(item res_id), 0 = empty

	int m_ActiveWeapon; // slot index of the currently held weapon (0..NUM_WEAPONS-1)
	int m_QueuedWeapon; // slot index to switch to, or -1

	int m_ReloadTimer;
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

	int m_TriggeredEvents;

	// ninja dash/swing state, driven by CWeaponNinja (permanent weapon: no
	// activation timer, the swing only runs for a short window after firing)
	struct
	{
		vec2 m_ActivationDir;
		int m_CurrentMoveTime;
		int m_OldVelAmount;
	} m_Ninja;

	// the player core for the physics
	CCharacterCore m_Core;

	// info for dead reckoning
	int m_ReckoningTick; // tick that we are performing dead reckoning From
	CCharacterCore m_SendCore; // core that we should send
	CCharacterCore m_ReckoningCore; // the dead reckoning core

	friend void CPlayer::SaveStatus(class CPlayerDB *pDB);
	friend void CPlayer::LoadStatus(class CPlayerDB *pDB);
};

#endif
