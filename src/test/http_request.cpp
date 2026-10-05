/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "test.h"
#include <gtest/gtest.h>

#include <base/system.h>

#include <engine/shared/http_request.h>

// PostData must replace the body, not append to it; memory_stream appends.
TEST(HttpRequest, PostDataReplacesInsteadOfAppending)
{
	CHttpRequest Request("POST", "https://example.invalid/", 5L, HTTP_IPRESOLVE_BOTH);
	ASSERT_EQ(Request.PostDataSize(), 0);

	Request.PostData((const unsigned char *) "first", 5);
	ASSERT_EQ(Request.PostDataSize(), 5);
	EXPECT_EQ(mem_comp(Request.PostData(), "first", 5), 0);

	// A second body must replace the first, not extend it.
	Request.PostData((const unsigned char *) "second", 6);
	EXPECT_EQ(Request.PostDataSize(), 6) << "a stale body was appended instead of replaced";
	EXPECT_EQ(mem_comp(Request.PostData(), "second", 6), 0);
}

// PostJson goes through PostData, so the same guarantee applies to it.
TEST(HttpRequest, PostJsonReplacesInsteadOfAppending)
{
	CHttpRequest Request("POST", "https://example.invalid/", 5L, HTTP_IPRESOLVE_BOTH);

	Request.PostJson("{\"a\":1}");
	const int FirstSize = Request.PostDataSize();
	ASSERT_EQ(FirstSize, 7);

	Request.PostJson("{\"b\":2}");
	EXPECT_EQ(Request.PostDataSize(), 7) << "the first JSON body was not replaced";
	EXPECT_EQ(mem_comp(Request.PostData(), "{\"b\":2}", 7), 0);
}

// PostJson must send exactly the JSON text and not a trailing NUL byte, which
// would make the body invalid for a strict JSON parser.
TEST(HttpRequest, PostJsonHasNoTrailingNul)
{
	CHttpRequest Request("POST", "https://example.invalid/", 5L, HTTP_IPRESOLVE_BOTH);
	Request.PostJson("{}");
	ASSERT_EQ(Request.PostDataSize(), 2);
	EXPECT_EQ(mem_comp(Request.PostData(), "{}", 2), 0);
}

// A request that never calls PostData must send an empty body.
TEST(HttpRequest, FreshRequestHasEmptyBody)
{
	CHttpRequest Request("POST", "https://example.invalid/", 5L, HTTP_IPRESOLVE_BOTH);
	EXPECT_EQ(Request.PostDataSize(), 0);
	EXPECT_EQ(Request.ReceivedDataSize(), 0);
}
