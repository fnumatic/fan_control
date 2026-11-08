/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared by the files of the minisforum_ec driver (main.c describes what the
 * driver does):
 *
 *   boards.c   supported boards and what differs between them
 *   ec.c       EC access: port primitives, reads, checks, verified writes
 *   handoff.c  taking a fan from the firmware and giving it back (fan_ops)
 *   fan.c      fan control: modes, curves, stepping, hunting, the monitor
 *   main.c     hwmon and debugfs interface, probe, sleep, module init
 *
 * Register map and limits first, then the shared types, then what each file
 * shares with the others.
 */
#ifndef MINISFORUM_EC_H
#define MINISFORUM_EC_H

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* pr_err() that also returns the error, where dev_err_probe() was. */
#define mec_fail(err, fmt, ...)	({ pr_err(fmt, ##__VA_ARGS__); (err); })

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/stringify.h>
#include <linux/workqueue.h>

#define DRVNAME			"minisforum_ec"

/* Super-I/O index/data ports: PawnIO LpcIO slot 0 (LpcIO.p:136-152). */
#define SIO_INDEX		0x2e
#define SIO_DATA		0x2f

/* Super-I/O configuration registers (PawnIoNative.cs:227-274). */
#define SIO_REG_CHIP_ID		0x20	/* 0x20..0x22: ID high, ID low, revision */
#define SIO_REG_D2_INDEX	0x2e	/* selects a register in the EC's D2 bank */
#define SIO_REG_D2_DATA		0x2f

/* D2 registers for indirect (I2EC) access to EC memory. */
#define D2_I2EC_ADDR_LOW	0x10
#define D2_I2EC_ADDR_HIGH	0x11
#define D2_I2EC_DATA		0x12

/* EC memory, I2EC addresses (F7bsdProfile.cs:28-88). */
#define EC_TEMP_SYS		0x0305
#define EC_TEMP_CPU		0x0309
#define EC_CPU_ROWS		0x0310	/* 8 x (base, upper, lower); row 7 is critical */
#define EC_CPU_SELECTOR		0x032f
#define EC_SYS_POLICY_ROWS	0x0330	/* 3 x (code, upper, lower) */
#define EC_SYS_TARGET		0x0885
#define EC_SYS_EFFECTIVE	0x0889
#define EC_CPU_TEMP_OVERRIDE	0x088a
#define EC_SYS_OVERRIDE		0x088b
#define EC_CPU_SLOPES		0x08b0	/* 8 slopes; slope 7 belongs to the critical row */
#define EC_SYS_POLICY_TAIL	0x08c0	/* 3 bytes, all zero */
#define EC_DUTY_CPU		0x1803	/* PWM duty, diagnostics only (see below) */
#define EC_DUTY_SYS		0x1804
#define EC_TACH_CPU		0x181e	/* counter low byte; high byte at +1 */
#define EC_TACH_SYS		0x1820

#define CPU_ROWS		7	/* normal rows; the critical row follows */
#define CODE_MAX		51	/* F7bsdProfile.cs:28 */
#define PWM_PER_CODE		5	/* hwmon pwmN 0..255 is CODE_MAX * 5 */
#define SYS_SENTINEL		0xff	/* F7bsdProfile.cs:29 */
#define STABLE_ATTEMPTS		4	/* PawnIoF7bsdBackend.cs:5 */
#define TACH_ATTEMPTS		3	/* PawnIoF7bsdBackend.cs:603 */
#define TACH_DIVIDEND		2156250	/* PawnIoF7bsdBackend.cs:655 */
#define CACHE_JIFFIES		HZ
#define SYS_RELEASE_STEPS	4	/* PawnIoF7bsdBackend.cs:6 */
#define SYS_HANDOFF_POLLS	16	/* PawnIoF7bsdBackend.cs:7 */
#define SYS_HANDOFF_POLL_MS	100	/* PawnIoF7bsdBackend.cs:8-9 */
#define SYS_POLICY_BYTES	12

/* Behaviour and safety limits of this driver; not from the plugin. */
#define WATCHDOG_SECONDS	30	/* a fixed value must be renewed within this */
#define SYS_TEMP_LIMIT		55	/* EC system temperature handing pwm2 to firmware */
#define CPU_TEMP_LIMIT		80	/* EC CPU temperature handing pwm1 to firmware */
#define LIMIT_HYST		5	/* ... until it falls this far below the limit */
#define CURVE_HYST		3	/* degrees below a curve point before leaving it */
#define SYS_MIN_CODE		8	/* codes 1..7 made the system fan hunt (both boards) */
#define SPINUP_CODE		20	/* a stopped fan restarts here and steps down */
#define MONITOR_JIFFIES		(2 * HZ)
#define STEP_JIFFIES		(4 * HZ)	/* at most one step per interval */
#define STEP_MAX_CODES		4	/* a large gap steps this many codes */
#define STEP_GAP_CODES		4	/* a gap of this many codes still steps one */
#define HUNT_WINDOW		6	/* monitor samples looked at for hunting */
#define HUNT_SPREAD		600	/* RPM swing over that window that counts as hunting */
#define HUNT_BACKOFF		(60 * HZ)
#define MAX_HUNTS		3
#define HUNT_FORGET		(30 * 60 * HZ)
#define SETTLE_JIFFIES		(10 * HZ)	/* after a write, before hunting is judged */
#define MAX_READ_FAILURES	3	/* consecutive telemetry failures before letting go */
#define MAX_ENGAGE_FAILURES	3	/* consecutive failed engages before mode 3 */
#define RELEASE_RETRY_SHIFT	7U	/* failed releases retry after 2 s, 4 s, ... 256 s */
#define RESUME_SETTLE_JIFFIES	(5 * HZ)	/* the EC reads zeros for a moment after waking */

struct cpu_table {
	u8 selector;
	u8 temp_override;
	u8 rows[CPU_ROWS + 1][3];	/* base, upper, lower */
	u8 slopes[CPU_ROWS + 1];
};

struct sys_policy {
	u8 rows[3][3];			/* code, upper, lower */
	u8 tail[3];
};

struct cpu_profile {
	u8 selector;
	const char *name;
	u8 bands[CPU_ROWS][2];		/* upper, lower */
	u8 bases[CPU_ROWS];
	u8 slopes[CPU_ROWS];
};

struct board_host;

struct board {
	const char *name;
	const struct board_host *host;
	size_t n_host;
	u8 ec_major;
	u8 ec_minor;
	const struct cpu_profile *cpu_profiles;
	size_t n_cpu_profiles;
	const struct sys_policy *sys_policy;
	u8 cpu_min_code;	/* codes 1..cpu_min_code-1 are refused */
};

enum sys_state {
	SYS_FIRMWARE,
	SYS_FIXED,
	SYS_RELEASING,
	SYS_UNSUPPORTED,
};

struct telemetry {
	int rpm[2];			/* CPU, system */
	u8 temp[2];			/* CPU, system; raw degrees C */
	u8 sys_effective;
	u8 sys_override;
	u8 sys_target;
	u8 duty[2];			/* CPU, system */
};

struct sys_regs {
	u8 effective;
	u8 override;
	u8 target;
};

struct ec_byte {
	u16 addr;
	u8 val;
};

enum fan_id { FAN_CPU, FAN_SYS, NR_FANS };	/* also the hwmon channel */

enum fan_mode {
	MODE_MANUAL = 1,	/* fixed code written to pwmN, renewed within WATCHDOG_SECONDS */
	MODE_CURVE = 2,		/* this driver's curve */
	MODE_FIRMWARE = 3,	/* the EC firmware's own policy */
};

#define CURVE_MAX_POINTS	8
#define CURVE_FIRMWARE		(-1)

/*
 * A curve is a list of temp:code points in ascending temperature. The point
 * in force is the last one whose temperature has been reached (the first
 * point below that); going down, a point is left once the temperature is more
 * than CURVE_HYST degrees below it. CURVE_FIRMWARE hands the fan to the
 * firmware.
 */
struct curve {
	int n;
	u8 temp[CURVE_MAX_POINTS];
	s8 code[CURVE_MAX_POINTS];
};

struct curve_param {
	const char *fan;
	enum fan_id id;
	struct curve curve;
};

struct mec_data;
struct fan;

struct fan_ops {
	/*
	 * Take the fan from the firmware; *start is the code then in force.
	 * *issued is set once a write may have reached the EC, even on failure.
	 */
	int (*engage)(struct mec_data *d, struct fan *f, int rpm, int *start, bool *issued);
	int (*set)(struct mec_data *d, struct fan *f, u8 code);
	/* Give the fan back to the firmware and verify that it has it. */
	int (*release)(struct mec_data *d, struct fan *f);
	/* Is the EC still in exactly the state this driver last wrote? */
	bool (*held)(struct mec_data *d, struct fan *f, const struct telemetry *t);
};

struct fan {
	enum fan_id id;
	const char *name;
	const struct fan_ops *ops;
	int min_code;
	u8 temp_limit;

	enum fan_mode mode;
	int manual_code;
	unsigned long deadline;		/* manual-mode watchdog */

	bool owned;			/* the EC may be following this driver */
	bool release_pending;		/* a release must complete before anything else */
	int cur;			/* last verified code, -1 if none */
	int level;			/* curve point in force */
	bool hot;			/* at or over temp_limit */
	unsigned long next_step;
	unsigned long settle_until;	/* hunting is not judged before this */
	bool holding;			/* stay with the firmware until hold_until */
	unsigned long hold_until;
	int hunts;
	unsigned long last_hunt;
	int rpm_win[HUNT_WINDOW];
	int win_n;
	int engage_failures;		/* consecutive failed engages */
	unsigned int release_failures;	/* consecutive failed releases */
	unsigned long next_release;	/* a pending release is retried from here */
};

struct mec_data {
	struct device *dev;
	struct mutex lock;		/* serialises every Super-I/O sequence and the state below */
	const struct cpu_profile *cpu_profile;
	bool valid;
	bool sys_state_warned;
	unsigned long updated;
	struct telemetry t;
	struct fan fans[NR_FANS];
	bool stopped;			/* unload or shutdown in progress */
	bool suspended;			/* system sleep */
	int read_failures;		/* consecutive telemetry failures in the monitor */
	struct delayed_work monitor;
};

/* boards.c */
extern const struct board *mec_board;
const struct cpu_profile *find_cpu_profile(u8 selector);
int board_min_code(enum fan_id id);
int __init mec_check_host(void);

/* ec.c */
extern const u16 ec_ident_addrs[6];
extern const char * const sys_state_names[];
u8 pnp_read(u8 reg);
int xram_read(u16 addr, u8 *val);
int read_stable(int (*read)(void *buf), void *out, void *tmp, size_t size);
int read_cpu_table(void *buf);
int read_sys_policy(void *buf);
bool plausible_temp(u8 val);
int read_telemetry(struct telemetry *t);
enum sys_state telemetry_sys_state(const struct telemetry *t);
int mec_check_controller(struct device *dev);
int read_sys_regs(struct sys_regs *r);
enum sys_state sys_regs_state(const struct sys_regs *r);
int check_sys_policy(struct device *dev);
size_t sys_policy_expectations(struct ec_byte *out);
int xram_write_verified(struct mec_data *d,
			const struct ec_byte *before, size_t n_before,
			const struct ec_byte *writes, size_t n_writes,
			bool *issued);

/* handoff.c */
extern const struct fan_ops sys_fan_ops;
extern const struct fan_ops cpu_fan_ops;
const char *check_cpu_static(const struct cpu_table *t, const struct cpu_profile *p);
bool cpu_is_canonical(const struct cpu_table *t, const struct cpu_profile *p);
bool cpu_is_fixed(const struct cpu_table *t, int code);
bool cpu_recoverable(const struct cpu_table *t, const struct cpu_profile *p);

/* fan.c */
extern struct curve_param curve_params[NR_FANS];
extern struct mutex curve_lock;
extern const char * const mode_names[];
int fan_release(struct mec_data *d, struct fan *f, const char *reason);
void mec_monitor(struct work_struct *work);
void mec_stop(struct mec_data *d, const char *reason);

#endif /* MINISFORUM_EC_H */
