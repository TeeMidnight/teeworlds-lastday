#ifndef ENGINE_MAP_MAPCREATOR_H
#define ENGINE_MAP_MAPCREATOR_H

#include "mapitems.h"

class CServer;

class CMapCreator
{
	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	array<CCreatorGroupInfo *> m_lpGroups;

	array<CCreatorImage *> m_lpImages;
	array<CCreatorEnvelope *> m_lpEnvelopes;

	array<char> m_JsonData;

	void *m_ImageLock;
	void *m_EnvelopeLock;
	void *m_GroupLock;

public:
	class IStorage *Storage() { return m_pStorage; };
	class IConsole *Console() { return m_pConsole; };

	CMapCreator(class IStorage *pStorage, class IConsole *pConsole);
	~CMapCreator();

	CCreatorImage *AddExternalImage(const char *pImageName, int Width, int Height);
	CCreatorImage *AddEmbeddedImage(const char *pImageName, bool Flag = false);

	CCreatorEnvelope *AddEnvelope(const char *pEnvName, EEnvType Type, bool Synchronized);

	CCreatorGroupInfo *AddGroup(const char *pName);

	void AddMiniMap();

	// stores arbitrary json data (e.g. the entrance definitions of a
	// generated world) which is written to the map as a MAPITEMTYPE_JSON
	// item on save
	void AddJsonData(const char *pJsonData, int Size);

	bool SaveMap(EMapType MapType, const char *pMap);
};

#endif // ENGINE_MAP_MAPCREATER_H
