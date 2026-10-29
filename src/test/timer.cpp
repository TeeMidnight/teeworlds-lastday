/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/tl/timer.h>

#include <gtest/gtest.h>

TEST(TickTimer, StoppedTimerIsExpired)
{
	tick_timer Timer;
	EXPECT_FALSE(Timer.active(0));
	EXPECT_TRUE(Timer.expired(0));
	EXPECT_EQ(Timer.remaining(0), 0);
}

TEST(TickTimer, ExpiresAtTheDeadline)
{
	tick_timer Timer;
	Timer.start(10, 5);
	EXPECT_TRUE(Timer.active(10));
	EXPECT_TRUE(Timer.active(14));
	EXPECT_EQ(Timer.remaining(10), 5);
	EXPECT_EQ(Timer.remaining(14), 1);
	EXPECT_FALSE(Timer.expired(14));
	EXPECT_TRUE(Timer.expired(15));
	EXPECT_EQ(Timer.remaining(15), 0);
}

TEST(TickTimer, ZeroDurationExpiresImmediately)
{
	tick_timer Timer;
	Timer.start(10, 0);
	EXPECT_FALSE(Timer.active(10));
	EXPECT_TRUE(Timer.expired(10));
}

// A frozen world skips entity ticks but the server clock keeps moving. An
// absolute deadline reports expired instead of leaving a stale countdown.
TEST(TickTimer, LongGapDoesNotLeaveAStaleCountdown)
{
	tick_timer Timer;
	Timer.start(100, 30);
	EXPECT_TRUE(Timer.active(129));
	EXPECT_TRUE(Timer.expired(10000));
	EXPECT_EQ(Timer.remaining(10000), 0);
}

TEST(TickTimer, InfiniteAndStopped)
{
	tick_timer Timer;
	Timer.set_infinite();
	EXPECT_TRUE(Timer.is_infinite());
	EXPECT_TRUE(Timer.active(0));
	EXPECT_TRUE(Timer.active(1 << 30));
	EXPECT_FALSE(Timer.expired(1 << 30));
	EXPECT_EQ(Timer.remaining(123), tick_timer::INFINITE);

	Timer.stop();
	EXPECT_FALSE(Timer.is_infinite());
	EXPECT_FALSE(Timer.active(123));
	EXPECT_TRUE(Timer.expired(123));
}
