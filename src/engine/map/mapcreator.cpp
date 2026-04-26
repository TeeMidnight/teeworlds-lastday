#include <base/system.h>

#include <engine/console.h>
#include <engine/storage.h>

#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/linereader.h>

#include <game/gamecore.h>
#include <game/layers.h>
#include <game/mapitems.h>

#include "mapcreator.h"

#include <engine/external/pnglite/pnglite.h>

class CImageInfo
{
public:
	enum
	{
		FORMAT_AUTO = -1,
		FORMAT_RGB = 0,
		FORMAT_RGBA = 1,
		FORMAT_ALPHA = 2,
	};

	/* Variable: width
		Contains the width of the image */
	int m_Width;

	/* Variable: height
		Contains the height of the image */
	int m_Height;

	/* Variable: format
		Contains the format of the image. See <Image Formats> for more information. */
	int m_Format;

	/* Variable: data
		Pointer to the image data. */
	void *m_pData;
};

static int LoadPNG(CImageInfo *pImg, class IStorage *pStorage, const char *pFilename)
{
	// open file for reading
	char aCompleteFilename[IO_MAX_PATH_LENGTH];
	IOHANDLE File = pStorage->OpenFile(pFilename, IOFLAG_READ, IStorage::TYPE_ALL, aCompleteFilename, sizeof(aCompleteFilename));
	if(!File)
	{
		dbg_msg("game/png", "failed to open file. filename='%s'", pFilename);
		return 0;
	}

	png_init(0, 0);
	png_t Png;
	int Error = png_open_read(&Png, 0, File);
	if(Error != PNG_NO_ERROR)
	{
		dbg_msg("game/png", "failed to read file. filename='%s'", aCompleteFilename);
		io_close(File);
		return 0;
	}

	if(Png.depth != 8 || Png.width > (2 << 12) || Png.height > (2 << 12))
	{
		dbg_msg("game/png", "invalid format. filename='%s'", aCompleteFilename);
		io_close(File);
		return 0;
	}

	if(Png.color_type == PNG_TRUECOLOR)
		pImg->m_Format = CImageInfo::FORMAT_RGB;
	else if(Png.color_type == PNG_TRUECOLOR_ALPHA)
		pImg->m_Format = CImageInfo::FORMAT_RGBA;
	else
	{
		dbg_msg("game/png", "invalid format. filename='%s'", aCompleteFilename);
		io_close(File);
		return 0;
	}

	unsigned char *pBuffer = (unsigned char *) mem_alloc(Png.width * Png.height * Png.bpp);
	png_get_data(&Png, pBuffer);
	io_close(File);

	pImg->m_Width = Png.width;
	pImg->m_Height = Png.height;
	pImg->m_pData = pBuffer;
	return 1;
}

CMapCreator::CMapCreator(IStorage *pStorage, IConsole *pConsole) :
	m_pStorage(pStorage),
	m_pConsole(pConsole)
{
	m_lpGroups.clear();
	m_lpImages.clear();
	m_lpEnvelopes.clear();

	m_ImageLock = lock_create();
	m_EnvelopeLock = lock_create();
}

CMapCreator::~CMapCreator()
{
	for(auto &pImage : m_lpImages)
	{
		if(pImage->m_pImageData)
		{
			mem_free(pImage->m_pImageData);
		}
		delete pImage;
	}
	m_lpImages.clear();
	for(auto &pEnv : m_lpEnvelopes)
	{
		lock_destroy(pEnv->m_PointLock);
		for(auto &pEnvPoint : pEnv->m_lpEnvPoints)
		{
			delete pEnvPoint;
		}
		delete pEnv;
	}
	m_lpEnvelopes.clear();

	for(auto &pGroup : m_lpGroups)
	{
		for(auto &pLayer : pGroup->m_lpLayers)
		{
			switch(pLayer->m_Type)
			{
				case ELayerType::TILES:
				{
					delete[]((CCreatorLayerTilemap *) pLayer)->m_pTiles;
				}
				break;
				case ELayerType::QUADS:
				{
					lock_destroy(((CCreatorLayerQuads *) pLayer)->m_QuadLock);
					for(auto &pQuad : ((CCreatorLayerQuads *) pLayer)->m_lpQuads)
						delete pQuad;
				}
				break;
				default:
					dbg_assert(false, "Invalid layer type");
			}
			delete pLayer;
		}
		lock_destroy(pGroup->m_LayerLock);
		pGroup->m_lpLayers.clear();
		delete pGroup;
	}
	m_lpGroups.clear();

	lock_destroy(m_ImageLock);
	lock_destroy(m_EnvelopeLock);
}

CCreatorImage *CMapCreator::AddEmbeddedImage(const char *pImageName, bool Flag)
{
	for(auto &pImage : m_lpImages)
	{
		if(str_comp(pImage->m_aName, pImageName) == 0)
		{
			return pImage;
		}
	}

	CImageInfo img;
	CImageInfo *pImg = &img;

	char aBuf[IO_MAX_PATH_LENGTH];
	if(Flag)
	{
		str_format(aBuf, sizeof(aBuf), "flags/%s.png", pImageName);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "mapres/%s.png", pImageName);
	}

	if(!LoadPNG(pImg, Storage(), aBuf))
	{
		dbg_msg("mapcreater", "failed to load image '%s'", aBuf);
		return nullptr;
	}

	lock_wait(m_ImageLock);
	CCreatorImage *pImage = m_lpImages[m_lpImages.add(new CCreatorImage())];
	lock_unlock(m_ImageLock);

	str_copy(pImage->m_aName, pImageName, sizeof(pImage->m_aName));

	pImage->m_External = false;
	pImage->m_pImageData = (unsigned char *) malloc((size_t) pImg->m_Width * pImg->m_Height * 4);

	pImage->m_Width = pImg->m_Width;
	pImage->m_Height = pImg->m_Height;

	pImage->m_ImageID = -1;

	unsigned char *pDataRGBA = pImage->m_pImageData;
	if(pImg->m_Format == CImageInfo::FORMAT_RGB)
	{
		// Convert to RGBA
		unsigned char *pDataRGB = (unsigned char *) pImg->m_pData;
		for(int i = 0; i < pImg->m_Width * pImg->m_Height; i++)
		{
			pDataRGBA[i * 4] = pDataRGB[i * 3];
			pDataRGBA[i * 4 + 1] = pDataRGB[i * 3 + 1];
			pDataRGBA[i * 4 + 2] = pDataRGB[i * 3 + 2];
			pDataRGBA[i * 4 + 3] = 255;
		}
	}
	else
	{
		mem_copy(pDataRGBA, pImg->m_pData, (size_t) pImg->m_Width * pImg->m_Height * 4);
	}

	mem_free(pImg->m_pData);

	return pImage;
}

CCreatorImage *CMapCreator::AddExternalImage(const char *pImageName, int Width, int Height)
{
	lock_wait(m_ImageLock);

	m_lpImages.add(new CCreatorImage());
	CCreatorImage *pImage = m_lpImages[m_lpImages.size() - 1];

	lock_unlock(m_ImageLock);

	str_copy(pImage->m_aName, pImageName, sizeof(pImage->m_aName));

	pImage->m_External = true;
	pImage->m_pImageData = nullptr;

	pImage->m_Width = Width;
	pImage->m_Height = Height;

	pImage->m_ImageID = -1;

	return pImage;
}

CCreatorEnvelope *CMapCreator::AddEnvelope(const char *pEnvName, EEnvType Type, bool Synchronized)
{
	lock_wait(m_EnvelopeLock);
	CCreatorEnvelope *pEnv;
	switch(Type)
	{
		case EEnvType::Pos: pEnv = m_lpEnvelopes[m_lpEnvelopes.add(new CCreatorPosEnvelope())]; break;
		case EEnvType::Color: pEnv = m_lpEnvelopes[m_lpEnvelopes.add(new CCreatorColorEnvelope())]; break;
		case EEnvType::Sound: pEnv = m_lpEnvelopes[m_lpEnvelopes.add(new CCreatorSoundEnvelope())]; break;
		default: lock_unlock(m_EnvelopeLock); return nullptr;
	}
	lock_unlock(m_EnvelopeLock);

	str_copy(pEnv->m_aName, pEnvName, sizeof(pEnv->m_aName));
	pEnv->m_Synchronized = Synchronized;
	pEnv->m_EnvID = -1;
	pEnv->m_lpEnvPoints.clear();

	return pEnv;
}

CCreatorGroupInfo *CMapCreator::AddGroup(const char *pName)
{
	lock_wait(m_GroupLock);
	CCreatorGroupInfo *pGroup = m_lpGroups[m_lpGroups.add(new CCreatorGroupInfo())];
	pGroup->m_LayerLock = lock_create();
	lock_unlock(m_GroupLock);

	str_copy(pGroup->m_aName, pName, sizeof(pGroup->m_aName));

	// default
	pGroup->m_UseClipping = false;

	pGroup->m_ParallaxX = 100;
	pGroup->m_ParallaxY = 100;
	pGroup->m_OffsetX = 0;
	pGroup->m_OffsetY = 0;
	pGroup->m_ClipX = 0;
	pGroup->m_ClipY = 0;
	pGroup->m_ClipW = 0;
	pGroup->m_ClipH = 0;

	return pGroup;
}

void CMapCreator::AddMiniMap()
{
	CCreatorGroupInfo *pMiniGroup = AddGroup("Minimap");
	pMiniGroup->m_ParallaxX = 25;
	pMiniGroup->m_ParallaxY = 25;
	const int BlockSize = 32 / 4;
	vec2 StartPos(-BlockSize / 2, -BlockSize / 2);

	{
		CCreatorGroupInfo *pPointerGroup = AddGroup("Pointer");
		CCreatorLayerQuads *pNewLayer = pPointerGroup->AddQuadsLayer("Pointer");
		pPointerGroup->m_ParallaxX = 0;
		pPointerGroup->m_ParallaxY = 0;
		pNewLayer->m_Flags |= LAYERFLAG_DETAIL;
		pNewLayer->AddQuad(StartPos, vec2(BlockSize, BlockSize), ColorRGBA{0, 0xff, 0xff, 100});
	}
	for(auto &pGroup : m_lpGroups)
	{
		if(pGroup == pMiniGroup)
			continue;
		for(auto &pLayer : pGroup->m_lpLayers)
		{
			if(pLayer->m_Type == ELayerType::TILES && pLayer->m_UseInMinimap)
			{
				CCreatorLayerQuads *pNewLayer = pMiniGroup->AddQuadsLayer(pLayer->m_aName);
				pNewLayer->m_Color = ColorRGBA{0, 255, 255, 55};
				pNewLayer->m_pImage = pLayer->m_pImage;
				pNewLayer->m_Flags |= LAYERFLAG_DETAIL;

				int Width, Height;
				Width = ((CCreatorLayerTilemap *) pLayer)->m_Width;
				Height = ((CCreatorLayerTilemap *) pLayer)->m_Height;
				CTile *pTiles = ((CCreatorLayerTilemap *) pLayer)->m_pTiles;
				for(int x = 0; x < Width; x++)
				{
					for(int y = 0; y < Height; y++)
					{
						int Index = pTiles[y * Width + x].m_Index;
						if(Index)
						{
							CCreatorQuad *pQuad = pNewLayer->AddQuad(StartPos + vec2(x, y) * BlockSize + vec2(BlockSize, BlockSize) / 2, vec2(BlockSize, BlockSize), pNewLayer->m_Color);
							pQuad->m_aTexcoords[0].u = pQuad->m_aTexcoords[2].u = (Index % 16) * 64;
							pQuad->m_aTexcoords[1].u = pQuad->m_aTexcoords[3].u = (Index % 16 + 1) * 64;
							pQuad->m_aTexcoords[0].v = pQuad->m_aTexcoords[1].v = (Index / 16) * 64;
							pQuad->m_aTexcoords[2].v = pQuad->m_aTexcoords[3].v = (Index / 16 + 1) * 64;
						}
					}
				}
			}
		}
	}
}

CCreatorLayerTilemap *CCreatorGroupInfo::AddTileLayer(const char *pName)
{
	lock_wait(m_LayerLock);
	CCreatorLayerTilemap *pLayer = (CCreatorLayerTilemap *) m_lpLayers[m_lpLayers.add(new CCreatorLayerTilemap())];
	lock_unlock(m_LayerLock);

	str_copy(pLayer->m_aName, pName, sizeof(pLayer->m_aName));
	pLayer->m_pTiles = nullptr;
	pLayer->m_pImage = nullptr;

	pLayer->m_Color = ColorRGBA(255, 255, 255, 255);
	pLayer->m_pColorEnv = nullptr;

	return pLayer;
}

CCreatorLayerQuads *CCreatorGroupInfo::AddQuadsLayer(const char *pName)
{
	lock_wait(m_LayerLock);
	CCreatorLayerQuads *pLayer = (CCreatorLayerQuads *) m_lpLayers[m_lpLayers.add(new CCreatorLayerQuads())];
	pLayer->m_QuadLock = lock_create();
	lock_unlock(m_LayerLock);

	str_copy(pLayer->m_aName, pName, sizeof(pLayer->m_aName));

	return pLayer;
}

CTile *CCreatorLayerTilemap::AddTiles(int Width, int Height)
{
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "add tiles to the layer '%s' twice", m_aName);

	dbg_assert(!m_pTiles, aBuf);

	m_pTiles = new CTile[Width * Height];
	m_Width = Width;
	m_Height = Height;

	return m_pTiles;
}

CCreatorQuad *CCreatorLayerQuads::AddQuad(vec2 Pos, vec2 Size, ColorRGBA Color)
{
	lock_wait(m_QuadLock);
	m_lpQuads.add(new CCreatorQuad());
	CCreatorQuad *pQuad = m_lpQuads[m_lpQuads.size() - 1];
	lock_unlock(m_QuadLock);

	int X0 = f2fx(Pos.x - Size.x / 2.0f);
	int X1 = f2fx(Pos.x + Size.x / 2.0f);
	int XC = f2fx(Pos.x);
	int Y0 = f2fx(Pos.y - Size.y / 2.0f);
	int Y1 = f2fx(Pos.y + Size.y / 2.0f);
	int YC = f2fx(Pos.y);

	pQuad->m_Pos = ivec2(XC, YC);

	pQuad->m_aPoints[0].x = pQuad->m_aPoints[2].x = X0;
	pQuad->m_aPoints[1].x = pQuad->m_aPoints[3].x = X1;
	pQuad->m_aPoints[0].y = pQuad->m_aPoints[1].y = Y0;
	pQuad->m_aPoints[2].y = pQuad->m_aPoints[3].y = Y1;

	for(int i = 0; i < 4; i++)
	{
		pQuad->m_aColors[i] = Color;
	}

	pQuad->m_aTexcoords[0].u = pQuad->m_aTexcoords[2].u = 0;
	pQuad->m_aTexcoords[1].u = pQuad->m_aTexcoords[3].u = 1024;
	pQuad->m_aTexcoords[0].v = pQuad->m_aTexcoords[1].v = 0;
	pQuad->m_aTexcoords[2].v = pQuad->m_aTexcoords[3].v = 1024;
	pQuad->m_pColorEnv = nullptr;
	pQuad->m_pPosEnv = nullptr;

	return pQuad;
}

CCreatorEnvPoint *CCreatorEnvelope::AddEnvPoint(int Time, int CurveType)
{
	lock_wait(m_PointLock);
	m_lpEnvPoints.add(new CCreatorEnvPoint());
	CCreatorEnvPoint *pEnvPoint = m_lpEnvPoints[m_lpEnvPoints.size() - 1];
	lock_unlock(m_PointLock);

	pEnvPoint->m_Time = Time;
	pEnvPoint->m_Curvetype = CurveType;
	pEnvPoint->m_aValues[0] = 0.f;
	pEnvPoint->m_aValues[1] = 0.f;
	pEnvPoint->m_aValues[2] = 0.f;
	pEnvPoint->m_aValues[3] = 0.f;

	return pEnvPoint;
}

static const char *GetMapByMapType(EMapType MapType)
{
	switch(MapType)
	{
		case EMapType::MAPTYPE_NORMAL: return "generatedmaps";
	}
	return "maps";
}

bool CMapCreator::SaveMap(EMapType MapType, const char *pMap)
{
	CDataFileWriter DataFile;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s/%s.map", GetMapByMapType(MapType), pMap);

	if(!DataFile.Open(Storage(), aPath))
	{
		dbg_msg("mapcreater", "failed to open file '%s'...", aPath);
		return false;
	}

	// save version
	{
		CMapItemVersion Item;
		Item.m_Version = CMapItemVersion::CURRENT_VERSION;
		DataFile.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(CMapItemVersion), &Item);
	}

	// save map info
	{
		CMapItemInfo Item;
		Item.m_Version = 1;
		Item.m_Author = -1;
		Item.m_MapVersion = -1;
		Item.m_Credits = -1;
		Item.m_License = -1;

		DataFile.AddItem(MAPITEMTYPE_INFO, 0, sizeof(CMapItemInfo), &Item);
	}

	int NumGroups, NumLayers, NumImages, NumEnvelopes;
	NumGroups = 0;
	NumLayers = 0;
	NumImages = 0;
	NumEnvelopes = 0;

	for(auto &pImage : m_lpImages)
	{
		CMapItemImage Item;
		Item.m_Version = CMapItemImage::CURRENT_VERSION;
		Item.m_MustBe1 = 1;

		Item.m_External = pImage->m_External;
		Item.m_Width = pImage->m_Width;
		Item.m_Height = pImage->m_Height;
		Item.m_ImageName = DataFile.AddData(str_length(pImage->m_aName) + 1, pImage->m_aName);

		if(pImage->m_pImageData)
		{
			Item.m_ImageData = DataFile.AddData(pImage->m_Width * pImage->m_Height * 4, pImage->m_pImageData);
		}
		else
		{
			Item.m_ImageData = -1;
		}
		pImage->m_ImageID = NumImages;

		DataFile.AddItem(MAPITEMTYPE_IMAGE, NumImages++, sizeof(CMapItemImage), &Item);
	}

	// save envelopes
	int PointCount = 0;
	for(auto &pEnv : m_lpEnvelopes)
	{
		CMapItemEnvelope Item;
		Item.m_Version = CMapItemEnvelope::CURRENT_VERSION;
		Item.m_Channels = pEnv->Channels();
		Item.m_StartPoint = PointCount;
		Item.m_NumPoints = pEnv->m_lpEnvPoints.size();
		Item.m_Synchronized = pEnv->m_Synchronized ? 1 : 0;
		StrToInts(Item.m_aName, sizeof(Item.m_aName) / sizeof(int), pEnv->m_aName);

		pEnv->m_EnvID = NumEnvelopes;

		DataFile.AddItem(MAPITEMTYPE_ENVELOPE, NumEnvelopes++, sizeof(CMapItemEnvelope), &Item);
		PointCount += Item.m_NumPoints;
	}

	// save points
	int TotalSize = sizeof(CEnvPoint) * PointCount;
	unsigned char *pPoints = (unsigned char *) mem_alloc(TotalSize);
	int Offset = 0;
	for(auto &pEnv : m_lpEnvelopes)
	{
		for(auto &pPoint : pEnv->m_lpEnvPoints)
		{
			CEnvPoint Point;
			Point.m_aValues[0] = f2fx(pPoint->m_aValues[0]);
			Point.m_aValues[1] = f2fx(pPoint->m_aValues[1]);
			Point.m_aValues[2] = f2fx(pPoint->m_aValues[2]);
			Point.m_aValues[3] = f2fx(pPoint->m_aValues[3]);
			for(int c = 0; c < 4; c++)
			{
				Point.m_aInTangentdx[c] = 0;
				Point.m_aInTangentdy[c] = 0;
				Point.m_aOutTangentdx[c] = 0;
				Point.m_aOutTangentdy[c] = 0;
			}
			Point.m_Curvetype = pPoint->m_Curvetype;
			Point.m_Time = pPoint->m_Time;
			mem_copy(pPoints + Offset, &Point, sizeof(CEnvPoint));
			Offset += sizeof(CEnvPoint);
		}
	}

	DataFile.AddItem(MAPITEMTYPE_ENVPOINTS, 0, TotalSize, pPoints);
	mem_free(pPoints);

	for(auto &pGroup : m_lpGroups)
	{
		// add group
		{
			CMapItemGroup Item;
			Item.m_Version = CMapItemGroup::CURRENT_VERSION;
			Item.m_ParallaxX = pGroup->m_ParallaxX;
			Item.m_ParallaxY = pGroup->m_ParallaxY;
			Item.m_OffsetX = pGroup->m_OffsetX;
			Item.m_OffsetY = pGroup->m_OffsetY;
			Item.m_StartLayer = NumLayers;
			Item.m_NumLayers = (int) pGroup->m_lpLayers.size();
			Item.m_UseClipping = pGroup->m_UseClipping ? 1 : 0;
			Item.m_ClipX = pGroup->m_ClipX;
			Item.m_ClipY = pGroup->m_ClipY;
			Item.m_ClipW = pGroup->m_ClipW;
			Item.m_ClipH = pGroup->m_ClipH;
			StrToInts(Item.m_aName, sizeof(Item.m_aName) / sizeof(int), pGroup->m_aName);

			DataFile.AddItem(MAPITEMTYPE_GROUP, NumGroups++, sizeof(CMapItemGroup), &Item);
		}

		for(auto &pLayer : pGroup->m_lpLayers)
		{
			if(pLayer->m_Type == ELayerType::TILES)
			{
				CCreatorLayerTilemap *pTilemap = (CCreatorLayerTilemap *) pLayer;

				CMapItemLayerTilemap Item;

				Item.m_Version = CMapItemLayerTilemap::CURRENT_VERSION;

				Item.m_Layer.m_Version = 0;
				Item.m_Layer.m_Flags = pLayer->m_Flags;
				Item.m_Layer.m_Type = LAYERTYPE_TILES;

				Item.m_Color.r = pTilemap->m_Color.r;
				Item.m_Color.g = pTilemap->m_Color.g;
				Item.m_Color.b = pTilemap->m_Color.b;
				Item.m_Color.a = pTilemap->m_Color.a;

				Item.m_ColorEnv = pTilemap->m_pColorEnv ? pTilemap->m_pColorEnv->m_EnvID : -1;
				Item.m_ColorEnvOffset = 0;

				Item.m_Width = pTilemap->m_Width;
				Item.m_Height = pTilemap->m_Height;
				Item.m_Flags = pTilemap->m_Flags;
				Item.m_Image = pTilemap->m_pImage ? pTilemap->m_pImage->m_ImageID : -1;

				Item.m_Data = DataFile.AddData(Item.m_Width * Item.m_Height * sizeof(CTile), pTilemap->m_pTiles);

				StrToInts(Item.m_aName, sizeof(Item.m_aName) / sizeof(int), pTilemap->m_aName);

				DataFile.AddItem(MAPITEMTYPE_LAYER, NumLayers++, sizeof(CMapItemLayerTilemap), &Item);
			}
			else if(pLayer->m_Type == ELayerType::QUADS)
			{
				CCreatorLayerQuads *pQuads = (CCreatorLayerQuads *) pLayer;

				CMapItemLayerQuads Item;

				Item.m_Version = CMapItemLayerQuads::CURRENT_VERSION;

				Item.m_Layer.m_Version = 0;
				Item.m_Layer.m_Flags = pLayer->m_Flags;
				Item.m_Layer.m_Type = LAYERTYPE_QUADS;

				Item.m_Image = pQuads->m_pImage ? pQuads->m_pImage->m_ImageID : -1;
				Item.m_NumQuads = pQuads->m_lpQuads.size();

				array<CQuad> lQuads;
				lQuads.hint_size(pQuads->m_lpQuads.size());
				for(auto &pOriginQuad : pQuads->m_lpQuads)
				{
					CQuad *pQuad = &lQuads.emplace();

					for(int i = 0; i < 4; i++)
					{
						pQuad->m_aColors[i].r = pOriginQuad->m_aColors[i].r;
						pQuad->m_aColors[i].g = pOriginQuad->m_aColors[i].g;
						pQuad->m_aColors[i].b = pOriginQuad->m_aColors[i].b;
						pQuad->m_aColors[i].a = pOriginQuad->m_aColors[i].a;

						pQuad->m_aPoints[i].x = pOriginQuad->m_aPoints[i].x;
						pQuad->m_aPoints[i].y = pOriginQuad->m_aPoints[i].y;

						pQuad->m_aTexcoords[i].x = pOriginQuad->m_aTexcoords[i].x;
						pQuad->m_aTexcoords[i].y = pOriginQuad->m_aTexcoords[i].y;
					}

					pQuad->m_aPoints[4].x = pOriginQuad->m_Pos.x;
					pQuad->m_aPoints[4].y = pOriginQuad->m_Pos.y;

					pQuad->m_ColorEnv = pOriginQuad->m_pColorEnv ? pOriginQuad->m_pColorEnv->m_EnvID : -1;
					pQuad->m_ColorEnvOffset = 0;

					pQuad->m_PosEnv = pOriginQuad->m_pPosEnv ? pOriginQuad->m_pPosEnv->m_EnvID : -1;
					pQuad->m_PosEnvOffset = 0;
				}

				StrToInts(Item.m_aName, sizeof(Item.m_aName) / sizeof(int), pQuads->m_aName);
				Item.m_Data = DataFile.AddDataSwapped(lQuads.size() * sizeof(CQuad), lQuads.base_ptr());

				DataFile.AddItem(MAPITEMTYPE_LAYER, NumLayers++, sizeof(CMapItemLayerQuads), &Item);
			}
		}
	}

	DataFile.Finish();

	return true;
}
