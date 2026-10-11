/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_NOISE_H
#define ENGINE_SERVER_NOISE_H

// Deterministic Perlin noise for the world generator. Every function is pure in
// (seed, coordinates), so the same seed always yields the same tiles; the
// permutation table is shuffled from the seed rather than from global state.
namespace noise
{

// the reference implementation's fade curve: 6t^5 - 15t^4 + 10t^3
float Fade(float t);

// linear interpolation
float Lerp(float a, float b, float t);

// classic Perlin gradient noise; the result is roughly in [-0.5, 0.5]
float Perlin2D(unsigned Seed, float X, float Y);
float Perlin3D(unsigned Seed, float X, float Y, float Z);

// fractal (fBm) noise: Octaves layers of Perlin noise, each one Lacunarity
// times more frequent and Persistence times weaker.
//
// The sum is renormalized by the *measured* amplitude, not the theoretical
// maximum a Perlin sum never reaches, so the result really spans about [-1, 1].
// Without that a 5 octave field would only swing +-0.17 and the terrain would be
// nearly flat. m_Amplitude overrides the measurement; 0 means "use the built in
// one".
struct CFractalParams
{
	int m_Octaves;
	float m_Frequency; // frequency of the first octave
	float m_Lacunarity; // frequency multiplier per octave (usually 2.0)
	float m_Persistence; // amplitude multiplier per octave (usually 0.5)
	float m_Amplitude; // empirical full scale amplitude, 0 = derive from Octaves
};

// the measured p99 amplitude of a Perlin fBm sum with the given octave count,
// used when CFractalParams::m_Amplitude is left at 0
float FractalAmplitude2D(const CFractalParams &rParams);
float FractalAmplitude3D(const CFractalParams &rParams);

float Fractal2D(unsigned Seed, const CFractalParams &rParams, float X, float Y);
float Fractal3D(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Z);

// Periodic variants, for the cylindrical worlds whose left and right edges are
// the same place.
//
// X is sampled on a circle of circumference Period, so the value at X equals the
// value at X + Period by construction. That is what makes the terrain meet
// itself seamlessly at the seam; patching the heightmap afterwards would leave a
// visible kink. Period is in the same unit as X (tiles); PeriodOffset rotates
// the pattern without breaking it.
float Fractal2DPeriodic(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Period, float PeriodOffset = 0.0f);
float Fractal3DPeriodic(unsigned Seed, const CFractalParams &rParams, float X, float Y, float Z, float Period, float PeriodOffset = 0.0f);

// maps a noise value from [-1, 1] to [0, 1]
float ToUnit(float Value);

} // namespace noise

#endif // ENGINE_SERVER_NOISE_H
