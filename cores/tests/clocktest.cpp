/*
 * Cartridge-clock test for the paired core.
 *
 * A cart with an RTC asks what time it is and the game turns the answer into
 * emulated state, so on two mirrored devices the answer has to be a function of
 * emulated progress rather than of either machine's wall clock. The paired core
 * arranges that by giving each console an epoch and advancing from it by frames.
 *
 * The arithmetic underneath is small and was wrong: the wrapper's clock started
 * at 0 while setInitState seeded cartridges at DUAL_POWER_ON_EPOCH, so the very
 * first `now() - baseTime` underflowed a uint64 and the game saw a nonsense
 * date. Nothing caught it, because the shim's fake core has no RTC and the real
 * one only runs on a handheld with a linking game in it.
 *
 * This drives the real Rtc against a real TimeSource, on the host, with no ROM
 * and no device.
 */
#include "mem/rtc.h"
#include "savestate.h"
#include "timesource.h"

#include <cstdio>
#include <cstring>
#include <stdint.h>

namespace {

int failures;

void check(bool ok, const char *what) {
	std::printf("  %s %s\n", ok ? "ok  " : "MISS", what);
	if (!ok) failures++;
}

void check_eq(uint64_t got, uint64_t want, const char *what) {
	if (got == want) {
		std::printf("  ok   %s\n", what);
	} else {
		std::printf("  MISS %s: got %llu want %llu\n", what,
		            (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

const uint64_t CYCLES_PER_FRAME = 70224;
const uint64_t CYCLES_PER_SECOND = 4194304;

/* The wrapper's clock, reproduced exactly: an epoch plus emulated frames. The
 * frame counter is shared because both consoles advance through the same paired
 * frames. */
uint64_t g_frames;

class PairedClock : public gambatte::TimeSource {
public:
	PairedClock() : epoch_(gambatte::DUAL_POWER_ON_EPOCH) {}
	uint64_t epoch() const { return epoch_; }
	virtual uint64_t now() const {
		return epoch_ + g_frames * CYCLES_PER_FRAME / CYCLES_PER_SECOND;
	}
	/* setEpoch and the cartridge rebase always travel together, which is the
	 * property that keeps elapsed time unchanged when the epoch moves. */
	int64_t adopt(uint64_t epoch) {
		const int64_t shift = (int64_t)epoch - (int64_t)epoch_;
		epoch_ = epoch;
		return shift;
	}
private:
	uint64_t epoch_;
};

/* One emulated console: its clock and its cartridge. */
struct Console {
	PairedClock clock;
	gambatte::Rtc rtc;

	Console() {
		rtc.setTimeSource(&clock);
		powerOn();
	}

	/* What setInitState does to the RTC fields, which is all this test needs
	 * of it: both bases start at the fixed power-on epoch. */
	void powerOn() {
		rtc.setBaseTime(gambatte::DUAL_POWER_ON_EPOCH);
		rtc.set(true, 8);
	}

	void adopt(uint64_t epoch) {
		rtc.shiftBase(clock.adopt(epoch));
	}

	/* Seconds the cartridge believes have passed - what the game reads back
	 * after latching. */
	uint64_t elapsed() {
		rtc.latch(0);
		rtc.latch(1);
		uint64_t seconds = 0;
		for (unsigned reg = 8; reg <= 12; reg++) {
			rtc.set(true, reg);
			const unsigned v = *rtc.getActive();
			switch (reg) {
			case 8:  seconds += v; break;
			case 9:  seconds += (uint64_t)v * 60; break;
			case 10: seconds += (uint64_t)v * 3600; break;
			case 11: seconds += (uint64_t)v * 86400; break;
			case 12: seconds += (uint64_t)(v & 1) * 256 * 86400; break;
			}
		}
		return seconds;
	}
};

/* Emulated time is counted in frames, so a request in seconds is rounded to
 * whole frames and read back through the same integer conversion. The round
 * trip can come up a second short - a frame is not a whole number of seconds -
 * which is granularity rather than drift: it is the same arithmetic on both
 * devices, and both land on the same second. Returns what the cartridge will
 * actually read, so the tests below assert against the real granularity instead
 * of pretending it is not there. */
uint64_t advance_seconds(unsigned seconds) {
	g_frames += (uint64_t)seconds * CYCLES_PER_SECOND / CYCLES_PER_FRAME;
	return g_frames * CYCLES_PER_FRAME / CYCLES_PER_SECOND;
}

/* Two handhelds, nine seconds apart, running the same pair. */
const uint64_t HOST_CLOCK   = 1786695747ull;
const uint64_t CLIENT_CLOCK = 1786695756ull;

} // namespace

int main() {
	std::printf("== paired cartridge clock\n");

	/* A cartridge that has just powered on has been running for no time at all.
	 * This is the case that underflowed: the clock read 0 while the cartridge's
	 * base was the power-on epoch, so the subtraction wrapped. */
	{
		g_frames = 0;
		Console c;
		check_eq(c.elapsed(), 0, "a freshly powered cartridge reads zero elapsed");
	}

	/* Adopting a real epoch is not the cartridge being left running for six
	 * years. It moves the clock and the cartridge's origin together. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		check_eq(c.elapsed(), 0, "adopting a real epoch leaves elapsed at zero");
		check_eq(c.clock.now(), HOST_CLOCK, "the clock now reads the player's time");
	}

	/* Time passes with emulation, at the rate a Game Boy runs. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		const uint64_t expect = advance_seconds(3600);
		check_eq(c.elapsed(), expect, "an emulated hour is an hour on the cartridge");
		check(expect + 1 >= 3600 && expect <= 3600,
		      "and lands within a second of the hour asked for");
	}

	/* The point of the whole exercise: two devices whose own clocks differ, told
	 * the same pair of epochs, must produce identical readings. Console A takes
	 * the host's clock and console B the client's on both devices. */
	{
		g_frames = 0;
		Console host_a, host_b, client_a, client_b;
		host_a.adopt(HOST_CLOCK);   host_b.adopt(CLIENT_CLOCK);
		client_a.adopt(HOST_CLOCK); client_b.adopt(CLIENT_CLOCK);
		advance_seconds(4000);
		check_eq(host_a.elapsed(), client_a.elapsed(),
		         "both devices agree about console A");
		check_eq(host_b.elapsed(), client_b.elapsed(),
		         "both devices agree about console B");
		check(host_a.clock.now() != host_b.clock.now(),
		      "the two consoles still show their own owners' times");
		check_eq(host_b.clock.now() - host_a.clock.now(),
		         CLIENT_CLOCK - HOST_CLOCK,
		         "and differ by exactly the two devices' clock skew");
	}

	/* A reconnect renegotiates the epochs against clocks that have moved on.
	 * The cartridge must not gain that time twice, nor lose it. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		advance_seconds(600);
		const uint64_t before = c.elapsed();
		c.adopt(HOST_CLOCK + 900);   /* 15 real minutes later */
		check_eq(c.elapsed(), before, "a reconnect neither gains nor loses cartridge time");
	}

	/* Loading content mid-handshake - which is how two *different* linked
	 * cartridges get built - powers the cartridge back to the fixed epoch while
	 * the agreed clock stays. The wrapper moves it on again; if it did not, the
	 * cartridge would be six years adrift. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		c.powerOn();
		c.rtc.shiftBase((int64_t)c.clock.epoch() -
		                (int64_t)gambatte::DUAL_POWER_ON_EPOCH);
		check_eq(c.elapsed(), 0, "a cartridge rebuilt mid-handshake keeps the agreed clock");
	}

	/* A cartridge whose saved clock is *ahead* of the console's. This is not
	 * exotic: a player crosses a timezone, a console's clock is reset, or - in a
	 * paired session - the two devices disagree and each console keeps its own
	 * owner's time. Two handhelds six hours apart made it certain.
	 *
	 * The unsigned subtraction underneath produced a value near 2^64, and
	 * doLatch normalises by subtracting 0x1FF days per iteration, so the
	 * emulator stopped responding on the first latch rather than showing a wrong
	 * date. Pokemon Gold/Silver - the first cartridge with an RTC to run under
	 * the paired core - froze both devices within a second of starting.
	 *
	 * If this test ever hangs rather than fails, that is the bug back again. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		/* The save says this cartridge was last running six hours from now. */
		c.rtc.setBaseTime(HOST_CLOCK + 6 * 3600);
		check_eq(c.elapsed(), 0, "a cartridge clock ahead of the console reads zero, not 2^64");
		advance_seconds(60);
		check_eq(c.elapsed(), 0, "and stays there until the console catches up");
	}

	/* Once the console does catch up, the cartridge starts counting normally. */
	{
		g_frames = 0;
		Console c;
		c.adopt(HOST_CLOCK);
		c.rtc.setBaseTime(HOST_CLOCK + 100);
		const uint64_t expect = advance_seconds(400);
		check(c.elapsed() > 0 && c.elapsed() <= expect,
		      "and resumes counting once the console passes it");
	}

	if (failures) {
		std::printf("\nFAIL: %d cartridge-clock check(s) failed\n", failures);
		return 1;
	}
	std::printf("\nPASS: cartridge clocks are a function of emulated progress\n");
	return 0;
}
