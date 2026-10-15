/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/item_id.h>

#include <base/system.h>

#include <game/localization_format.h>

#include <map>
#include <string>

// ---------------------------------------------------------------------------
// name registry
//
// The item loader tells the registry the source/display name of every item it
// knows. Formatting an id that was never loaded still works: it falls back to
// the namespaced source name recorded when the id was parsed, and finally to
// the raw UUID.
// ---------------------------------------------------------------------------
namespace {
struct SItemIdName
{
	std::string m_Source;
	std::string m_Display;
};

struct UuidLess
{
	bool operator()(const Uuid &A, const Uuid &B) const { return uuid_comp(A, B) < 0; }
};

std::map<Uuid, SItemIdName, UuidLess> &NameRegistry()
{
	static std::map<Uuid, SItemIdName, UuidLess> s_Registry;
	return s_Registry;
}

const SItemIdName *FindName(const Uuid &Id)
{
	const auto &Registry = NameRegistry();
	auto It = Registry.find(Id);
	return It == Registry.end() ? nullptr : &It->second;
}

// The name a non-parsed but known item keeps: the registry may already hold the
// display name even for an id that was created from a UUID alone.
void RememberSource(const CItemID &Id, const char *pSourceName)
{
	if (Id.IsZero() || !pSourceName || !pSourceName[0])
		return;
	NameRegistry()[Id.Value()].m_Source = pSourceName;
}
} // namespace

// ---------------------------------------------------------------------------
// CItemID
// ---------------------------------------------------------------------------
CItemID CItemID::FromName(const char *pName)
{
	if(!pName || !pName[0])
		return CItemID();

	// Normalize into a namespace. A bare name belongs to the default namespace,
	// so "stone" and "lastday:stone" must hash to the same id.
	char aNormalized[128];
	if(str_find(pName, ":") != nullptr) // contains a colon
		str_copy(aNormalized, pName, sizeof(aNormalized));
	else
		str_format(aNormalized, sizeof(aNormalized), "lastday:%s", pName);

	char aHashInput[192];
	str_format(aHashInput, sizeof(aHashInput), "lastday:item/%s", aNormalized);
	const CItemID Id(calculate_uuid(aHashInput));
	RememberSource(Id, aNormalized);
	return Id;
}

bool CItemID::Parse(const char *pStr, CItemID *pOut)
{
	if(!pStr || !pStr[0])
		return false;

	Uuid UuidValue;
	if(parse_uuid(&UuidValue, pStr) == 0)
	{
		*pOut = CItemID(UuidValue);
		return true;
	}

	*pOut = FromName(pStr);
	return true;
}

bool CItemID::IsZero() const
{
	return uuid_comp(m_Uuid, UUID_ZEROED) == 0;
}

bool CItemID::operator==(const CItemID &Other) const
{
	return uuid_comp(m_Uuid, Other.m_Uuid) == 0;
}

const char *CItemID::SourceName() const
{
	const SItemIdName *pName = FindName(m_Uuid);
	return (pName && !pName->m_Source.empty()) ? pName->m_Source.c_str() : "";
}

void CItemID::FormatUuid(char *pOut, int Size) const
{
	format_uuid(m_Uuid, pOut, Size);
}

void ItemIDRegisterName(const CItemID &Id, const char *pSourceName, const char *pDisplayName)
{
	if(Id.IsZero())
		return;
	SItemIdName &Name = NameRegistry()[Id.Value()];
	if(pSourceName && pSourceName[0])
		Name.m_Source = pSourceName;
	if(pDisplayName && pDisplayName[0])
		Name.m_Display = pDisplayName;
}

const char *ItemIDDisplayName(const CItemID &Id)
{
	const SItemIdName *pName = FindName(Id.Value());
	// 1. a loaded item shows its localized display name
	if(pName && !pName->m_Display.empty())
		return LocalizeString(pName->m_Display.c_str(), "Item Name");
	// 2. an unloaded item still shows the namespaced name it was parsed from
	if(pName && !pName->m_Source.empty())
		return pName->m_Source.c_str();
	// 3. nothing is known: print the uuid
	static thread_local char s_aBuffer[UUID_MAXSTRSIZE];
	Id.FormatUuid(s_aBuffer, sizeof(s_aBuffer));
	return s_aBuffer;
}
