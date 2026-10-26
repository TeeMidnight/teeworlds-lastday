/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/scripting/script_runtime.h>

#include <base/system.h>
#include <engine/storage.h>

#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>

#include <base/tl/array.h>

#include <pocketpy.h>
#include <pybind11/embed.h>

#include <string>

namespace py = pybind11;

namespace
{
// Installs the callback's world for the duration of one Python call, including
// nested host calls and every early return of CScriptRuntime::Call.
class CActiveWorldScope
{
public:
	CActiveWorldScope(CScriptRuntime *pRuntime, CGameWorld *pWorld) :
		m_pRuntime(pRuntime), m_pPrev(nullptr), m_Set(pWorld != nullptr)
	{
		if(m_Set)
			m_pPrev = m_pRuntime->SetActiveWorld(pWorld);
	}
	~CActiveWorldScope()
	{
		if(m_Set)
			m_pRuntime->SetActiveWorld(m_pPrev);
	}

private:
	CScriptRuntime *m_pRuntime;
	CGameWorld *m_pPrev;
	bool m_Set;
};

struct SWatchdog
{
	SWatchdog(int TimeoutMs) { py_watchdog_begin(TimeoutMs); }
	~SWatchdog() { py_watchdog_end(); }
};

py::tuple Vec2Tuple(float x, float y)
{
	py::tuple Result(2);
	Result[0] = py::cast(x);
	Result[1] = py::cast(y);
	return Result;
}

bool ToBool(py::handle Value)
{
	// PocketPy's pybind11 subset only casts a real bool to bool. Scripts often
	// write ctx["consumed"] = 1, so accept integer truthiness as well.
	try
	{
		return py::cast<bool>(Value);
	}
	catch(const std::exception &)
	{
	}
	try
	{
		return py::cast<int>(Value) != 0;
	}
	catch(const std::exception &)
	{
	}
	return false;
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
}

py::object JsonLoads(const std::string &Text)
{
	try
	{
		py::object Json = py::module_::import("json");
		return Json.attr("loads")(Text);
	}
	catch(const py::python_error &)
	{
		return py::dict();
	}
}
} // namespace

class CScriptRuntime::CImpl
{
public:
	struct SModule
	{
		std::string m_Path;
		py::module_ m_Module;
		bool m_Loaded = false;
	};

	py::scoped_interpreter *m_pInterpreter = nullptr;
	// The project uses tl::array instead of std::vector; modules are owned
	// manually and released before the interpreter is finalized.
	array<SModule *> m_Modules;
	// module handle for data/scripts/_context.py (the class-based context layer)
	int m_ContextModule = -1;
	// world of the callback that is currently executing (see ActiveWorld())
	CGameWorld *m_pActiveWorld = nullptr;
	std::string m_LastError;
};

CScriptRuntime *CScriptRuntime::Get()
{
	static CScriptRuntime s_Runtime;
	return &s_Runtime;
}

bool CScriptRuntime::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	if(m_pImpl)
		return true;

	m_pGameServer = pGameServer;
	m_pStorage = pStorage;

	m_pImpl = new CImpl();
	try
	{
		// scoped_interpreter registers the embedded modules and owns the VM
		// lifetime. All script modules are destroyed before it is released.
		m_pImpl->m_pInterpreter = new py::scoped_interpreter();
	}
	catch(const std::exception &e)
	{
		dbg_msg("script", "pocketpy init failed: %s", e.what());
		delete m_pImpl;
		m_pImpl = nullptr;
		return false;
	}

	// The context layer defines Player/Entity/Item/Buff/Vec2/Context. It is
	// loaded before any item/buff/weapon module so every callback receives a
	// class instance instead of a bare dict.
	m_pImpl->m_ContextModule = LoadModule("scripts/_context.py");
	if(m_pImpl->m_ContextModule < 0)
		dbg_msg("script", "failed to load scripts/_context.py; script callbacks will not run");

	dbg_msg("script", "pocketpy runtime ready (timeout=%dms)", m_TimeoutMs);
	return true;
}

void CScriptRuntime::Shutdown()
{
	if(!m_pImpl)
		return;

	// Python objects must die before the interpreter. Clear modules first.
	for(int i = 0; i < m_pImpl->m_Modules.size(); i++)
		delete m_pImpl->m_Modules[i];
	m_pImpl->m_Modules.clear();
	delete m_pImpl->m_pInterpreter;
	m_pImpl->m_pInterpreter = nullptr;
	delete m_pImpl;
	m_pImpl = nullptr;
	m_pGameServer = nullptr;
	m_pStorage = nullptr;
}

bool CScriptRuntime::Ready() const
{
	return m_pImpl && m_pImpl->m_pInterpreter;
}

int CScriptRuntime::LoadModule(const char *pPath)
{
	if(!Ready() || !pPath || !pPath[0] || !m_pStorage)
		return -1;

	// Reuse the handle for an already loaded path. Re-executing into the same
	// module replaces its functions atomically enough for the server tick.
	int Index = -1;
	for(int i = 0; i < (int) m_pImpl->m_Modules.size(); i++)
	{
		if(m_pImpl->m_Modules[i]->m_Path == pPath)
		{
			Index = i;
			break;
		}
	}

	void *pData = nullptr;
	unsigned DataSize = 0;
	if(!m_pStorage->ReadFile(pPath, IStorage::TYPE_ALL, &pData, &DataSize) || !pData)
	{
		m_pImpl->m_LastError = "could not read script: ";
		m_pImpl->m_LastError += pPath;
		dbg_msg("script", "%s", m_pImpl->m_LastError.c_str());
		return -1;
	}
	std::string Source(static_cast<const char *>(pData), DataSize);
	free(pData);

	try
	{
		if(Index < 0)
		{
			CImpl::SModule *pModule = new CImpl::SModule();
			pModule->m_Path = pPath;
			pModule->m_Module = py::module_::create(("lastday_script_" + std::to_string(m_pImpl->m_Modules.size())).c_str());
			m_pImpl->m_Modules.add(pModule);
			Index = m_pImpl->m_Modules.size() - 1;
		}

		CImpl::SModule &Module = *m_pImpl->m_Modules[Index];
		py::exec(Source, Module.m_Module.attr("__dict__"));
		Module.m_Loaded = true;
		m_pImpl->m_LastError.clear();
		dbg_msg("script", "loaded '%s'", pPath);
		return Index;
	}
	catch(const py::python_error &e)
	{
		m_pImpl->m_LastError = e.what();
		dbg_msg("script", "failed to load '%s':\n%s", pPath, e.what());
		return -1;
	}
	catch(const std::exception &e)
	{
		m_pImpl->m_LastError = e.what();
		dbg_msg("script", "failed to load '%s': %s", pPath, e.what());
		return -1;
	}
}

bool CScriptRuntime::ReloadAll()
{
	bool Ok = true;
	for(int i = 0; i < (int) m_pImpl->m_Modules.size(); i++)
	{
		const std::string Path = m_pImpl->m_Modules[i]->m_Path;
		if(LoadModule(Path.c_str()) < 0)
			Ok = false;
	}
	return Ok;
}

int CScriptRuntime::ModuleCount() const
{
	return m_pImpl ? (int) m_pImpl->m_Modules.size() : 0;
}

const char *CScriptRuntime::ModulePath(int Module) const
{
	if(!m_pImpl || Module < 0 || Module >= (int) m_pImpl->m_Modules.size())
		return "";
	return m_pImpl->m_Modules[Module]->m_Path.c_str();
}

CGameWorld *CScriptRuntime::ActiveWorld() const
{
	if(m_pImpl && m_pImpl->m_pActiveWorld)
		return m_pImpl->m_pActiveWorld;

	CGameContext *pGame = m_pGameServer;
	if(!pGame)
		return nullptr;

	CGameWorld *pWorld = pGame->MainWorld();
	if(pWorld)
		return pWorld;

	// Before the main world exists (or for a fresh server) use any loaded world
	// so console driven calls still have a sane default.
	CGameWorld *pFirst = nullptr;
	pGame->m_pWorlds.for_each([](CGameWorld *&pEntry, void *pUser) -> void
	{
		CGameWorld **ppOut = static_cast<CGameWorld **>(pUser);
		if(!*ppOut && pEntry)
			*ppOut = pEntry;
	}, &pFirst);
	return pFirst;
}

CGameWorld *CScriptRuntime::SetActiveWorld(CGameWorld *pWorld)
{
	if(!m_pImpl)
		return nullptr;
	CGameWorld *pPrev = m_pImpl->m_pActiveWorld;
	m_pImpl->m_pActiveWorld = pWorld;
	return pPrev;
}

bool CScriptRuntime::HasFunction(int Module, const char *pFunction) const
{
	if(!Ready() || !pFunction || Module < 0 || Module >= (int) m_pImpl->m_Modules.size())
		return false;
	const CImpl::SModule &M = *m_pImpl->m_Modules[Module];
	return M.m_Loaded && py::hasattr(M.m_Module, pFunction);
}

bool CScriptRuntime::Call(int Module, const char *pFunction, SScriptContext &Ctx)
{
	if(!m_Enabled)
		return true;

	if(!Ready() || !pFunction || Module < 0 || Module >= (int) m_pImpl->m_Modules.size())
		return false;

	CImpl::SModule &M = *m_pImpl->m_Modules[Module];
	if(!M.m_Loaded || !py::hasattr(M.m_Module, pFunction))
	{
		// Optional callbacks are not an error. The caller can use HasFunction
		// when it needs to know whether the script actually implemented one.
		return true;
	}

	try
	{
		CGameWorld *pCallbackWorld = m_pGameServer && Ctx.m_WorldID ? m_pGameServer->WorldByID(Ctx.m_WorldID) : nullptr;
		CActiveWorldScope WorldScope(this, pCallbackWorld);

		py::dict PyCtx;
		PyCtx["event"] = (const char *) Ctx.m_aEvent;
		PyCtx["player"] = Ctx.m_ClientID;
		PyCtx["entity"] = Ctx.m_EntityID;
		PyCtx["world"] = Ctx.m_WorldID;
		PyCtx["item"] = Ctx.m_ItemId.SourceName() ? Ctx.m_ItemId.SourceName() : "";
		PyCtx["buff"] = (const char *) Ctx.m_aBuff;
		PyCtx["weapon"] = (const char *) Ctx.m_aWeapon;
		PyCtx["kind"] = (const char *) Ctx.m_aKind;
		PyCtx["pos"] = Vec2Tuple(Ctx.m_Pos.x, Ctx.m_Pos.y);
		PyCtx["dir"] = Vec2Tuple(Ctx.m_Dir.x, Ctx.m_Dir.y);
		PyCtx["now"] = Ctx.m_Now;
		PyCtx["tick_speed"] = Ctx.m_TickSpeed;
		PyCtx["range_scale"] = Ctx.m_RangeScale;
		PyCtx["params"] = JsonLoads(Ctx.m_Params);
		PyCtx["state"] = JsonLoads(Ctx.m_State);
		PyCtx["extra"] = JsonLoads(Ctx.m_Extra);

		py::dict Ammo;
		Ammo["damage"] = Ctx.m_AmmoDamage;
		Ammo["projectiles"] = Ctx.m_AmmoProjectiles;
		Ammo["speed"] = Ctx.m_AmmoSpeed;
		Ammo["lifetime"] = Ctx.m_AmmoLifetime;
		Ammo["effect"] = Ctx.m_AmmoEffect;
		PyCtx["ammo"] = Ammo;

		PyCtx["result"] = py::dict();
		PyCtx["consumed"] = Ctx.m_Consumed;
		PyCtx["reload_ticks"] = Ctx.m_ReloadTicks;
		PyCtx["destroy"] = Ctx.m_Destroy;

		if(m_pImpl->m_ContextModule < 0 || m_pImpl->m_ContextModule >= m_pImpl->m_Modules.size())
			return false;

		CImpl::SModule &ContextModule = *m_pImpl->m_Modules[m_pImpl->m_ContextModule];
		py::object Factory = py::getattr(ContextModule.m_Module, "make_context");
		py::object CtxObject = Factory(PyCtx);

		SWatchdog Watchdog(m_TimeoutMs);
		py::object Fn = py::getattr(M.m_Module, pFunction);
		py::object Ret = Fn(CtxObject);
		(void) Ret; // handlers may return a value; the context object is authoritative

		Ctx.m_State = JsonDumps(CtxObject.attr("state"));
		Ctx.m_Result = JsonDumps(CtxObject.attr("result"));
		Ctx.m_Consumed = ToBool(CtxObject.attr("consumed"));
		Ctx.m_ReloadTicks = py::cast<int>(CtxObject.attr("reload_ticks"));
		Ctx.m_Destroy = ToBool(CtxObject.attr("destroy"));
		m_pImpl->m_LastError.clear();
		return true;
	}
	catch(const py::python_error &e)
	{
		m_pImpl->m_LastError = e.what();
		dbg_msg("script", "error in %s:%s\n%s", M.m_Path.c_str(), pFunction, e.what());
		return false;
	}
	catch(const std::exception &e)
	{
		m_pImpl->m_LastError = e.what();
		dbg_msg("script", "error in %s:%s: %s", M.m_Path.c_str(), pFunction, e.what());
		return false;
	}
}

const char *CScriptRuntime::LastError() const
{
	return m_pImpl ? m_pImpl->m_LastError.c_str() : "script runtime not initialized";
}
