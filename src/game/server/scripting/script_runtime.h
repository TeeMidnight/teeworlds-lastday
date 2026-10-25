/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_SCRIPTING_SCRIPT_RUNTIME_H
#define GAME_SERVER_SCRIPTING_SCRIPT_RUNTIME_H

#include <base/system.h>
#include <base/vmath.h>

#include <game/item_id.h>

#include <string>

class CGameContext;
class IStorage;

// One callback context. The scripting layer copies the plain fields into a
// Python dict, calls the module function, then reads back state/result. This
// keeps pybind11 types out of the game headers and makes every call
// serializable for logging and save games.
struct SScriptContext
{
	CGameContext *m_pGameServer = nullptr;
	int m_ClientID = -1;
	int m_EntityID = -1;
	unsigned m_WorldID = 0;
	CItemID m_ItemId;
	char m_aEvent[64] = "";
	char m_aBuff[64] = "";
	char m_aWeapon[64] = "";
	char m_aKind[64] = "";
	vec2 m_Pos = vec2(0.0f, 0.0f);
	vec2 m_Dir = vec2(0.0f, 0.0f);
	int m_Now = 0;
	int m_TickSpeed = 50;
	float m_RangeScale = 1.0f;

	// ammo / weapon values (0 means "not supplied")
	int m_AmmoDamage = 0;
	int m_AmmoProjectiles = 0;
	float m_AmmoSpeed = 1.0f;
	float m_AmmoLifetime = 1.0f;
	int m_AmmoEffect = 0;

	// arbitrary JSON object handed to the script as ctx["extra"]
	std::string m_Extra = "{}";
	// JSON object handed to the script as ctx["params"]
	std::string m_Params = "{}";
	// JSON object handed to the script as ctx["state"]; the script may mutate it
	std::string m_State = "{}";
	// JSON object written back by the script as ctx["result"]
	std::string m_Result = "{}";

	bool m_Consumed = false;
	int m_ReloadTicks = 0;
	bool m_Destroy = false;
};

// The process-wide PocketPy VM. PocketPy's C++ binding layer keeps process
// state, so this is initialized once at server start and lives until shutdown.
class CScriptRuntime
{
public:
	static CScriptRuntime *Get();

	bool Init(CGameContext *pGameServer, IStorage *pStorage);
	void Shutdown();
	bool Ready() const;

	// Loads a .py file into a fresh module. Returns a handle >= 0, or -1.
	// Loading the same path again replaces the module in place, keeping the
	// handle valid for callers that cached it.
	int LoadModule(const char *pPath);
	bool ReloadAll();

	int ModuleCount() const;
	const char *ModulePath(int Module) const;
	bool HasFunction(int Module, const char *pFunction) const;

	// Calls Module.Function(ctx). Returns false on a Python error or timeout;
	// LastError() then contains a traceback.
	bool Call(int Module, const char *pFunction, SScriptContext &Ctx);

	const char *LastError() const;
	bool Enabled() const { return m_Enabled; }
	void SetEnabled(bool Enabled) { m_Enabled = Enabled; }

	CGameContext *GameServer() const { return m_pGameServer; }
	IStorage *Storage() const { return m_pStorage; }

	int TimeoutMs() const { return m_TimeoutMs; }
	void SetTimeoutMs(int TimeoutMs) { m_TimeoutMs = TimeoutMs; }

private:
	class CImpl;
	CImpl *m_pImpl = nullptr;
	CGameContext *m_pGameServer = nullptr;
	IStorage *m_pStorage = nullptr;
	bool m_Enabled = true;
	int m_TimeoutMs = 20;
};

#endif
