/*
 * Determinism test for NetplayLocalSerialBus.
 *
 * The bus decides what byte an emulated console receives, so its answers become
 * emulated state. Two devices mirroring the same pair must therefore get the
 * same answers from it - the same bytes, in the same order, for the same
 * sequence of emulated events. Today they do not: on a real session the two
 * handhelds disagreed even about how many exchanges had occurred (385 on one,
 * 0 on the other over the same window), and one of them hit the 50ms send
 * deadline and fed a fabricated 0xFF into its console.
 *
 * This reproduces that without a ROM, a device, or a frontend.
 *
 * Both consoles are modelled as threads issuing a *fixed* script of serial
 * operations - fixed because that is the point: identical emulated behaviour
 * must produce identical bus results no matter how the two threads are
 * scheduled. Jitter is injected deliberately to explore interleavings that a
 * real device hits occasionally and a test would otherwise never see.
 *
 *   bustest [runs]
 *
 * Exit status is 0 only if every run produced identical output.
 */
#include "local_serial.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

/* One console's script. Gambatte's clock owner calls send() and blocks; the
 * externally clocked console polls check() from inside its CPU loop. A console
 * does both over a session, so the script mixes them. */
struct Op {
	enum Kind { SEND, CHECK, SPIN } kind;
	unsigned char byte;
	unsigned jitter_us;   /* real time burnt, to explore interleavings */
	unsigned long cc;     /* emulated cycle this op happens at */
};

struct Result {
	unsigned op;
	unsigned endpoint;
	int outcome;          /* send: byte received; check: 1 taken, 0 not */
	unsigned char byte;
};

NetplayLocalSerialBus *g_bus;
int g_frame;
std::vector<Op> g_script[2];
std::vector<Result> g_results[2];

/* Deterministic per-endpoint jitter, so the *script* is identical run to run
 * and only the OS scheduling varies. */
void spin(unsigned us) {
	if (us) usleep(us);
}

void *console(void *arg) {
	const unsigned ep = (unsigned)(long)arg;
	/* beginFrame() has already marked both consoles active. */
	for (size_t i = 0; i < g_script[ep].size(); i++) {
		const Op &op = g_script[ep][i];
		/* The CPU loop reports progress constantly, whether or not a transfer
		 * is happening. Without that a busy console strands a peer that is
		 * waiting to find out whether a request is coming. */
		g_bus->advance(ep, op.cc);
		Result r;
		r.op = (unsigned)(g_frame * 10000 + (int)i);
		r.endpoint = ep;
		r.byte = op.byte;
		switch (op.kind) {
		case Op::SEND: {
			unsigned char got = g_bus->send(ep, op.cc, op.byte, false);
			r.outcome = got;
			g_results[ep].push_back(r);
			break;
		}
		case Op::CHECK: {
			unsigned char in = 0;
			bool fast = false;
			bool took = g_bus->check(ep, op.cc, op.byte, in, fast);
			r.outcome = took ? 1 : 0;
			r.byte = took ? in : 0;
			g_results[ep].push_back(r);
			break;
		}
		case Op::SPIN:
			break;
		}
		spin(op.jitter_us);
	}
	/* The same lifecycle the wrapper uses. Run the cycle counter out to the end
	 * of the frame, stop running - which is what lets a peer still waiting learn
	 * that nothing more is coming - then stay available and service any late
	 * request before finally dropping out. */
	g_bus->advance(ep, 70224);
	g_bus->setRunning(ep, false);
	for (int service = 0; service < 512 && g_bus->waitForService(ep); service++) {
		unsigned char in = 0;
		bool fast = false;
		Result r;
		r.op = (unsigned)(g_frame * 10000 + 5000 + service);
		r.endpoint = ep;
		r.byte = 0;
		const bool took = g_bus->check(ep, 70224, 0xEE, in, fast);
		r.outcome = took ? 1 : 0;
		r.byte = took ? in : 0;
		g_results[ep].push_back(r);
		if (!took) break;
	}
	/* Hold at the frame barrier, exactly as the wrapper does, so an unanswered
	 * send cannot end merely because this console dropped out first. */
	for (int leave = 0; leave < 512 && !g_bus->leaveFrame(ep); leave++) {
		unsigned char in = 0;
		bool fast = false;
		Result r;
		r.op = (unsigned)(g_frame * 10000 + 8000 + leave);
		r.endpoint = ep;
		const bool took = g_bus->check(ep, 70224, 0xEE, in, fast);
		r.outcome = took ? 1 : 0;
		r.byte = took ? in : 0;
		g_results[ep].push_back(r);
	}
	g_bus->setActive(ep, false);
	return 0;
}

/* Which situation the consoles are in. */
enum Scenario {
	/* Both listening: every clock is answered. This is what Tetris DX does, and
	 * it ran 42929 exchanges across 11400 frames on two devices without a
	 * disagreement. */
	COOPERATIVE,
	/* One console clocks into a peer that is not listening. Real hardware just
	 * reads idle, and Super Mario Bros. Deluxe does it constantly - eleven
	 * unanswered sends in three hundred frames. An unanswered send is the only
	 * case that can end by the peer simply going away, which is the one ending
	 * that was decided by thread timing rather than by emulated state. */
	DEAF_PEER,
};

Scenario g_scenario = COOPERATIVE;

/* A fixed, reproducible script for both consoles. Console 0 mostly clocks,
 * console 1 mostly polls, and they swap roles part way through - which is what
 * a Game Link session actually does. */
void build_scripts() {
	g_script[0].clear();
	g_script[1].clear();
	unsigned s = 12345;
	/* Both consoles emulate the same frame, so their cycles advance together.
	 * The clocking console steps in transfer-sized strides; the polling one
	 * checks more often, as a CPU loop would. */
	unsigned long cc0 = 0, cc1 = 0;
	for (int i = 0; i < 200; i++) {
		s = s * 1103515245u + 12345u;
		const unsigned j = (s >> 16) % 40;
		cc0 += 300;
		cc1 += 300;
		if (g_scenario == DEAF_PEER) {
			/* Console 0 keeps clocking; console 1 only emulates. Its cycles are
			 * still published - a console busy elsewhere is not a console that
			 * has gone away - but it never takes anything off the cable. */
			Op a = { Op::SEND, (unsigned char)(0x40 + i), j, cc0 };
			Op b = { Op::SPIN, 0, (j * 7) % 37, cc1 };
			g_script[0].push_back(a);
			g_script[1].push_back(b);
		} else if (i < 120) {
			Op a = { Op::SEND,  (unsigned char)(0x40 + i), j, cc0 };
			Op b = { Op::CHECK, (unsigned char)(0x80 + i), (j * 7) % 37, cc1 };
			g_script[0].push_back(a);
			g_script[1].push_back(b);
		} else {
			Op a = { Op::CHECK, (unsigned char)(0x40 + i), j, cc0 };
			Op b = { Op::SEND,  (unsigned char)(0x80 + i), (j * 7) % 37, cc1 };
			g_script[0].push_back(a);
			g_script[1].push_back(b);
		}
	}
}

/* Several frames per run. A single frame cannot exercise the handoff at a frame
 * boundary, and that is exactly where an unanswered send used to end by the peer
 * going away rather than by anything the emulation decided. */
const int FRAMES_PER_RUN = 4;

std::string run_once() {
	NetplayLocalSerialBus bus;
	g_bus = &bus;
	g_results[0].clear();
	g_results[1].clear();
	bus.reset();

	for (int frame = 0; frame < FRAMES_PER_RUN; frame++) {
		g_frame = frame;
		bus.beginFrame();
		pthread_t t0, t1;
		pthread_create(&t0, 0, console, (void *)0L);
		pthread_create(&t1, 0, console, (void *)1L);
		pthread_join(t0, 0);
		pthread_join(t1, 0);
	}

	/* The observable contract: for each console, the sequence of answers the
	 * bus gave it. Anything that varies here varies inside an emulator. */
	std::string out;
	char line[64];
	for (unsigned ep = 0; ep < 2; ep++) {
		for (size_t i = 0; i < g_results[ep].size(); i++) {
			const Result &r = g_results[ep][i];
			std::snprintf(line, sizeof(line), "%u:%u:%d:%02x\n",
			              r.endpoint, r.op, r.outcome, r.byte);
			out += line;
		}
	}
	NetplayLocalSerialStats st;
	bus.snapshot(st);
	std::snprintf(line, sizeof(line), "exchanges=%llu timeouts=%llu simul=%llu\n",
	              (unsigned long long)st.exchanges,
	              (unsigned long long)st.send_timeouts,
	              (unsigned long long)st.simultaneous_sends);
	out += line;
	return out;
}

} // namespace

int run_scenario(const char *name, Scenario scenario, int runs) {
	g_scenario = scenario;
	build_scripts();
	std::printf("  -- %s\n", name);

	std::string first;
	int differing = 0;
	for (int i = 0; i < runs; i++) {
		const std::string got = run_once();
		if (i == 0) {
			first = got;
		} else if (got != first) {
			differing++;
			if (differing == 1) {
				/* Name the first operation that answered differently: the whole
				 * point is to find which decision is still timing-dependent. */
				size_t a = 0, b = 0;
				while (a < first.size() && b < got.size()) {
					const size_t ea = first.find('\n', a), eb = got.find('\n', b);
					const std::string la = first.substr(a, ea - a);
					const std::string lb = got.substr(b, eb - b);
					if (la != lb) {
						std::printf("    first difference: run1 %-24s vs run%d %s\n",
						            la.c_str(), i + 1, lb.c_str());
						break;
					}
					a = ea + 1; b = eb + 1;
				}
			}
		}
		/* Show the summary line of each run so a failure is legible. */
		const size_t p = got.rfind("exchanges=");
		std::printf("  run %2d: %s", i + 1,
		            p == std::string::npos ? "(no summary)\n" : got.c_str() + p);
	}

	return differing;
}

int main(int argc, char **argv) {
	const int runs = argc > 1 ? std::atoi(argv[1]) : 8;
	int differing = 0;
	differing += run_scenario("both consoles listening", COOPERATIVE, runs);
	differing += run_scenario("clocking into a peer that never listens", DEAF_PEER, runs);

	if (differing) {
		std::printf("\nFAIL: %d run(s) disagreed with the first of their scenario\n",
		            differing);
		return 1;
	}
	std::printf("\nPASS: all %d runs of each scenario identical\n", runs);
	return 0;
}
