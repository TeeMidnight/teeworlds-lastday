/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_DATABASE_DATABASE_UTIL_H
#define GAME_SERVER_DATABASE_DATABASE_UTIL_H

#include <base/tl/array.h>
#include <engine/shared/jobs.h>
#include <game/server/database/database.h>
#include <game/server/database/jsonpath.h>

// serialize a value to a JSON literal
inline void JsonValueToString(char *pOut, int Size, int Value)
{
	str_format(pOut, Size, "%d", Value);
}

inline void JsonValueToString(char *pOut, int Size, bool Value)
{
	str_copy(pOut, Value ? "true" : "false", Size);
}

inline void JsonValueToString(char *pOut, int Size, float Value)
{
	str_format(pOut, Size, "%g", Value);
}

inline void JsonValueToString(char *pOut, int Size, const char *pValue)
{
	// JSON string literal: quote and escape
	int Offset = 0;
	if(Offset + 1 < Size)
		pOut[Offset++] = '"';
	for(const char *p = pValue; *p && Offset + 2 < Size; p++)
	{
		if(*p == '"' || *p == '\\')
		{
			pOut[Offset++] = '\\';
			pOut[Offset++] = *p;
		}
		else
		{
			pOut[Offset++] = *p;
		}
	}
	if(Offset + 1 < Size)
		pOut[Offset++] = '"';
	pOut[Offset] = '\0';
}

// store a value at the given json path of a player's data column. The value
// is serialized to a JSON literal automatically, so callers just pass the
// path and the value:
//   SetJsonField(pDB, uuid, CJsonPath().Key("forgetting"), 0);
template<typename T>
void SetJsonField(CDatabase *pDB, const Uuid &uuid, const CJsonPath &path, const T &value)
{
	char aBuf[256];
	JsonValueToString(aBuf, sizeof(aBuf), value);
	pDB->SetJson(uuid, path, aBuf);
}

// deserialize a JSON literal into a value. note: SQLite's json_extract
// returns booleans as "1"/"0" while PostgreSQL returns "true"/"false", so the
// bool case accepts both.
inline void JsonValueFromString(const char *pStr, int &Value) { Value = str_toint(pStr); }
inline void JsonValueFromString(const char *pStr, bool &Value) { Value = str_comp(pStr, "true") == 0 || str_comp(pStr, "1") == 0; }
inline void JsonValueFromString(const char *pStr, float &Value) { Value = str_tofloat(pStr); }

// read a value at the given json path of a player's data column and write it
// into `*pValue`. returns true when the field exists:
//   bool HideTip;
//   if(GetJsonField(pDB, uuid, CJsonPath().Key("hide_tip"), &HideTip)) ...
template<typename T>
bool GetJsonField(CDatabase *pDB, const Uuid &uuid, const CJsonPath &path, T *pValue)
{
	char aBuf[256];
	if(!pDB->GetJson(uuid, path, aBuf, sizeof(aBuf)))
		return false;
	JsonValueFromString(aBuf, *pValue);
	return true;
}

// read a raw json value (e.g. a whole object) into a buffer
inline bool GetJsonFieldRaw(CDatabase *pDB, const Uuid &uuid, const CJsonPath &path, char *pOut, int Size)
{
	return pDB->GetJson(uuid, path, pOut, Size);
}

// ---------------------------------------------------------------
// asynchronous writes
//
// The game thread fills a CDbJob with an immutable snapshot of what should be
// written, then hands it to the thread pool. The job never touches live game
// state, so a world or player can change (or be destroyed) while its write is
// still queued. Reads stay synchronous: their result is needed before the
// caller can continue.
// ---------------------------------------------------------------
struct CDbField
{
	CJsonPath m_Path;
	char m_aValue[128];
};

struct CDbJob
{
	enum
	{
		TYPE_PLAYER,
		TYPE_WORLD,
	};

	CJob m_Job;
	bool m_Dispatched;
	int m_Type;
	CDatabase *m_pDB;
	Uuid m_Uuid;

	// player writes: fields to set, then fields to remove
	array<CDbField> m_lSet;
	array<CDbField> m_lDel;

	// world write: the whole row is replaced
	char m_aMapName[64];
	array<char> m_Json;
	bool m_HasJson;

	CDbJob() :
		m_Dispatched(false),
		m_Type(TYPE_PLAYER),
		m_pDB(0),
		m_HasJson(false)
	{
		m_Uuid = UUID_ZEROED;
		m_aMapName[0] = '\0';
	}
};

template<typename T>
inline void DbSet(CDbJob &Job, const CJsonPath &Path, const T &Value)
{
	CDbField Field;
	Field.m_Path = Path;
	JsonValueToString(Field.m_aValue, sizeof(Field.m_aValue), Value);
	Job.m_lSet.add(Field);
}

inline void DbDel(CDbJob &Job, const CJsonPath &Path)
{
	CDbField Field;
	Field.m_Path = Path;
	Field.m_aValue[0] = '\0';
	Job.m_lDel.add(Field);
}

// Executes a queued write; a free function so it can be given to the pool.
inline int RunDbJob(void *pData)
{
	CDbJob *pJob = static_cast<CDbJob *>(pData);
	if(!pJob->m_pDB)
		return 0;

	if(pJob->m_Type == CDbJob::TYPE_WORLD)
	{
		// Saving replaces the whole document, so only an empty world needs a
		// delete; doing both would open a crash window with no row at all.
		if(pJob->m_HasJson)
			pJob->m_pDB->SaveWorldSave(pJob->m_aMapName, pJob->m_Json.base_ptr());
		else
			pJob->m_pDB->DeleteWorldSave(pJob->m_aMapName);
		return 0;
	}

	for(int i = 0; i < pJob->m_lDel.size(); i++)
		pJob->m_pDB->DelJson(pJob->m_Uuid, pJob->m_lDel[i].m_Path);
	for(int i = 0; i < pJob->m_lSet.size(); i++)
		pJob->m_pDB->SetJson(pJob->m_Uuid, pJob->m_lSet[i].m_Path, pJob->m_lSet[i].m_aValue);
	return 0;
}

#endif
