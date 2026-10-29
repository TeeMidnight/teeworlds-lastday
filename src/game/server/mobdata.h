/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_MOBDATA_H
#define GAME_SERVER_MOBDATA_H

#include <base/system.h>
#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/uuid.h>
#include <base/vmath.h>
#include <game/item_id.h>
#include <game/server/bot.h>

class CGameContext;
class IStorage;
struct _json_value;
typedef struct _json_value json_value;

// Content-defined monsters. A mob definition owns identity, faction, combat
// tuning, abilities, dialogue and drops; the entity (CMonsterEntity) is only
// the runtime body. Keeping the two apart means a new monster is a json file,
// not a new C++ class.
enum EMobDialogueEvent
{
	MOBDIALOGUE_SPAWN = 0,
	MOBDIALOGUE_SIGHT,
	MOBDIALOGUE_COMBAT,
	MOBDIALOGUE_HURT,
	MOBDIALOGUE_KILL,
	MOBDIALOGUE_DEATH,
	MOBDIALOGUE_IDLE,
	MOBDIALOGUE_LOW_HEALTH,
	NUM_MOBDIALOGUE_EVENTS,
};

enum
{
	MAX_MOB_DIALOGUE_LINES = 8,
	MAX_MOB_DIALOGUE_LENGTH = 192,
};

struct SDialogueSet
{
	int m_NumLines;
	char m_aaLines[MAX_MOB_DIALOGUE_LINES][MAX_MOB_DIALOGUE_LENGTH];

	SDialogueSet() : m_NumLines(0) { m_aaLines[0][0] = '\0'; }
};

struct SMobDialogue
{
	SDialogueSet m_aEvents[NUM_MOBDIALOGUE_EVENTS];
};

// Substitutes every {player} placeholder in an already localized dialogue
// line. Localize first: the translation table is keyed by the source text,
// which is the form that still carries the placeholder.
void ReplaceDialoguePlayerToken(char *pBuf, int Size, const char *pPlayerName);

struct SMobDrop
{
	CItemID m_Id;
	char m_aItem[64];
	int m_CountMin;
	int m_CountMax;
	float m_Chance;

	SMobDrop() : m_CountMin(1), m_CountMax(1), m_Chance(1.0f) { m_aItem[0] = '\0'; }
};

struct SMobMelee
{
	int m_Damage;
	int m_Cooldown;
	float m_Range;

	SMobMelee() : m_Damage(3), m_Cooldown(17), m_Range(48.0f) {}
};

struct SMobRanged
{
	bool m_Enabled;
	int m_Weapon;
	int m_Damage;
	int m_Cooldown;
	int m_Force;
	int m_Span;
	int m_Sound;
	float m_Range;
	float m_MinRange;
	float m_AimSpread; // degrees, 0 = perfectly accurate

	SMobRanged() :
		m_Enabled(false), m_Weapon(WEAPON_GUN), m_Damage(2), m_Cooldown(60), m_Force(1), m_Span(600), m_Sound(SOUND_GUN_FIRE), m_Range(360.0f), m_MinRange(96.0f), m_AimSpread(0.0f) {}
};

struct SMobLeap
{
	bool m_Enabled;
	int m_Cooldown;
	float m_Range;
	float m_ForceX;
	float m_ForceY;

	SMobLeap() : m_Enabled(false), m_Cooldown(120), m_Range(220.0f), m_ForceX(6.0f), m_ForceY(-7.0f) {}
};

struct SMobHealAura
{
	bool m_Enabled;
	float m_Radius;
	int m_Amount;
	int m_Interval;

	SMobHealAura() : m_Enabled(false), m_Radius(128.0f), m_Amount(1), m_Interval(50) {}
};

struct SMobArmorAura
{
	bool m_Enabled;
	float m_Radius;
	int m_Amount;
	int m_Interval;

	SMobArmorAura() : m_Enabled(false), m_Radius(128.0f), m_Amount(1), m_Interval(100) {}
};

struct SMobEnrage
{
	int m_HealthPercent; // 0 disables
	int m_DamageBonus;
	int m_SpeedPercent;

	SMobEnrage() : m_HealthPercent(0), m_DamageBonus(1), m_SpeedPercent(30) {}
};

struct SMobWorm
{
	int m_Segments;
	float m_Speed;
	int m_BurrowTicks;
	int m_SurfaceTicks;
	int m_Damage;
	float m_BiteRadius;
	float m_ChaseRadius;

	SMobWorm() :
		m_Segments(6), m_Speed(2.6f), m_BurrowTicks(90), m_SurfaceTicks(70), m_Damage(2), m_BiteRadius(48.0f), m_ChaseRadius(520.0f) {}
};

struct SMobSummon
{
	char m_aMob[64];
	int m_Count;
	int m_HealthPercent; // summon once when health drops below this

	SMobSummon() : m_Count(2), m_HealthPercent(50) { m_aMob[0] = '\0'; }
};

struct SMobDef
{
	char m_aId[64];
	char m_aName[64];
	char m_aGroup[32];
	char m_aAI[32];
	int m_Faction;
	int m_Health;
	int m_Armor;
	float m_SpeedScale;
	float m_AggroRadius;
	float m_AttackRange;
	float m_KeepDistance;
	int m_JumpChance;
	bool m_FightHostileGroups;
	bool m_HasSkin;
	CBotSkin m_Skin;
	bool m_IsWorm;
	bool m_IsBoss;
	SMobSummon m_Summon;
	SMobWorm m_Worm;
	SMobMelee m_Melee;
	SMobRanged m_Ranged;
	SMobLeap m_Leap;
	SMobHealAura m_HealAura;
	SMobArmorAura m_ArmorAura;
	SMobEnrage m_Enrage;
	SMobDialogue m_Dialogue;
	array<SMobDrop> m_lDrops;

	SMobDef() :
		m_Faction(BOTFACTION_HOSTILE), m_Health(10), m_Armor(0), m_SpeedScale(1.0f), m_AggroRadius(360.0f), m_AttackRange(48.0f), m_KeepDistance(0.0f), m_JumpChance(12), m_FightHostileGroups(true), m_HasSkin(false), m_IsWorm(false), m_IsBoss(false)
	{
		m_aId[0] = '\0';
		m_aName[0] = '\0';
		str_copy(m_aGroup, "default", sizeof(m_aGroup));
		str_copy(m_aAI, "hunter", sizeof(m_aAI));
	}
};

struct SMobPoolEntry
{
	const SMobDef *m_pDef;
	int m_Weight;
};

struct SMobSpawn
{
	const SMobDef *m_pDef;
	char m_aWorld[64];
	vec2 m_Pos;
	char m_aName[64];
	Uuid m_Id;
};

// Loads data/mobs/*.json (definitions), data/mobs/pool.json (random spawn
// weights) and data/mobs/spawns.json (fixed content spawns).
class CMobSystem
{
public:
	~CMobSystem();

	void Init(CGameContext *pGameServer, IStorage *pStorage);
	void Clear();

	const SMobDef *Find(const char *pId) const;
	const SMobDef *DefAt(int Index) const { return Index >= 0 && Index < m_lDefs.size() ? m_lDefs[Index] : nullptr; }
	int NumDefs() const { return m_lDefs.size(); }

	// Weighted random definition for a hostile/neutral population, or null
	// when the pool is empty.
	const SMobDef *RandomDef() const;
	int NumPoolEntries() const { return m_lPool.size(); }

	int NumSpawns() const { return m_lSpawns.size(); }
	const SMobSpawn *SpawnAt(int Index) const { return Index >= 0 && Index < m_lSpawns.size() ? &m_lSpawns[Index] : nullptr; }

	// Shared content parsers, also used by the NPC loader: both content kinds
	// speak the same dialogue/aura vocabulary.
	static void LoadDialogue(const json_value &rValue, SMobDialogue *pDialogue);
	static void LoadSkin(const json_value &rValue, CBotSkin *pSkin);

private:
	CGameContext *m_pGameServer = nullptr;
	IStorage *m_pStorage = nullptr;

	array<SMobDef *> m_lDefs;
	hash_table<unsigned, SMobDef *, 8> m_Defs;
	array<SMobPoolEntry> m_lPool;
	int m_PoolWeight = 0;
	array<SMobSpawn> m_lSpawns;

	static int ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadDef(const char *pDefaultId, const char *pFilePath);
	void LoadPool(const char *pFilePath);
	void LoadSpawns(const char *pFilePath);
};

#endif // GAME_SERVER_MOBDATA_H
