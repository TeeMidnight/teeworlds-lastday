/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/tl/sorted_array.h>

#include <gtest/gtest.h>

TEST(SortedArray, SortEmptyRange)
{
	sorted_array<int> x;
	x.sort_range();
}

// tl::sort takes the bounds from front() and back(), which do not exist on an
// empty range: sorting nothing has to stay a no-op, exactly like the
// std::sort(begin, end) it replaced.
TEST(SortedArray, SortEmptyArray)
{
	array<int> v;
	sort(v.all());
	EXPECT_EQ(v.size(), 0);
}

TEST(SortedArray, SortEmptyArrayWithComparator)
{
	array<int> v;
	sort(v.all(), [](int a, int b)
	{ return a < b; });
	EXPECT_EQ(v.size(), 0);
}

TEST(SortedArray, SortEmptyRangeBy)
{
	sorted_array<int> x;
	x.sort_range_by([](int a, int b)
	{ return a < b; });
	EXPECT_EQ(x.size(), 0);
}

TEST(SortedArray, SortArray)
{
	array<int> v;
	v.add(3);
	v.add(1);
	v.add(2);
	sort(v.all());
	EXPECT_EQ(v.size(), 3);
	EXPECT_EQ(v[0], 1);
	EXPECT_EQ(v[1], 2);
	EXPECT_EQ(v[2], 3);
}
