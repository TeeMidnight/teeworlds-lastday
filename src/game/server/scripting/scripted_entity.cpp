/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/scripting/scripted_entity.h>

#include <base/math.h>
#include <base/system.h>
#include <base/tl/stream.h>

#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/entities/dropped_pickup.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/server/scripting/script_runtime.h>

#include <pybind11/embed.h>

#include <string>

namespace py = pybind11;

namespace
{
// The world of the CScriptedEntity callback that is currently running. A module
// function called from on_spawn/on_tick/on_hit/on_destroy must spawn its helpers
// into that same world, including when the callback runs while a ring
// neighbour is being snapshotted.
CGameWorld *s_pActiveWorld = nullptr;

struct SActiveWorldScope
{
	CGameWorld *m_pPrev;
	explicit SActiveWorldScope(CGameWorld *pWorld) : m_pPrev(s_pActiveWorld) { s_pActiveWorld = pWorld; }
	~SActiveWorldScope() { s_pActiveWorld = m_pPrev; }
};

CGameContext *Game()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime ? pRuntime->GameServer() : nullptr;
}

CGameWorld *ActiveWorldOrMain()
{
	if(s_pActiveWorld)
		return s_pActiveWorld;

	CGameContext *pGame = Game();
	if(!pGame)
		return nullptr;

	CGameWorld *pWorld = pGame->MainWorld();
	if(pWorld)
		return pWorld;

	CGameWorld *pFirst = nullptr;
	pGame->m_pWorlds.for_each([](CGameWorld *&pEntry, void *pUser) -> void {
		CGameWorld **ppOut = static_cast<CGameWorld **>(pUser);
		if(!*ppOut && pEntry)
			*ppOut = pEntry;
	}, &pFirst);
	return pFirst;
}

CScriptedEntity *FindScriptedEntity(int EntityID)
{
	CGameWorld *pWorld = ActiveWorldOrMain();
	if(pWorld)
	{
		CEntity *pEntity = pWorld->FindEntityByID(EntityID);
		if(pEntity && pEntity->ObjType() == CGameWorld::ENTTYPE_SCRIPTED)
			return static_cast<CScriptedEntity *>(pEntity);
	}

	// callbacks normally have an active world; a console/global call may name an
	// entity of any loaded world, so fall back to a scan of all of them.
	CGameContext *pGame = Game();
	if(!pGame)
		return nullptr;

	struct SFind
	{
		int m_ID;
		CScriptedEntity *m_pFound;
	} Find = {EntityID, nullptr};
	pGame->m_pWorlds.for_each([](CGameWorld *&pEntry, void *pUser) -> void {
		SFind *pFind = static_cast<SFind *>(pUser);
		if(pFind->m_pFound || !pEntry)
			return;
		CEntity *pEntity = pEntry->FindEntityByID(pFind->m_ID);
		if(pEntity && pEntity->ObjType() == CGameWorld::ENTTYPE_SCRIPTED)
			pFind->m_pFound = static_cast<CScriptedEntity *>(pEntity);
	}, &Find);
	return Find.m_pFound;
}

bool ValidPos(float X, float Y)
{
	return absolute(X) < 1.0e7f && absolute(Y) < 1.0e7f;
}

py::tuple Vec2Tuple(float X, float Y)
{
	py::tuple Result(2);
	Result[0] = py::cast(X);
	Result[1] = py::cast(Y);
	return Result;
}

std::string JsonDumps(py::handle Value)
{
	try
	{
		py::object Json = py::module_::import("json");
		return Json.attr("dumps")(Value).cast<std::string>();
	}
	catch(const py::python_error &)
	{
		return "{}";
	}
	catch(const std::exception &)
	{
		return "{}";
	}
}

// A module argument that should be json is accepted either as a ready made
// string or as a Python dict/list which is dumped here.
std::string JsonArg(const py::object &Value)
{
	if(Value.is_none())
		return "{}";
	if(py::isinstance<py::str>(Value))
		return py::cast<std::string>(Value);
	return JsonDumps(Value);
}

int ReadInt(const json_value &Value, int Default)
{
	if(Value.type == json_integer)
		return (int) (json_int_t) Value;
	if(Value.type == json_double)
		return (int) (double) Value.u.dbl;
	return Default;
}

float ReadFloat(const json_value &Value, float Default)
{
	if(Value.type == json_integer)
		return (float) (json_int_t) Value;
	if(Value.type == json_double)
		return (float) Value.u.dbl;
	return Default;
}

void WriteJsonValue(CJsonWriter &Writer, const json_value &Value)
{
	switch(Value.type)
	{
	case json_object:
		Writer.BeginObject();
		for(unsigned i = 0; i < Value.u.object.length; i++)
		{
			Writer.WriteAttribute(Value.u.object.values[i].name);
			WriteJsonValue(Writer, *Value.u.object.values[i].value);
		}
		Writer.EndObject();
		break;
	case json_array:
		Writer.BeginArray();
		for(unsigned i = 0; i < Value.u.array.length; i++)
			WriteJsonValue(Writer, *Value.u.array.values[i]);
		Writer.EndArray();
		break;
	case json_integer:
		Writer.WriteInt64Value(Value.u.integer);
		break;
	case json_double:
		Writer.WriteDoubleValue(Value.u.dbl);
		break;
	case json_string:
		Writer.WriteStrValue(Value.u.string.ptr);
		break;
	case json_boolean:
		Writer.WriteBoolValue(Value.u.boolean != 0);
		break;
	case json_none:
	case json_null:
	default:
		Writer.WriteNullValue();
		break;
	}
}

bool JsonIdFromFilename(const char *pFilename, char *pId, int Size)
{
	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return false;
	if(Len - 5 >= Size)
		return false;
	str_copy(pId, pFilename, Size);
	pId[Len - 5] = '\0';
	return true;
}
} // namespace

std::string ScriptJsonToString(const struct _json_value *pValue)
{
	if(!pValue)
		return "{}";

	array<char> lJson;
	memory_stream<char> Stream(&lJson);
	CJsonWriter Writer(&Stream);
	WriteJsonValue(Writer, *pValue);
	lJson.add(0);
	return std::string(lJson.base_ptr());
}

//////////////////////////////////////////////////
// scripted entity
//////////////////////////////////////////////////
CScriptedEntity::CScriptedEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pKind, int Module,
	int MaxHealth, int Health, float Radius, const Uuid &OwnerAccount, int OwnerClient, bool Persistent,
	const char *pParams, const char *pState, const char *pVisual) :
	CHitableEntity(pGameWorld, CGameWorld::ENTTYPE_SCRIPTED, 0, Pos, Radius)
{
	str_copy(m_aKind, pKind ? pKind : "", sizeof(m_aKind));
	m_Module = Module;
	m_MaxHealth = maximum(1, MaxHealth);
	m_Health = Health < 0 ? m_MaxHealth : clamp(Health, 1, m_MaxHealth);
	m_OwnerAccount = OwnerAccount;
	m_OwnerClient = OwnerClient;
	m_Persistent = Persistent;
	m_Params = pParams && pParams[0] ? pParams : "{}";
	m_State = pState && pState[0] ? pState : "{}";
	m_bKilled = false;
	m_bCtxDestroy = false;
	m_LastErrorTick = -100000;

	m_Range = maximum(32.0f, Radius);
	m_DropCount = 1;

	// range and the optional death drop come from the placement params; the
	// script sees the same params and may keep its own copies in state.
	CJsonParser Parser;
	json_value *pRoot = Parser.ParseString(m_Params.c_str(), "entity params");
	if(pRoot && pRoot->type == json_object)
	{
		m_Range = maximum(32.0f, (float) ReadInt((*pRoot)["range"], round_to_int(m_Range)));
		const json_value &DropItem = (*pRoot)["drop_item"];
		if(DropItem.type == json_string)
			CItemID::Parse(DropItem.u.string.ptr, &m_DropItem);
		m_DropCount = maximum(1, ReadInt((*pRoot)["drop_count"], 1));
	}

	m_lLasers.hint_size(MAX_VISUAL_LASERS);
	m_lPickups.hint_size(MAX_VISUAL_PICKUPS);
	for(int i = 0; i < MAX_VISUAL_LASERS; i++)
		m_aLaserIDs[i] = Server()->SnapNewID();
	for(int i = 0; i < MAX_VISUAL_PICKUPS; i++)
		m_aPickupIDs[i] = Server()->SnapNewID();

	SetVisual(pVisual);

	GameWorld()->InsertEntity(this);
}

CScriptedEntity::~CScriptedEntity()
{
	for(int i = 0; i < MAX_VISUAL_LASERS; i++)
		Server()->SnapFreeID(m_aLaserIDs[i]);
	for(int i = 0; i < MAX_VISUAL_PICKUPS; i++)
		Server()->SnapFreeID(m_aPickupIDs[i]);
}

void CScriptedEntity::SetVisual(const char *pJson)
{
	m_lLasers.clear();
	m_lPickups.clear();
	if(!pJson || !pJson[0])
		return;

	CJsonParser Parser;
	json_value *pRoot = Parser.ParseString(pJson, "entity visual");
	if(!pRoot || pRoot->type != json_object)
		return;

	const json_value &rLasers = (*pRoot)["lasers"];
	for(unsigned i = 0; rLasers.type == json_array && i < rLasers.u.array.length && m_lLasers.size() < MAX_VISUAL_LASERS; i++)
	{
		const json_value &rLaser = rLasers[i];
		SVisualLaser Laser;
		if(rLaser.type == json_object)
		{
			Laser.m_From = vec2(ReadFloat(rLaser["x1"], 0.0f), ReadFloat(rLaser["y1"], 0.0f));
			Laser.m_To = vec2(ReadFloat(rLaser["x2"], 0.0f), ReadFloat(rLaser["y2"], 0.0f));
		}
		else if(rLaser.type == json_array && rLaser.u.array.length >= 4)
		{
			Laser.m_From = vec2(ReadFloat(rLaser[0], 0.0f), ReadFloat(rLaser[1], 0.0f));
			Laser.m_To = vec2(ReadFloat(rLaser[2], 0.0f), ReadFloat(rLaser[3], 0.0f));
		}
		else
		{
			continue;
		}
		m_lLasers.add(Laser);
	}

	const json_value &rPickups = (*pRoot)["pickups"];
	for(unsigned i = 0; rPickups.type == json_array && i < rPickups.u.array.length && m_lPickups.size() < MAX_VISUAL_PICKUPS; i++)
	{
		const json_value &rPickup = rPickups[i];
		SVisualPickup Pickup;
		if(rPickup.type == json_object)
		{
			Pickup.m_Pos = vec2(ReadFloat(rPickup["x"], 0.0f), ReadFloat(rPickup["y"], 0.0f));
			Pickup.m_Type = ReadInt(rPickup["type"], PICKUP_HEALTH);
		}
		else if(rPickup.type == json_array && rPickup.u.array.length >= 3)
		{
			Pickup.m_Pos = vec2(ReadFloat(rPickup[0], 0.0f), ReadFloat(rPickup[1], 0.0f));
			Pickup.m_Type = ReadInt(rPickup[2], PICKUP_HEALTH);
		}
		else
		{
			continue;
		}
		m_lPickups.add(Pickup);
	}
}

void CScriptedEntity::SetHealth(int Health)
{
	m_Health = clamp(Health, 0, m_MaxHealth);
}

bool CScriptedEntity::CallEvent(const char *pEvent, int Player, CEntity *pFrom, int Damage, int Weapon)
{
	m_bCtxDestroy = false;
	if(m_Module < 0)
		return false;

	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime || !pRuntime->Ready())
		return false;

	SScriptContext Ctx;
	Ctx.m_pGameServer = GameServer();
	Ctx.m_ClientID = Player;
	Ctx.m_EntityID = GetID();
	Ctx.m_WorldID = GameWorld()->WorldID();
	Ctx.m_Pos = m_Pos;
	Ctx.m_Now = Server()->Tick();
	Ctx.m_TickSpeed = Server()->TickSpeed();
	Ctx.m_Params = m_Params;
	Ctx.m_State = m_State;
	Ctx.m_AmmoDamage = maximum(0, Damage);
	str_copy(Ctx.m_aEvent, pEvent, sizeof(Ctx.m_aEvent));
	str_copy(Ctx.m_aKind, m_aKind, sizeof(Ctx.m_aKind));

	char aExtra[192];
	str_format(aExtra, sizeof(aExtra), "{\"damage\":%d,\"weapon\":%d,\"from\":%d}",
		maximum(0, Damage), Weapon, Player);
	Ctx.m_Extra = aExtra;

	(void) pFrom;

	SActiveWorldScope Scope(GameWorld());
	if(!pRuntime->Call(m_Module, pEvent, Ctx))
	{
		const int Now = Server()->Tick();
		if(Now - m_LastErrorTick > Server()->TickSpeed() * 5)
		{
			m_LastErrorTick = Now;
			dbg_msg("entity", "script error for kind '%s' in %s: %s", m_aKind, pEvent, pRuntime->LastError());
		}
		return false;
	}

	m_State = Ctx.m_State;
	m_bCtxDestroy = Ctx.m_Destroy;
	return true;
}

void CScriptedEntity::CallSpawn()
{
	CallEvent("on_spawn", m_OwnerClient, nullptr, 0, -1);
}

void CScriptedEntity::CallLoad()
{
	CallEvent("on_load", m_OwnerClient, nullptr, 0, -1);
}

void CScriptedEntity::CallSave()
{
	CallEvent("on_save", -1, nullptr, 0, -1);
}

void CScriptedEntity::Tick()
{
	if(m_bKilled || IsMarkedForDestroy())
		return;

	CallEvent("on_tick", -1, nullptr, 0, -1);
	if(m_bCtxDestroy || m_Health <= 0)
		Kill();
}

bool CScriptedEntity::Damage(int Amount, int FromClient, int Weapon, CEntity *pFrom)
{
	if(m_bKilled || IsMarkedForDestroy())
		return false;

	const int Attacker = pFrom ? pFrom->GetHitAttacker() : FromClient;
	CallEvent("on_hit", Attacker, pFrom, Amount, Weapon);

	if(m_bKilled || IsMarkedForDestroy())
		return true;

	m_Health -= maximum(1, Amount);
	if(m_Health <= 0)
		Kill();
	else
		GameWorld()->CreateHammerHit(m_Pos);
	return true;
}

bool CScriptedEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	return Damage(Dmg, pFrom ? pFrom->GetHitAttacker() : -1, Weapon, pFrom);
}

void CScriptedEntity::Kill()
{
	if(m_bKilled || IsMarkedForDestroy())
		return;
	m_bKilled = true;

	CallEvent("on_destroy", -1, nullptr, 0, -1);

	if(!m_DropItem.IsZero())
		new CDroppedPickup(GameWorld(), m_Pos + vec2(0.0f, -16.0f), vec2(0.0f, 0.0f), m_DropItem, m_DropCount);

	GameWorld()->DestroyEntity(this);
}

int CScriptedEntity::OwnerNear()
{
	CGameContext *pGame = GameServer();
	if(!pGame)
		return -1;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(!pPlayer || pPlayer->IsDummy())
			continue;

		// an account is stable across reconnects and client slot reuse; only
		// fall back to the slot when the entity was placed offline
		bool IsOwner;
		if(m_OwnerAccount != UUID_ZEROED)
			IsOwner = pPlayer->m_LoggedIn && pPlayer->m_AccountUuid == m_OwnerAccount;
		else
			IsOwner = m_OwnerClient >= 0 && pPlayer->GetCID() == m_OwnerClient;
		if(!IsOwner)
			continue;

		if(pPlayer->GameWorld() != GameWorld())
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		if(distance(pChr->GetPos(), m_Pos) <= m_Range)
			return pPlayer->GetCID();
	}
	return -1;
}

void CScriptedEntity::SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient)
{
	if(NetworkClippedLine(SnappingClient, From, To))
		return;

	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, ID, sizeof(CNetObj_Laser)));
	if(!pObj)
		return;

	pObj->m_X = round_to_int(To.x);
	pObj->m_Y = round_to_int(To.y);
	pObj->m_FromX = round_to_int(From.x);
	pObj->m_FromY = round_to_int(From.y);
	// a fresh start tick keeps the beam at full brightness instead of fading
	pObj->m_StartTick = Server()->Tick();
}

void CScriptedEntity::Snap(int SnappingClient)
{
	if(m_bKilled || IsMarkedForDestroy() || NetworkClipped(SnappingClient))
		return;

	for(int i = 0; i < m_lPickups.size(); i++)
	{
		const SVisualPickup &Pickup = m_lPickups[i];
		const vec2 Pos = m_Pos + Pickup.m_Pos;
		if(NetworkClipped(SnappingClient, Pos))
			continue;

		CNetObj_Pickup *pObj = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, m_aPickupIDs[i], sizeof(CNetObj_Pickup)));
		if(!pObj)
			continue;
		pObj->m_X = round_to_int(Pos.x);
		pObj->m_Y = round_to_int(Pos.y);
		pObj->m_Type = Pickup.m_Type;
	}

	for(int i = 0; i < m_lLasers.size(); i++)
	{
		const SVisualLaser &Laser = m_lLasers[i];
		SnapBeam(m_aLaserIDs[i], m_Pos + Laser.m_From, m_Pos + Laser.m_To, SnappingClient);
	}
}

//////////////////////////////////////////////////
// entity definitions and spawning
//////////////////////////////////////////////////
CScriptedEntitySystem::~CScriptedEntitySystem()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();
}

void CScriptedEntitySystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	m_pStorage = pStorage;
	Load();
}

int CScriptedEntitySystem::ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	CScriptedEntitySystem *pSelf = static_cast<CScriptedEntitySystem *>(pUser);
	if(IsDir || !pFilename[0] || str_comp(pFilename, ".") == 0 || str_comp(pFilename, "..") == 0)
		return 0;

	char aKind[64];
	if(!JsonIdFromFilename(pFilename, aKind, sizeof(aKind)))
		return 0;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "entities/%s", pFilename);
	pSelf->LoadEntity(aKind, aPath);
	return 0;
}

void CScriptedEntitySystem::LoadEntity(const char *pDefaultKind, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("entity", "could not parse '%s'", pFilePath);
		return;
	}

	SScriptedEntityDef *pDef = new SScriptedEntityDef();
	const char *pKind = (*pJson)["kind"];
	str_copy(pDef->m_aKind, pKind[0] ? pKind : pDefaultKind, sizeof(pDef->m_aKind));
	str_copy(pDef->m_aScript, (*pJson)["script"], sizeof(pDef->m_aScript));
	pDef->m_Health = maximum(1, ReadInt((*pJson)["health"], 10));
	pDef->m_Radius = maximum(1.0f, ReadFloat((*pJson)["radius"], 32.0f));

	const json_value &rPersistent = (*pJson)["persistent"];
	pDef->m_Persistent = rPersistent.type == json_boolean ? rPersistent.u.boolean != 0 : false;

	const json_value &rParams = (*pJson)["params"];
	pDef->m_Params = rParams.type == json_object || rParams.type == json_array ? ScriptJsonToString(&rParams) : "{}";
	const json_value &rVisual = (*pJson)["visual"];
	pDef->m_Visual = rVisual.type == json_object || rVisual.type == json_array ? ScriptJsonToString(&rVisual) : "{}";

	if(pDef->m_aScript[0])
	{
		pDef->m_Module = CScriptRuntime::Get()->LoadModule(pDef->m_aScript);
		if(pDef->m_Module < 0)
		{
			// a definition without its behaviour would silently do nothing, so
			// skip it and let the log say why placement is unavailable
			dbg_msg("entity", "script '%s' for entity '%s' failed to load, skipping kind", pDef->m_aScript, pDef->m_aKind);
			delete pDef;
			return;
		}
	}

	if(m_Defs.get(str_quickhash(pDef->m_aKind)))
	{
		dbg_msg("entity", "duplicate entity kind '%s', keeping the first", pDef->m_aKind);
		delete pDef;
		return;
	}

	m_Defs.set(str_quickhash(pDef->m_aKind), pDef);
	m_lDefs.add(pDef);
	dbg_msg("entity", "loaded entity '%s' (script '%s', health %d, radius %.0f, persistent %d)",
		pDef->m_aKind, pDef->m_aScript, pDef->m_Health, pDef->m_Radius, pDef->m_Persistent);
}

void CScriptedEntitySystem::Load()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();

	if(!m_pStorage)
		return;
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "entities", ListCallback, this);
}

void CScriptedEntitySystem::ReloadScripts()
{
	for(int i = 0; i < m_lDefs.size(); i++)
	{
		SScriptedEntityDef *pDef = m_lDefs[i];
		if(pDef->m_aScript[0])
			pDef->m_Module = CScriptRuntime::Get()->LoadModule(pDef->m_aScript);
	}
}

const SScriptedEntityDef *CScriptedEntitySystem::GetDef(const char *pKind) const
{
	if(!pKind || !pKind[0])
		return nullptr;
	SScriptedEntityDef *const *ppDef = m_Defs.get(str_quickhash(pKind));
	return ppDef ? *ppDef : nullptr;
}

CScriptedEntity *CScriptedEntitySystem::Spawn(const char *pKind, CGameWorld *pWorld, vec2 Pos,
	const char *pParams, const Uuid &OwnerAccount, int OwnerClient, int Persistent, int MaxHealth,
	int Health, const char *pState, bool FromLoad)
{
	if(!pWorld || !pKind || !pKind[0])
		return nullptr;

	const SScriptedEntityDef *pDef = GetDef(pKind);
	if(!pDef)
	{
		dbg_msg("entity", "unknown scripted entity kind '%s'", pKind);
		return nullptr;
	}

	const char *pUseParams = pParams && pParams[0] ? pParams : pDef->m_Params.c_str();
	const int UseMaxHealth = MaxHealth > 0 ? MaxHealth : pDef->m_Health;
	const bool UsePersistent = Persistent < 0 ? pDef->m_Persistent : Persistent != 0;
	const char *pUseState = pState && pState[0] ? pState : "{}";

	CScriptedEntity *pEntity = new CScriptedEntity(pWorld, Pos, pDef->m_aKind, pDef->m_Module,
		UseMaxHealth, Health, pDef->m_Radius, OwnerAccount, OwnerClient, UsePersistent,
		pUseParams, pUseState, pDef->m_Visual.c_str());

	pEntity->CallSpawn();
	if(FromLoad)
		pEntity->CallLoad();
	return pEntity;
}

CScriptedEntity *CScriptedEntitySystem::SpawnHealthJar(CGameWorld *pWorld, vec2 Pos, const CItemID &ItemId,
	const Uuid &OwnerAccount, int OwnerClient, const char *pOwnerName,
	int Cooldown, int Range, int Health, const char *pState, bool FromLoad)
{
	char aItem[UUID_MAXSTRSIZE];
	ItemId.FormatUuid(aItem, sizeof(aItem));
	char aOwner[UUID_MAXSTRSIZE];
	format_uuid(OwnerAccount, aOwner, sizeof(aOwner));

	array<char> lJson;
	memory_stream<char> Stream(&lJson);
	CJsonWriter Writer(&Stream);
	Writer.BeginObject();
	Writer.WriteAttribute("item");
	Writer.WriteStrValue(aItem);
	Writer.WriteAttribute("owner");
	Writer.WriteStrValue(aOwner);
	Writer.WriteAttribute("owner_name");
	Writer.WriteStrValue(pOwnerName ? pOwnerName : "");
	Writer.WriteAttribute("cooldown");
	Writer.WriteIntValue(maximum(1, Cooldown));
	Writer.WriteAttribute("range");
	Writer.WriteIntValue(maximum(32, Range));
	Writer.WriteAttribute("health");
	Writer.WriteIntValue(maximum(1, Health));
	Writer.EndObject();
	lJson.add(0);

	return Spawn("health_jar", pWorld, Pos, lJson.base_ptr(), OwnerAccount, OwnerClient,
		1, Health, -1, pState, FromLoad);
}

//////////////////////////////////////////////////
// lastday_entity module
//////////////////////////////////////////////////
PYBIND11_EMBEDDED_MODULE(lastday_entity, m)
{
	m.def("spawn", [](const std::string &Kind, float X, float Y, py::object Params) -> int {
		CGameWorld *pWorld = ActiveWorldOrMain();
		CGameContext *pGame = Game();
		if(!pWorld || !pGame || !pGame->ScriptedEntities() || !ValidPos(X, Y))
			return -1;
		const std::string Json = JsonArg(Params);
		CScriptedEntity *pEntity = pGame->ScriptedEntities()->Spawn(Kind.c_str(), pWorld, vec2(X, Y), Json.c_str());
		return pEntity ? pEntity->GetID() : -1;
	});

	m.def("destroy", [](int EntityID) -> bool {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity)
			return false;
		pEntity->Kill();
		return true;
	});

	m.def("pos", [](int EntityID) {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity)
			return Vec2Tuple(0.0f, 0.0f);
		return Vec2Tuple(pEntity->GetPos().x, pEntity->GetPos().y);
	});

	m.def("health", [](int EntityID) -> int {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		return pEntity ? pEntity->Health() : 0;
	});

	m.def("damage", [](int EntityID, int Amount, int FromClient, int Weapon) -> bool {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		return pEntity && pEntity->Damage(Amount, FromClient, Weapon, nullptr);
	});

	m.def("set_visual", [](int EntityID, py::object Visual) -> bool {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity)
			return false;
		const std::string Json = JsonArg(Visual);
		pEntity->SetVisual(Json.c_str());
		return true;
	});

	m.def("set_health", [](int EntityID, int Health) -> bool {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity)
			return false;
		pEntity->SetHealth(Health);
		return true;
	});

	m.def("set_persistent", [](int EntityID, bool Persistent) -> bool {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity)
			return false;
		pEntity->SetPersistent(Persistent);
		return true;
	});

	// Constructor-style entity factories: the Python HealthDrop/DroppedPickup
	// classes in _context.py call these, so scripts write HealthDrop(...)
	// instead of a procedural spawn call. C++ still owns the entity lifetime.
	m.def("create_health_drop", [](float X, float Y, float VX, float VY) -> int {
		CGameWorld *pWorld = ActiveWorldOrMain();
		if(!pWorld || !ValidPos(X, Y))
			return -1;
		CDroppedPickup *pDrop = new CDroppedPickup(pWorld, vec2(X, Y), vec2(VX, VY), 1);
		return pDrop->GetID();
	});

	m.def("health_drop_near", [](float X, float Y, float Radius) -> bool {
		CGameWorld *pWorld = ActiveWorldOrMain();
		if(!pWorld || !ValidPos(X, Y))
			return false;
		array<CEntity *> lDrops;
		const float Reach = Radius > 0.0f ? Radius : 96.0f;
		const int Num = pWorld->FindEntities(vec2(X, Y), Reach, lDrops, CGameWorld::ENTTYPE_DROPPEDPICKUP);
		for(int i = 0; i < Num; i++)
		{
			CDroppedPickup *pDrop = static_cast<CDroppedPickup *>(lDrops[i]);
			if(pDrop && pDrop->IsHealthDrop())
				return true;
		}
		return false;
	});

	m.def("create_dropped_pickup", [](float X, float Y, float DX, float DY, const std::string &ItemId, int Count) -> int {
		CGameWorld *pWorld = ActiveWorldOrMain();
		if(!pWorld || !ValidPos(X, Y) || Count <= 0)
			return -1;

		CItemID Id;
		if(!CItemID::Parse(ItemId.c_str(), &Id) || Id.IsZero())
			return -1;

		CDroppedPickup *pPickup = new CDroppedPickup(pWorld, vec2(X, Y), vec2(DX, DY), Id, Count);
		return pPickup->GetID();
	});

	// Compatibility names kept for scripts that predate the class wrappers.
	m.def("drop_health", [](float X, float Y) -> int {
		CGameWorld *pWorld = ActiveWorldOrMain();
		if(!pWorld || !ValidPos(X, Y))
			return -1;
		array<CEntity *> lDrops;
		const int Num = pWorld->FindEntities(vec2(X, Y), 96.0f, lDrops, CGameWorld::ENTTYPE_DROPPEDPICKUP);
		for(int i = 0; i < Num; i++)
		{
			CDroppedPickup *pExisting = static_cast<CDroppedPickup *>(lDrops[i]);
			if(pExisting && pExisting->IsHealthDrop())
				return -1;
		}
		CDroppedPickup *pDrop = new CDroppedPickup(pWorld, vec2(X, Y),
			vec2((float) (random_int() % 3) - 1.0f, 1.0f), 1);
		return pDrop->GetID();
	});

	m.def("drop_item", [](float X, float Y, const std::string &ItemId, int Count) -> bool {
		CGameWorld *pWorld = ActiveWorldOrMain();
		if(!pWorld || !ValidPos(X, Y) || Count <= 0)
			return false;
		CItemID Id;
		if(!CItemID::Parse(ItemId.c_str(), &Id) || Id.IsZero())
			return false;
		new CDroppedPickup(pWorld, vec2(X, Y), vec2(0.0f, 0.0f), Id, Count);
		return true;
	});

	m.def("owner_near", [](int EntityID) -> int {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		return pEntity ? pEntity->OwnerNear() : -1;
	});

	m.def("player_near", [](int EntityID, float Radius) -> int {
		CScriptedEntity *pEntity = FindScriptedEntity(EntityID);
		if(!pEntity || Radius <= 0.0f)
			return -1;
		CCharacter *pChr = static_cast<CCharacter *>(pEntity->GameWorld()->ClosestEntity(
			pEntity->GetPos(), Radius, CGameWorld::ENTTYPE_CHARACTER, nullptr, true));
		return pChr && pChr->IsAlive() ? pChr->GetCID() : -1;
	});
}
