/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_PROJECTILE_H
#define GAME_SERVER_ENTITIES_PROJECTILE_H

#include <base/uuid.h>
#include <game/server/entity.h>

// PLAYER_TEAM_BLUE / PLAYER_TEAM_RED live in entity.h: an entity without a
// client slot (a projectile, a bot) reports its team through them.

class CProjectile : public CChildEntity
{
public:
	CProjectile(CGameWorld *pGameWorld, int Type, int Owner, vec2 Pos, vec2 Dir, int Span,
		int Damage, bool Explosive, float Force, int SoundImpact, int Weapon);

	vec2 GetPos(float Time);
	void FillInfo(CNetObj_Projectile *pProj);

	// A prop-owned projectile can name the entity it must never hit (the
	// campfire's own hitbox, for example). The id is resolved every tick so a
	// destroyed ignore target does not leave a dangling pointer.
	void SetIgnoreEntity(int EntityID) { m_IgnoreEntityID = EntityID; }

	// Bots have no client slot, so a bot-owned projectile carries its firing
	// bot separately. Victims use this for retaliation.
	void SetOwnerBot(const Uuid &BotID) { m_OwnerBotID = BotID; }
	const Uuid &OwnerBot() const { return m_OwnerBotID; }

	void LoseOwner();

	virtual void Reset();
	virtual void Tick();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);

	// a projectile's m_Pos is its launch point, so a crossing rebases the launch
	// rather than the current point (see CEntity::RingPos)
	virtual vec2 RingPos();
	virtual void OnRingTransfer(float ArrivalX);
	virtual bool RingCrosses() const override { return true; }

private:
	vec2 m_Direction;
	int m_LifeSpan;
	int m_OwnerTeam;
	int m_Type;
	int m_Damage;
	int m_SoundImpact;
	int m_Weapon;
	float m_Force;
	int m_StartTick;
	int m_IgnoreEntityID;
	Uuid m_OwnerBotID;
	bool m_Explosive;
};

#endif
