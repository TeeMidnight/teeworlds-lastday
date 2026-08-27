#ifndef GAME_SERVER_ENTITIES_RESOURCE_H
#define GAME_SERVER_ENTITIES_RESOURCE_H

#include <game/server/entity.h>

// a resource point placed on a red team spawn point by the map generator.
// The "display" json field decides which pickup sprite the clients render.
// The "respawntime" json field decides how long (in seconds) a depleted
// resource waits before it respawns; 0 means it never respawns.
class CResourceEntity : public CEntity
{
public:
	CResourceEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pResId, const char *pDisplay, int Hardness, int RespawnTime);

	virtual void Snap(int SnappingClient);
	virtual void Tick();

	// returns the remaining hardness after taking a hit, and true when the
	// resource is depleted (it starts its respawn countdown)
	bool TakeHit(int Hardness);

	const char *ResId() const { return m_aResId; }
	int RemainingHardness() const { return m_Hardness; }
	bool IsRespawning() const { return m_RespawnTick > 0; }

private:
	char m_aResId[32];
	char m_aDisplay[32];
	int m_Hardness;
	int m_MaxHardness;
	int m_RespawnTime;
	int m_RespawnTick;
};

#endif
