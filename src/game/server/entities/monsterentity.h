/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_MONSTERENTITY_H
#define GAME_SERVER_ENTITIES_MONSTERENTITY_H

#include "botentity.h"

#include <game/server/mobdata.h>

class CCharacter;
struct SMobDef;

/*
	Class: CMonsterEntity
		A content-defined tee-shaped creature. It shares the bot body, physics
		and snapshot path with CBotEntity, but its behaviour comes from a mob
		definition: faction, target rules, combat abilities, dialogue and loot.

		The AI is deliberately native rather than a script: a monster can be
		spawned in large numbers and must not pay a Python callback per tick.
		Scripted bot AI remains available for scripted set pieces.
*/
class CMonsterEntity : public CBotEntity
{
public:
	CMonsterEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const SMobDef *pDef, const char *pName = nullptr);

	// --- IBotEntity ---
	const char *BotTypeName() const override { return "monster"; }

	// --- CEntity ---
	void OnBotTick() override;
	void OnAction() override;
	bool TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	void Die(CEntity *pKiller, int Weapon) override;
	bool OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	void OnBotHit(CEntity *pTarget, bool Killed) override;
	int GetHitAttacker() override;

	const SMobDef *Def() const { return m_pDef; }
	const SMobDef *MobDef() const override { return m_pDef; }
	// 0..1, used by the boss HUD and by a future health bar
	float HealthFraction() const { return m_MaxHealth > 0 ? (float) m_Health / (float) m_MaxHealth : 0.0f; }

protected:
	// target acquisition, faction relations and retaliation
	CEntity *FindMonsterTarget();
	bool IsValidTarget(CEntity *pTarget, bool RequireLineOfSight);
	bool IsHostileBot(const IBotEntity *pBot) const;
	bool IsHostilePlayer(const CCharacter *pChr) const;
	bool CanSee(CEntity *pTarget);

	// one behaviour step per strategy
	void ThinkHunter(CEntity *pTarget);
	void ThinkSkirmisher(CEntity *pTarget);
	void ThinkSentry(CEntity *pTarget);
	void ThinkWanderer(CEntity *pTarget);
	void ThinkCompanion(CEntity *pTarget);
	void ThinkSupport(CEntity *pTarget);
	void WanderStep();

	// abilities
	bool FireRanged(vec2 Target);
	bool TryLeap(vec2 Target);
	void TickAuras();
	void TickDialogue();
	void TickEnrage();
	void DropLoot(CEntity *pKiller);

	// dialogue / state
	void SayEvent(int Event, bool Force = false);
	void RememberAttacker(CEntity *pFrom);
	CEntity *RetaliationTarget();
	CCharacter *NearestPlayer(float Radius);

	const SMobDef *m_pDef;
	char m_aName[64];
	vec2 m_SpawnPos;
	int m_SpawnWorldID;

	int m_LastSpeakTick;
	int m_aLastEventTick[NUM_MOBDIALOGUE_EVENTS];
	bool m_SaidSight;
	bool m_SaidLowHealth;

	int m_RangedTimer;
	int m_LeapTimer;
	int m_HealTimer;
	int m_ArmorTimer;
	bool m_Enraged;

	int m_RetaliateUntil;
	Uuid m_LastAttackerBot;
	int m_LastAttackerClient;

	int m_WanderDir;
	int m_WanderChangeTick;
	int m_LastTargetTick;
	CEntity *m_pLastTarget;
};

#endif // GAME_SERVER_ENTITIES_MONSTERENTITY_H
