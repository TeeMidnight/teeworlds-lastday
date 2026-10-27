/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>

#include "noise.h"

#include <math.h>

namespace noise {

// the permutation table is built once per seed; N is a power of two so the
// wrap around is a cheap mask instead of a modulo
enum
{
	PERM_SIZE = 256,
	PERM_MASK = PERM_SIZE - 1,
};

// Derives permutation entries from the seed. Local so the noise module does not
// depend on the generator.
static unsigned SeedHash(unsigned Seed, unsigned Salt)
{
	unsigned h = Seed * 0x9E3779B1u;
	h ^= Salt * 0x85EBCA6Bu;
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

// Fisher-Yates shuffled [0, 255] followed by a copy of itself, so lookups never
// need a second wrap.
static void BuildPermutation(unsigned Seed, int *pPerm)
{
	for(int i = 0; i < PERM_SIZE; i++)
		pPerm[i] = i;

	for(int i = PERM_SIZE - 1; i > 0; i--)
	{
		const int j = (int) (SeedHash(Seed, (unsigned) i + 1u) % (unsigned) (i + 1));
		const int Tmp = pPerm[i];
		pPerm[i] = pPerm[j];
		pPerm[j] = Tmp;
	}

	for(int i = 0; i < PERM_SIZE; i++)
		pPerm[PERM_SIZE + i] = pPerm[i];
}

// The permutation table only depends on Seed, and the generator evaluates the
// same handful of seeds for every one of the 200k tiles in a floor. Building
// the table inside every Perlin call made generation spend most of its time
// shuffling 256 entries; keep one table per distinct seed instead.
//
// The cache is thread local: the server generates on its worker thread and the
// tools generate on the main thread, and the tables are immutable once built.
struct SPermCacheEntry
{
	bool m_Valid;
	unsigned m_Seed;
	int m_aPerm[PERM_SIZE * 2];
};

static const int *CachedPermutation(unsigned Seed)
{
	enum
	{
		CACHE_SIZE = 128,
		CACHE_PROBES = 16
	};
	static thread_local SPermCacheEntry s_aCache[CACHE_SIZE];

	// Open addressing instead of a direct-mapped slot: two hot seeds that hash
	// to the same slot must not evict each other on every single sample.
	const unsigned Start = Seed & (CACHE_SIZE - 1);
	for(unsigned i = 0; i < CACHE_PROBES; i++)
	{
		SPermCacheEntry &Entry = s_aCache[(Start + i) & (CACHE_SIZE - 1)];
		if(Entry.m_Valid && Entry.m_Seed == Seed)
			return Entry.m_aPerm;
		if(!Entry.m_Valid)
		{
			BuildPermutation(Seed, Entry.m_aPerm);
			Entry.m_Seed = Seed;
			Entry.m_Valid = true;
			return Entry.m_aPerm;
		}
	}

	// The probe window is full (far more distinct seeds than a floor uses):
	// replace the natural slot rather than scanning the whole table.
	SPermCacheEntry &Entry = s_aCache[Start];
	BuildPermutation(Seed, Entry.m_aPerm);
	Entry.m_Seed = Seed;
	Entry.m_Valid = true;
	return Entry.m_aPerm;
}

// the 12 gradient directions of the classic 3D implementation
static float Grad3D(int Hash, float X, float Y, float Z)
{
	// low 4 bits: 12 of the 16 cases are the edge gradients, 4 repeat them
	const int h = Hash & 15;
	const float u = h < 8 ? X : Y;
	const float v = h < 4 ? Y : (h == 12 || h == 14 ? X : Z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

static float Grad2D(int Hash, float X, float Y)
{
	// 4 diagonal directions
	switch(Hash & 3)
	{
		case 0: return X + Y;
		case 1: return -X + Y;
		case 2: return X - Y;
		default: return -X - Y;
	}
}

float Fade(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float Lerp(float a, float b, float t)
{
	return a + t * (b - a);
}

// floor to int; the C library's floor disagrees with a cast on negatives
static int FastFloor(float Value)
{
	const int i = (int) Value;
	return Value < (float) i ? i - 1 : i;
}

// wraps a positive/negative lattice coordinate into [0, PERM_SIZE)
static int WrapCoord(int Value)
{
	return Value & PERM_MASK;
}

float Perlin2D(unsigned Seed, float X, float Y)
{
	const int *aPerm = CachedPermutation(Seed);

	const int Xi = FastFloor(X);
	const int Yi = FastFloor(Y);
	const float Xf = X - (float) Xi;
	const float Yf = Y - (float) Yi;

	const int X0 = WrapCoord(Xi);
	const int Y0 = WrapCoord(Yi);
	const int X1 = WrapCoord(Xi + 1);
	const int Y1 = WrapCoord(Yi + 1);

	const int A = aPerm[X0] + Y0;
	const int B = aPerm[X1] + Y0;
	const int C = aPerm[X0] + Y1;
	const int D = aPerm[X1] + Y1;

	const float U = Fade(Xf);
	const float V = Fade(Yf);

	const float X1f = Xf - 1.0f;
	const float Y1f = Yf - 1.0f;

	const float Value = Lerp(
		Lerp(Grad2D(aPerm[A], Xf, Yf), Grad2D(aPerm[B], X1f, Yf), U),
		Lerp(Grad2D(aPerm[C], Xf, Y1f), Grad2D(aPerm[D], X1f, Y1f), U),
		V);

	// the 2D version sums two unit gradients, so the raw range is wider than
	// [-1, 1]; scale it back
	return Value * 0.5f;
}

float Perlin3D(unsigned Seed, float X, float Y, float Z)
{
	const int *aPerm = CachedPermutation(Seed);

	const int Xi = FastFloor(X);
	const int Yi = FastFloor(Y);
	const int Zi = FastFloor(Z);
	const float Xf = X - (float) Xi;
	const float Yf = Y - (float) Yi;
	const float Zf = Z - (float) Zi;

	const int X0 = WrapCoord(Xi);
	const int Y0 = WrapCoord(Yi);
	const int Z0 = WrapCoord(Zi);
	const int X1 = WrapCoord(Xi + 1);
	const int Y1 = WrapCoord(Yi + 1);
	const int Z1 = WrapCoord(Zi + 1);

	const int A = aPerm[X0] + Y0;
	const int B = aPerm[X1] + Y0;
	const int C = aPerm[X0] + Y1;
	const int D = aPerm[X1] + Y1;
	const int AA = aPerm[A] + Z0;
	const int AB = aPerm[B] + Z0;
	const int AC = aPerm[A] + Z1;
	const int AD = aPerm[B] + Z1;
	const int BA = aPerm[C] + Z0;
	const int BB = aPerm[D] + Z0;
	const int BC = aPerm[C] + Z1;
	const int BD = aPerm[D] + Z1;

	const float U = Fade(Xf);
	const float V = Fade(Yf);
	const float W = Fade(Zf);

	const float X1f = Xf - 1.0f;
	const float Y1f = Yf - 1.0f;
	const float Z1f = Zf - 1.0f;

	const float Value = Lerp(
		Lerp(
			Lerp(Grad3D(aPerm[AA], Xf, Yf, Zf), Grad3D(aPerm[BA], X1f, Yf, Zf), U),
			Lerp(Grad3D(aPerm[AB], Xf, Y1f, Zf), Grad3D(aPerm[BB], X1f, Y1f, Zf), U),
			V),
		Lerp(
			Lerp(Grad3D(aPerm[AC], Xf, Yf, Z1f), Grad3D(aPerm[BC], X1f, Yf, Z1f), U),
			Lerp(Grad3D(aPerm[AD], Xf, Y1f, Z1f), Grad3D(aPerm[BD], X1f, Y1f, Z1f), U),
			V),
		W);

	// the 3D gradients reach about 1.5 in the worst case (2 * sqrt(2) / 2 for
	// the axis aligned ones), this brings the result back near [-1, 1]
	return Value * 0.66f;
}

float FractalAmplitude2D(const CFractalParams &rParams)
{
	// Measured full scale amplitude of the Perlin fBm sum per octave count: one
	// octave of 2D Perlin spans about +-0.25 and each extra octave adds roughly
	// half again. Counts past the table creep towards the 1/(1-persistence) limit.
	static const float s_aAmplitude[] = {0.25f, 0.32f, 0.37f, 0.41f, 0.44f, 0.46f, 0.48f};
	const int Octaves = maximum(1, rParams.m_Octaves);
	if(Octaves < (int) (sizeof(s_aAmplitude) / sizeof(s_aAmplitude[0])))
		return s_aAmplitude[Octaves - 1];
	return s_aAmplitude[sizeof(s_aAmplitude) / sizeof(s_aAmplitude[0]) - 1];
}

float FractalAmplitude3D(const CFractalParams &rParams)
{
	// 3D gradients reach further than 2D ones, so the measured scale is larger
	static const float s_aAmplitude[] = {0.42f, 0.55f, 0.63f, 0.68f, 0.71f, 0.74f, 0.76f};
	const int Octaves = maximum(1, rParams.m_Octaves);
	if(Octaves < (int) (sizeof(s_aAmplitude) / sizeof(s_aAmplitude[0])))
		return s_aAmplitude[Octaves - 1];
	return s_aAmplitude[sizeof(s_aAmplitude) / sizeof(s_aAmplitude[0]) - 1];
}

float Fractal2D(unsigned Seed, const CFractalParams &rParams, float X, float Y)
{
	const int Octaves = maximum(1, rParams.m_Octaves);
	float Frequency = rParams.m_Frequency;
	float Amplitude = 1.0f;
	float Sum = 0.0f;

	for(int i = 0; i < Octaves; i++)
	{
		// own seed per octave, so the layers do not line up
		Sum += Perlin2D(Seed + (unsigned) i * 0x9E3779B1u, X * Frequency, Y * Frequency) * Amplitude;
		Frequency *= rParams.m_Lacunarity;
		Amplitude *= rParams.m_Persistence;
	}

	const float Scale = rParams.m_Amplitude > 0.0f ? rParams.m_Amplitude : FractalAmplitude2D(rParams);
	if(Scale <= 0.0f)
		return 0.0f;
	return clamp(Sum / Scale, -1.0f, 1.0f);
}

float Fractal3D(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Z)
{
	const int Octaves = maximum(1, rParams.m_Octaves);
	float Frequency = rParams.m_Frequency;
	float Amplitude = 1.0f;
	float Sum = 0.0f;

	for(int i = 0; i < Octaves; i++)
	{
		Sum += Perlin3D(Seed + (unsigned) i * 0x9E3779B1u, X * Frequency, Y * Frequency, Z * Frequency) * Amplitude;
		Frequency *= rParams.m_Lacunarity;
		Amplitude *= rParams.m_Persistence;
	}

	const float Scale = rParams.m_Amplitude > 0.0f ? rParams.m_Amplitude : FractalAmplitude3D(rParams);
	if(Scale <= 0.0f)
		return 0.0f;
	return clamp(Sum / Scale, -1.0f, 1.0f);
}

float ToUnit(float Value)
{
	return clamp((Value + 1.0f) * 0.5f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// periodic (cylindrical) sampling
// ---------------------------------------------------------------------------

// Maps X onto a circle of circumference Period, in the noise's own coordinates.
//
// The noise samples X at X * Frequency, so walking Period tiles must be one full
// turn in noise space: the radius is Period * Frequency / 2pi. Getting that wrong
// makes the circle tiny against the noise lattice and flattens the whole field.
struct SCircle
{
	float m_X; // first circle axis, in noise space
	float m_Y; // second circle axis
};

static SCircle CircleAt(float X, float Frequency, float Period, float PeriodOffset)
{
	if(Period <= 0.0f)
		return SCircle{X * Frequency, 0.0f};

	// one full turn per Period tiles, expressed in the noise's coordinates
	const float Radius = (Period * Frequency) / 6.28318530718f;

	// Wrap the position onto the turn first. Sampling X = Period gives exactly
	// the angle of X = 0 instead of one that merely rounds back to it, which is
	// what lets a ring's closing seam (instance Size meets instance 1) match
	// tile for tile rather than approximately. Integer-valued coordinates (the
	// generator always samples whole tiles) make this exact.
	float Position = fmodf(X + PeriodOffset, Period);
	if(Position < 0.0f)
		Position += Period;
	const float Angle = (Position / Period) * 6.28318530718f;
	SCircle Circle;
	Circle.m_X = cosf(Angle) * Radius;
	Circle.m_Y = sinf(Angle) * Radius;
	return Circle;
}

float Fractal2DPeriodic(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Period, float PeriodOffset)
{
	const int Octaves = maximum(1, rParams.m_Octaves);
	float Frequency = rParams.m_Frequency;
	float Amplitude = 1.0f;
	float Sum = 0.0f;

	for(int i = 0; i < Octaves; i++)
	{
		// radius is derived per octave, so higher octaves keep detail while
		// every one still closes the loop over Period
		const SCircle Circle = CircleAt(X, Frequency, Period, PeriodOffset);
		Sum += Perlin2D(Seed + (unsigned) i * 0x9E3779B1u,
			       Circle.m_X, Circle.m_Y) *
		       Amplitude;
		Frequency *= rParams.m_Lacunarity;
		Amplitude *= rParams.m_Persistence;
	}

	const float Scale = rParams.m_Amplitude > 0.0f ? rParams.m_Amplitude : FractalAmplitude2D(rParams);
	if(Scale <= 0.0f)
		return 0.0f;
	return clamp(Sum / Scale, -1.0f, 1.0f);
}

float Fractal3DPeriodic(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Z, float Period, float PeriodOffset)
{
	const int Octaves = maximum(1, rParams.m_Octaves);
	float Frequency = rParams.m_Frequency;
	float Amplitude = 1.0f;
	float Sum = 0.0f;

	for(int i = 0; i < Octaves; i++)
	{
		// the circle spans X/Z, so the wrap runs along the map's horizontal
		// axis while Y stays vertical
		const SCircle Circle = CircleAt(X, Frequency, Period, PeriodOffset);
		Sum += Perlin3D(Seed + (unsigned) i * 0x9E3779B1u,
			       Circle.m_X, Y * Frequency, Circle.m_Y) *
		       Amplitude;
		Frequency *= rParams.m_Lacunarity;
		Amplitude *= rParams.m_Persistence;
	}

	const float Scale = rParams.m_Amplitude > 0.0f ? rParams.m_Amplitude : FractalAmplitude3D(rParams);
	if(Scale <= 0.0f)
		return 0.0f;
	return clamp(Sum / Scale, -1.0f, 1.0f);
}

} // namespace noise
