/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_CURRENCY_H
#define GAME_SERVER_CURRENCY_H

#include <base/system.h>

#include <errno.h>
#include <stdlib.h>

// Currency (unit: G) is not an item and has no in-memory wallet. Every change
// is a row in the currency_ledger table: a signed int64 delta plus a short
// reason that says what the operation did. The balance of an account is
// always SUM(delta), so debits and credits are auditable and cannot drift.
constexpr int64 CURRENCY_MAX_AMOUNT = 1000000000000LL; // 1e12 G per operation
constexpr int CURRENCY_REASON_SIZE = 192;

// Parse a positive whole amount typed by a player. Returns false for anything
// that is not a plain number in [1, CURRENCY_MAX_AMOUNT].
inline bool CurrencyParse(const char *pText, int64 *pOut)
{
	if(!pText || !pText[0] || !pOut)
		return false;

	errno = 0;
	char *pEnd = 0;
	const long long Value = strtoll(pText, &pEnd, 10);
	if(errno == ERANGE || pEnd == pText || !pEnd)
		return false;
	// the vote reason box may have trailing whitespace
	while(*pEnd == ' ' || *pEnd == '\t' || *pEnd == '\r' || *pEnd == '\n')
		pEnd++;
	if(*pEnd != '\0')
		return false;
	if(Value <= 0 || (int64) Value > CURRENCY_MAX_AMOUNT)
		return false;

	*pOut = (int64) Value;
	return true;
}

#endif // GAME_SERVER_CURRENCY_H
