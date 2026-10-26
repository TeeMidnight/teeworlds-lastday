/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#ifndef GAME_SERVER_ENTITIES_BOTENTITY_H
#define GAME_SERVER_ENTITIES_BOTENTITY_H

#include <base/uuid.h>

#include <generated/protocol.h>

#include <game/gamecore.h>
#include <game/server/bot.h>
#include <game/server/entity.h>

class CBotManager;
class IBotAI;
struct SMobDef;

/*
	Class: IBotEntity
		The bot contract: an autonomous tee that lives in the world like any
		other hittable entity, but has no CPlayer and no client slot.

		It exists so variants (dummy, monster, scripted NPC, ...) can be added
		without touching CBotManager: derive, implement the pure virtuals and
		register the class with CBotManager::RegisterBotType().
*/
class IBotEntity : public CHitableEntity
{
public:
	IBotEntity(CGameWorld *pGameWorld, int ObjType, vec2 Pos, int ProximityRadius = 0) :
		CHitableEntity(pGameWorld, ObjType, 0, Pos, ProximityRadius)
	{
	}

	// --- identity ---
	virtual Uuid GetBotID() const = 0;
	// Type name the manager uses to pick a factory when spawning ("default").
	virtual const char *BotTypeName() const = 0;

	// --- identity presented to clients ---
	virtual const CBotTeeInfo *GetTeeInfo() const = 0;
	// Team the bot fights for; used for friendly fire and snap colouring.
	virtual int GetBotTeam() const = 0;
	// Social stance (hostile / neutral / friendly); target selection uses it.
	virtual int GetBotFaction() const = 0;
	// The content definition behind a monster, or null for a generic bot.
	virtual const SMobDef *MobDef() const { return nullptr; }

	// The key CWorldCore registers this bot's core under, so it must be unique
	// among live entities and stay inside [0, MAX_TEES) for the bot's whole
	// life. This is NOT the id the bot snaps as: that one is per receiving
	// client (BotTeeIdForVersion, resolved by FindClientID).
	virtual int TeeID() const = 0;

	// --- physics ---
	// The core this bot registered in CWorldCore. A ring proxy uses it to pass
	// a hook drag on to the real bot.
	virtual CCharacterCore *Core() = 0;

	// --- state ---
	virtual bool IsAlive() const = 0;
	virtual int GetHealth() const = 0;
	virtual int GetArmor() const = 0;

	// --- lifecycle ---
	// Kill the bot; the manager is told so it can retire the BotID.
	virtual void Die(CEntity *pKiller, int Weapon) = 0;

	// Hand the bot over to another world's manager, re-binding its core. Its
	// private tee id is global, so it is kept. Used by the NPC respawn teleport
	// and by ring crossings.
	virtual void MoveToWorld(CGameWorld *pNewWorld, vec2 Pos) = 0;

	// --- behaviour hooks ---
	// Called once per game tick before the physics core advances: this is
	// where a variant decides its input.
	virtual void OnBotTick() = 0;

	// Called after damage was applied and the health indicator was created.
	// Returning false makes TakeDamage report the hit as absorbed, matching
	// CCharacter's contract.
	virtual bool OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) = 0;
};

/*
	Class: CBotEntity
		The default bot: a hammer-wielding tee that wanders around, chases the
		closest visible hittable entity and swings at it.

		It owns a CCharacterCore, registered in CWorldCore under TeeID(), so it
		moves and collides like a player character and can be hooked by one.
*/
class CBotEntity : public IBotEntity
{
public:
	// same as character's size
	static const int ms_PhysSize = 28;

	// lastday caps health and armor at these values (see CCharacter)
	static const int MAX_HEALTH = 10;
	static const int MAX_ARMOR = 10;

	CBotEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team, int Faction = BOTFACTION_HOSTILE);
	~CBotEntity() override;

	// --- CEntity ---
	void Reset() override;
	void Tick() override;
	void TickDefered() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;
	void ShiftForSnap(vec2 Delta) override;
	void PostSnap() override;

	// --- CHitableEntity ---
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	int GetHitAttacker() override;

	// --- IBotEntity ---
	Uuid GetBotID() const override { return m_BotID; }
	const char *BotTypeName() const override { return "default"; }
	const CBotTeeInfo *GetTeeInfo() const override { return &m_TeeInfo; }
	int GetBotTeam() const override { return m_Team; }
	int GetBotFaction() const override { return m_Faction; }
	int TeeID() const override { return m_TeeID; }
	bool IsAlive() const override { return m_Alive; }
	int GetHealth() const override { return m_Health; }
	int GetArmor() const override { return m_Armor; }
	void Die(CEntity *pKiller, int Weapon) override;
	void MoveToWorld(CGameWorld *pNewWorld, vec2 Pos) override;
	bool RingCrosses() const override { return true; }
	void OnBotTick() override;
	bool OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;

	// --- damage helpers ---
	bool IncreaseHealth(int Amount);
	bool IncreaseArmor(int Amount);
	// raw damage entry point (no friendly-fire filtering); TakeHit wraps it.
	// Virtual so a variant (the NPC) can drop the knockback impulse.
	virtual bool TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon);

	// the physics core, exposed for variants that want to drive it directly
	CCharacterCore *Core() override { return &m_Core; }

	// --- AI strategy ------------------------------------------------------
	// Takes ownership of pAI and deletes the previous strategy. Never call this
	// from inside a strategy hook: use RequestAI(), which is applied after the
	// tick's hook has returned.
	void SetAI(IBotAI *pAI);
	// Resolves a name through the shared factory ("chase", "wander", "stand",
	// "script:<path>", "<name>.py" or a bare name under scripts/bot_ai/).
	// Keeps the current strategy when the name does not resolve.
	bool SetAIByName(const char *pName);
	// Queues a strategy change for the end of this tick. Safe from a hook.
	bool RequestAI(const char *pName);
	IBotAI *AI() const { return m_pAI; }
	// Name of the running strategy, or "none".
	const char *AIName() const;

	// --- input surface used by IBotAI -------------------------------------
	// A strategy only decides input; the body, physics and snapping stay here.
	CNetObj_PlayerInput &Input() { return m_Input; }
	const CNetObj_PlayerInput &Input() const { return m_Input; }
	void SetDirection(int Direction) { m_Input.m_Direction = Direction < 0 ? -1 : (Direction > 0 ? 1 : 0); }
	void SetJump(bool Jump) { m_Input.m_Jump = Jump ? 1 : 0; }
	void SetFire(bool Fire) { m_Input.m_Fire = Fire ? 1 : 0; }
	void SetAimTarget(vec2 Target) { m_CursorTarget = Target; }
	vec2 CursorTarget() const { return m_CursorTarget; }
	int ReloadTimer() const { return m_ReloadTimer; }

	/*
		Function: PickTeeID
			Chooses the private TeeID for a bot. The global CBotManager derives
			it from the BotID (so it is stable) and keeps it unique among the
			live bots of every world.
		Returns:
			A free id in [BOT_TEEID_07_MIN, BOT_TEEID_08_MAX], or -1 when the
			whole range is taken.
	*/
	static int PickTeeID(CGameWorld *pGameWorld, Uuid BotID);

	// Picks the entity the default action should chase, or null for "wander".
	// Public because a strategy decides what to chase.
	virtual CEntity *FindTarget();
	// Smoothly moves the aim cursor towards m_CursorTarget; shared by the
	// built-in strategies and by variants such as the NPC.
	void AimCursor();

protected:
	// Decides this tick's input. The default implementation delegates to m_pAI;
	// a variant (the NPC) overrides it and ignores the strategy layer.
	virtual void OnAction();
	// Swings the held weapon; called after the input was decided
	virtual void DoWeapon();
	// Called for every hittable a melee swing landed on; Killed is true when the
	// swing was the one that brought a character down. A variant can react.
	virtual void OnBotHit(CEntity *pTarget, bool Killed);
	// Applies a strategy change queued by RequestAI(); safe point only.
	void ApplyPendingAI();

	CBotTeeInfo m_TeeInfo;
	Uuid m_BotID;
	// Fixed for the bot's lifetime: it is the key CWorldCore holds the core
	// under. Not the id the bot snaps as; see IBotEntity::TeeID.
	int m_TeeID;
	int m_Team;
	int m_Faction;
	bool m_Alive;

	int m_Health;
	int m_Armor;
	int m_MaxHealth;
	int m_MaxArmor;

	int m_Emote;
	int m_AttackTick;
	int m_ReloadTimer;

	int m_TriggeredEvents;

	// the physics core, same shape as CCharacter's
	CCharacterCore m_Core;

	// info for dead reckoning
	int m_ReckoningTick;
	CCharacterCore m_SendCore;
	CCharacterCore m_ReckoningCore;

	CNetObj_PlayerInput m_Input;
	CNetObj_PlayerInput m_PrevInput;
	vec2 m_CursorTarget;

	// the strategy layer; owned, replaced through SetAI()/RequestAI()
	IBotAI *m_pAI;
	// a strategy change requested from inside a hook, applied after OnAction()
	char m_aPendingAI[192];
	bool m_bPendingAI;

	// set when the bot has no weapon item system backing it: it swings a
	// hammer, the one weapon that needs neither an item nor ammo
	int m_WeaponSlot;
	// melee tuning; content variants (a monster) override the hammer defaults
	int m_MeleeDamage;
	int m_MeleeCooldown;
	float m_MeleeRange;

public:
	void SetMeleeTuning(int Damage, int Cooldown, float Range)
	{
		m_MeleeDamage = maximum(1, Damage);
		m_MeleeCooldown = maximum(1, Cooldown);
		m_MeleeRange = maximum(8.0f, Range);
	}
};

#endif // GAME_SERVER_ENTITIES_BOTENTITY_H
