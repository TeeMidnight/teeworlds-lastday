#ifndef GAME_SERVER_DATABASE_JSONPATH_H
#define GAME_SERVER_DATABASE_JSONPATH_H

#include <base/system.h>

// A fluent builder for a dot-notation json path, e.g.
//   CJsonPath().Key("inventory").Index(0).Key("count")  ->  "inventory.0.count"
// The built path is passed to CPlayerDB::GetJson / SetJson / DelJson.
class CJsonPath
{
	char m_aPath[128];
	int m_Length;

	void Append(const char *pSegment)
	{
		if(m_Length > 0)
			m_aPath[m_Length++] = '.';
		const int Len = str_length(pSegment);
		mem_copy(m_aPath + m_Length, pSegment, Len);
		m_Length += Len;
		m_aPath[m_Length] = '\0';
	}

public:
	CJsonPath() : m_Length(0)
	{
		m_aPath[0] = '\0';
	}

	CJsonPath &Key(const char *pKey)
	{
		Append(pKey);
		return *this;
	}

	CJsonPath &Index(int Index)
	{
		char aBuf[16];
		str_format(aBuf, sizeof(aBuf), "%d", Index);
		Append(aBuf);
		return *this;
	}

	const char *c_str() const { return m_aPath; }
	operator const char *() const { return m_aPath; }
};

#endif
