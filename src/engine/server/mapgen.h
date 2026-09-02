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
			bool m_PasteAir; // whether empty struct tiles overwrite the base (default true)
			array<char> m_EntrancesJson; // serialized entrances json of this struct (optional)
			array<char> m_ResourcesJson; // serialized resources json of this struct (optional)
		};

		char m_aFloorName[64]; // unique floor/map name (the file name)
		char m_aBaseMap[64]; // the base map file to load (the "base" field)
		array<CStruct> m_Structs;
		array<char> m_DefaultEntrances;
		array<char> m_ResourcesJson; // serialized resources json of this base (optional)
	};

	// keyed by str_quickhash of the base map name
	hash_table<unsigned, CInstruction *, 4> m_lInstructions;

	static void FreeInstruction(CInstruction *&pInstruction, void *pUser);

	// IStorage::ListDirectory callback: loads every "<floorname>.json" file
	// found in the "maps/worlds" directory
	static int ListWorldsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);

	// parse a single floor config file and register its CInstruction
	void LoadFloor(const char *pFloorName, const char *pFilePath);

	// serializes the entrance array, appending "_<seed>" to the entrance
	// targets that are themselves generated worlds (configured in
	// worlds.json); static maps like the main world Connector keep their
	// plain name. The entrances carried by the struct maps themselves are
	// appended as well.
	void WriteEntrancesWithSeed(CJsonWriter *pWriter, const json_value &rEntrances, int Seed, const array<CStructEntrance> &lStructEntrances);

public:
	CMapGen(class IStorage *pStorage, class IConsole *pConsole);
	~CMapGen();

	// Generates a new map for the given floor (the floor name, e.g. the map
	// name prefix "<FloorName>_<Seed>"). The floor's instruction carries the
	// actual base map file to load. The result is written to
	// "generatedmaps/<FloorName>_<Seed>.map". Returns true on success.
	bool RequestNewMap(const char *pFloorName, int Seed);
};

#endif // ENGINE_SERVER_MAPGEN_H
