/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_MAP_H
#define ENGINE_MAP_H

#include <base/hash.h>
#include "kernel.h"

class IMap : public IInterface
{
	MACRO_INTERFACE("map", 0)
public:
	virtual int GetDataSize(int Index) = 0;
	virtual void *GetData(int Index) = 0;
	virtual void *GetDataSwapped(int Index) = 0;
	virtual void UnloadData(int Index) = 0;
	virtual void *GetItem(int Index, int *Type, int *pID) = 0;
	virtual void GetType(int Type, int *pStart, int *pNum) = 0;
	virtual void *FindItem(int Type, int ID) = 0;
	virtual int NumItems() = 0;
};

class IEngineMap : public IMap
{
	MACRO_INTERFACE("enginemap", 0)
public:
	virtual bool Load(const char *pMapName, class IStorage *pStorage = 0) = 0;
	// Load a map from bytes that are already in memory: a map restored from a
	// pack, or one received over the network. The bytes are copied, so the
	// caller may free them as soon as this returns.
	virtual bool LoadFromMemory(const char *pMapName, const void *pData, int Size) = 0;
	virtual bool IsLoaded() = 0;
	virtual void Unload() = 0;
	virtual SHA256_DIGEST Sha256() = 0;
	virtual unsigned Crc() = 0;
};

extern IEngineMap *CreateEngineMap();

#endif
