#ifndef ENGINE_MAP_MAPITEMS_H
#define ENGINE_MAP_MAPITEMS_H

#include <base/color.h>
#include <base/tl/array.h>
#include <base/tl/string.h>

#include <game/mapitems.h>

enum class EMapType
{
	MAPTYPE_NORMAL = 0
};

enum class ELayerType
{
	TILES = 0,
	QUADS
};

enum class EEnvType
{
	None = -1,
	Pos = 0,
	Color,
	Sound,
};

struct CCreatorImage
{
	char m_aName[32];

	bool m_External;

	int m_Width;
	int m_Height;

	int m_ImageID;

	unsigned char *m_pImageData;
};

struct CCreatorEnvPoint
{
	int m_Time; // in ms
	int m_Curvetype;
	float m_aValues[4];
};

struct CCreatorEnvelope
{
	char m_aName[32];
	bool m_Synchronized;

	int m_EnvID;

	array<CCreatorEnvPoint *> m_lpEnvPoints;

	void *m_PointLock;

	CCreatorEnvPoint *AddEnvPoint(int Time, int CurveType);

	virtual EEnvType Type() const = 0;
	virtual int Channels() const = 0;
	virtual ~CCreatorEnvelope() {};
};

struct ILayerInfo
{
	char m_aName[32];

	ELayerType m_Type;
	int m_Flags;
	bool m_UseInMinimap;

	CCreatorImage *m_pImage;

	ILayerInfo()
	{
		m_pImage = nullptr;
		m_Flags = 0;
		m_UseInMinimap = false;
	}
};

struct CCreatorLayerTilemap : public ILayerInfo
{
	CTile *m_pTiles;

	int m_Width;
	int m_Height;
	int m_Flags;

	CCreatorEnvelope *m_pColorEnv;
	ColorRGBA m_Color;

	CCreatorLayerTilemap() :
		ILayerInfo()
	{
		m_Type = ELayerType::TILES;
		CCreatorLayerTilemap::m_Flags = 0;
	}

	CTile *AddTiles(int Width, int Height);
};

struct CCreatorQuad
{
	ivec2 m_aPoints[4];
	ivec2 m_aTexcoords[4];

	ivec2 m_Pos;

	ColorRGBA m_aColors[4];

	CCreatorEnvelope *m_pColorEnv;
	CCreatorEnvelope *m_pPosEnv;
};

struct CCreatorLayerQuads : public ILayerInfo
{
	ColorRGBA m_Color;

	array<CCreatorQuad *> m_lpQuads;

	void *m_QuadLock;

	CCreatorLayerQuads() :
		ILayerInfo()
	{
		m_Type = ELayerType::QUADS;
	}

	CCreatorQuad *AddQuad(vec2 Pos, vec2 Size, ColorRGBA Color = ColorRGBA(255, 255, 255, 255));
};

struct CCreatorGroupInfo
{
	char m_aName[32];
	bool m_UseClipping;

	int m_OffsetX;
	int m_OffsetY;
	int m_ParallaxX;
	int m_ParallaxY;

	int m_ClipX;
	int m_ClipY;
	int m_ClipW;
	int m_ClipH;

	void *m_LayerLock;

	array<ILayerInfo *> m_lpLayers;

	CCreatorLayerTilemap *AddTileLayer(const char *pName);
	CCreatorLayerQuads *AddQuadsLayer(const char *pName);
};

struct CCreatorPosEnvelope : public CCreatorEnvelope
{
	EEnvType Type() const override { return EEnvType::Pos; }
	int Channels() const override { return 3; }
	~CCreatorPosEnvelope() override = default;
};

struct CCreatorColorEnvelope : public CCreatorEnvelope
{
	EEnvType Type() const override { return EEnvType::Color; }
	int Channels() const override { return 4; }
	~CCreatorColorEnvelope() override = default;
};

struct CCreatorSoundEnvelope : public CCreatorEnvelope
{
	EEnvType Type() const override { return EEnvType::Sound; }
	int Channels() const override { return 1; }
	~CCreatorSoundEnvelope() override = default;
};

#endif // ENGINE_MAP_MAPITEMS_H
