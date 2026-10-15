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

protected:
	void OnAction() override;
	// a peaceful NPC never acquires a target
	CEntity *FindTarget() override { return nullptr; }

private:
	// the closest live player within the roam radius, or null
	CEntity *ClosestPlayer();
	// random walk, kept inside the spawn radius
	void Wander();

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
