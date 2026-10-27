/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */

#include <base/system.h>
#include <base/tl/algorithm.h>
#include <engine/console.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include "localization.h"

#include <string>

static void LanguageCodeFromFilename(const char *pFilename, char *pOut, int OutSize)
{
	pOut[0] = '\0';
	if(!pFilename)
		return;
	const char *pBase = pFilename;
	for(const char *p = pFilename; *p; ++p)
	{
		if(*p == '/' || *p == '\\')
			pBase = p + 1;
	}
	str_copy(pOut, pBase, OutSize);
	const int Len = str_length(pOut);
	if(Len > 5 && str_comp_nocase(pOut + Len - 5, ".json") == 0)
		pOut[Len - 5] = '\0';
}

struct SListLanguagesContext
{
	array<std::string> *m_pFiles;
};

static int ListLanguageFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	if(IsDir || !pFilename)
		return 0;
	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp_nocase(pFilename + Len - 5, ".json") != 0)
		return 0;
	if(str_comp_nocase(pFilename, "index.json") == 0)
		return 0;
	SListLanguagesContext *pContext = static_cast<SListLanguagesContext *>(pUser);
	pContext->m_pFiles->add(pFilename);
	return 0;
}

CLocalizationDatabase::CLocalizationDatabase()
{
	m_VersionCounter = 0;
	m_CurrentVersion = 0;
	m_DefaultLanguage = -1;
	m_ActiveLanguage = 0;
}

void CLocalizationDatabase::Clear()
{
	m_Strings.clear();
	m_StringsHeap.Reset();
	m_aLanguages.clear();
	m_DefaultLanguage = -1;
	m_ActiveLanguage = 0;
	m_CurrentVersion = ++m_VersionCounter;
}

int CLocalizationDatabase::AddLanguage(const char *pCode, const char *pName)
{
	char aCode[16];
	LanguageCodeFromFilename(pCode, aCode, sizeof(aCode));
	if(!aCode[0])
		return -1;

	const int Existing = FindLanguage(aCode);
	if(Existing >= 0)
		return Existing;

	SLanguage Language;
	str_copy(Language.m_aCode, aCode, sizeof(Language.m_aCode));
	str_copy(Language.m_aName, pName && pName[0] ? pName : aCode, sizeof(Language.m_aName));
	m_aLanguages.add(Language);
	return m_aLanguages.size() - 1;
}

int CLocalizationDatabase::FindLanguage(const char *pCode) const
{
	char aCode[16];
	LanguageCodeFromFilename(pCode, aCode, sizeof(aCode));
	if(!aCode[0])
		return -1;
	for(int i = 0; i < m_aLanguages.size(); i++)
	{
		if(str_comp_nocase(m_aLanguages[i].m_aCode, aCode) == 0)
			return i;
	}
	return -1;
}

const char *CLocalizationDatabase::LanguageCode(int Language) const
{
	if(Language < 0 || Language >= m_aLanguages.size())
		return "";
	return m_aLanguages[Language].m_aCode;
}

const char *CLocalizationDatabase::LanguageName(int Language) const
{
	if(Language < 0 || Language >= m_aLanguages.size())
		return "";
	return m_aLanguages[Language].m_aName;
}

void CLocalizationDatabase::SetDefaultLanguage(int Language)
{
	if(Language < 0 || Language >= m_aLanguages.size())
		Language = m_aLanguages.size() > 0 ? 0 : -1;
	m_DefaultLanguage = Language;
}

void CLocalizationDatabase::SetActiveLanguage(int Language)
{
	if(Language < 0 || Language >= m_aLanguages.size())
		Language = DefaultLanguage();
	m_ActiveLanguage = Language;
}

void CLocalizationDatabase::AddString(int Language, const char *pOrgStr, const char *pNewStr, const char *pContext)
{
	CString s;
	s.m_Hash = str_quickhash(pOrgStr);
	s.m_ContextHash = str_quickhash(pContext);
	s.m_Language = Language;
	m_Strings.set(s, m_StringsHeap.StoreString(*pNewStr ? pNewStr : pOrgStr));
}

void CLocalizationDatabase::AddString(const char *pOrgStr, const char *pNewStr, const char *pContext)
{
	AddString(DefaultLanguage(), pOrgStr, pNewStr, pContext);
}

bool CLocalizationDatabase::LoadLanguage(int Language, const char *pFilename, IStorage *pStorage, IConsole *pConsole, bool Server)
{
	if(Language < 0 || Language >= m_aLanguages.size() || !pFilename || !pFilename[0] || !pStorage)
		return false;

	CJsonParser JsonParser;
	const json_value *pJsonData = JsonParser.ParseFile(pFilename, pStorage);
	if(pJsonData == 0)
	{
		if(pConsole)
			pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "localization", JsonParser.Error());
		return false;
	}

	// an optional display name (the menu shows it instead of the file code)
	const json_value &rName = (*pJsonData)["name"];
	if(rName.type == json_string)
		str_copy(m_aLanguages[Language].m_aName, (const char *) rName, sizeof(m_aLanguages[Language].m_aName));

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "loaded '%s' as '%s'", pFilename, m_aLanguages[Language].m_aCode);
	if(pConsole)
		pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "localization", aBuf);

	// extract data
	auto LoadSection = [&](const char *pSection)
	{
		const json_value &rStart = (*pJsonData)[pSection];
		if(rStart.type != json_array)
			return;
		for(unsigned i = 0; i < rStart.u.array.length; ++i)
		{
			bool Valid = true;
			const char *pOr = (const char *) rStart[i]["or"];
			const char *pTr = (const char *) rStart[i]["tr"];
			char aInfo[256];
			while(pOr[0] && pTr[0])
			{
				for(; pOr[0] && pOr[0] != '%'; ++pOr)
					;
				for(; pTr[0] && pTr[0] != '%'; ++pTr)
					;
				if(pOr[0] && pTr[0] && ((pOr[1] == ' ' && pTr[1] == 0) || (pOr[1] == 0 && pTr[1] == ' '))) // skip  false positive
					break;
				if((pOr[0] && (!pTr[0] || pOr[1] != pTr[1])) || (pTr[0] && (!pOr[0] || pTr[1] != pOr[1])))
				{
					Valid = false;
					str_format(aInfo, sizeof(aInfo), "skipping invalid entry or:'%s', tr:'%s'", (const char *) rStart[i]["or"], (const char *) rStart[i]["tr"]);
					if(pConsole)
						pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "localization", aInfo);
					break;
				}
				if(pOr[0])
					++pOr;
				if(pTr[0])
					++pTr;
			}
			if(Valid)
				AddString(Language, (const char *) rStart[i]["or"], (const char *) rStart[i]["tr"], (const char *) rStart[i]["context"]);
		}
	};

	if(Server)
		LoadSection("server strings");
	else
		LoadSection("client strings");
	// item strings carry the item names/descriptions/types and are loaded on both
	LoadSection("item strings");

	return true;
}

bool CLocalizationDatabase::LoadAll(const char *pDefaultFilename, IStorage *pStorage, IConsole *pConsole, bool Server)
{
	Clear();

	bool DefaultLoaded = false;
	if(pDefaultFilename && pDefaultFilename[0])
	{
		char aCode[16];
		LanguageCodeFromFilename(pDefaultFilename, aCode, sizeof(aCode));
		if(aCode[0])
		{
			const int Language = AddLanguage(aCode, aCode);
			m_DefaultLanguage = Language;
			DefaultLoaded = LoadLanguage(Language, pDefaultFilename, pStorage, pConsole, Server);
			if(!DefaultLoaded)
			{
				// do not keep a placeholder for a missing/broken sv_languagefile
				// in the selectable list
				m_aLanguages.remove_index(Language);
				m_DefaultLanguage = -1;
			}
		}
	}

	if(!DefaultLoaded)
		m_DefaultLanguage = -1;

	// every JSON file in languages/ becomes a selectable language. The default
	// file above is skipped when it loaded successfully; if it did not, the scan
	// gives it a second chance.
	array<std::string> Files;
	if(pStorage)
	{
		SListLanguagesContext Context = {&Files};
		pStorage->ListDirectory(IStorage::TYPE_ALL, "languages", ListLanguageFilesCallback, &Context);
	}
	sort(Files.all());
	for(const std::string &File : Files)
	{
		char aCode[16];
		LanguageCodeFromFilename(File.c_str(), aCode, sizeof(aCode));
		if(!aCode[0])
			continue;
		const int LanguageCount = m_aLanguages.size();
		const int Language = AddLanguage(aCode, aCode);
		if(Language == m_DefaultLanguage && DefaultLoaded)
			continue;
		const std::string Path = "languages/" + File;
		if(!LoadLanguage(Language, Path.c_str(), pStorage, pConsole, Server))
		{
			// drop a broken file instead of offering an empty language
			if(Language == LanguageCount)
				m_aLanguages.remove_index(Language);
			continue;
		}
	}

	// English is the source language: when it has no file, every lookup falls
	// back to the original string, which is exactly what an empty database does.
	int English = FindLanguage("en");
	if(English < 0)
		English = AddLanguage("en", "English");
	if(m_DefaultLanguage < 0)
		m_DefaultLanguage = English;

	m_ActiveLanguage = m_DefaultLanguage;
	m_CurrentVersion = ++m_VersionCounter;

	if(pConsole)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "default '%s', %d language(s) available",
			LanguageCode(m_DefaultLanguage), m_aLanguages.size());
		pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "localization", aBuf);
	}
	return true;
}

bool CLocalizationDatabase::Load(const char *pFilename, IStorage *pStorage, IConsole *pConsole, bool Server)
{
	return LoadAll(pFilename, pStorage, pConsole, Server);
}

const char *CLocalizationDatabase::FindStringExact(int Language, unsigned Hash, unsigned ContextHash) const
{
	if(Language < 0 || Language >= m_aLanguages.size())
		return 0;
	static unsigned DefaultHash = str_quickhash("");

	CString String;
	String.m_Hash = Hash;
	String.m_ContextHash = ContextHash;
	String.m_Language = Language;

	const char *const *ppReplacement = m_Strings[String];
	if(!ppReplacement)
	{
		String.m_ContextHash = DefaultHash;
		ppReplacement = m_Strings[String];
	}
	return ppReplacement ? *ppReplacement : 0;
}

const char *CLocalizationDatabase::FindString(int Language, unsigned Hash, unsigned ContextHash) const
{
	// A missing translation falls back to the source text handled by Localize().
	// The server default is only used when the player has no valid language.
	return FindStringExact(Language, Hash, ContextHash);
}

const char *CLocalizationDatabase::FindString(unsigned Hash, unsigned ContextHash) const
{
	int Language = ActiveLanguage();
	if(Language < 0 || Language >= m_aLanguages.size())
		Language = DefaultLanguage();
	return FindStringExact(Language, Hash, ContextHash);
}

const char *Localize(const char *pStr, const char *pContext)
{
	const char *pNewStr = g_Localization.FindString(str_quickhash(pStr), str_quickhash(pContext));
	return pNewStr ? pNewStr : pStr;
}

CLocConstString::CLocConstString(const char *pStr, const char *pContext)
{
	m_pDefaultStr = pStr;
	m_pCurrentStr = pStr;
	m_Hash = str_quickhash(m_pDefaultStr);
	m_ContextHash = str_quickhash(pContext);
	m_Version = -1;
	m_Language = -1;
}

void CLocConstString::Reload()
{
	m_Version = g_Localization.Version();
	m_Language = g_Localization.ActiveLanguage();
	const char *pNewStr = g_Localization.FindString(m_Hash, m_ContextHash);
	m_pCurrentStr = pNewStr;
	if(!m_pCurrentStr)
		m_pCurrentStr = m_pDefaultStr;
}

CLocalizationDatabase g_Localization;
