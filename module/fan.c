// SPDX-License-Identifier: GPL-2.0
/*
 * Fan control: one state machine for both fans. Modes, curves and their module
 * parameters, stepping towards the goal, hunting detection and the monitor.
 */

#include "minisforum_ec.h"

const char * const mode_names[] = {
	[MODE_MANUAL] = "manual",
	[MODE_CURVE] = "curve",
	[MODE_FIRMWARE] = "firmware",
};

struct curve_param curve_params[NR_FANS] = {
	[FAN_CPU] = {
		.fan = "cpu",
		.id = FAN_CPU,
		.curve = {
			.n = 5,
			.temp = { 0, 25, 45, 54, 66 },
			.code = { 0, 10, 14, 18, CURVE_FIRMWARE },
		},
	},
	[FAN_SYS] = {
		.fan = "system",
		.id = FAN_SYS,
		.curve = {
			.n = 4,
			.temp = { 0, 25, 40, 50 },
			.code = { 0, 10, 16, CURVE_FIRMWARE },
		},
	},
};
DEFINE_MUTEX(curve_lock);	/* protects curve_params[].curve */

static int curve_parse(const char *val, const struct curve_param *cp, struct curve *out)
{
	char buf[256], *s, *tok, *colon, *v;
	unsigned int temp, rpm;
	int code;

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	s = strim(buf);
	if (!*s)
		return 0;

	while ((tok = strsep(&s, ",")) != NULL) {
		colon = strchr(tok, ':');
		if (!colon || out->n == CURVE_MAX_POINTS)
			return -EINVAL;
		*colon = '\0';
		if (kstrtouint(strim(tok), 10, &temp) || temp > 120)
			return -EINVAL;
		if (out->n && temp <= out->temp[out->n - 1])
			return -EINVAL;
		v = strim(colon + 1);
		if (!strcmp(v, "firmware") || !strcmp(v, "fw")) {
			code = CURVE_FIRMWARE;
		} else {
			if (kstrtouint(v, 10, &rpm) || rpm > CODE_MAX * 100)
				return -EINVAL;
			code = DIV_ROUND_CLOSEST(rpm, 100);
			/* 1..49 would round to a stop: refuse it rather than stop the fan. */
			if (rpm && !code)
				return -EINVAL;
			if (code > 0 && code < board_min_code(cp->id))
				return -EINVAL;
		}
		out->temp[out->n] = temp;
		out->code[out->n] = code;
		out->n++;
	}
	return 0;
}

static int curve_set(const char *val, const struct kernel_param *kp)
{
	struct curve_param *cp = kp->arg;
	struct curve c;
	int ret;

	ret = curve_parse(val, cp, &c);
	if (ret) {
		pr_err("invalid %s_curve; expected temp:rpm or temp:firmware, comma separated, temperatures ascending, at most %d points, rpm 0 or %d..%d\n",
		       cp->fan, CURVE_MAX_POINTS, board_min_code(cp->id) * 100, CODE_MAX * 100);
		return ret;
	}
	scoped_guard(mutex, &curve_lock)
		cp->curve = c;
	return 0;
}

static int curve_get(char *buffer, const struct kernel_param *kp)
{
	struct curve_param *cp = kp->arg;
	struct curve c;
	int i, len = 0;

	scoped_guard(mutex, &curve_lock)
		c = cp->curve;
	for (i = 0; i < c.n; i++) {
		if (c.code[i] == CURVE_FIRMWARE)
			len += scnprintf(buffer + len, PAGE_SIZE - len, "%s%u:firmware",
					 i ? "," : "", c.temp[i]);
		else
			len += scnprintf(buffer + len, PAGE_SIZE - len, "%s%u:%d",
					 i ? "," : "", c.temp[i], c.code[i] * 100);
	}
	len += scnprintf(buffer + len, PAGE_SIZE - len, "\n");
	return len;
}

static const struct kernel_param_ops curve_ops = {
	.set = curve_set,
	.get = curve_get,
};

module_param_cb(cpu_curve, &curve_ops, &curve_params[FAN_CPU], 0644);
MODULE_PARM_DESC(cpu_curve,
		 "CPU fan curve on the EC CPU temperature, default \"0:0,25:1000,45:1400,54:1800,66:firmware\"; "
		 "empty leaves the fan to the firmware; "
		 "rpm 0 or 600..5100 (F7BSI) / 1000..5100 (F7BSC)");
module_param_cb(system_curve, &curve_ops, &curve_params[FAN_SYS], 0644);
MODULE_PARM_DESC(system_curve,
		 "System fan curve on the EC system temperature, default \"0:0,25:1000,40:1600,50:firmware\"; empty leaves the fan to the firmware; rpm 0 or 800..5100");

/* Generic fan logic */

int fan_release(struct mec_data *d, struct fan *f, const char *reason)
{
	unsigned long delay;
	int ret;

	if (!f->owned && !f->release_pending)
		return 0;
	f->release_pending = true;
	d->valid = false;
	ret = f->ops->release(d, f);
	if (ret) {
		/* Back off, so that an EC state that cannot be released does not flood the log. */
		delay = MONITOR_JIFFIES << min(f->release_failures, RELEASE_RETRY_SHIFT);
		f->release_failures++;
		f->next_release = jiffies + delay;
		pr_err("%s fan: returning it to firmware failed %u times (%d, %s); next retry in %lu s\n",
		       f->name, f->release_failures, ret, reason, delay / HZ);
		return ret;
	}
	f->release_failures = 0;
	f->owned = false;
	f->release_pending = false;
	f->cur = -1;
	f->win_n = 0;
	pr_info("%s fan returned to firmware: %s\n", f->name, reason);
	return 0;
}

static void fan_hold(struct fan *f, unsigned long duration)
{
	f->holding = true;
	f->hold_until = jiffies + duration;
}

static int fan_engage(struct mec_data *d, struct fan *f, int rpm)
{
	bool issued = false;
	int start = 0, ret;

	d->valid = false;
	ret = f->ops->engage(d, f, rpm, &start, &issued);
	if (ret) {
		if (issued) {
			/* A write may have reached the EC: release until that succeeds. */
			f->owned = true;
			fan_release(d, f, "taking control failed");
		} else if (!f->ops->release(d, f)) {
			/*
			 * Nothing was written, but a state another EC writer left
			 * (a repairable CPU table, a fixed-target handoff) is handed
			 * back now. If that fails it is not this driver's to retry:
			 * the next engage tries again after the hold.
			 */
			pr_info("%s fan left with the firmware: taking control failed\n",
				f->name);
		}
		return ret;
	}
	f->owned = true;
	f->cur = start;
	f->next_step = jiffies + STEP_JIFFIES;
	f->settle_until = jiffies + SETTLE_JIFFIES;
	f->win_n = 0;
	pr_info("%s fan under driver control (%s), starting at %d RPM\n",
		f->name, mode_names[f->mode], start * 100);
	return 0;
}

static int fan_set(struct mec_data *d, struct fan *f, int code)
{
	int ret;

	d->valid = false;
	ret = f->ops->set(d, f, code);
	if (ret) {
		fan_release(d, f, "target write failed");
		return ret;
	}
	f->cur = code;
	f->settle_until = jiffies + SETTLE_JIFFIES;
	f->win_n = 0;
	return 0;
}

/*
 * Step towards the goal. A jump makes the EC's speed loop hunt, but one code
 * per step cannot follow a load ramp either: the step is one code per
 * STEP_GAP_CODES of gap, at most STEP_MAX_CODES. Codes 1..min_code-1 are
 * skipped; a stopped fan restarts at SPINUP_CODE and steps from there
 * towards the goal.
 */
static int fan_next_code(const struct fan *f, int goal)
{
	int step, next;

	if (f->cur == 0)
		return SPINUP_CODE;
	step = clamp(DIV_ROUND_UP(abs(goal - f->cur), STEP_GAP_CODES), 1, STEP_MAX_CODES);
	next = f->cur + (goal > f->cur ? step : -step);
	if (next > 0 && next < f->min_code)
		next = goal > f->cur ? f->min_code : 0;
	return next;
}

static int curve_goal(struct mec_data *d, struct fan *f, const struct curve *c, u8 temp)
{
	int level = clamp(f->level, 0, c->n - 1);

	while (level + 1 < c->n && temp >= c->temp[level + 1])
		level++;
	while (level > 0 && temp + CURVE_HYST < c->temp[level])
		level--;
	if (level != f->level) {
		if (c->code[level] == CURVE_FIRMWARE)
			pr_info("%s fan: %u C, curve point %u C: firmware\n",
				f->name, temp, c->temp[level]);
		else
			pr_info("%s fan: %u C, curve point %u C: %d RPM\n",
				f->name, temp, c->temp[level], c->code[level] * 100);
		f->level = level;
	}
	return c->code[level];
}

/* A steady target whose RPM still swings by HUNT_SPREAD means the EC loop is hunting. */
static void fan_check_hunting(struct mec_data *d, struct fan *f, int rpm)
{
	int i, lo = INT_MAX, hi = 0;

	if (f->hunts && time_after(jiffies, f->last_hunt + HUNT_FORGET))
		f->hunts = 0;
	if (time_before(jiffies, f->settle_until))
		return;

	f->rpm_win[f->win_n % HUNT_WINDOW] = rpm;
	if (++f->win_n < HUNT_WINDOW)
		return;
	for (i = 0; i < HUNT_WINDOW; i++) {
		lo = min(lo, f->rpm_win[i]);
		hi = max(hi, f->rpm_win[i]);
	}
	if (hi - lo <= HUNT_SPREAD)
		return;

	f->hunts++;
	f->last_hunt = jiffies;
	pr_warn("%s fan hunting at %d RPM (measured %d..%d)\n",
		f->name, f->cur * 100, lo, hi);
	fan_release(d, f, "hunting");
	if (f->hunts >= MAX_HUNTS) {
		f->mode = MODE_FIRMWARE;
		f->hunts = 0;
		pr_warn("%s fan hunted %d times; left to the firmware until pwm%d_enable is set again\n",
			f->name, MAX_HUNTS, f->id + 1);
	} else {
		fan_hold(f, HUNT_BACKOFF);
	}
}

static void fan_tick(struct mec_data *d, struct fan *f, const struct curve *c,
		     const struct telemetry *t)
{
	u8 temp = t->temp[f->id];
	int rpm = t->rpm[f->id];
	const char *why;
	int goal;

	if (f->release_pending) {
		if (!time_before(jiffies, f->next_release))
			fan_release(d, f, "retry");
		return;
	}

	if (!f->hot && (temp >= f->temp_limit || !plausible_temp(temp))) {
		f->hot = true;
		pr_warn("%s fan: EC temperature %u C, limit %u C\n",
			f->name, temp, f->temp_limit);
	} else if (f->hot && plausible_temp(temp) && temp + LIMIT_HYST < f->temp_limit) {
		f->hot = false;
		pr_info("%s fan: EC temperature back to %u C\n", f->name, temp);
	}

	if (f->mode == MODE_MANUAL && time_after(jiffies, f->deadline)) {
		f->mode = c->n ? MODE_CURVE : MODE_FIRMWARE;
		pr_info("%s fan: no pwm%d write for %d s, back to %s\n",
			f->name, f->id + 1, WATCHDOG_SECONDS, mode_names[f->mode]);
	}
	if (f->mode == MODE_CURVE && !c->n)
		f->mode = MODE_FIRMWARE;
	if (f->holding && !time_before(jiffies, f->hold_until))
		f->holding = false;

	if (f->hot)
		why = "temperature limit";
	else if (f->mode == MODE_FIRMWARE)
		why = "firmware mode";
	else if (f->holding)
		why = "backing off";
	else
		why = NULL;

	if (why)
		goal = CURVE_FIRMWARE;
	else if (f->mode == MODE_MANUAL)
		goal = f->manual_code;
	else
		goal = curve_goal(d, f, c, temp);

	if (goal > 0 && goal < f->min_code)
		goal = f->min_code;

	if (goal == CURVE_FIRMWARE) {
		if (f->owned)
			fan_release(d, f, why ?: "curve");
		return;
	}

	if (!f->owned) {
		if (!fan_engage(d, f, rpm)) {
			f->engage_failures = 0;
		} else if (++f->engage_failures >= MAX_ENGAGE_FAILURES) {
			f->mode = MODE_FIRMWARE;
			f->engage_failures = 0;
			pr_warn("%s fan: taking control failed %d times; left to the firmware until pwm%d_enable is set again\n",
				f->name, MAX_ENGAGE_FAILURES, f->id + 1);
		} else {
			fan_hold(f, HUNT_BACKOFF);
		}
		return;
	}

	if (!f->ops->held(d, f, t)) {
		pr_err("%s fan: EC state changed under driver control\n", f->name);
		fan_release(d, f, "unexpected state");
		fan_hold(f, HUNT_BACKOFF);
		return;
	}

	if (f->cur != goal) {
		if (time_before(jiffies, f->next_step))
			return;
		if (fan_set(d, f, fan_next_code(f, goal)))
			fan_hold(f, HUNT_BACKOFF);
		else
			f->next_step = jiffies + STEP_JIFFIES;
		return;
	}

	if (goal > 0)
		fan_check_hunting(d, f, rpm);
}

void mec_monitor(struct work_struct *work)
{
	struct mec_data *d = container_of(to_delayed_work(work), struct mec_data, monitor);
	struct curve curves[NR_FANS];
	struct telemetry t;
	int i;

	scoped_guard(mutex, &curve_lock)
		for (i = 0; i < NR_FANS; i++)
			curves[i] = curve_params[i].curve;

	guard(mutex)(&d->lock);
	if (d->stopped || d->suspended)
		return;

	if (!read_telemetry(&t)) {
		d->read_failures = 0;
		d->t = t;
		d->updated = jiffies;
		d->valid = true;
		for (i = 0; i < NR_FANS; i++)
			fan_tick(d, &d->fans[i], &curves[i], &t);
	} else if (++d->read_failures >= MAX_READ_FAILURES) {
		/* Without temperatures and RPM nothing can be judged: let the firmware decide. */
		for (i = 0; i < NR_FANS; i++) {
			struct fan *f = &d->fans[i];

			if (f->release_pending && time_before(jiffies, f->next_release))
				continue;
			if (f->owned || f->release_pending) {
				fan_release(d, f, "EC telemetry unreadable");
				fan_hold(f, HUNT_BACKOFF);
			}
		}
	}
	queue_delayed_work(system_dfl_wq, &d->monitor, MONITOR_JIFFIES);
}

#define STOP_RELEASE_ATTEMPTS	3

void mec_stop(struct mec_data *d, const char *reason)
{
	int i, attempt;

	scoped_guard(mutex, &d->lock)
		d->stopped = true;
	cancel_delayed_work_sync(&d->monitor);

	guard(mutex)(&d->lock);
	for (i = 0; i < NR_FANS; i++) {
		for (attempt = 0; attempt < STOP_RELEASE_ATTEMPTS; attempt++) {
			if (!fan_release(d, &d->fans[i], reason))
				break;
			msleep(200);
		}
		if (attempt == STOP_RELEASE_ATTEMPTS)
			pr_crit("%s fan may still be under driver control; power the machine off completely before using the EC again\n",
				d->fans[i].name);
	}
}
