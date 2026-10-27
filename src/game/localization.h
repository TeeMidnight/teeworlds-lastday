/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_LOCALIZATION_H
#define GAME_LOCALIZATION_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <engine/shared/memheap.h>

class CLocalizationDatabase
{
	class CString
	{
	public:
		unsigned m_Hash;
		unsigned m_ContextHash;
		int m_Language;

		bool operator<(const CString &Other) const
		{
			if(m_Hash != Other.m_Hash)
				return m_Hash < Other.m_Hash;
			if(m_ContextHash != Other.m_ContextHash)
				return m_ContextHash < Other.m_ContextHash;
			return m_Language < Other.m_Language;
		}
		bool operator<=(const CString &Other) const
		{
			return *this < Other || *this == Other;
		}
		bool operator==(const CString &Other) const
		{
			return m_Hash == Other.m_Hash && m_ContextHash == Other.m_ContextHash && m_Language == Other.m_Language;
		}
	};

	class CStringHashFunction : public CString
	{
	public:
		static unsigned hash(CString Key) { return Key.m_Hash ^ ((unsigned) Key.m_Language * 2654435761u); }
		static bool equal(CString Key1, CString Key2) { return Key1 == Key2; }
	};

	struct SLanguage
	{
		char m_aCode[16];
		char m_aName[64];
	};

	hash_table<CString, const char *, 64, CStringHashFunction> m_Strings;
	CHeap m_StringsHeap;
	int m_VersionCounter;
	int m_CurrentVersion;
	int m_DefaultLanguage;
	int m_ActiveLanguage;
	array<SLanguage> m_aLanguages;

	const char *FindStringExact(int Language, unsigned Hash, unsigned ContextHash) const;

public:
	CLocalizationDatabase();

	// remove every language and translation
	void Clear();
	// reload the default language and every other file in languages/
	bool LoadAll(const char *pDefaultFilename, class IStorage *pStorage, class IConsole *pConsole, bool Server);
	// load one language file without clearing the others; the language must
	// have been added with AddLanguage first
	bool LoadLanguage(int Language, const char *pFilename, class IStorage *pStorage, class IConsole *pConsole, bool Server);
	// backwards compatible entry point: reload everything with pFilename as
	// the default language
	bool Load(const char *pFilename, class IStorage *pStorage, class IConsole *pConsole, bool Server);

	int Version() const { return m_CurrentVersion; }

	int NumLanguages() const { return m_aLanguages.size(); }
	int DefaultLanguage() const { return m_DefaultLanguage >= 0 ? m_DefaultLanguage : 0; }
	int ActiveLanguage() const { return m_ActiveLanguage; }
	void SetActiveLanguage(int Language);
	void SetDefaultLanguage(int Language);
	int FindLanguage(const char *pCode) const;
	const char *LanguageCode(int Language) const;
	const char *LanguageName(int Language) const;
	int AddLanguage(const char *pCode, const char *pName);

	void AddString(int Language, const char *pOrgStr, const char *pNewStr, const char *pContext);
	void AddString(const char *pOrgStr, const char *pNewStr, const char *pContext);
	const char *FindString(int Language, unsigned Hash, unsigned ContextHash) const;
	const char *FindString(unsigned Hash, unsigned ContextHash) const;
};

extern CLocalizationDatabase g_Localization;

// Temporarily selects the language used by Localize()/LocalizeString()/
// LocalizeFormat(). Nested scopes restore the previous language.
class CLanguageScope
{
	int m_Previous;

public:
	explicit CLanguageScope(int Language) :
		m_Previous(g_Localization.ActiveLanguage())
	{
		g_Localization.SetActiveLanguage(Language);
	}
	~CLanguageScope()
	{
		g_Localization.SetActiveLanguage(m_Previous);
	}
	CLanguageScope(const CLanguageScope &) = delete;
	CLanguageScope &operator=(const CLanguageScope &) = delete;
};

class CLocConstString
{
	const char *m_pDefaultStr;
	const char *m_pCurrentStr;
	unsigned m_Hash;
	unsigned m_ContextHash;
	int m_Version;
	int m_Language;

public:
	CLocConstString(const char *pStr, const char *pContext = "");
	void Reload();

	inline operator const char *()
	{
		if(m_Version != g_Localization.Version() || m_Language != g_Localization.ActiveLanguage())
			Reload();
		return m_pCurrentStr;
	}
};

#endif
