/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_NPC_ID_H
#define GAME_NPC_ID_H

#include <base/uuid.h>

#include <format>

// A configured NPC's identity: a UUID derived deterministically from a namespaced
// string id ("lastday:blacksmith"). It mirrors CItemID so both content kinds
// behave the same: an id keeps its meaning across a rename and two content packs
// cannot collide. The class wraps the 16 byte Uuid and stays trivially copyable.
class CNpcID
{
	Uuid m_Uuid{};

public:
	CNpcID() = default;
	explicit CNpcID(Uuid UuidValue) : m_Uuid(UuidValue) {}

	// Build an id from a namespaced content name. A name without a namespace is
	// placed in the default "lastday" namespace, so "blacksmith" and
	// "lastday:blacksmith" are the same NPC.
	static CNpcID FromName(const char *pName);
	// Parse either a canonical UUID string (36 chars) or a namespaced name.
	// Returns false when the string is empty.
	static bool Parse(const char *pStr, CNpcID *pOut);

	Uuid Value() const { return m_Uuid; }
	bool IsZero() const;

	bool operator==(const CNpcID &Other) const;
	bool operator!=(const CNpcID &Other) const { return !(*this == Other); }

	// The namespaced source string the id was parsed from, or "" when the id
	// only ever existed as a UUID.
	const char *SourceName() const;

	// Canonical 36 character UUID string. pOut must hold UUID_MAXSTRSIZE bytes.
	void FormatUuid(char *pOut, int Size) const;
};

// Register the source and display names of an NPC. The display name is
// deliberately not localized: an NPC name is content, not UI text, so it never
// needs a translation entry.
void NpcIDRegisterName(const CNpcID &Id, const char *pSourceName, const char *pDisplayName);

// The name a formatted CNpcID should print:
//   1. the registered display name,
//   2. otherwise the namespaced source name the id was parsed from,
//   3. otherwise the canonical UUID string.
const char *NpcIDDisplayName(const CNpcID &Id);

namespace std {
template<>
struct formatter<CNpcID, char>
{
	constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

	auto format(const CNpcID &Id, format_context &ctx) const
	{
		return std::format_to(ctx.out(), "{}", NpcIDDisplayName(Id));
	}
};
} // namespace std

#endif // GAME_NPC_ID_H
