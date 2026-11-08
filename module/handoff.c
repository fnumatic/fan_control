// SPDX-License-Identifier: GPL-2.0
/*
 * Taking a fan from the firmware and giving it back: the CPU table checks, the
 * system fan's 0xff fixed-target handoff and the CPU fan's table rewrite, as
 * the fan_ops that fan.c drives.
 */

#include "minisforum_ec.h"

/* Critical row (base, upper, lower) and its slope (F7bsdProfile.cs:88). */
static const u8 cpu_critical_row[3] = { 51, 100, 93 };
#define CPU_CRITICAL_SLOPE	0

/* CPU table checks (F7bsdProfile.cs:120-221) */

/* Selector, temperature override, bands and critical row: never written here. */
const char *check_cpu_static(const struct cpu_table *t, const struct cpu_profile *p)
{
	int r;

	if (!p || t->selector != p->selector)
		return "CPU table selector is unknown or changed";
	if (t->temp_override)
		return "CPU temperature override is not zero";
	for (r = 0; r < CPU_ROWS; r++)
		if (t->rows[r][1] != p->bands[r][0] || t->rows[r][2] != p->bands[r][1])
			return "CPU temperature bands do not match the selected table";
	if (memcmp(t->rows[CPU_ROWS], cpu_critical_row, sizeof(cpu_critical_row)) ||
	    t->slopes[CPU_ROWS] != CPU_CRITICAL_SLOPE)
		return "CPU critical row is not (51, 100, 93, 0)";
	return NULL;
}

bool cpu_is_canonical(const struct cpu_table *t, const struct cpu_profile *p)
{
	int r;

	for (r = 0; r < CPU_ROWS; r++)
		if (t->rows[r][0] != p->bases[r] || t->slopes[r] != p->slopes[r])
			return false;
	return true;
}

/* All seven bases at one code and the slopes at zero: what this driver writes. */
bool cpu_is_fixed(const struct cpu_table *t, int code)
{
	int r;

	for (r = 0; r < CPU_ROWS; r++)
		if (t->rows[r][0] != code || t->slopes[r])
			return false;
	return true;
}

/*
 * States an interrupted enter, update or restore can leave behind
 * (F7bsdProfile.cs:388-506).
 */
static bool slope_restore_prefix(const u8 *obs, const u8 *canon)
{
	int start, end, i;

	for (start = 0; start <= CPU_ROWS; start++) {
		for (end = start; end <= CPU_ROWS; end++) {
			for (i = 0; i < CPU_ROWS; i++)
				if (obs[i] != (i >= start && i < end ? canon[i] : 0))
					break;
			if (i == CPU_ROWS)
				return true;
		}
	}
	return false;
}

static bool enter_or_restore_prefix(const u8 *obs, const u8 *canon)
{
	int restored, written, i;

	for (restored = 0; restored < CPU_ROWS; restored++) {
		if (memcmp(obs, canon, restored))
			continue;
		for (written = restored + 1; written <= CPU_ROWS; written++) {
			for (i = restored; i < written && obs[i] == obs[restored]; i++)
				;
			if (i == written &&
			    !memcmp(obs + written, canon + written, CPU_ROWS - written))
				return true;
		}
	}
	return false;
}

static bool update_or_restore_prefix(const u8 *obs, const u8 *canon)
{
	int restored, t, i;

	for (restored = 0; restored < CPU_ROWS; restored++) {
		if (memcmp(obs, canon, restored))
			continue;
		for (t = restored; t < CPU_ROWS && obs[t] == obs[restored]; t++)
			;
		if (t == CPU_ROWS)
			return true;
		for (i = t; i < CPU_ROWS && obs[i] == obs[t]; i++)
			;
		if (i == CPU_ROWS)
			return true;
	}
	return false;
}

bool cpu_recoverable(const struct cpu_table *t, const struct cpu_profile *p)
{
	u8 bases[CPU_ROWS], slopes[CPU_ROWS];
	int r;

	for (r = 0; r < CPU_ROWS; r++) {
		bases[r] = t->rows[r][0];
		slopes[r] = t->slopes[r];
	}
	if (!memcmp(bases, p->bases, CPU_ROWS))
		return slope_restore_prefix(slopes, p->slopes);
	for (r = 0; r < CPU_ROWS; r++)
		if (slopes[r] || bases[r] > CODE_MAX)
			return false;
	return enter_or_restore_prefix(bases, p->bases) ||
	       update_or_restore_prefix(bases, p->bases);
}

/* System fan: the 0xff fixed-target handoff */

/* PawnIoF7bsdBackend.cs:520-544 */
static int sys_wait_effective(struct device *dev, bool owned)
{
	u8 last = 0;
	int i, ret;

	for (i = 0; i < SYS_HANDOFF_POLLS; i++) {
		ret = xram_read(EC_SYS_EFFECTIVE, &last);
		if (ret)
			return ret;
		if (owned ? last == SYS_SENTINEL : plausible_temp(last))
			return 0;
		if (i + 1 < SYS_HANDOFF_POLLS)
			msleep(SYS_HANDOFF_POLL_MS);
	}
	pr_err("system fan did not %s; effective byte ended at 0x%02x\n",
	       owned ? "enter fixed-target mode" : "return to firmware mode", last);
	return -EIO;
}

/*
 * Seed the maximum target, clear the override and wait until the firmware
 * owns the fan again (PawnIoF7bsdBackend.cs:427-518).
 */
static int sys_release_core(struct mec_data *d)
{
	struct device *dev = d->dev;
	struct ec_byte before[2], write;
	struct sys_regs r, failed = { };
	bool write_failed = false;
	int step, ret;

	for (step = 0; step < SYS_RELEASE_STEPS; step++) {
		ret = read_sys_regs(&r);
		if (ret)
			return ret;
		if (write_failed && r.override == failed.override && r.target == failed.target)
			return -EIO;
		write_failed = false;

		switch (sys_regs_state(&r)) {
		case SYS_FIRMWARE:
			return 0;
		case SYS_RELEASING:
			ret = sys_wait_effective(dev, false);
			if (ret)
				return ret;
			ret = read_sys_regs(&r);
			if (ret)
				return ret;
			if (sys_regs_state(&r) != SYS_FIRMWARE) {
				pr_err("firmware did not resume system fan control\n");
				return -EIO;
			}
			return 0;
		case SYS_FIXED:
			before[0] = (struct ec_byte){ EC_SYS_OVERRIDE, SYS_SENTINEL };
			before[1] = (struct ec_byte){ EC_SYS_TARGET, r.target };
			if (r.target == CODE_MAX)
				write = (struct ec_byte){ EC_SYS_OVERRIDE, 0 };
			else
				write = (struct ec_byte){ EC_SYS_TARGET, CODE_MAX };
			if (xram_write_verified(d, before, ARRAY_SIZE(before), &write, 1, NULL)) {
				write_failed = true;
				failed = r;
			}
			break;
		default:
			pr_err("system fan state is unsupported (effective 0x%02x, override 0x%02x, target %u); no recovery write attempted\n",
			       r.effective, r.override, r.target);
			return -EIO;
		}
	}
	pr_err("system fan release did not reach firmware state\n");
	return -EIO;
}

/*
 * Set the override, keeping the firmware's target (PawnIoF7bsdBackend.cs:392-415).
 * In firmware mode the effective byte follows a temperature and the target
 * follows the firmware's bands, so a precondition that fails before any write
 * is tried once more. The code in force is read back once the firmware has let
 * go of the target.
 */
static int sys_engage(struct mec_data *d, struct fan *f, int rpm, int *start, bool *issued)
{
	struct device *dev = d->dev;
	struct ec_byte before[SYS_POLICY_BYTES + 3];
	struct ec_byte write = { EC_SYS_OVERRIDE, SYS_SENTINEL };
	struct sys_regs r;
	int attempt, ret;
	size_t n;
	u8 target;

	ret = check_sys_policy(dev);
	if (ret)
		return ret;

	for (attempt = 0; attempt < 2; attempt++) {
		ret = read_sys_regs(&r);
		if (ret)
			return ret;
		if (sys_regs_state(&r) != SYS_FIRMWARE) {
			pr_err("system fan is not under firmware control: effective 0x%02x, override 0x%02x, target %u\n",
			       r.effective, r.override, r.target);
			return -EIO;
		}

		n = sys_policy_expectations(before);
		before[n++] = (struct ec_byte){ EC_SYS_EFFECTIVE, r.effective };
		before[n++] = (struct ec_byte){ EC_SYS_OVERRIDE, r.override };
		before[n++] = (struct ec_byte){ EC_SYS_TARGET, r.target };

		ret = xram_write_verified(d, before, n, &write, 1, issued);
		if (!ret || *issued)
			break;
	}
	if (ret)
		return ret;

	ret = sys_wait_effective(dev, true);
	if (ret)
		return ret;
	ret = xram_read(EC_SYS_TARGET, &target);
	if (ret)
		return ret;
	if (target > CODE_MAX) {
		pr_err("system fan target %u after the handoff\n", target);
		return -EIO;
	}
	*start = target;
	return 0;
}

/* PawnIoF7bsdBackend.cs:316-390 */
static int sys_set(struct mec_data *d, struct fan *f, u8 code)
{
	struct device *dev = d->dev;
	struct ec_byte before[SYS_POLICY_BYTES + 3];
	struct ec_byte write = { EC_SYS_TARGET, code };
	struct sys_regs r;
	size_t n;
	int ret;

	ret = check_sys_policy(dev);
	if (ret)
		return ret;
	n = sys_policy_expectations(before);
	before[n++] = (struct ec_byte){ EC_SYS_EFFECTIVE, SYS_SENTINEL };
	before[n++] = (struct ec_byte){ EC_SYS_OVERRIDE, SYS_SENTINEL };
	before[n++] = (struct ec_byte){ EC_SYS_TARGET, f->cur };
	ret = xram_write_verified(d, before, n, &write, 1, NULL);
	if (ret)
		return ret;
	ret = read_sys_regs(&r);
	if (ret)
		return ret;
	if (r.effective != SYS_SENTINEL || r.override != SYS_SENTINEL || r.target != code) {
		pr_err("fixed-target state not confirmed after writing %u\n", code);
		return -EIO;
	}
	return 0;
}

static int sys_release(struct mec_data *d, struct fan *f)
{
	return sys_release_core(d);
}

static bool sys_held(struct mec_data *d, struct fan *f, const struct telemetry *t)
{
	return t->sys_effective == SYS_SENTINEL && t->sys_override == SYS_SENTINEL &&
	       t->sys_target == f->cur;
}

const struct fan_ops sys_fan_ops = {
	.engage = sys_engage,
	.set = sys_set,
	.release = sys_release,
	.held = sys_held,
};

/* CPU fan: the same code in the seven normal rows of the BIOS-selected table */

#define CPU_SNAPSHOT_BYTES	(2 + sizeof_field(struct cpu_table, rows) + \
				 sizeof_field(struct cpu_table, slopes))

/* The whole snapshot as write preconditions (PawnIoF7bsdBackend.cs:227-232). */
static size_t cpu_expectations(const struct cpu_table *t, struct ec_byte *out)
{
	const u8 *rows = &t->rows[0][0];
	size_t i, n = 0;

	out[n++] = (struct ec_byte){ EC_CPU_SELECTOR, t->selector };
	out[n++] = (struct ec_byte){ EC_CPU_TEMP_OVERRIDE, t->temp_override };
	for (i = 0; i < sizeof(t->rows); i++)
		out[n++] = (struct ec_byte){ EC_CPU_ROWS + i, rows[i] };
	for (i = 0; i < sizeof(t->slopes); i++)
		out[n++] = (struct ec_byte){ EC_CPU_SLOPES + i, t->slopes[i] };
	return n;
}

static int read_cpu_stable(struct device *dev, struct cpu_table *t)
{
	struct cpu_table tmp;
	int ret;

	ret = read_stable(read_cpu_table, t, &tmp, sizeof(*t));
	if (ret)
		pr_err("CPU fan table is not stable\n");
	return ret;
}

/* Slopes to zero, then every base to the code (F7bsdProfile.cs:306-317). */
static int cpu_engage(struct mec_data *d, struct fan *f, int rpm, int *start, bool *issued)
{
	struct ec_byte before[CPU_SNAPSHOT_BYTES], writes[2 * CPU_ROWS];
	struct cpu_table t;
	const char *problem;
	int code, r, n = 0, ret;

	code = clamp(DIV_ROUND_CLOSEST(rpm, 100), 0, CODE_MAX);
	if (code > 0 && code < f->min_code)
		code = f->min_code;

	ret = read_cpu_stable(d->dev, &t);
	if (ret)
		return ret;
	problem = check_cpu_static(&t, d->cpu_profile);
	if (!problem && !cpu_is_canonical(&t, d->cpu_profile))
		problem = "CPU table is not the canonical firmware table";
	if (problem) {
		pr_err("%s; CPU fan left to the firmware\n", problem);
		return -EIO;
	}

	for (r = 0; r < CPU_ROWS; r++)
		writes[n++] = (struct ec_byte){ EC_CPU_SLOPES + r, 0 };
	for (r = 0; r < CPU_ROWS; r++)
		writes[n++] = (struct ec_byte){ EC_CPU_ROWS + 3 * r, code };
	ret = xram_write_verified(d, before, cpu_expectations(&t, before), writes, n, issued);
	if (!ret)
		*start = code;
	return ret;
}

/* PawnIoF7bsdBackend.cs:203-252 */
static int cpu_set(struct mec_data *d, struct fan *f, u8 code)
{
	struct ec_byte before[CPU_SNAPSHOT_BYTES], writes[CPU_ROWS];
	struct cpu_table t;
	const char *problem;
	int r, ret;

	ret = read_cpu_stable(d->dev, &t);
	if (ret)
		return ret;
	problem = check_cpu_static(&t, d->cpu_profile);
	if (!problem && !cpu_is_fixed(&t, f->cur))
		problem = "CPU table is not the state last written by this driver";
	if (problem) {
		pr_err("%s\n", problem);
		return -EIO;
	}

	for (r = 0; r < CPU_ROWS; r++)
		writes[r] = (struct ec_byte){ EC_CPU_ROWS + 3 * r, code };
	return xram_write_verified(d, before, cpu_expectations(&t, before), writes, CPU_ROWS, NULL);
}

/*
 * Slopes to zero, canonical bases, canonical slopes, then verify the whole
 * table (F7bsdProfile.cs:319-332, PawnIoF7bsdBackend.cs:264-314).
 */
static int cpu_release(struct mec_data *d, struct fan *f)
{
	const struct cpu_profile *p = d->cpu_profile;
	struct ec_byte before[CPU_SNAPSHOT_BYTES], writes[3 * CPU_ROWS];
	struct cpu_table t;
	const char *problem;
	int r, n = 0, ret;

	ret = read_cpu_stable(d->dev, &t);
	if (ret)
		return ret;
	problem = check_cpu_static(&t, p);
	if (problem) {
		pr_err("%s; no CPU restore write attempted\n", problem);
		return -EIO;
	}
	if (cpu_is_canonical(&t, p))
		return 0;
	if (!cpu_recoverable(&t, p)) {
		pr_err("CPU table is in a state this driver cannot produce; no restore write attempted\n");
		return -EIO;
	}

	for (r = 0; r < CPU_ROWS; r++)
		writes[n++] = (struct ec_byte){ EC_CPU_SLOPES + r, 0 };
	for (r = 0; r < CPU_ROWS; r++)
		writes[n++] = (struct ec_byte){ EC_CPU_ROWS + 3 * r, p->bases[r] };
	for (r = 0; r < CPU_ROWS; r++)
		writes[n++] = (struct ec_byte){ EC_CPU_SLOPES + r, p->slopes[r] };
	xram_write_verified(d, before, cpu_expectations(&t, before), writes, n, NULL);

	ret = read_cpu_stable(d->dev, &t);
	if (ret)
		return ret;
	if (check_cpu_static(&t, p) || !cpu_is_canonical(&t, p)) {
		pr_err("the canonical CPU table was not restored\n");
		return -EIO;
	}
	return 0;
}

/* A mismatch is read once more before it counts: one bad snapshot is not trusted. */
static bool cpu_held(struct mec_data *d, struct fan *f, const struct telemetry *unused)
{
	struct cpu_table t;
	int attempt;

	for (attempt = 0; attempt < 2; attempt++)
		if (!read_cpu_table(&t) && !check_cpu_static(&t, d->cpu_profile) &&
		    cpu_is_fixed(&t, f->cur))
			return true;
	return false;
}

const struct fan_ops cpu_fan_ops = {
	.engage = cpu_engage,
	.set = cpu_set,
	.release = cpu_release,
	.held = cpu_held,
};
