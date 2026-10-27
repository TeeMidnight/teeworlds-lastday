/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>
#include <generated/protocol.h>

#include "entities/botentity.h"
#include "gamecontext.h"
#include "mobdata.h"

static const char *gs_pSkinParts[NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};

static int PackHsla(int Hue, int Sat, int Lgt, int Alp)
{
	return ((Alp & 0xff) << 24) | ((Hue & 0xff) << 16) | ((Sat & 0xff) << 8) | (Lgt & 0xff);
}

static int ReadInt(const json_value &Value, int Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (int) (json_int_t) Value;
	return Default;
}

static float ReadFloat(const json_value &Value, float Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (float) (double) Value;
	return Default;
}

static bool ReadBool(const json_value &Value, bool Default)
{
	if(Value.type == json_boolean)
		return (bool) Value;
	if(Value.type == json_integer)
		return Value.u.integer != 0;
	return Default;
}

static int WeaponFromName(const char *pName)
{
	if(!pName || !pName[0])
		return WEAPON_GUN;
	if(str_comp_nocase(pName, "shotgun") == 0)
		return WEAPON_SHOTGUN;
	if(str_comp_nocase(pName, "grenade") == 0)
		return WEAPON_GRENADE;
	if(str_comp_nocase(pName, "laser") == 0)
		return WEAPON_LASER;
	return WEAPON_GUN;
}

static void CopyLine(SDialogueSet *pSet, const char *pText)
{
	if(!pText || !pText[0] || pSet->m_NumLines >= MAX_MOB_DIALOGUE_LINES)
		return;
	str_copy(pSet->m_aaLines[pSet->m_NumLines], pText, MAX_MOB_DIALOGUE_LENGTH);
	pSet->m_NumLines++;
}

static int DialogueEventFromName(const char *pName)
{
	if(!pName || !pName[0])
		return -1;
	if(str_comp_nocase(pName, "spawn") == 0)
		return MOBDIALOGUE_SPAWN;
	if(str_comp_nocase(pName, "sight") == 0 || str_comp_nocase(pName, "see_player") == 0)
		return MOBDIALOGUE_SIGHT;
	if(str_comp_nocase(pName, "combat") == 0 || str_comp_nocase(pName, "attack") == 0)
		return MOBDIALOGUE_COMBAT;
	if(str_comp_nocase(pName, "hurt") == 0 || str_comp_nocase(pName, "damaged") == 0)
		return MOBDIALOGUE_HURT;
	if(str_comp_nocase(pName, "kill") == 0)
		return MOBDIALOGUE_KILL;
	if(str_comp_nocase(pName, "death") == 0 || str_comp_nocase(pName, "die") == 0)
		return MOBDIALOGUE_DEATH;
	if(str_comp_nocase(pName, "idle") == 0)
		return MOBDIALOGUE_IDLE;
	if(str_comp_nocase(pName, "low_health") == 0 || str_comp_nocase(pName, "low") == 0)
		return MOBDIALOGUE_LOW_HEALTH;
	return -1;
}

CMobSystem::~CMobSystem()
{
	Clear();
}

void CMobSystem::Clear()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();
	m_lPool.clear();
	m_PoolWeight = 0;
	m_lSpawns.clear();
}

int CMobSystem::ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	CMobSystem *pSelf = static_cast<CMobSystem *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// pool.json and spawns.json are sibling configuration files, not mob
	// definitions; they are loaded explicitly after every definition exists
	if(str_comp_nocase(pFilename, "pool.json") == 0)
		return 0;
	if(str_comp_nocase(pFilename, "spawns.json") == 0)
		return 0;

	char aDefaultId[64];
	if(Len - 5 >= (int) sizeof(aDefaultId))
	{
		dbg_msg("mob", "mob file name too long: '%s'", pFilename);
		return 0;
	}
	str_copy(aDefaultId, pFilename, sizeof(aDefaultId));
	aDefaultId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "mobs/%s", pFilename);
	pSelf->LoadDef(aDefaultId, aPath);
	return 0;
}

void CMobSystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	m_pStorage = pStorage;

	Clear();
	if(m_pStorage)
		m_pStorage->ListDirectory(IStorage::TYPE_ALL, "mobs", ListCallback, this);

	// the pool is resolved after every definition is registered, so a pool
	// entry can name a mob defined in any file order
	if(m_pStorage)
	{
		LoadPool("mobs/pool.json");
		LoadSpawns("mobs/spawns.json");
	}

	dbg_msg("mob", "loaded %d mob definition(s), %d pool entr%s, %d fixed spawn(s)",
		m_lDefs.size(), m_lPool.size(), m_lPool.size() == 1 ? "y" : "ies", m_lSpawns.size());
}

const SMobDef *CMobSystem::Find(const char *pId) const
{
	if(!pId || !pId[0])
		return nullptr;
	SMobDef *const *ppDef = m_Defs.get(str_quickhash(pId));
	return ppDef ? *ppDef : nullptr;
}

const SMobDef *CMobSystem::RandomDef() const
{
	if(!m_lPool.size() || m_PoolWeight <= 0)
		return nullptr;

	int Roll = random_int() % m_PoolWeight;
	for(int i = 0; i < m_lPool.size(); i++)
	{
		Roll -= m_lPool[i].m_Weight;
		if(Roll < 0)
			return m_lPool[i].m_pDef;
	}
	return m_lPool[m_lPool.size() - 1].m_pDef;
}

void CMobSystem::LoadSkin(const json_value &rValue, CBotSkin *pSkin)
{
	if(rValue.type != json_object || !pSkin)
		return;
	for(int i = 0; i < NUM_SKINPARTS; i++)
	{
		const json_value &rPart = rValue[gs_pSkinParts[i]];
		if(rPart.type != json_object)
			continue;

		str_copy(pSkin->m_aaSkinPartNames[i], rPart["name"], sizeof(pSkin->m_aaSkinPartNames[i]));
		const bool Custom = ReadBool(rPart["custom_color"], false);
		pSkin->m_aUseCustomColors[i] = Custom ? 1 : 0;
		pSkin->m_aSkinPartColors[i] = Custom ?
			PackHsla(ReadInt(rPart["hue"], 0), ReadInt(rPart["sat"], 0), ReadInt(rPart["lgt"], 0), ReadInt(rPart["alp"], 255)) : 0;
	}
}

void CMobSystem::LoadDialogue(const json_value &rValue, SMobDialogue *pDialogue)
{
	if(rValue.type != json_object || !pDialogue)
		return;
	for(unsigned i = 0; i < rValue.u.object.length; i++)
	{
		const char *pKey = rValue.u.object.values[i].name;
		const int Event = DialogueEventFromName(pKey);
		if(Event < 0)
			continue;

		const json_value &rLines = *rValue.u.object.values[i].value;
		SDialogueSet *pSet = &pDialogue->m_aEvents[Event];
		if(rLines.type == json_string)
			CopyLine(pSet, rLines.u.string.ptr);
		else if(rLines.type == json_array)
		{
			for(unsigned l = 0; l < rLines.u.array.length; l++)
				if(rLines[l].type == json_string)
					CopyLine(pSet, rLines[l].u.string.ptr);
		}
	}
}

void CMobSystem::LoadDef(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("mob", "failed to load mob '%s' from '%s'", pDefaultId, pFilePath);
		return;
	}

	const char *pId = (*pJson)["id"];
	if(!pId[0])
		pId = pDefaultId;
	if(!pId[0])
		return;
	if(Find(pId))
	{
		dbg_msg("mob", "duplicate mob id '%s' in '%s', keeping the first", pId, pFilePath);
		return;
	}

	SMobDef *pDef = new SMobDef();
	str_copy(pDef->m_aId, pId, sizeof(pDef->m_aId));

	const char *pName = (*pJson)["name"];
	str_copy(pDef->m_aName, pName[0] ? pName : pId, sizeof(pDef->m_aName));

	const char *pFaction = (*pJson)["faction"];
	pDef->m_Faction = BotFactionFromString(pFaction, BOTFACTION_HOSTILE);

	const char *pGroup = (*pJson)["group"];
	if(pGroup[0])
		str_copy(pDef->m_aGroup, pGroup, sizeof(pDef->m_aGroup));

	// ai may be a plain strategy name or an object carrying the same field
	const json_value &rAI = (*pJson)["ai"];
	if(rAI.type == json_string)
		str_copy(pDef->m_aAI, rAI.u.string.ptr, sizeof(pDef->m_aAI));
	else if(rAI.type == json_object)
	{
		const char *pStrategy = rAI["strategy"];
		if(pStrategy[0])
			str_copy(pDef->m_aAI, pStrategy, sizeof(pDef->m_aAI));
	}

	pDef->m_Health = maximum(1, ReadInt((*pJson)["health"], pDef->m_Health));
	pDef->m_Armor = clamp(ReadInt((*pJson)["armor"], pDef->m_Armor), 0, CBotEntity::MAX_ARMOR);
	pDef->m_SpeedScale = clamp(ReadFloat((*pJson)["speed_scale"], pDef->m_SpeedScale), 0.25f, 3.0f);
	pDef->m_AggroRadius = maximum(32.0f, ReadFloat((*pJson)["aggro_radius"], pDef->m_AggroRadius));
	pDef->m_AttackRange = maximum(16.0f, ReadFloat((*pJson)["attack_range"], pDef->m_AttackRange));
	pDef->m_KeepDistance = maximum(0.0f, ReadFloat((*pJson)["keep_distance"], pDef->m_KeepDistance));
	pDef->m_JumpChance = clamp(ReadInt((*pJson)["jump_chance"], pDef->m_JumpChance), 0, 1000);
	pDef->m_FightHostileGroups = ReadBool((*pJson)["fight_hostile_groups"], pDef->m_FightHostileGroups);

	// type may be "tee" (default) or "worm"; a worm still uses a mob def for
	// its identity, faction and drops
	const char *pType = (*pJson)["type"];
	pDef->m_IsWorm = pType[0] && str_comp_nocase(pType, "worm") == 0;
	pDef->m_IsBoss = ReadBool((*pJson)["boss"], false);
	const json_value &rSummon = (*pJson)["summon"];
	if(rSummon.type == json_object)
	{
		const char *pSummonMob = rSummon["mob"];
		str_copy(pDef->m_Summon.m_aMob, pSummonMob, sizeof(pDef->m_Summon.m_aMob));
		pDef->m_Summon.m_Count = clamp(ReadInt(rSummon["count"], pDef->m_Summon.m_Count), 0, 12);
		pDef->m_Summon.m_HealthPercent = clamp(ReadInt(rSummon["health_percent"], pDef->m_Summon.m_HealthPercent), 1, 100);
	}
	if(pDef->m_IsWorm)
	{
		const json_value &rWorm = (*pJson)["worm"];
		if(rWorm.type == json_object)
		{
			pDef->m_Worm.m_Segments = clamp(ReadInt(rWorm["segments"], pDef->m_Worm.m_Segments), 2, 12);
			pDef->m_Worm.m_Speed = clamp(ReadFloat(rWorm["speed"], pDef->m_Worm.m_Speed), 0.5f, 12.0f);
			pDef->m_Worm.m_BurrowTicks = maximum(10, ReadInt(rWorm["burrow_ticks"], pDef->m_Worm.m_BurrowTicks));
			pDef->m_Worm.m_SurfaceTicks = maximum(10, ReadInt(rWorm["surface_ticks"], pDef->m_Worm.m_SurfaceTicks));
			pDef->m_Worm.m_Damage = maximum(1, ReadInt(rWorm["damage"], pDef->m_Worm.m_Damage));
			pDef->m_Worm.m_BiteRadius = maximum(16.0f, ReadFloat(rWorm["bite_radius"], pDef->m_Worm.m_BiteRadius));
			pDef->m_Worm.m_ChaseRadius = maximum(64.0f, ReadFloat(rWorm["chase_radius"], pDef->m_Worm.m_ChaseRadius));
		}
	}

	const json_value &rSkin = (*pJson)["skin"];
	if(rSkin.type == json_object)
	{
		LoadSkin(rSkin, &pDef->m_Skin);
		pDef->m_HasSkin = true;
	}

	const json_value &rAbilities = (*pJson)["abilities"];
	if(rAbilities.type == json_object)
	{
		const json_value &rMelee = rAbilities["melee"];
		if(rMelee.type == json_object)
		{
			pDef->m_Melee.m_Damage = maximum(1, ReadInt(rMelee["damage"], pDef->m_Melee.m_Damage));
			pDef->m_Melee.m_Cooldown = maximum(1, ReadInt(rMelee["cooldown"], pDef->m_Melee.m_Cooldown));
			pDef->m_Melee.m_Range = maximum(16.0f, ReadFloat(rMelee["range"], pDef->m_Melee.m_Range));
		}

		const json_value &rRanged = rAbilities["ranged"];
		if(rRanged.type == json_object)
		{
			pDef->m_Ranged.m_Enabled = ReadBool(rRanged["enabled"], true);
			pDef->m_Ranged.m_Weapon = WeaponFromName(rRanged["weapon"]);
			pDef->m_Ranged.m_Damage = maximum(1, ReadInt(rRanged["damage"], pDef->m_Ranged.m_Damage));
			pDef->m_Ranged.m_Cooldown = maximum(1, ReadInt(rRanged["cooldown"], pDef->m_Ranged.m_Cooldown));
			pDef->m_Ranged.m_Force = maximum(0, ReadInt(rRanged["force"], pDef->m_Ranged.m_Force));
			pDef->m_Ranged.m_Span = maximum(1, ReadInt(rRanged["span"], pDef->m_Ranged.m_Span));
			pDef->m_Ranged.m_Range = maximum(32.0f, ReadFloat(rRanged["range"], pDef->m_Ranged.m_Range));
			pDef->m_Ranged.m_MinRange = maximum(0.0f, ReadFloat(rRanged["min_range"], pDef->m_Ranged.m_MinRange));
			const char *pSound = rRanged["sound"];
			if(pSound[0])
				pDef->m_Ranged.m_Sound = str_comp_nocase(pSound, "shotgun") == 0 ? SOUND_SHOTGUN_FIRE : SOUND_GUN_FIRE;
		}

		const json_value &rLeap = rAbilities["leap"];
		if(rLeap.type == json_object)
		{
			pDef->m_Leap.m_Enabled = ReadBool(rLeap["enabled"], true);
			pDef->m_Leap.m_Cooldown = maximum(1, ReadInt(rLeap["cooldown"], pDef->m_Leap.m_Cooldown));
			pDef->m_Leap.m_Range = maximum(32.0f, ReadFloat(rLeap["range"], pDef->m_Leap.m_Range));
			pDef->m_Leap.m_ForceX = ReadFloat(rLeap["force_x"], pDef->m_Leap.m_ForceX);
			pDef->m_Leap.m_ForceY = ReadFloat(rLeap["force_y"], pDef->m_Leap.m_ForceY);
		}

		const json_value &rHeal = rAbilities["heal_aura"];
		if(rHeal.type == json_object)
		{
			pDef->m_HealAura.m_Enabled = ReadBool(rHeal["enabled"], true);
			pDef->m_HealAura.m_Radius = maximum(16.0f, ReadFloat(rHeal["radius"], pDef->m_HealAura.m_Radius));
			pDef->m_HealAura.m_Amount = maximum(0, ReadInt(rHeal["amount"], pDef->m_HealAura.m_Amount));
			pDef->m_HealAura.m_Interval = maximum(1, ReadInt(rHeal["interval"], pDef->m_HealAura.m_Interval));
		}

		const json_value &rArmor = rAbilities["armor_aura"];
		if(rArmor.type == json_object)
		{
			pDef->m_ArmorAura.m_Enabled = ReadBool(rArmor["enabled"], true);
			pDef->m_ArmorAura.m_Radius = maximum(16.0f, ReadFloat(rArmor["radius"], pDef->m_ArmorAura.m_Radius));
			pDef->m_ArmorAura.m_Amount = maximum(0, ReadInt(rArmor["amount"], pDef->m_ArmorAura.m_Amount));
			pDef->m_ArmorAura.m_Interval = maximum(1, ReadInt(rArmor["interval"], pDef->m_ArmorAura.m_Interval));
		}

		const json_value &rEnrage = rAbilities["enrage"];
		if(rEnrage.type == json_object)
		{
			pDef->m_Enrage.m_HealthPercent = clamp(ReadInt(rEnrage["health_percent"], pDef->m_Enrage.m_HealthPercent), 0, 100);
			pDef->m_Enrage.m_DamageBonus = maximum(0, ReadInt(rEnrage["damage_bonus"], pDef->m_Enrage.m_DamageBonus));
			pDef->m_Enrage.m_SpeedPercent = maximum(0, ReadInt(rEnrage["speed_percent"], pDef->m_Enrage.m_SpeedPercent));
		}
	}

	LoadDialogue((*pJson)["dialogue"], &pDef->m_Dialogue);

	const json_value &rDrops = (*pJson)["drops"];
	if(rDrops.type == json_array)
	{
		for(unsigned i = 0; i < rDrops.u.array.length; i++)
		{
			const json_value &rDrop = rDrops[i];
			if(rDrop.type != json_object)
				continue;
			const char *pItem = rDrop["item"];
			if(!pItem[0])
				continue;

			SMobDrop &Drop = pDef->m_lDrops.emplace();
			str_copy(Drop.m_aItem, pItem, sizeof(Drop.m_aItem));
			Drop.m_Id = CItemID::FromName(pItem);
			Drop.m_CountMin = maximum(1, ReadInt(rDrop["count_min"], 1));
			Drop.m_CountMax = maximum(Drop.m_CountMin, ReadInt(rDrop["count_max"], Drop.m_CountMin));
			Drop.m_Chance = clamp(ReadFloat(rDrop["chance"], 1.0f), 0.0f, 1.0f);
		}
	}

	m_lDefs.add(pDef);
	m_Defs.set(str_quickhash(pDef->m_aId), pDef);

	dbg_msg("mob", "loaded mob '%s' ('%s') faction %s group '%s' ai '%s' hp %d armor %d drops %d%s",
		pDef->m_aId, pDef->m_aName, BotFactionName(pDef->m_Faction), pDef->m_aGroup,
		pDef->m_aAI, pDef->m_Health, pDef->m_Armor, pDef->m_lDrops.size(),
		pDef->m_IsWorm ? " (worm)" : "");
}

void CMobSystem::LoadPool(const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
		return;

	const json_value &rEntries = (*pJson)["entries"];
	if(rEntries.type != json_array)
	{
		dbg_msg("mob", "%s has no \"entries\" array", pFilePath);
		return;
	}

	for(unsigned i = 0; i < rEntries.u.array.length; i++)
	{
		const json_value &rEntry = rEntries[i];
		if(rEntry.type != json_object)
			continue;
		const char *pMob = rEntry["mob"];
		const SMobDef *pDef = Find(pMob);
		if(!pDef)
		{
			dbg_msg("mob", "pool names unknown mob '%s', skipping", pMob);
			continue;
		}
		const int Weight = maximum(1, ReadInt(rEntry["weight"], 1));
		SMobPoolEntry &Pool = m_lPool.emplace();
		Pool.m_pDef = pDef;
		Pool.m_Weight = Weight;
		m_PoolWeight += Weight;
	}
}

void CMobSystem::LoadSpawns(const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_array)
		return;

	for(unsigned i = 0; i < pJson->u.array.length; i++)
	{
		const json_value &rEntry = (*pJson)[i];
		if(rEntry.type != json_object)
			continue;
		const char *pMob = rEntry["mob"];
		const SMobDef *pDef = Find(pMob);
		if(!pDef)
		{
			dbg_msg("mob", "spawn names unknown mob '%s', skipping", pMob);
			continue;
		}

		SMobSpawn &Spawn = m_lSpawns.emplace();
		Spawn.m_pDef = pDef;
		str_copy(Spawn.m_aWorld, rEntry["world"], sizeof(Spawn.m_aWorld));
		Spawn.m_Pos = vec2(ReadInt(rEntry["x"], 0) * 32.0f + 16.0f, ReadInt(rEntry["y"], 0) * 32.0f + 16.0f);
		const char *pName = rEntry["name"];
		str_copy(Spawn.m_aName, pName[0] ? pName : pDef->m_aName, sizeof(Spawn.m_aName));

		char aSeed[192];
		str_format(aSeed, sizeof(aSeed), "lastday:mobspawn:%s:%s:%d:%d:%u", pDef->m_aId, Spawn.m_aWorld,
			(int) Spawn.m_Pos.x, (int) Spawn.m_Pos.y, i);
		Spawn.m_Id = calculate_uuid(aSeed);
	}
}
