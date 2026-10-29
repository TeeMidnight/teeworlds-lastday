/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef BASE_TL_TIMER_H
#define BASE_TL_TIMER_H

/*
	Timer: tick_timer

	A deadline on a tick clock instead of a counter that is decremented from
	Tick(). The clock is passed in by the caller (usually Server()->Tick()),
	so a consumer that was not simulated for a while - a world frozen because
	nobody is in it - does not leave a stale countdown behind. As soon as it
	runs again, every deadline is compared against the current tick and the
	timer either reports active or already expired.

	A timer armed with N ticks expires when the clock reaches Now + N. An
	infinite timer never expires; a stopped timer (the default) is expired.
*/
class tick_timer
{
public:
	enum
	{
		INFINITE = -1,
	};

	tick_timer() :
		m_Deadline(0) {}

	explicit tick_timer(int Deadline) :
		m_Deadline(Deadline) {}

	// Arm the timer to expire Ticks after Now. A negative or zero tick count
	// expires immediately.
	void start(int Now, int Ticks)
	{
		m_Deadline = Now + (Ticks > 0 ? Ticks : 0);
	}

	void start_at(int Deadline) { m_Deadline = Deadline; }
	void stop() { m_Deadline = 0; }
	void set_infinite() { m_Deadline = INFINITE; }

	bool is_infinite() const { return m_Deadline == INFINITE; }
	bool active(int Now) const { return is_infinite() || Now < m_Deadline; }
	bool expired(int Now) const { return !is_infinite() && Now >= m_Deadline; }
	int remaining(int Now) const
	{
		if(is_infinite())
			return INFINITE;
		return m_Deadline > Now ? m_Deadline - Now : 0;
	}
	int deadline() const { return m_Deadline; }

private:
	int m_Deadline;
};

#endif // BASE_TL_TIMER_H
