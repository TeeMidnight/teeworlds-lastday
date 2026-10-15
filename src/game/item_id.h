/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_ITEM_ID_H
#define GAME_ITEM_ID_H

#include <base/uuid.h>

#include <format>

// A content item's identity: a UUID derived deterministically from a namespaced
// string id ("lastday:stone", "addon:steel_ingot", ...). Items are no longer
// identified by a bare hash of a file name, so two content packs can never
// collide and an id keeps its meaning even when an item is renamed.
//
// The class wraps the 16 byte Uuid and stays a trivially copyable value type so
// inventories and snapshots can mem_zero/assign it cheaply.
class CItemID
{
	Uuid m_Uuid{};

public:
	CItemID() = default;
	explicit CItemID(Uuid UuidValue) : m_Uuid(UuidValue) {}

	// Build an id from a namespaced content name. A name without a namespace is
	// placed in the default "lastday" namespace, so "stone" and "lastday:stone"
	// are the same item.
	static CItemID FromName(const char *pName);
	// Parse either a canonical UUID string (36 chars) or a namespaced name.
	// Returns false when the string is empty.
	static bool Parse(const char *pStr, CItemID *pOut);

	Uuid Value() const { return m_Uuid; }
	bool IsZero() const;

	bool operator==(const CItemID &Other) const;
	bool operator!=(const CItemID &Other) const { return !(*this == Other); }

	// The namespaced source string the id was parsed from, or "" when the id
	// only ever existed as a UUID. Used for menu commands and as a fallback
	// display name.
	const char *SourceName() const;

	// Canonical 36 character UUID string. pOut must hold UUID_MAXSTRSIZE bytes.
	void FormatUuid(char *pOut, int Size) const;
};

// Register the source and display (untranslated) names of an item, so that
// formatting an CItemID can resolve it. Called by the item loader.
void ItemIDRegisterName(const CItemID &Id, const char *pSourceName, const char *pDisplayName);

// The name a formatted CItemID should print:
//   1. the localized display name of a registered item,
//   2. otherwise the namespaced source name the id was parsed from,
//   3. otherwise the canonical UUID string.
const char *ItemIDDisplayName(const CItemID &Id);

namespace std {
template<>
struct formatter<CItemID, char>
{
	constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

	auto format(const CItemID &Id, format_context &ctx) const
	{
		return std::format_to(ctx.out(), "{}", ItemIDDisplayName(Id));
	}
};
} // namespace std

#endif // GAME_ITEM_ID_H
