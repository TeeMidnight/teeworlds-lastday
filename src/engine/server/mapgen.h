#ifndef ENGINE_SERVER_MAPGEN_H
#define ENGINE_SERVER_MAPGEN_H

#include <base/tl/hashtable.h>
#include <base/tl/string.h>

class CJsonWriter;
struct _json_value;
typedef struct _json_value json_value;
struct CStructEntrance;
class CMapGen
{
	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	class IStorage *Storage() { return m_pStorage; };
	class IConsole *Console() { return m_pConsole; };

	struct CInstruction
	{
		struct CStruct
		{
			char m_aBaseMap[64];
			int m_GenerateProba;
			array<char> m_EntrancesJson; // serialized entrances json of this struct (optional)
		};

		char m_aBaseMap[64];
		array<CStruct> m_Structs;
		array<char> m_DefaultEntrances;
	};

	// keyed by str_quickhash of the base map name
	hash_table<unsigned, CInstruction *, 4> m_lInstructions;

	static void FreeInstruction(CInstruction *&pInstruction, void *pUser);

	// serializes the entrance array, appending "_<seed>" to the entrance
	// targets that are themselves generated worlds (configured in
	// worlds.json); static maps like the main world Connector keep their
	// plain name. The entrances carried by the struct maps themselves are
	// appended as well.
	void WriteEntrancesWithSeed(CJsonWriter *pWriter, const json_value &rEntrances, int Seed, const array<CStructEntrance> &lStructEntrances);

public:
	CMapGen(class IStorage *pStorage, class IConsole *pConsole);
	~CMapGen();

	// Generates a new map from the given base map, merging the configured
	// struct maps at the positions marked by the flag stand entities of the
	// base map's game layer. The result is written to
	// "generatedmaps/<BaseMap>.map". Returns true on success.
	bool RequestNewMap(const char *pBaseMap, int Seed);
};

#endif // ENGINE_SERVER_MAPGEN_H
