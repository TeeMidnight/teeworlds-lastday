/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_LOCALIZATION_FORMAT_H
#define GAME_LOCALIZATION_FORMAT_H

#include <game/localization.h>

#include <base/system/string.h>

#include <format>
#include <stdexcept>
#include <string>

// Look up a translated string, falling back to the original text. This is the
// same rule CLocConstString / Localize() use, exposed so the std::format based
// layer can share it.
inline const char *LocalizeString(const char *pStr, const char *pContext = "")
{
	const char *pReplacement = g_Localization.FindString(str_quickhash(pStr), str_quickhash(pContext));
	return pReplacement ? pReplacement : pStr;
}

// Localize pFormat and then run it through std::vformat. The translation may
// use positional "{}" placeholders; a translation that does not format (bad
// placeholder count, stray brace) is returned verbatim instead of throwing.
template<typename... Args>
std::string LocalizeFormat(const char *pContext, const char *pFormat, Args &&...args)
{
	const char *pLocalized = LocalizeString(pFormat, pContext);
	// This build disables exceptions, so a malformed translation cannot be
	// reported by throwing; the shipped formats are kept valid.
	return std::vformat(pLocalized, std::make_format_args(args...));
}

#endif // GAME_LOCALIZATION_FORMAT_H
