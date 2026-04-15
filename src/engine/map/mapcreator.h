#ifndef ENGINE_MAP_MAPCREATOR_H
#define ENGINE_MAP_MAPCREATOR_H

#include "mapitems.h"

class CServer;

class CMapCreator
{
	class IStorage *m_pStorage;
	class IConsole *m_pConsole;

	array<CCreatorGroupInfo *> m_apGroups;

	array<CCreatorImage *> m_apImages;
	array<CCreatorEnvelope *> m_apEnvelopes;

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

	bool SaveMap(EMapType MapType, const char *pMap);
};

#endif // ENGINE_MAP_MAPCREATER_H
