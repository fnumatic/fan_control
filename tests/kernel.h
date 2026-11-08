/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stand-ins for the kernel API that module/fan.c and module/minisforum_ec.h
 * use, so that fan.c compiles as a normal program for tests/fan_test.c. The
 * Makefile points every <linux/...> include of minisforum_ec.h here. Time,
 * logging and the workqueue are provided by fan_test.c.
 */
#ifndef TESTS_KERNEL_H
#define TESTS_KERNEL_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define KBUILD_MODNAME	"minisforum_ec"	/* kbuild passes it on the command line */

typedef int8_t s8;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define __init
#define __initconst
#define __exit

#define HZ		1000
#define PAGE_SIZE	4096

/* The fake clock; fan_test.c moves it. */
extern unsigned long jiffies;
#define time_after(a, b)	((long)((b) - (a)) < 0)
#define time_before(a, b)	time_after(b, a)

/* Every log line goes to fan_test.c, which shows them for a failed test. */
void test_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define pr_crit(fmt, ...)	test_log(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)	test_log(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)	test_log(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)	test_log(pr_fmt(fmt), ##__VA_ARGS__)

#define min(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi)	min(max(v, lo), hi)
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, d)	(((x) + (d) / 2) / (d))	/* x >= 0 only, as fan.c uses it */
#define container_of(ptr, type, member)	((type *)((char *)(ptr) - offsetof(type, member)))

/* One thread in the tests: locks only have to compile. */
struct mutex {
	int unused;
};
#define DEFINE_MUTEX(name)		struct mutex name = { 0 }
#define guard(type)			(void)
#define scoped_guard(type, lock)	for (int _once = ((void)(lock), 1); _once; _once = 0)

struct device;
struct workqueue_struct;
struct work_struct {
	int unused;
};
struct delayed_work {
	struct work_struct work;
};
#define to_delayed_work(w)	container_of(w, struct delayed_work, work)
extern struct workqueue_struct *system_dfl_wq;
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
			unsigned long delay);
static inline bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return false;
}

static inline void msleep(unsigned int ms)
{
}

/* Module parameters: the tests call the kernel_param_ops directly. */
struct kernel_param {
	void *arg;
};
struct kernel_param_ops {
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
};
#define module_param_cb(name, ops, arg, perm)
#define MODULE_PARM_DESC(name, desc)

/* Kernel string helpers, with the behaviour fan.c relies on. */
static inline ssize_t strscpy(char *dst, const char *src, size_t size)
{
	size_t len = strnlen(src, size);

	if (len == size) {
		if (size) {
			memcpy(dst, src, size - 1);
			dst[size - 1] = '\0';
		}
		return -E2BIG;
	}
	memcpy(dst, src, len + 1);
	return len;
}

static inline char *strim(char *s)
{
	size_t n = strlen(s);

	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	while (isspace((unsigned char)*s))
		s++;
	return s;
}

/* Digits with an optional leading '+' and one trailing newline, as the kernel's. */
static inline int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	unsigned long v;
	char *end;

	if (*s == '+')
		s++;
	if (!isdigit((unsigned char)*s))
		return -EINVAL;
	errno = 0;
	v = strtoul(s, &end, base);
	if (*end == '\n')
		end++;
	if (*end || errno || v > UINT_MAX)
		return -EINVAL;
	*res = v;
	return 0;
}

static inline int scnprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static inline int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!size)
		return 0;
	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 0;
	return (size_t)n >= size ? (int)size - 1 : n;
}

#endif /* TESTS_KERNEL_H */
