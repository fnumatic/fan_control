// SPDX-License-Identifier: GPL-2.0
/*
 * Access to the EC: Super-I/O and I2EC port primitives, the read allowlist,
 * stable reads of telemetry and tables, the controller and policy checks, and
 * the verified write path.
 */

#include "minisforum_ec.h"

/* Controller signature, compared with the PNP bytes (F7bsdProfile.cs:34-35). */
const u16 ec_ident_addrs[] = { 0x2000, 0x2001, 0x2002, 0x200d, 0x180c, 0x1841 };

/*
 * Every EC address this driver may read: the plugin's read allowlist
 * (F7bsdProfile.cs:90-97) plus the two PWM duty registers. Those two are read
 * for diagnostics only, to observe the system-fan behaviour of upstream
 * issue #14 (technical.md:121-124; 0x1803/0x1804 are pwm1/pwm2 in the
 * author's diagnostic log).
 */
static const struct {
	u16 first;
	u16 last;
} xram_readable_ranges[] = {
	{ 0x0305, 0x0305 },
	{ 0x0309, 0x0309 },
	{ 0x0310, 0x0327 },
	{ 0x032f, 0x032f },
	{ 0x0330, 0x0338 },
	{ 0x0885, 0x0885 },
	{ 0x0889, 0x088b },
	{ 0x08b0, 0x08b7 },
	{ 0x08c0, 0x08c2 },
	{ 0x1803, 0x1804 },
	{ 0x180c, 0x180c },
	{ 0x181e, 0x1821 },
	{ 0x1841, 0x1841 },
	{ 0x2000, 0x2002 },
	{ 0x200d, 0x200d },
};

const char * const sys_state_names[] = {
	[SYS_FIRMWARE] = "firmware",
	[SYS_FIXED] = "fixed-target",
	[SYS_RELEASING] = "releasing",
	[SYS_UNSUPPORTED] = "unsupported",
};

/* Port primitives. Callers hold mec_data.lock and the I/O region. */

static void sio_write(u8 reg, u8 val)
{
	outb(reg, SIO_INDEX);
	outb(val, SIO_DATA);
}

static u8 sio_read(u8 reg)
{
	outb(reg, SIO_INDEX);
	return inb(SIO_DATA);
}

/* PawnIoNative.cs:274 */
static void sio_park_pnp(void)
{
	outb(SIO_REG_CHIP_ID, SIO_INDEX);
}

/* PawnIoNative.cs:266-272 */
static void sio_park(void)
{
	sio_write(SIO_REG_D2_INDEX, D2_I2EC_ADDR_LOW);
	sio_park_pnp();
}

/* PawnIoNative.cs:227-233 */
u8 pnp_read(u8 reg)
{
	u8 val = sio_read(reg);

	sio_park_pnp();
	return val;
}

static void d2_write(u8 reg, u8 val)
{
	sio_write(SIO_REG_D2_INDEX, reg);
	sio_write(SIO_REG_D2_DATA, val);
}

/* PawnIoNative.cs:235-243, 258-264 */
static u8 i2ec_read(u16 addr)
{
	u8 val;

	d2_write(D2_I2EC_ADDR_HIGH, addr >> 8);
	d2_write(D2_I2EC_ADDR_LOW, addr & 0xff);
	sio_write(SIO_REG_D2_INDEX, D2_I2EC_DATA);
	val = sio_read(SIO_REG_D2_DATA);
	sio_park();
	return val;
}

/* PawnIoNative.cs:245-254 */
static void i2ec_write(u16 addr, u8 val)
{
	d2_write(D2_I2EC_ADDR_HIGH, addr >> 8);
	d2_write(D2_I2EC_ADDR_LOW, addr & 0xff);
	sio_write(SIO_REG_D2_INDEX, D2_I2EC_DATA);
	sio_write(SIO_REG_D2_DATA, val);
	sio_park();
}

static bool xram_readable(u16 addr)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(xram_readable_ranges); i++)
		if (addr >= xram_readable_ranges[i].first && addr <= xram_readable_ranges[i].last)
			return true;
	return false;
}

int xram_read(u16 addr, u8 *val)
{
	if (WARN_ONCE(!xram_readable(addr),
		      DRVNAME ": EC read 0x%04x is not allowed\n", addr))
		return -EPERM;
	*val = i2ec_read(addr);
	return 0;
}

static int xram_read_range(u16 first, u8 *out, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = xram_read(first + i, &out[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Configuration snapshots are accepted only when two consecutive reads agree
 * (PawnIoF7bsdBackend.cs:546-562).
 */
int read_stable(int (*read)(void *buf), void *out, void *tmp, size_t size)
{
	int i, ret;

	ret = read(out);
	if (ret)
		return ret;
	for (i = 1; i < STABLE_ATTEMPTS; i++) {
		ret = read(tmp);
		if (ret)
			return ret;
		if (!memcmp(tmp, out, size))
			return 0;
		memcpy(out, tmp, size);
	}
	return -EIO;
}

int read_cpu_table(void *buf)
{
	struct cpu_table *t = buf;
	int ret;

	ret = xram_read(EC_CPU_SELECTOR, &t->selector);
	if (ret)
		return ret;
	ret = xram_read(EC_CPU_TEMP_OVERRIDE, &t->temp_override);
	if (ret)
		return ret;
	ret = xram_read_range(EC_CPU_ROWS, &t->rows[0][0], sizeof(t->rows));
	if (ret)
		return ret;
	return xram_read_range(EC_CPU_SLOPES, t->slopes, sizeof(t->slopes));
}

int read_sys_policy(void *buf)
{
	struct sys_policy *p = buf;
	int ret;

	ret = xram_read_range(EC_SYS_POLICY_ROWS, &p->rows[0][0], sizeof(p->rows));
	if (ret)
		return ret;
	return xram_read_range(EC_SYS_POLICY_TAIL, p->tail, sizeof(p->tail));
}

/*
 * Tachometer: low, high, low again; the two low bytes must agree
 * (PawnIoF7bsdBackend.cs:597-615, 638-657).
 */
static int read_tach(u16 low, int *rpm)
{
	u8 lo1, hi, lo2;
	u16 counter;
	int i, ret;

	for (i = 0; i < TACH_ATTEMPTS; i++) {
		ret = xram_read(low, &lo1);
		if (!ret)
			ret = xram_read(low + 1, &hi);
		if (!ret)
			ret = xram_read(low, &lo2);
		if (ret)
			return ret;
		if (lo1 != lo2)
			continue;
		counter = lo1 | hi << 8;
		*rpm = counter ? TACH_DIVIDEND / counter : 0;
		return 0;
	}
	return -EIO;
}

/* F7bsdProfile.cs:304 */
bool plausible_temp(u8 val)
{
	return val >= 1 && val <= 120;
}

/* F7bsdProfile.cs:223-250 */
static enum sys_state classify_sys(u8 effective, u8 override, u8 target)
{
	if (target > CODE_MAX)
		return SYS_UNSUPPORTED;
	if (override == 0) {
		if (plausible_temp(effective))
			return SYS_FIRMWARE;
		return effective == SYS_SENTINEL ? SYS_RELEASING : SYS_UNSUPPORTED;
	}
	if (override == SYS_SENTINEL &&
	    (effective == SYS_SENTINEL || plausible_temp(effective)))
		return SYS_FIXED;
	return SYS_UNSUPPORTED;
}

int read_telemetry(struct telemetry *t)
{
	int ret;

	ret = read_tach(EC_TACH_CPU, &t->rpm[0]);
	if (ret)
		return ret;
	ret = read_tach(EC_TACH_SYS, &t->rpm[1]);
	if (ret)
		return ret;
	ret = xram_read(EC_TEMP_CPU, &t->temp[0]);
	if (ret)
		return ret;
	ret = xram_read(EC_TEMP_SYS, &t->temp[1]);
	if (ret)
		return ret;
	ret = xram_read(EC_SYS_EFFECTIVE, &t->sys_effective);
	if (ret)
		return ret;
	ret = xram_read(EC_SYS_OVERRIDE, &t->sys_override);
	if (ret)
		return ret;
	ret = xram_read(EC_SYS_TARGET, &t->sys_target);
	if (ret)
		return ret;
	ret = xram_read(EC_DUTY_CPU, &t->duty[0]);
	if (ret)
		return ret;
	return xram_read(EC_DUTY_SYS, &t->duty[1]);
}

enum sys_state telemetry_sys_state(const struct telemetry *t)
{
	return classify_sys(t->sys_effective, t->sys_override, t->sys_target);
}

/*
 * PNP and controller signatures (PawnIoNative.cs:62-88). The plugin checks
 * them once when it opens the port; this driver also checks before every write.
 */
int mec_check_controller(struct device *dev)
{
	u8 pnp[3], ident[ARRAY_SIZE(ec_ident_addrs)];
	u8 expected[ARRAY_SIZE(ec_ident_addrs)] = { 0x55, 0x71, 0x00, 0x43, 0x14, 0x7f };
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pnp); i++)
		pnp[i] = pnp_read(SIO_REG_CHIP_ID + i);
	if (pnp[0] != 0x55 || pnp[1] != 0x71) {
		pr_err("Super-I/O chip ID %*ph is not an IT5571; no EC memory access attempted\n",
		       (int)sizeof(pnp), pnp);
		return -ENODEV;
	}

	/* Only the silicon revision may vary, and it must match the PNP byte. */
	expected[2] = pnp[2];
	for (i = 0; i < ARRAY_SIZE(ident); i++) {
		ret = xram_read(ec_ident_addrs[i], &ident[i]);
		if (ret)
			return ret;
	}
	if (memcmp(ident, expected, sizeof(ident))) {
		pr_err("controller signature %*ph does not match %*ph\n",
		       (int)sizeof(ident), ident, (int)sizeof(expected), expected);
		return -ENODEV;
	}
	return 0;
}

/* System-fan control */

int read_sys_regs(struct sys_regs *r)
{
	int ret;

	ret = xram_read(EC_SYS_EFFECTIVE, &r->effective);
	if (ret)
		return ret;
	ret = xram_read(EC_SYS_OVERRIDE, &r->override);
	if (ret)
		return ret;
	return xram_read(EC_SYS_TARGET, &r->target);
}

enum sys_state sys_regs_state(const struct sys_regs *r)
{
	return classify_sys(r->effective, r->override, r->target);
}

int check_sys_policy(struct device *dev)
{
	struct sys_policy pol, tmp;
	int ret;

	ret = read_stable(read_sys_policy, &pol, &tmp, sizeof(pol));
	if (ret) {
		pr_err("system fan policy is not stable\n");
		return ret;
	}
	if (memcmp(&pol, mec_board->sys_policy, sizeof(pol))) {
		pr_err("system fan policy %*ph does not match %s\n",
		       (int)sizeof(pol), &pol, mec_board->name);
		return -EIO;
	}
	return 0;
}

/* Expected policy bytes as write preconditions (F7bsdProfile.cs:297-302). */
size_t sys_policy_expectations(struct ec_byte *out)
{
	const struct sys_policy *pol = mec_board->sys_policy;
	const u8 *rows = &pol->rows[0][0];
	size_t i, n = 0;

	for (i = 0; i < sizeof(pol->rows); i++)
		out[n++] = (struct ec_byte){ EC_SYS_POLICY_ROWS + i, rows[i] };
	for (i = 0; i < sizeof(pol->tail); i++)
		out[n++] = (struct ec_byte){ EC_SYS_POLICY_TAIL + i, pol->tail[i] };
	return n;
}

/*
 * Write allowlist (F7bsdProfile.cs:346-386): the system override (0 or 0xff),
 * the system target and the seven CPU bases (0..51), and the seven CPU slopes
 * (0 or their canonical value).
 */
static bool xram_writable(const struct mec_data *d, u16 addr, u8 val)
{
	int r;

	if (addr == EC_SYS_TARGET)
		return val <= CODE_MAX;
	if (addr == EC_SYS_OVERRIDE)
		return val == 0 || val == SYS_SENTINEL;
	for (r = 0; r < CPU_ROWS; r++) {
		if (addr == EC_CPU_ROWS + 3 * r)
			return val <= CODE_MAX;
		if (addr == EC_CPU_SLOPES + r)
			return val == 0 || (d->cpu_profile && val == d->cpu_profile->slopes[r]);
	}
	return false;
}

/*
 * Check the controller signature and every precondition, then write each
 * byte and read it back (PawnIoNative.cs:125-157). *issued, if given, is set
 * just before the first write, like the plugin's beforeWrites callback: from
 * there on a failure may leave the EC following this driver.
 */
int xram_write_verified(struct mec_data *d,
			const struct ec_byte *before, size_t n_before,
			const struct ec_byte *writes, size_t n_writes,
			bool *issued)
{
	struct device *dev = d->dev;
	size_t i;
	u8 val;
	int ret;

	for (i = 0; i < n_writes; i++)
		if (WARN_ONCE(!xram_writable(d, writes[i].addr, writes[i].val),
			      DRVNAME ": EC write 0x%04x=0x%02x is not allowed\n",
			      writes[i].addr, writes[i].val))
			return -EPERM;

	ret = mec_check_controller(dev);
	if (ret)
		return ret;

	for (i = 0; i < n_before; i++) {
		ret = xram_read(before[i].addr, &val);
		if (ret)
			return ret;
		if (val != before[i].val) {
			pr_err("EC precondition failed at 0x%04x: expected 0x%02x, read 0x%02x\n",
			       before[i].addr, before[i].val, val);
			return -EIO;
		}
	}

	if (issued)
		*issued = true;
	for (i = 0; i < n_writes; i++) {
		i2ec_write(writes[i].addr, writes[i].val);
		ret = xram_read(writes[i].addr, &val);
		if (ret)
			return ret;
		if (val != writes[i].val) {
			pr_err("EC verification failed at 0x%04x: wrote 0x%02x, read 0x%02x\n",
			       writes[i].addr, writes[i].val, val);
			return -EIO;
		}
	}
	return 0;
}
