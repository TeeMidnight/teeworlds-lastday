#ifndef GAME_SERVER_DATABASE_ACCOUNT_H
#define GAME_SERVER_DATABASE_ACCOUNT_H

#include <base/system.h>

// Hash a plaintext password into the "salt_hex$hash_hex" form that is stored
// in the database. Uses PBKDF2-HMAC-SHA256 with a random per-password
// salt.
void HashPassword(char *pOut, int Size, const char *pPassword);

// Verify a plaintext password against a stored hash (constant-time compare).
// Returns true when they match.
bool VerifyPassword(const char *pStored, const char *pPassword);

#endif
