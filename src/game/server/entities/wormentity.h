/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_WORMENTITY_H
#define GAME_SERVER_ENTITIES_WORMENTITY_H

#include <base/tl/array.h>
#include <base/uuid.h>
#include <base/vmath.h>

#include "botentity.h"

#include <game/server/mobdata.h>

class CWormEntity;

/*
	Class: CWormSegmentEntity
		One tee-shaped body link of a burrower. It has a bot identity and a
		CCharacterCore, so it snaps and collides exactly like any other tee, but
		the worm drives its position every tick. All damage it takes is routed to
		the worm, so hitting any link hurts the whole creature.
*/
class CWormSegmentEntity : public CBotEntity
{
public:
	CWormSegmentEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo,
		int Team, int Faction, CWormEntity *pWorm);
	~CWormSegmentEntity() override;

	const char *BotTypeName() const override { return "worm_segment"; }

	void Reset() override;
	void Tick() override {}
	void TickDefered() override {}
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	int GetHitAttacker() override;
	void Die(CEntity *pKiller, int Weapon) override;

	// moves the link directly; the worm has already computed the trail
	void SetWormPos(vec2 Pos, vec2 Dir);
	// hidden links are underground: they do not collide, hook or snap
	void SetWormVisible(bool Visible);
	void SetWorm(CWormEntity *pWorm) { m_pWorm = pWorm; }
	// removes the link from the world and the bot manager
	void Despawn();

private:
	CWormEntity *m_pWorm;
	bool m_Despawning;
};

/*
	Class: CWormEntity
		A segmented burrower. The head and body are bots (so they look like a
		chain of tees), while this entity owns the shared health, the burrow
		cycle, contact damage and loot. It moves through terrain instead of
		carving it, so the map stays immutable.
*/
class CWormEntity : public CHitableEntity
{
public:
	CWormEntity(CGameWorld *pWorld, vec2 Pos, const SMobDef *pDef, Uuid HeadID, const char *pName);
	~CWormEntity() override;

	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	int GetHitAttacker() override;

	bool Alive() const { return m_Alive; }
	bool Burrowed() const { return m_Burrowed; }
	const SMobDef *Def() const { return m_pDef; }
	IBotEntity *HeadSegment();

private:
	void UpdateSegments();
	void SetVisible(bool Visible);
	void Bite(CCharacter *pChr);
	void DropLoot();
	void WormDie();
	void Speak(int Event);

	const SMobDef *m_pDef;
	char m_aName[64];
	int m_Health;
	int m_MaxHealth;
	int m_Armor;
	bool m_Alive;
	bool m_Burrowed;
	int m_StateTimer;
	int m_BiteTimer;
	int m_LastHitTick;
	int m_WanderTimer;
	vec2 m_WanderDir;
	Uuid m_HeadID;
	array<Uuid> m_lSegments;
	array<vec2> m_lPath;
};

#endif // GAME_SERVER_ENTITIES_WORMENTITY_H
