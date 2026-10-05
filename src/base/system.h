/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/*
	Compatibility umbrella for the old monolithic <base/system.h>.

	The base layer was split into one module per topic under base/system/
	when the engine moved to C++. This header keeps the historical include
	path working so existing translation units do not all have to be
	rewritten at once.
*/
#ifndef BASE_SYSTEM_H
#define BASE_SYSTEM_H

#include <base/system/base.h>
#include <base/system/debug.h>
#include <base/system/mem.h>
#include <base/system/string.h>
#include <base/system/io.h>
#include <base/system/fs.h>
#include <base/system/time.h>
#include <base/system/thread.h>
#include <base/system/lock.h>
#include <base/system/semaphore.h>
#include <base/system/net.h>
#include <base/system/misc.h>
#include <base/system/aio.h>

#endif // BASE_SYSTEM_H
