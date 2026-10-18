/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/npc_id.h>

#include <base/system.h>

#include <base/tl/hashtable.h>

#include <string>

// ---------------------------------------------------------------------------
// name registry
//
// Unlike items, an NPC name is content and is never localized: the registry
// simply maps the deterministic UUID back to the name authored in the npc json.
// ---------------------------------------------------------------------------
namespace {
struct SNpcIdName
{
	std::string m_Source;
	std::string m_Display;
};

hash_table<Uuid, SNpcIdName, 64, uuid_table_function> &NameRegistry()
{
	static hash_table<Uuid, SNpcIdName, 64, uuid_table_function> s_Registry;
	return s_Registry;
}

SNpcIdName &NameSlot(const Uuid &Id)
{
	SNpcIdName *pName = NameRegistry().get(Id);
	if(!pName)
	{
		SNpcIdName Name;
		pName = NameRegistry().set(Id, Name);
	}
	return *pName;
}

const SNpcIdName *FindName(const Uuid &Id)
{
	return NameRegistry().get(Id);
}

void RememberSource(const CNpcID &Id, const char *pSourceName)
{
	if(Id.IsZero() || !pSourceName || !pSourceName[0])
		return;
	NameSlot(Id.Value()).m_Source = pSourceName;
}
} // namespace

// ---------------------------------------------------------------------------
// CNpcID
// ---------------------------------------------------------------------------
CNpcID CNpcID::FromName(const char *pName)
{
	if(!pName || !pName[0])
		return CNpcID();

	// Normalize into a namespace. A bare name belongs to the default namespace,
	// so "blacksmith" and "lastday:blacksmith" hash to the same id.
	char aNormalized[128];
	if(str_find(pName, ":") != nullptr)
		str_copy(aNormalized, pName, sizeof(aNormalized));
	else
		str_format(aNormalized, sizeof(aNormalized), "lastday:%s", pName);

	char aHashInput[192];
	str_format(aHashInput, sizeof(aHashInput), "lastday:npc/%s", aNormalized);
	const CNpcID Id(calculate_uuid(aHashInput));
	RememberSource(Id, aNormalized);
	return Id;
}

bool CNpcID::Parse(const char *pStr, CNpcID *pOut)
{
	if(!pStr || !pStr[0])
		return false;

	Uuid UuidValue;
	if(parse_uuid(&UuidValue, pStr) == 0)
	{
		*pOut = CNpcID(UuidValue);
		return true;
	}

	*pOut = FromName(pStr);
	return true;
}

bool CNpcID::IsZero() const
{
	return uuid_comp(m_Uuid, UUID_ZEROED) == 0;
}

bool CNpcID::operator==(const CNpcID &Other) const
{
	return uuid_comp(m_Uuid, Other.m_Uuid) == 0;
}

const char *CNpcID::SourceName() const
{
	const SNpcIdName *pName = FindName(m_Uuid);
	return (pName && !pName->m_Source.empty()) ? pName->m_Source.c_str() : "";
}

void CNpcID::FormatUuid(char *pOut, int Size) const
{
	format_uuid(m_Uuid, pOut, Size);
}

void NpcIDRegisterName(const CNpcID &Id, const char *pSourceName, const char *pDisplayName)
{
	if(Id.IsZero())
		return;
	SNpcIdName &Name = NameSlot(Id.Value());
	if(pSourceName && pSourceName[0])
		Name.m_Source = pSourceName;
	if(pDisplayName && pDisplayName[0])
		Name.m_Display = pDisplayName;
}

const char *NpcIDDisplayName(const CNpcID &Id)
{
	const SNpcIdName *pName = FindName(Id.Value());
	// 1. the name authored in the npc json (never localized)
	if(pName && !pName->m_Display.empty())
		return pName->m_Display.c_str();
	// 2. an unloaded npc still shows the namespaced name it was parsed from
	if(pName && !pName->m_Source.empty())
		return pName->m_Source.c_str();
	// 3. nothing is known: print the uuid
	static thread_local char s_aBuffer[UUID_MAXSTRSIZE];
	Id.FormatUuid(s_aBuffer, sizeof(s_aBuffer));
	return s_aBuffer;
}
