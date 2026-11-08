// SPDX-License-Identifier: GPL-2.0
/*
 * hwmon driver for the ITE IT5571 embedded controller of the Minisforum
 * UM760 Slim (board F7BSI, EC firmware 0.5) and UM790 Pro (board F7BSC,
 * EC firmware 2.6).
 *
 * The register map, admission rules and I2EC access sequence come from the
 * "f7bsi" and "f7bsc" profiles of FanControl.MinisforumUM (github.com/zuyan9),
 * commit d61e46c. File:line references point into that repository.
 *
 * Each fan (pwm1 CPU, pwm2 system) runs in one of three modes: a fixed value
 * written by userspace, this driver's temperature curve (module parameters
 * cpu_curve and system_curve), or the EC firmware's own policy.
 *
 * EC memory writes are limited to what the plugin writes: the system-fan
 * handoff (override 0x088b to 0x00/0xff, target 0x0885 to 0..51) and the
 * seven bases and slopes of the BIOS-selected CPU table, never its bands or
 * critical row. Every write checks its preconditions first and is read back.
 *
 * The driver moves a fan towards its goal in steps of one to four codes that
 * grow with the gap, because a larger jump makes the EC's speed loop hunt; a
 * stopped fan restarts at SPINUP_CODE and steps down. It
 * gives the fan back to the firmware at a curve's firmware point, in mode 3,
 * when its EC temperature reaches its limit or reads implausibly, when it
 * hunts, when the EC is not in the state it last wrote, when a write or the
 * telemetry fails, on unload, sleep and shutdown. A fixed value that is not
 * renewed for WATCHDOG_SECONDS falls back to the curve.
 */

#include "minisforum_ec.h"

#ifndef MEC_VERSION
#error "MEC_VERSION comes from dkms.conf through Kbuild"
#endif

static struct platform_device *mec_pdev;

static int mec_update(struct mec_data *d)
{
	struct telemetry t;
	enum sys_state state;
	int ret;

	if (d->valid && time_before(jiffies, d->updated + CACHE_JIFFIES))
		return 0;

	ret = read_telemetry(&t);
	if (ret)
		return ret;

	state = telemetry_sys_state(&t);
	if (!d->fans[FAN_SYS].owned && state != SYS_FIRMWARE && !d->sys_state_warned) {
		pr_warn("system fan left firmware control without this driver: effective 0x%02x, override 0x%02x, target %u (%s)\n",
			t.sys_effective, t.sys_override, t.sys_target,
			sys_state_names[state]);
		d->sys_state_warned = true;
	}

	d->t = t;
	d->updated = jiffies;
	d->valid = true;
	return 0;
}

/* hwmon interface */

static const char * const fan_labels[] = { "CPU", "System" };
static const char * const temp_labels[] = { "EC CPU", "EC System" };

static umode_t mec_is_visible(const void *drvdata, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	if (type == hwmon_pwm)
		return 0644;
	return 0444;
}

static int mec_read(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val)
{
	struct mec_data *d = dev_get_drvdata(dev);
	struct fan *f;
	int ret;

	guard(mutex)(&d->lock);
	ret = mec_update(d);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_fan:
		*val = d->t.rpm[channel];
		return 0;
	case hwmon_temp:
		*val = d->t.temp[channel] * 1000;
		return 0;
	case hwmon_pwm:
		f = &d->fans[channel];
		switch (attr) {
		case hwmon_pwm_input:
			/* EC target code 0..51 (nominally code * 100 RPM), as hwmon's 0..255. */
			if (f->owned && f->cur >= 0)
				*val = f->cur * PWM_PER_CODE;
			else if (channel == FAN_SYS)
				*val = min_t(int, d->t.sys_target, CODE_MAX) * PWM_PER_CODE;
			else
				return -ENODATA;
			return 0;
		case hwmon_pwm_enable:
			*val = f->mode;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static bool code_valid(const struct fan *f, long val)
{
	return val == 0 || (val >= f->min_code && val <= CODE_MAX);
}

/*
 * pwmN_enable: 1 fixed value (starting at the current speed), 2 curve,
 * 3 firmware. pwmN: the fixed value, 0..255 in steps of PWM_PER_CODE (one EC
 * code), accepted in mode 1 only; each write restarts the watchdog, after
 * which the fan returns to its curve.
 */
static int mec_write(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long val)
{
	struct mec_data *d = dev_get_drvdata(dev);
	struct fan *f;
	int code, ret;
	bool has_curve;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;
	f = &d->fans[channel];

	scoped_guard(mutex, &curve_lock)
		has_curve = curve_params[channel].curve.n > 0;

	guard(mutex)(&d->lock);
	if (d->stopped)
		return -ENODEV;
	if (d->suspended)
		return -EBUSY;

	switch (attr) {
	case hwmon_pwm_input:
		if (f->mode != MODE_MANUAL)
			return -EBUSY;
		if (val < 0 || val > CODE_MAX * PWM_PER_CODE)
			return -EINVAL;
		code = DIV_ROUND_CLOSEST(val, PWM_PER_CODE);
		/* 1..2 would round to a stop: refuse it rather than stop the fan. */
		if ((val && !code) || !code_valid(f, code))
			return -EINVAL;
		f->manual_code = code;
		break;
	case hwmon_pwm_enable:
		if (val == MODE_MANUAL) {
			ret = mec_update(d);
			if (ret)
				return ret;
			if (f->mode != MODE_MANUAL) {
				if (f->owned && f->cur >= 0)
					code = f->cur;
				else if (channel == FAN_SYS)
					code = d->t.sys_target;
				else
					code = DIV_ROUND_CLOSEST(d->t.rpm[channel], 100);
				code = clamp(code, 0, CODE_MAX);
				if (!code_valid(f, code))
					code = f->min_code;
				f->manual_code = code;
			}
		} else if (val == MODE_CURVE) {
			if (!has_curve)
				return -EINVAL;
		} else if (val != MODE_FIRMWARE) {
			return -EINVAL;
		}
		if (val != f->mode)
			pr_info("%s fan: mode %s\n", f->name, mode_names[val]);
		f->mode = val;
		f->holding = false;
		f->hunts = 0;
		f->engage_failures = 0;
		f->next_release = jiffies;	/* a pending release is retried now */
		break;
	default:
		return -EOPNOTSUPP;
	}
	f->deadline = jiffies + WATCHDOG_SECONDS * HZ;
	mod_delayed_work(system_dfl_wq, &d->monitor, 0);
	return 0;
}

static int mec_read_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		*str = fan_labels[channel];
		return 0;
	case hwmon_temp:
		*str = temp_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops mec_hwmon_ops = {
	.is_visible = mec_is_visible,
	.read = mec_read,
	.read_string = mec_read_string,
	.write = mec_write,
};

static const struct hwmon_channel_info * const mec_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info mec_chip_info = {
	.ops = &mec_hwmon_ops,
	.info = mec_info,
};

/* debugfs: a fresh, uncached dump of everything the driver can read. */

static int state_show(struct seq_file *s, void *unused)
{
	struct mec_data *d = s->private;
	struct cpu_table cpu, cpu_tmp;
	struct sys_policy pol, pol_tmp;
	const struct cpu_profile *profile;
	const char *cpu_problem;
	struct telemetry t;
	u8 pnp[3], ident[ARRAY_SIZE(ec_ident_addrs)];
	struct fan *f;
	size_t i;
	int r, ret;

	guard(mutex)(&d->lock);

	for (i = 0; i < ARRAY_SIZE(pnp); i++)
		pnp[i] = pnp_read(SIO_REG_CHIP_ID + i);
	for (i = 0; i < ARRAY_SIZE(ident); i++) {
		ret = xram_read(ec_ident_addrs[i], &ident[i]);
		if (ret)
			return ret;
	}
	seq_printf(s, "pnp:          %*ph\n", (int)sizeof(pnp), pnp);
	seq_printf(s, "controller:   %*ph\n", (int)sizeof(ident), ident);

	ret = read_stable(read_cpu_table, &cpu, &cpu_tmp, sizeof(cpu));
	if (ret) {
		seq_puts(s, "cpu table:    no stable snapshot\n");
	} else {
		profile = find_cpu_profile(cpu.selector);
		cpu_problem = check_cpu_static(&cpu, profile);
		if (!cpu_problem && cpu_is_fixed(&cpu, d->fans[FAN_CPU].cur) && d->fans[FAN_CPU].owned)
			cpu_problem = "held by this driver";
		else if (!cpu_problem && !cpu_is_canonical(&cpu, profile))
			cpu_problem = "NOT canonical";
		seq_printf(s, "cpu table:    selector 0x%02x (%s), temp override 0x%02x, %s\n",
			   cpu.selector, profile ? profile->name : "?",
			   cpu.temp_override, cpu_problem ?: "canonical");
		seq_puts(s, "  row  base upper lower slope\n");
		for (r = 0; r <= CPU_ROWS; r++)
			seq_printf(s, "  %3d  %4u %5u %5u %5u%s\n", r,
				   cpu.rows[r][0], cpu.rows[r][1], cpu.rows[r][2],
				   cpu.slopes[r], r == CPU_ROWS ? "  critical" : "");
	}

	ret = read_stable(read_sys_policy, &pol, &pol_tmp, sizeof(pol));
	if (ret)
		seq_puts(s, "sys policy:   no stable snapshot\n");
	else
		seq_printf(s, "sys policy:   %*ph (%s)\n", (int)sizeof(pol), &pol,
			   memcmp(&pol, mec_board->sys_policy, sizeof(pol)) ?
			   "MISMATCH" : "matches");

	ret = read_telemetry(&t);
	if (ret) {
		seq_printf(s, "telemetry:    read failed (%d)\n", ret);
		return 0;
	}
	seq_printf(s, "sys state:    effective 0x%02x, override 0x%02x, target %u -> %s\n",
		   t.sys_effective, t.sys_override, t.sys_target,
		   sys_state_names[telemetry_sys_state(&t)]);
	seq_printf(s, "fan rpm:      cpu %d, sys %d\n", t.rpm[0], t.rpm[1]);
	seq_printf(s, "pwm duty:     cpu (0x%04x) %u, sys (0x%04x) %u\n",
		   EC_DUTY_CPU, t.duty[0], EC_DUTY_SYS, t.duty[1]);
	seq_printf(s, "ec temp:      cpu %u C, sys %u C\n", t.temp[0], t.temp[1]);

	for (i = 0; i < NR_FANS; i++) {
		f = &d->fans[i];
		seq_printf(s, "%-6s fan:   mode %s, %s, code %d, curve point %d%s%s%s, hunts %d\n",
			   f->name, mode_names[f->mode],
			   f->release_pending ? "RELEASE PENDING" :
			   f->owned ? "driver control" : "firmware control",
			   f->cur, f->level, f->hot ? ", HOT" : "",
			   f->holding ? ", backing off" : "",
			   f->mode == MODE_MANUAL ? ", manual" : "", f->hunts);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(state);

static void mec_debugfs_remove(void *dir)
{
	debugfs_remove_recursive(dir);
}

static void fan_init(struct fan *f, enum fan_id id, const char *name,
		     const struct fan_ops *ops, u8 temp_limit)
{
	f->id = id;
	f->name = name;
	f->ops = ops;
	f->min_code = board_min_code(id);
	f->temp_limit = temp_limit;
	f->mode = MODE_FIRMWARE;
	f->cur = -1;
}

/*
 * Admission checks that touch the hardware: signatures, CPU table, exact
 * system policy, and both fans back with the firmware. A state left by an
 * earlier session that this driver can produce is repaired first
 * (PawnIoF7bsdBackend.cs:49-126); anything else refuses the load.
 */
static int mec_admit(struct mec_data *d, struct telemetry *t)
{
	struct device *dev = d->dev;
	struct cpu_table cpu, cpu_tmp;
	const struct cpu_profile *p;
	enum sys_state state;
	struct sys_regs r;
	const char *problem;
	bool cpu_repair;
	int ret;

	guard(mutex)(&d->lock);

	/* Every check first, no write until all of them pass. */
	ret = mec_check_controller(dev);
	if (ret)
		return ret;

	ret = read_stable(read_cpu_table, &cpu, &cpu_tmp, sizeof(cpu));
	if (ret)
		return mec_fail(ret, "CPU fan table is not stable\n");
	p = find_cpu_profile(cpu.selector);
	problem = check_cpu_static(&cpu, p);
	if (problem)
		return mec_fail(-ENODEV, "%s (selector 0x%02x)\n",
				problem, cpu.selector);
	cpu_repair = !cpu_is_canonical(&cpu, p);
	if (cpu_repair && !cpu_recoverable(&cpu, p))
		return mec_fail(-ENODEV,
				"CPU table is neither canonical nor a state this driver can repair\n");
	d->cpu_profile = p;

	ret = check_sys_policy(dev);
	if (ret)
		return -ENODEV;

	/*
	 * Only the three state bytes (PawnIoF7bsdBackend.cs:74): a tachometer
	 * glitch must not keep a leftover from being repaired.
	 */
	ret = read_sys_regs(&r);
	if (ret)
		return ret;
	state = sys_regs_state(&r);
	if (state == SYS_UNSUPPORTED)
		return mec_fail(-ENODEV,
				"system fan state is unsupported: effective 0x%02x, override 0x%02x, target %u\n",
				r.effective, r.override, r.target);

	/*
	 * Repair what an earlier session left (PawnIoF7bsdBackend.cs:84-99):
	 * every leftover is marked first, so that a failed repair still has the
	 * rest released on the way out, and the system fan goes first because
	 * the firmware has no temperature fallback while it is in the handoff.
	 */
	d->fans[FAN_SYS].owned = state != SYS_FIRMWARE;
	d->fans[FAN_CPU].owned = cpu_repair;
	if (state != SYS_FIRMWARE) {
		pr_warn("system fan found in %s state (effective 0x%02x, override 0x%02x, target %u)\n",
			sys_state_names[state], r.effective, r.override, r.target);
		if (fan_release(d, &d->fans[FAN_SYS], "leftover from an earlier session"))
			return mec_fail(-EIO,
					"could not return the system fan to firmware; power the machine off completely\n");
		ret = read_sys_regs(&r);
		if (ret)
			return ret;
		if (sys_regs_state(&r) != SYS_FIRMWARE)
			return mec_fail(-EIO,
					"system fan did not return to firmware control\n");
	}
	if (cpu_repair) {
		pr_warn("CPU table left by an earlier session; restoring it\n");
		if (fan_release(d, &d->fans[FAN_CPU], "leftover from an earlier session"))
			return mec_fail(-EIO,
					"could not restore the CPU table; power the machine off completely\n");
	}
	ret = check_sys_policy(dev);
	if (ret)
		return -ENODEV;

	ret = read_telemetry(t);
	if (ret)
		return mec_fail(ret, "telemetry read failed\n");
	return 0;
}

static void mec_remove_action(void *data)
{
	mec_stop(data, "driver unload");
}

static int mec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct telemetry t;
	struct mec_data *d;
	struct device *hwmon;
	struct dentry *dir;
	int i, ret;

	if (acpi_check_region(SIO_INDEX, 2, DRVNAME))
		return mec_fail(-EBUSY,
				"ACPI declares an operation region over ports 0x2e-0x2f\n");
	/* Exclusive (not muxed): other Super-I/O drivers get -EBUSY. */
	if (!devm_request_region(dev, SIO_INDEX, 2, DRVNAME))
		return mec_fail(-EBUSY, "ports 0x2e-0x2f are already in use\n");

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	fan_init(&d->fans[FAN_CPU], FAN_CPU, "cpu", &cpu_fan_ops, CPU_TEMP_LIMIT);
	fan_init(&d->fans[FAN_SYS], FAN_SYS, "system", &sys_fan_ops, SYS_TEMP_LIMIT);
	INIT_DELAYED_WORK(&d->monitor, mec_monitor);
	ret = devm_mutex_init(dev, &d->lock);
	if (ret)
		return ret;

	/*
	 * Runs after hwmon is gone and before the ports are released; also on
	 * probe failure.
	 */
	ret = devm_add_action_or_reset(dev, mec_remove_action, d);
	if (ret)
		return ret;

	ret = mec_admit(d, &t);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, d);

	scoped_guard(mutex, &curve_lock)
		for (i = 0; i < NR_FANS; i++)
			if (curve_params[i].curve.n)
				d->fans[i].mode = MODE_CURVE;

	hwmon = devm_hwmon_device_register_with_info(dev, DRVNAME, d,
						     &mec_chip_info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	dir = debugfs_create_dir(DRVNAME, NULL);
	debugfs_create_file("state", 0400, dir, d, &state_fops);
	ret = devm_add_action_or_reset(dev, mec_debugfs_remove, dir);
	if (ret)
		return ret;

	pr_info("%s v%s; CPU table %s (0x%02x); cpu fan %s, %d RPM; system fan %s, %d RPM\n",
		mec_board->name, MEC_VERSION, d->cpu_profile->name, d->cpu_profile->selector,
		mode_names[d->fans[FAN_CPU].mode], t.rpm[0],
		mode_names[d->fans[FAN_SYS].mode], t.rpm[1]);
	queue_delayed_work(system_dfl_wq, &d->monitor, 0);
	return 0;
}

/*
 * System sleep (suspend, hibernate): hand both fans back first, and refuse to
 * sleep if that fails. The selected modes apply again RESUME_SETTLE_JIFFIES
 * after resume, once the EC has its state back.
 */
static int mec_suspend(struct device *dev)
{
	struct mec_data *d = dev_get_drvdata(dev);
	int i, ret = 0;

	scoped_guard(mutex, &d->lock) {
		d->suspended = true;
		for (i = 0; i < NR_FANS; i++)
			if (fan_release(d, &d->fans[i], "system sleep"))
				ret = -EIO;
		if (ret) {
			d->suspended = false;
			mod_delayed_work(system_dfl_wq, &d->monitor, 0);
		}
	}
	if (!ret)
		cancel_delayed_work_sync(&d->monitor);
	return ret;
}

static int mec_resume(struct device *dev)
{
	struct mec_data *d = dev_get_drvdata(dev);

	scoped_guard(mutex, &d->lock)
		d->suspended = false;
	mod_delayed_work(system_dfl_wq, &d->monitor, RESUME_SETTLE_JIFFIES);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mec_pm_ops, mec_suspend, mec_resume);

/* Reboot and power-off: never leave a fan under driver control. */
static void mec_shutdown(struct platform_device *pdev)
{
	struct mec_data *d = platform_get_drvdata(pdev);

	if (d)
		mec_stop(d, "shutdown");
}

static struct platform_driver mec_driver = {
	.driver = {
		.name = DRVNAME,
		.pm = pm_sleep_ptr(&mec_pm_ops),
	},
	.shutdown = mec_shutdown,
};

static int __init mec_init(void)
{
	int ret;

	ret = mec_check_host();
	if (ret)
		return ret;

	mec_pdev = platform_create_bundle(&mec_driver, mec_probe, NULL, 0, NULL, 0);
	return PTR_ERR_OR_ZERO(mec_pdev);
}

static void __exit mec_exit(void)
{
	platform_device_unregister(mec_pdev);
	platform_driver_unregister(&mec_driver);
}

module_init(mec_init);
module_exit(mec_exit);

MODULE_DESCRIPTION("Minisforum UM760 Slim (F7BSI) and UM790 Pro (F7BSC) EC fan monitor and control");
MODULE_LICENSE("GPL");
MODULE_VERSION(MEC_VERSION);
