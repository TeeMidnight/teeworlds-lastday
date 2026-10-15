/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#ifndef GAME_SERVER_ENTITIES_NPCENTITY_H
#define GAME_SERVER_ENTITIES_NPCENTITY_H

#include "botentity.h"

struct SNpcDef;

/*
	Class: CNpcEntity
		A peaceful town NPC, a bot variant with its own AI. It never looks for
		a target and never fights: it wanders randomly around the point where
		it spawned and is steered back whenever it drifts outside a small
		radius of it. A player coming close makes it stand still and turn to
		look at them. Hit knockback is discarded, so players cannot push it
		around, but it still takes damage.
*/
class CNpcEntity : public CBotEntity
{
public:
	CNpcEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team);
	// builds a NPC from its configuration, so name, skin, health and regen come
	// from the npc json rather than from a random roll
	CNpcEntity(CGameWorld *pWorld, const SNpcDef *pDef, int Team);

	// --- IBotEntity ---
	const char *BotTypeName() const override { return "npc"; }

	// --- CBotEntity ---
	void Tick() override;

	// --- CHitableEntity / CBotEntity ---
	// an NPC is immovable: the impulse of a hit is dropped, the damage is not
	bool TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;

	const SNpcDef *NpcDef() const { return m_pDef; }

	// --- aggro events ---
	void Die(CEntity *pKiller, int Weapon) override;
	bool OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	void OnBotHit(CEntity *pTarget, bool Killed) override;

protected:
	void OnAction() override;
	// a peaceful NPC never acquires a target
	CEntity *FindTarget() override { return nullptr; }

private:
	// the client behind a hit source, or -1 (projectiles report their owner)
	static int HitClientID(CEntity *pFrom);
	// add a signed delta to this NPC's aggro ledger for one client
	void AddAggro(int ClientID, int Delta);
	// show the NPC's remaining health to one client
	void ShowHealth(int ClientID);

	// the closest player whose ledger aggro is over the threshold, or -1
	int FindHatedPlayer();
	// move towards a hated player, but never past the chase radius
	void Chase(int ClientID);
	// the aggro of one client, cached from the ledger for a short while
	int CachedAggro(int ClientID);
	// keep the cache in step with a delta that was just written
	void UpdateAggroCache(const Uuid &Account, int Delta);

	// begin the scripted respawn: teleport to the main world, emote, come back
	void StartRespawn();
	// advance the scripted respawn once it has started
	void TickRespawn();
	// hand the bot over to another world's bot manager and re-bind its core
	void MoveToWorld(CGameWorld *pNewWorld, vec2 Pos);

	// announce the shop to players that enter the trading range
	void TickInteract();

	// the closest live player within the roam radius, or null
	CEntity *ClosestPlayer();
	// random walk, kept inside the spawn radius
	void Wander();

	// one cached aggro level per account seen recently, refreshed from the
	// ledger at most once a second so reading it stays off the hot path
	struct SNpcAggroEntry
	{
		Uuid m_Account;
		int m_Aggro;
		int m_RefreshTick;
	};
	array<SNpcAggroEntry> m_lAggro;

	// who was inside the trading range last tick, so the unlock notice fires
	// once per entry instead of every frame
	bool m_aClientInRange[MAX_CLIENTS] = {};

	// the scripted respawn. While teleporting the NPC is invincible and its AI
	// is suspended; it is moved to the main world, emotes, then comes home.
	enum ENpcState
	{
		NPCSTATE_ALIVE = 0,
		NPCSTATE_TELEPORTING,
	};
	int m_State = NPCSTATE_ALIVE;
	int m_StateTick = 0;
	bool m_Emoted = false;
	bool m_Invincible = false;
	unsigned m_HomeWorldID = 0;
	vec2 m_HomePos = vec2(0.0f, 0.0f);

	// the configuration this NPC was built from (null for a random one)
	const SNpcDef *m_pDef;
	// health regenerated per second, accumulated so the rate can stay integral
	int m_HealthRegen;
	int m_RegenCounter;

	// where the NPC was spawned; it may not wander far from it
	vec2 m_SpawnPos;
	// the direction it is currently wandering in (-1, 0 or 1)
	int m_WanderDir;
};

#endif // GAME_SERVER_ENTITIES_NPCENTITY_H
