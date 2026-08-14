/* Shifts time(2) by SKEW_CLOCK_SECONDS for one process.
 *
 * Two handhelds are never set to the same second - the pair this was written
 * for was nine seconds apart - and that difference is the whole reason the
 * cartridge clocks have to be negotiated rather than read locally. A test where
 * both sides run on one machine cannot see the difference between "used the
 * clocks the two devices exchanged" and "each read its own", because on one
 * machine those are the same number.
 *
 * Preloading this into the client gives the two roles genuinely different
 * clocks, so an implementation that read locally would install a different pair
 * on each side and the comparison in dual.sh would catch it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

static long skew(void)
{
	static long cached;
	static int loaded;
	if (!loaded) {
		const char *v = getenv("SKEW_CLOCK_SECONDS");
		cached = v && *v ? atol(v) : 0;
		loaded = 1;
	}
	return cached;
}

time_t time(time_t *out)
{
	static time_t (*real)(time_t *);
	if (!real)
		real = (time_t (*)(time_t *))dlsym(RTLD_NEXT, "time");
	time_t now = real(NULL) + (time_t)skew();
	if (out)
		*out = now;
	return now;
}
