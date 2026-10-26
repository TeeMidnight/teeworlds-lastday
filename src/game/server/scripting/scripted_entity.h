/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_SCRIPTING_SCRIPTED_ENTITY_H
#define GAME_SERVER_SCRIPTING_SCRIPTED_ENTITY_H

#include <base/system.h>
#include <base/uuid.h>
#include <base/tl/array.h>
#include <base/tl/hashtable.h>

#include <game/item_id.h>
#include <game/server/entity.h>

#include <string>

class CGameContext;
class CGameWorld;
class IStorage;
struct _json_value;

// world-unit footprint of the built-in health jar visual. The placement code in
// CGameContext checks this box before the entity (and its script) exists.
enum
{
	SCRIPTED_JAR_WIDTH = 64,
	SCRIPTED_JAR_HEIGHT = 96,
};

// Serialize any parsed json value back into compact json text. The entity state
// and the entity definition files both keep their nested objects as text, so a
// definition can be handed to a script unchanged.
std::string ScriptJsonToString(const struct _json_value *pValue);

// A definition loaded from data/entities/<kind>.json:
//
//   {
//     "kind": "health_jar",
//     "script": "scripts/entities/health_jar.py",
//     "health": 10,
//     "radius": 32,
//     "persistent": true,
//     "params": {},
//     "visual": {}
//   }
//
// The C++ side owns position, health and lifetime. Everything the kind does is
// a script callback: on_spawn, on_load, on_tick, on_hit, on_destroy and on_save.
struct SScriptedEntityDef
{
	char m_aKind[64] = "";
	char m_aScript[128] = "";
	int m_Health = 10;
	float m_Radius = 32.0f;
	bool m_Persistent = false;
	std::string m_Params = "{}";
	std::string m_Visual = "{}";
	int m_Module = -1;
};

// A hitable world object whose behaviour lives in a Python module and whose
// drawing is a small list of snapshot primitives written by that module.
class CScriptedEntity : public CHitableEntity
{
public:
	enum
	{
		// one entity can draw at most this many beams / pickups, and therefore
		// hand over at most this many snapshot ids in one set_visual() call.
		MAX_VISUAL_LASERS = 12,
		MAX_VISUAL_PICKUPS = 4,
	};

	struct SVisualLaser
	{
		vec2 m_From;
		vec2 m_To;
		// snapshot id allocated by the script; -1 means "do not draw"
		int m_ID = -1;
	};

	struct SVisualPickup
	{
		vec2 m_Pos;
		int m_Type;
		// snapshot id allocated by the script; -1 means "do not draw"
		int m_ID = -1;
	};

	CScriptedEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pKind, int Module,
		int MaxHealth, int Health, float Radius, const Uuid &OwnerAccount, int OwnerClient, bool Persistent,
		const char *pParams, const char *pState, const char *pVisual);
	~CScriptedEntity() override;

	void Tick() override;
	void Snap(int SnappingClient) override;
	bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;

	// script-side damage path shared by TakeHit and lastday_entity.damage()
	bool Damage(int Amount, int FromClient, int Weapon, CEntity *pFrom);
	// on_destroy + optional item drop + queue the entity for removal
	void Kill();
	// lifecycle callbacks used by CScriptedEntitySystem
	void CallSpawn();
	void CallLoad();
	// lets on_save refresh the state before CGameWorld reads it
	void CallSave();

	// Visual primitives, set by lastday_entity.set_visual(). Every primitive
	// carries a snapshot id that the script allocated (ctx.snap_new_id()); the
	// entity takes ownership of those ids, releases them when the visual is
	// replaced and when the entity is destroyed. The entity never reserves an
	// id of its own in advance.
	void SetVisual(const char *pJson);
	const array<SVisualLaser> &Lasers() const { return m_lLasers; }
	const array<SVisualPickup> &Pickups() const { return m_lPickups; }

	// persistence / script accessors
	const char *Kind() const { return m_aKind; }
	int Module() const { return m_Module; }
	int Health() const { return m_Health; }
	int MaxHealth() const { return m_MaxHealth; }
	float Range() const { return m_Range; }
	const Uuid &OwnerAccount() const { return m_OwnerAccount; }
	int OwnerClient() const { return m_OwnerClient; }
	bool Persistent() const { return m_Persistent; }
	const std::string &State() const { return m_State; }
	const std::string &Params() const { return m_Params; }
	void SetState(const char *pState) { m_State = pState ? pState : "{}"; }
	void SetHealth(int Health);
	void SetPersistent(bool Persistent) { m_Persistent = Persistent; }

	// closest client that owns this entity within its range, or -1
	int OwnerNear();

private:
	bool CallEvent(const char *pEvent, int Player, CEntity *pFrom, int Damage, int Weapon);
	void SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient);
	// returns every snapshot id currently owned by the visual to its floor pool
	void ReleaseVisualIDs();

	char m_aKind[64];
	int m_Module;
	int m_Health;
	int m_MaxHealth;
	float m_Range;
	Uuid m_OwnerAccount;
	int m_OwnerClient;
	bool m_Persistent;
	std::string m_Params;
	std::string m_State;
	// optional item spawned where the entity died (params "drop_item"/"drop_count")
	CItemID m_DropItem;
	int m_DropCount;

	array<SVisualLaser> m_lLasers;
	array<SVisualPickup> m_lPickups;

	bool m_bKilled;
	// ctx["destroy"] observed in the last callback
	bool m_bCtxDestroy;
	int m_LastErrorTick;
};

// Loads data/entities/*.json and spawns CScriptedEntity instances. One instance
// lives in CGameContext, like CItemSystem and CBuffSystem.
class CScriptedEntitySystem
{
public:
	~CScriptedEntitySystem();

	void Init(CGameContext *pGameServer, IStorage *pStorage);
	void Load();
	void ReloadScripts();

	// Health = current hp, -1 (default) means full. MaxHealth = 0 means the
	// definition's health (placement may pass the item's health instead).
	CScriptedEntity *Spawn(const char *pKind, CGameWorld *pWorld, vec2 Pos,
		const char *pParams = nullptr, const Uuid &OwnerAccount = UUID_ZEROED,
		int OwnerClient = -1, int Persistent = -1, int MaxHealth = 0,
		int Health = -1, const char *pState = nullptr, bool FromLoad = false);

	// builds the placement params of a health jar and spawns the scripted kind
	CScriptedEntity *SpawnHealthJar(CGameWorld *pWorld, vec2 Pos, const CItemID &ItemId,
		const Uuid &OwnerAccount, int OwnerClient, const char *pOwnerName,
		int Cooldown, int Range, int Health, const char *pState = nullptr,
		bool FromLoad = false);

	const SScriptedEntityDef *GetDef(const char *pKind) const;
	int NumDefs() const { return m_lDefs.size(); }
	const SScriptedEntityDef *DefAt(int Index) const { return Index >= 0 && Index < m_lDefs.size() ? m_lDefs[Index] : nullptr; }

private:
	CGameContext *m_pGameServer = nullptr;
	IStorage *m_pStorage = nullptr;

	array<SScriptedEntityDef *> m_lDefs;
	hash_table<unsigned, SScriptedEntityDef *, 8> m_Defs;

	static int ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadEntity(const char *pDefaultKind, const char *pFilePath);
};

#endif
