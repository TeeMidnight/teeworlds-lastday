#ifndef GAME_SERVER_DATABASE_DATABASE_UTIL_H
#define GAME_SERVER_DATABASE_DATABASE_UTIL_H

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
//   SetJsonField(pDB, uuid, CJsonPath().Key("sanity"), 100);
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

#endif
