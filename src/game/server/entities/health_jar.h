/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_HEALTH_JAR_H
#define GAME_SERVER_ENTITIES_HEALTH_JAR_H

#include <base/uuid.h>

#include <game/item_id.h>
#include <game/server/entity.h>

class CPlayer;

/*
	Class: CHealthJarEntity
		A laser-built container: a two tile wide, three tile tall jar frame with
		a closed bottom and lid, and a health pickup sitting in its middle.

		The frame is only a drawing (lasers do not collide), so a tee can walk
		through it. The middle pickup is a snap only: it never heals by itself.
		While the owner stands inside m_Range, the jar drops a real health pickup
		(the same healing as a normal PICKUP_HEALTH) to the ground, at most once
		per cooldown. A jar never holds more than one unclaimed drop at a time.

		Ownership follows the account when the placer is logged in, so a client
		slot reused after a reconnect never inherits someone else's jar. A jar
		placed offline falls back to the placer's client id and loses its owner
		when the server restarts.
*/
class CHealthJarEntity : public CHitableEntity
{
public:
	enum
	{
		// two tiles wide, three tiles tall, centred on m_Pos
		JAR_WIDTH = 64,
		JAR_HEIGHT = 96,
		// bottom, two walls, two shoulders and the lid
		NUM_BEAMS = 6,
	};

	CHealthJarEntity(CGameWorld *pGameWorld, vec2 Pos, const CItemID &ItemId, int Cooldown, int Range, int Health, const Uuid &OwnerAccount, int OwnerClient, const char *pOwnerName);
	~CHealthJarEntity() override;

	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;

	const CItemID &ItemId() const { return m_ItemId; }
	const Uuid &OwnerAccount() const { return m_OwnerAccount; }
	const char *OwnerName() const { return m_aOwnerName; }

private:
	bool IsOwner(const CPlayer *pPlayer) const;
	bool OwnerNear();
	bool DropNearby();
	void SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient);

	CItemID m_ItemId;
	// account of the placer, or UUID_ZEROED when the placer was not logged in
	Uuid m_OwnerAccount;
	// placer's client slot, used only while no account is known
	int m_OwnerClient;
	char m_aOwnerName[64];
	int m_CooldownTicks;
	int m_Range;
	int m_Health;
	int m_NextDropTick;
	int m_aBeamIDs[NUM_BEAMS];
};

#endif // GAME_SERVER_ENTITIES_HEALTH_JAR_H
