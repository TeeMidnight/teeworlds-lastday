/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_MAP_GENERATEDMAPS_H
#define ENGINE_MAP_GENERATEDMAPS_H

#include <engine/storage.h>
#include <game/version.h>

// Generated maps are cached per mod version: the generator's output can change
// between releases, so a map from another version must not be reused. The path
// is relative to the storage save path.
#define GENERATED_MAPS_DIR "generatedmaps/" MOD_NAME "/" MOD_VERSION

// One append-only map pack per floor lives next to the generated maps. The
// pack is the durable copy: a generated instance is appended as soon as it is
// written, and an instance that is missing from the disk cache is restored
// from the pack instead of being generated again.
#define MAPPACK_DIR "mappacks/" MOD_NAME "/" MOD_VERSION

// Creates the generated map directory and its parents on the save path.
// IStorage::CreateFolder is not recursive, so every level is created on its
// own. An already existing directory is not an error: the caller only needs
// the directory to exist, and the map write itself reports a real failure.
static inline void EnsureGeneratedMapsDir(IStorage *pStorage)
{
	if(!pStorage)
		return;
	pStorage->CreateFolder("generatedmaps", IStorage::TYPE_SAVE);
	pStorage->CreateFolder("generatedmaps/" MOD_NAME, IStorage::TYPE_SAVE);
	pStorage->CreateFolder(GENERATED_MAPS_DIR, IStorage::TYPE_SAVE);
}

// Same for the map pack directory.
static inline void EnsureMappackDir(IStorage *pStorage)
{
	if(!pStorage)
		return;
	pStorage->CreateFolder("mappacks", IStorage::TYPE_SAVE);
	pStorage->CreateFolder("mappacks/" MOD_NAME, IStorage::TYPE_SAVE);
	pStorage->CreateFolder(MAPPACK_DIR, IStorage::TYPE_SAVE);
}

#endif // ENGINE_MAP_GENERATEDMAPS_H
