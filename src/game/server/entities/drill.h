/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_DRILL_H
#define GAME_SERVER_ENTITIES_DRILL_H

#include <game/item_id.h>
#include <game/server/entity.h>

// A placed laser-tripod drill. It is a building: a pickup at its centre, three
// short fixed beams as legs, and one beam that follows the controller's cursor
// and bores outward while fire is held. It retracts at its maximum length, when
// the player lets go, or when it touches a resource node; a contact harvests
// its rated hardness (power) from that node, so better drill materials strip a
// node in fewer passes. The beam deliberately ignores terrain, so it can reach
// ore that a hammer cannot.
class CDrillEntity : public CHitableEntity
{
public:
	// how close a player must stand to take control
	enum
	{
		CONTROL_RADIUS = 4 * 32,
	};
	// world units per tick
	static constexpr float EXTEND_SPEED = 6.0f;
	static constexpr float RETRACT_SPEED = 12.0f;

	CDrillEntity(CGameWorld *pGameWorld, vec2 Pos, const CItemID &ItemId, int MaxLength, int Power, int Owner);
	~CDrillEntity() override;

	void Tick() override;
	void Snap(int SnappingClient) override;
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;

	int Controller() const { return m_Controller; }
	void SetController(int ClientID);

	const CItemID &ItemId() const { return m_ItemId; }
	int MaxLength() const { return m_MaxLength; }
	vec2 Dir() const { return m_Dir; }
	void SetDirection(vec2 Dir);

private:
	enum EState
	{
		STATE_IDLE = 0,
		STATE_EXTENDING,
		STATE_RETRACTING,
	};

	vec2 Tip() const { return m_Pos + m_Dir * m_Length; }
	class CResourceEntity *FindResource(vec2 *pOutPos);
	void Harvest(class CResourceEntity *pRes, vec2 HitPos);
	void SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient);

	CItemID m_ItemId;
	int m_Owner; // client that placed it, for returning the item on dismantle
	int m_Controller; // client currently steering, -1 = none
	int m_MaxLength;
	// hardness removed from a resource node per boring pass
	int m_Power;
	float m_Length;
	vec2 m_Dir;
	EState m_State;
	// set after an automatic retraction so holding fire does not immediately
	// start the next pass; the player has to release and press again
	bool m_WaitRelease;
	// 0..2: the tripod legs, 3: the boring beam. Every snapshot item needs its
	// own id, so the drill owns four.
	int m_aBeamIDs[4];
};

#endif // GAME_SERVER_ENTITIES_DRILL_H
