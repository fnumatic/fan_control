// SPDX-License-Identifier: GPL-2.0
/*
 * Logic tests of module/fan.c: the real code, compiled as a normal program
 * against tests/kernel.h, with a fake EC behind fan_ops and read_telemetry().
 * The monitor runs every 2 s of a fake clock, as the workqueue would run it.
 * Run with "make test".
 */
#include "../module/fan.c"

/* ---- Fake kernel: clock, log, workqueue ---- */

unsigned long jiffies;
struct workqueue_struct *system_dfl_wq;

static char logbuf[1 << 16];
static size_t loglen;

void test_log(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
	va_end(ap);
	if (n > 0)
		loglen = min(loglen + n, sizeof(logbuf) - 1);
}

static bool logged(const char *text)
{
	return strstr(logbuf, text);
}

static struct mec_data d;
static int n_queued;
static unsigned long queued_delay;

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
			unsigned long delay)
{
	if (dwork == &d.monitor) {
		n_queued++;
		queued_delay = delay;
	}
	return true;
}

/* ---- Fake board and EC ---- */

static int cpu_min_code;

int board_min_code(enum fan_id id)
{
	return id == FAN_CPU ? cpu_min_code : SYS_MIN_CODE;
}

/* As in ec.c. */
bool plausible_temp(u8 val)
{
	return val >= 1 && val <= 120;
}

static struct telemetry tele;	/* what read_telemetry() returns */
static int fail_reads;		/* read_telemetry() fails this many more times */

int read_telemetry(struct telemetry *t)
{
	if (fail_reads) {
		fail_reads--;
		return -EIO;
	}
	*t = tele;
	return 0;
}

#define MAX_EVENTS 32

/* One fan's side of the EC, and what the driver asked of it. */
struct fake_fan {
	int code;		/* the code the EC runs; engage reports it as the start */
	bool changed;		/* held(): the EC no longer runs what was written */
	int fail_engage;	/* fail this many more engages ... */
	bool engage_wrote;	/* ... after a write may have reached the EC */
	int fail_set;		/* fail this many more target writes */
	int fail_release;	/* fail this many more releases */
	int n_engage, n_set, n_release;
	int engage_s[MAX_EVENTS], set_s[MAX_EVENTS], set_code[MAX_EVENTS], release_s[MAX_EVENTS];
};

static struct fake_fan ec[NR_FANS];
static unsigned long t0;

/* Seconds since setup(). */
static int now(void)
{
	return (int)((jiffies - t0) / HZ);
}

static void record(int *list, int *n, int value)
{
	if (*n < MAX_EVENTS)
		list[*n] = value;
	(*n)++;
}

static int fake_engage(struct mec_data *md, struct fan *f, int rpm, int *start, bool *issued)
{
	struct fake_fan *e = &ec[f->id];

	record(e->engage_s, &e->n_engage, now());
	if (e->fail_engage) {
		e->fail_engage--;
		*issued = e->engage_wrote;
		return -EIO;
	}
	*start = e->code;
	return 0;
}

static int fake_set(struct mec_data *md, struct fan *f, u8 code)
{
	struct fake_fan *e = &ec[f->id];

	if (e->n_set < MAX_EVENTS)
		e->set_code[e->n_set] = code;
	record(e->set_s, &e->n_set, now());
	if (e->fail_set) {
		e->fail_set--;
		return -EIO;
	}
	e->code = code;
	return 0;
}

static int fake_release(struct mec_data *md, struct fan *f)
{
	struct fake_fan *e = &ec[f->id];

	record(e->release_s, &e->n_release, now());
	if (e->fail_release) {
		e->fail_release--;
		return -EIO;
	}
	return 0;
}

static bool fake_held(struct mec_data *md, struct fan *f, const struct telemetry *t)
{
	return !ec[f->id].changed;
}

static const struct fan_ops fake_ops = {
	.engage = fake_engage,
	.set = fake_set,
	.release = fake_release,
	.held = fake_held,
};

/* ---- Harness ---- */

#define CPU (&d.fans[FAN_CPU])
#define SYS (&d.fans[FAN_SYS])

static struct curve default_curves[NR_FANS];
static int failed;

#define CHECK(cond) do {							\
	if (!(cond)) {								\
		printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond);		\
		failed = 1;							\
	}									\
} while (0)

#define CHECK_EQ(a, b) do {							\
	long _a = (a), _b = (b);						\
	if (_a != _b) {								\
		printf("    %s:%d: %s is %ld, expected %ld\n",			\
		       __FILE__, __LINE__, #a, _a, _b);				\
		failed = 1;							\
	}									\
} while (0)

/* A recorded list (seconds or codes) against a literal one. */
#define CHECK_LIST(list, n, ...)						\
	check_list(__FILE__, __LINE__, #list, list, n, (const int[]){ __VA_ARGS__ },	\
		   sizeof((const int[]){ __VA_ARGS__ }) / sizeof(int))

static void print_list(const int *list, int n)
{
	printf("{");
	for (int i = 0; i < min(n, MAX_EVENTS); i++)
		printf("%s%d", i ? ", " : "", list[i]);
	printf("%s}", n > MAX_EVENTS ? ", ..." : "");
}

static void check_list(const char *file, int line, const char *what, const int *got, int n,
		       const int *want, int n_want)
{
	if (n == n_want && !memcmp(got, want, n * sizeof(int)))
		return;
	printf("    %s:%d: %s is ", file, line, what);
	print_list(got, n);
	printf(", expected ");
	print_list(want, n_want);
	printf("\n");
	failed = 1;
}

static void setup(void)
{
	memset(&d, 0, sizeof(d));
	memset(ec, 0, sizeof(ec));
	memset(&tele, 0, sizeof(tele));
	fail_reads = 0;
	n_queued = 0;
	queued_delay = 0;
	loglen = 0;
	logbuf[0] = '\0';
	t0 = jiffies = ULONG_MAX - 30 * HZ;	/* the clock wraps 30 s into every test */
	cpu_min_code = 6;			/* F7BSI */
	for (int i = 0; i < NR_FANS; i++) {
		struct fan *f = &d.fans[i];

		curve_params[i].curve = default_curves[i];
		/* As fan_init() in main.c; each test puts the fans it uses in a mode. */
		f->id = i;
		f->name = i == FAN_CPU ? "cpu" : "system";
		f->ops = &fake_ops;
		f->min_code = board_min_code(i);
		f->temp_limit = i == FAN_CPU ? CPU_TEMP_LIMIT : SYS_TEMP_LIMIT;
		f->mode = MODE_FIRMWARE;
		f->cur = -1;
		ec[i].code = 10;
		tele.temp[i] = 30;
		tele.rpm[i] = 1000;
	}
}

/* One run of the monitor, as the workqueue would call it, then 2 s pass. */
static void tick(void)
{
	mec_monitor(&d.monitor.work);
	jiffies += 2 * HZ;
}

/* Run the monitor every 2 s up to, not including, second s. */
static void run_until(int s)
{
	while (now() < s)
		tick();
}

static void set_curve(enum fan_id id, const char *val)
{
	struct kernel_param kp = { .arg = &curve_params[id] };

	CHECK_EQ(curve_ops.set(val, &kp), 0);
}

/* ---- Stepping ---- */

/* Breaks if the gap divisor, the four-code cap or the direction changes. */
static void test_step_grows_with_gap(void)
{
	static const struct { int cur, goal, want; } cases[] = {
		{ 10, 11, 11 }, { 10, 14, 11 }, { 10, 15, 12 }, { 10, 18, 12 },
		{ 10, 19, 13 }, { 10, 23, 14 }, { 10, 51, 14 },
		{ 30, 10, 26 }, { 12, 10, 11 }, { 30, 29, 29 },
	};
	struct fan f = { .min_code = 6 };

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		f.cur = cases[i].cur;
		CHECK_EQ(fan_next_code(&f, cases[i].goal), cases[i].want);
	}
}

/* Breaks if a stopped fan is not spun up first, or a code under the minimum is written. */
static void test_stopped_fan_restarts_and_minimum_is_skipped(void)
{
	static const struct { int cur, goal, min, want; } cases[] = {
		{ 0, 10, 6, 20 }, { 0, 30, 6, 20 },	/* spin up at 2000 RPM first */
		{ 3, 10, 6, 6 },			/* up past 5: straight to the minimum */
		{ 8, 0, 6, 6 }, { 6, 0, 6, 0 },		/* down to the minimum, then stop */
		{ 7, 0, 6, 0 },				/* 5 is under the minimum: stop */
		{ 9, 30, 10, 13 }, { 11, 10, 10, 10 },	/* F7BSC CPU minimum */
	};
	struct fan f = { 0 };

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		f.cur = cases[i].cur;
		f.min_code = cases[i].min;
		CHECK_EQ(fan_next_code(&f, cases[i].goal), cases[i].want);
	}
}

/* ---- Curves ---- */

/* Breaks if the parser drops or misreads a valid point. */
static void test_curve_parameter_accepts(void)
{
	const struct curve_param *cpu = &curve_params[FAN_CPU];
	struct curve c;

	CHECK_EQ(curve_parse("0:0,25:1000,45:1400,54:1800,66:firmware", cpu, &c), 0);
	CHECK_EQ(c.n, 5);
	CHECK_LIST(((int[]){ c.temp[0], c.temp[1], c.temp[2], c.temp[3], c.temp[4] }), 5,
		   0, 25, 45, 54, 66);
	CHECK_LIST(((int[]){ c.code[0], c.code[1], c.code[2], c.code[3], c.code[4] }), 5,
		   0, 10, 14, 18, CURVE_FIRMWARE);

	CHECK_EQ(curve_parse(" 0:0 , 30 : fw \n", cpu, &c), 0);
	CHECK_EQ(c.n, 2);
	CHECK_LIST(((int[]){ c.temp[0], c.temp[1], c.code[0], c.code[1] }), 4,
		   0, 30, 0, CURVE_FIRMWARE);

	/* Rounded to the nearest code. */
	CHECK_EQ(curve_parse("0:1049,10:1050,20:5100", cpu, &c), 0);
	CHECK_LIST(((int[]){ c.code[0], c.code[1], c.code[2] }), 3, 10, 11, 51);

	/* Empty: the fan stays with the firmware. */
	CHECK_EQ(curve_parse("", cpu, &c), 0);
	CHECK_EQ(c.n, 0);
}

/* Breaks if a malformed or unsafe curve gets through. */
static void test_curve_parameter_refuses(void)
{
	static const char * const bad[] = {
		"0:1000,0:1200",			/* temperatures not ascending */
		"30:1000,20:1200",
		"0:0,1:0,2:0,3:0,4:0,5:0,6:0,7:0,8:0",	/* nine points */
		"0:49",					/* would round to a stop */
		"0:5200",				/* over 5100 RPM */
		"121:1000",				/* over 120 C */
		"abc", "10", "10:", ":1000", "-5:1000", "10:-1000", "10:1000x", "10:1000,",
	};
	struct curve c;

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		if (curve_parse(bad[i], &curve_params[FAN_CPU], &c) != -EINVAL) {
			printf("    \"%s\" was accepted\n", bad[i]);
			failed = 1;
		}
}

/* Breaks if a curve may ask for a speed under the board's minimum. */
static void test_curve_respects_board_minimum(void)
{
	const struct curve_param *cpu = &curve_params[FAN_CPU];
	const struct curve_param *sys = &curve_params[FAN_SYS];
	struct curve c;

	CHECK_EQ(curve_parse("0:500", cpu, &c), -EINVAL);	/* F7BSI CPU: 600 */
	CHECK_EQ(curve_parse("0:600", cpu, &c), 0);
	CHECK_EQ(curve_parse("0:700", sys, &c), -EINVAL);	/* system: 800 */
	CHECK_EQ(curve_parse("0:800", sys, &c), 0);
	cpu_min_code = 10;					/* F7BSC CPU: 1000 */
	CHECK_EQ(curve_parse("0:900", cpu, &c), -EINVAL);
	CHECK_EQ(curve_parse("0:1000", cpu, &c), 0);
}

/* Breaks if reading back differs from what was set, or a refused write changes the curve. */
static void test_curve_reads_back_and_refused_write_keeps_it(void)
{
	struct kernel_param kp = { .arg = &curve_params[FAN_CPU] };
	char buf[PAGE_SIZE];

	CHECK_EQ(curve_ops.set("0:0, 30:1200 ,60:fw", &kp), 0);
	CHECK_EQ(curve_ops.get(buf, &kp), (long)strlen("0:0,30:1200,60:firmware\n"));
	CHECK(!strcmp(buf, "0:0,30:1200,60:firmware\n"));

	CHECK_EQ(curve_ops.set("0:0,20:500", &kp), -EINVAL);
	curve_ops.get(buf, &kp);
	CHECK(!strcmp(buf, "0:0,30:1200,60:firmware\n"));

	CHECK_EQ(curve_ops.set("", &kp), 0);
	curve_ops.get(buf, &kp);
	CHECK(!strcmp(buf, "\n"));
}

/* Breaks if a curve point is left without the 3 C margin, or entered late. */
static void test_curve_point_needs_margin_to_leave(void)
{
	/* Default CPU curve: 0:0, 25:1000, 45:1400, 54:1800, 66:firmware */
	static const struct { u8 temp; int want; } steps[] = {
		{ 30, 10 }, { 45, 14 }, { 42, 14 }, { 41, 10 }, { 24, 10 }, { 21, 0 },
		{ 54, 18 }, { 66, CURVE_FIRMWARE }, { 63, CURVE_FIRMWARE }, { 62, 18 },
	};

	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
		CHECK_EQ(curve_goal(&d, CPU, &curve_params[FAN_CPU].curve, steps[i].temp),
			 steps[i].want);
}

/* ---- Control ---- */

/* Breaks if the fan is written before it is taken, too often, or in the wrong steps. */
static void test_engages_then_steps_every_4_s(void)
{
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].code = 17;		/* the firmware runs 1700 RPM; 30 C wants 1000 */
	run_until(40);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0);
	CHECK_LIST(ec[FAN_CPU].set_code, ec[FAN_CPU].n_set, 15, 13, 12, 11, 10);
	CHECK_LIST(ec[FAN_CPU].set_s, ec[FAN_CPU].n_set, 4, 8, 12, 16, 20);
	CHECK_EQ(ec[FAN_CPU].n_release, 0);
	CHECK_EQ(ec[FAN_SYS].n_engage, 0);	/* firmware mode: left alone */
}

/* Breaks if a curve point under the board's minimum reaches the EC. */
static void test_goal_under_board_minimum_is_raised(void)
{
	set_curve(FAN_CPU, "0:700");	/* accepted before the board was known (F7BSI: 600) */
	CPU->min_code = 10;		/* then the board turns out to be an F7BSC */
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].code = 20;
	run_until(40);
	CHECK_LIST(ec[FAN_CPU].set_code, ec[FAN_CPU].n_set, 17, 15, 13, 12, 11, 10);
}

/* Breaks if the 80 C limit or its 5 C hysteresis changes. */
static void test_temperature_limit_hands_fan_to_firmware(void)
{
	static const u8 temps[] = { 70, 80, 76, 75, 74 };	/* one per 2 s */

	set_curve(FAN_CPU, "0:1000,60:2000");	/* no firmware point of its own */
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].code = 20;
	for (size_t i = 0; i < sizeof(temps); i++) {
		tele.temp[FAN_CPU] = temps[i];
		tick();
	}
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 8);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 2);
}

/* Breaks if a temperature of 0 or over 120 C is believed. */
static void test_implausible_temperature_hands_fan_to_firmware(void)
{
	/* 0 C is under the limit but not believable: going hot, and staying hot at 80 C. */
	static const u8 temps[] = { 40, 0, 40, 80, 0, 40 };

	set_curve(FAN_CPU, "0:1000,60:2000");
	CPU->mode = MODE_CURVE;
	for (size_t i = 0; i < sizeof(temps); i++) {
		tele.temp[FAN_CPU] = temps[i];
		tick();
	}
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 4, 10);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 2, 6);
}

/* Breaks if a fixed value outlives the 30 s watchdog, or ends early. */
static void test_fixed_value_falls_back_to_curve_after_30_s(void)
{
	/* As mec_write() for pwm1 = 100 in mode 1; the curve wants 1000 RPM at 30 C. */
	CPU->mode = MODE_MANUAL;
	CPU->manual_code = 20;
	CPU->deadline = jiffies + 30 * HZ;
	run_until(34);
	CHECK_LIST(ec[FAN_CPU].set_code, ec[FAN_CPU].n_set, 13, 15, 17, 18, 19, 20, 17);
	CHECK_LIST(ec[FAN_CPU].set_s, ec[FAN_CPU].n_set, 4, 8, 12, 16, 20, 24, 32);
	CHECK_EQ(CPU->mode, MODE_CURVE);
}

/* Breaks if an expired fixed value without a curve keeps the fan. */
static void test_fixed_value_without_curve_falls_back_to_firmware(void)
{
	set_curve(FAN_CPU, "");
	CPU->mode = MODE_MANUAL;
	CPU->manual_code = 10;
	CPU->deadline = jiffies + 30 * HZ;
	run_until(36);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 32);
	CHECK_EQ(CPU->mode, MODE_FIRMWARE);
}

/* RPM swinging 1000/1700 on alternate samples. */
static void swing_rpm(void)
{
	tele.rpm[FAN_CPU] = now() / 2 % 2 ? 1700 : 1000;
}

/* Breaks if the 6-sample window, the 600 RPM spread or the 60 s back-off changes. */
static void test_hunting_releases_and_backs_off_60_s(void)
{
	CPU->mode = MODE_CURVE;
	while (now() < 82) {
		swing_rpm();
		tick();
	}
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 20);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 80);
}

/* Breaks if a fan that keeps hunting is taken back forever. */
static void test_third_hunt_leaves_fan_with_firmware(void)
{
	CPU->mode = MODE_CURVE;
	while (now() < 400) {
		swing_rpm();
		tick();
	}
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 20, 100, 180);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 80, 160);
	CHECK_EQ(CPU->mode, MODE_FIRMWARE);
}

/* Breaks if the swing right after a write counts as hunting. */
static void test_swing_within_settle_time_is_not_hunting(void)
{
	CPU->mode = MODE_CURVE;
	while (now() < 60) {
		if (now() < 10)
			swing_rpm();
		else
			tele.rpm[FAN_CPU] = 1000;
		tick();
	}
	CHECK_EQ(ec[FAN_CPU].n_release, 0);
}

/* ---- Failures ---- */

/* Breaks if failed engages are retried without the 60 s back-off, or forever. */
static void test_failed_engages_back_off_then_give_up(void)
{
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].fail_engage = 3;
	run_until(300);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 60, 120);
	/* Each failure hands back what another EC writer may have left. */
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 0, 60, 120);
	CHECK_EQ(CPU->mode, MODE_FIRMWARE);
}

/* Breaks if a failed engage that may have written stops releasing before it succeeds. */
static void test_failed_engage_after_write_keeps_releasing(void)
{
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].fail_engage = 1;
	ec[FAN_CPU].engage_wrote = true;
	ec[FAN_CPU].fail_release = 1;
	run_until(62);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 0, 2);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 60);
}

/* Breaks if the driver keeps a fan whose EC state changed behind its back. */
static void test_changed_ec_state_releases(void)
{
	CPU->mode = MODE_CURVE;
	tick();
	ec[FAN_CPU].changed = true;
	tick();
	ec[FAN_CPU].changed = false;
	run_until(64);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 2);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 62);
}

/* Breaks if stepping goes on after a failed target write. */
static void test_failed_target_write_releases(void)
{
	CPU->mode = MODE_CURVE;
	ec[FAN_CPU].code = 17;
	ec[FAN_CPU].fail_set = 1;
	run_until(66);
	CHECK_LIST(ec[FAN_CPU].set_s, ec[FAN_CPU].n_set, 4);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 4);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 64);
}

/* Breaks if a failed release is not retried, or retried without a growing delay. */
static void test_failed_release_is_retried_with_growing_delay(void)
{
	set_curve(FAN_CPU, "0:1000,60:2000");
	CPU->mode = MODE_CURVE;
	tele.temp[FAN_CPU] = 70;
	tick();
	tele.temp[FAN_CPU] = 80;		/* the limit: release, which fails 3 times */
	ec[FAN_CPU].fail_release = 3;
	run_until(30);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 2, 4, 8, 16);
	CHECK(!CPU->owned && !CPU->release_pending);
}

/* Breaks if one bad read releases the fans, or endless bad reads never do. */
static void test_unreadable_telemetry_releases_both_fans(void)
{
	CPU->mode = MODE_CURVE;
	SYS->mode = MODE_CURVE;
	tick();
	fail_reads = 3;
	run_until(68);
	CHECK_LIST(ec[FAN_CPU].release_s, ec[FAN_CPU].n_release, 6);
	CHECK_LIST(ec[FAN_SYS].release_s, ec[FAN_SYS].n_release, 6);
	CHECK_LIST(ec[FAN_CPU].engage_s, ec[FAN_CPU].n_engage, 0, 66);
	CHECK_LIST(ec[FAN_SYS].engage_s, ec[FAN_SYS].n_engage, 0, 66);
}

/* Breaks if the monitor stops rescheduling itself, or runs after stop or during sleep. */
static void test_monitor_requeues_itself_until_stopped(void)
{
	run_until(6);
	CHECK_EQ(n_queued, 3);
	CHECK_EQ(queued_delay, 2 * HZ);

	CPU->mode = MODE_CURVE;
	d.suspended = true;
	tick();
	d.suspended = false;
	d.stopped = true;
	tick();
	CHECK_EQ(n_queued, 3);
	CHECK_EQ(ec[FAN_CPU].n_engage, 0);
}

/* Breaks if unload gives up on the first failed release. */
static void test_unload_retries_release(void)
{
	CPU->mode = MODE_CURVE;
	SYS->mode = MODE_CURVE;
	tick();
	ec[FAN_CPU].fail_release = 2;
	mec_stop(&d, "driver unload");
	CHECK_EQ(ec[FAN_CPU].n_release, 3);
	CHECK_EQ(ec[FAN_SYS].n_release, 1);
	CHECK(!CPU->owned && !SYS->owned);
	CHECK(!logged("may still be under driver control"));
}

/* Breaks if unload stays silent about a fan it could not hand back. */
static void test_unload_warns_when_release_keeps_failing(void)
{
	CPU->mode = MODE_CURVE;
	tick();
	ec[FAN_CPU].fail_release = 3;
	mec_stop(&d, "driver unload");
	CHECK_EQ(ec[FAN_CPU].n_release, 3);
	CHECK(logged("cpu fan may still be under driver control"));
}

static const struct {
	void (*fn)(void);
	const char *what;
} tests[] = {
	{ test_step_grows_with_gap, "step grows with the gap, 1 to 4 codes" },
	{ test_stopped_fan_restarts_and_minimum_is_skipped,
	  "stopped fan restarts at 2000 RPM, codes under the minimum are skipped" },
	{ test_curve_parameter_accepts, "curve parameter accepts valid curves" },
	{ test_curve_parameter_refuses, "curve parameter refuses malformed curves" },
	{ test_curve_respects_board_minimum, "curve parameter respects the board's minimum" },
	{ test_curve_reads_back_and_refused_write_keeps_it,
	  "curve reads back as set; a refused write keeps the old one" },
	{ test_curve_point_needs_margin_to_leave, "curve point is left only 3 C below it" },
	{ test_engages_then_steps_every_4_s, "fan is taken, then stepped every 4 s" },
	{ test_goal_under_board_minimum_is_raised,
	  "curve point under the board's minimum is raised to it" },
	{ test_temperature_limit_hands_fan_to_firmware,
	  "temperature limit hands the fan to the firmware until 5 C below" },
	{ test_implausible_temperature_hands_fan_to_firmware,
	  "implausible temperature hands the fan to the firmware" },
	{ test_fixed_value_falls_back_to_curve_after_30_s,
	  "fixed value falls back to the curve after 30 s" },
	{ test_fixed_value_without_curve_falls_back_to_firmware,
	  "fixed value without a curve falls back to the firmware" },
	{ test_hunting_releases_and_backs_off_60_s, "hunting releases the fan for 60 s" },
	{ test_third_hunt_leaves_fan_with_firmware, "third hunt leaves the fan with the firmware" },
	{ test_swing_within_settle_time_is_not_hunting,
	  "swing within 10 s of a write is not hunting" },
	{ test_failed_engages_back_off_then_give_up,
	  "failed engages back off 60 s, then give up after 3" },
	{ test_failed_engage_after_write_keeps_releasing,
	  "failed engage after a write releases until it succeeds" },
	{ test_changed_ec_state_releases, "EC state changed behind the driver releases the fan" },
	{ test_failed_target_write_releases, "failed target write releases the fan" },
	{ test_failed_release_is_retried_with_growing_delay,
	  "failed release is retried with a growing delay" },
	{ test_unreadable_telemetry_releases_both_fans,
	  "telemetry unreadable 3 times releases both fans" },
	{ test_monitor_requeues_itself_until_stopped,
	  "monitor runs every 2 s, not after stop or during sleep" },
	{ test_unload_retries_release, "unload retries a failed release" },
	{ test_unload_warns_when_release_keeps_failing,
	  "unload warns when a fan cannot be handed back" },
};

int main(void)
{
	int n_failed = 0;

	for (int i = 0; i < NR_FANS; i++)
		default_curves[i] = curve_params[i].curve;

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		setup();
		failed = 0;
		tests[i].fn();
		printf("%s %s\n", failed ? "FAIL" : "ok  ", tests[i].what);
		if (failed) {
			n_failed++;
			printf("    driver log:\n%s", logbuf);
		}
	}
	printf("%zu tests, %d failed\n", sizeof(tests) / sizeof(tests[0]), n_failed);
	return n_failed ? 1 : 0;
}
