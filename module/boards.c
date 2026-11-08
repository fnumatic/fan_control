// SPDX-License-Identifier: GPL-2.0
/*
 * Supported boards and what differs between them: host identity, EC firmware
 * version, the BIOS CPU tables, the system-fan policy and the CPU-fan minimum.
 */

#include "minisforum_ec.h"

#define STANDARD_BANDS \
	{ { 25, 0 }, { 45, 25 }, { 54, 45 }, { 66, 54 }, { 76, 66 }, { 88, 76 }, { 93, 88 } }
#define PERFORMANCE_BANDS \
	{ { 25, 0 }, { 45, 25 }, { 54, 45 }, { 66, 54 }, { 80, 66 }, { 88, 80 }, { 93, 88 } }

/*
 * Canonical BIOS-selected CPU tables (PlatformProfiles.cs:293-313, 360-378);
 * names from MinisforumUMSeriesPlugin.cs:239-241.
 */
static const struct cpu_profile f7bsi_cpu_profiles[] = {
	{ 0x00, "Default", STANDARD_BANDS,
	  { 0, 16, 18, 22, 25, 27, 29 }, { 0, 10, 44, 25, 20, 16, 75 } },
	{ 0xb1, "Balance", STANDARD_BANDS,
	  { 0, 16, 18, 22, 25, 27, 29 }, { 0, 10, 44, 25, 20, 16, 75 } },
	{ 0xb2, "Performance", PERFORMANCE_BANDS,
	  { 0, 18, 22, 29, 31, 34, 37 }, { 0, 20, 77, 16, 25, 37, 125 } },
};

/* PlatformProfiles.cs:360-368: the F7BSC BIOS ships the standard bands only. */
static const struct cpu_profile f7bsc_cpu_profiles[] = {
	{ 0x00, "Default", STANDARD_BANDS,
	  { 0, 16, 18, 21, 28, 34, 36 }, { 0, 10, 33, 58, 60, 16, 200 } },
	{ 0xb1, "Balance", STANDARD_BANDS,
	  { 0, 16, 18, 21, 28, 34, 36 }, { 0, 10, 33, 58, 60, 16, 200 } },
	{ 0xb2, "Performance", STANDARD_BANDS,
	  { 0, 18, 21, 28, 36, 40, 46 }, { 0, 15, 77, 66, 40, 50, 100 } },
};

/* PlatformProfiles.cs:323-329 */
static const struct sys_policy f7bsi_sys_policy = {
	.rows = { { 0, 25, 0 }, { 15, 83, 25 }, { 40, 100, 83 } },
};

/* PlatformProfiles.cs:315-321: F7BSC uses the standard (faster) system policy. */
static const struct sys_policy f7bsc_sys_policy = {
	.rows = { { 0, 25, 0 }, { 20, 83, 25 }, { 51, 100, 83 } },
};

/* Host identity: PlatformProfiles.cs:439-446 (F7BSI), 421-434 (F7BSC). */
struct board_host {
	int field;
	const char *value;
};

static const struct board_host f7bsi_host[] = {
	{ DMI_PRODUCT_NAME, "EliteMini Series" },
	{ DMI_PRODUCT_VERSION, "1.0" },
	{ DMI_PRODUCT_FAMILY, "EliteMini" },
	{ DMI_PRODUCT_SKU, "MGF7BSI" },
	{ DMI_BOARD_NAME, "F7BSI" },
	{ DMI_BOARD_VERSION, "1.0" },
};

static const struct board_host f7bsc_host[] = {
	{ DMI_PRODUCT_NAME, "Venus series" },
	{ DMI_BOARD_NAME, "F7BSC" },
	{ DMI_BOARD_VERSION, "Default string" },
};

static const struct board boards[] = {
	{
		.name = "F7BSI (UM760 Slim)",
		.host = f7bsi_host,
		.n_host = ARRAY_SIZE(f7bsi_host),
		.ec_major = 0,
		.ec_minor = 5,
		.cpu_profiles = f7bsi_cpu_profiles,
		.n_cpu_profiles = ARRAY_SIZE(f7bsi_cpu_profiles),
		.sys_policy = &f7bsi_sys_policy,
		.cpu_min_code = 6,	/* code 5 made the CPU fan hunt (measured) */
	},
	{
		.name = "F7BSC (UM790 Pro)",
		.host = f7bsc_host,
		.n_host = ARRAY_SIZE(f7bsc_host),
		.ec_major = 2,
		.ec_minor = 6,
		.cpu_profiles = f7bsc_cpu_profiles,
		.n_cpu_profiles = ARRAY_SIZE(f7bsc_cpu_profiles),
		.sys_policy = &f7bsc_sys_policy,
		/*
		 * Measured on F7BSC: at code 9 (900 RPM) the CPU fan swung
		 * between 570 and 1969 RPM; code 10 held 1000 RPM within
		 * 14 RPM.
		 */
		.cpu_min_code = 10,
	},
};

/* Selected once by mec_check_host() before any port access; immutable after. */
const struct board *mec_board;

/*
 * Before a board is selected, command-line parameters are already parsed. The
 * F7BSI minimum, the loosest, applies until then; fan_tick() raises a lower
 * goal to the selected board's minimum.
 */
#define FALLBACK_CPU_MIN_CODE	6

const struct cpu_profile *find_cpu_profile(u8 selector)
{
	size_t i;

	for (i = 0; i < mec_board->n_cpu_profiles; i++)
		if (mec_board->cpu_profiles[i].selector == selector)
			return &mec_board->cpu_profiles[i];
	return NULL;
}

/* Codes 1..min_code-1 are refused: they make that fan hunt. */
int board_min_code(enum fan_id id)
{
	if (id == FAN_SYS)
		return SYS_MIN_CODE;
	return mec_board ? mec_board->cpu_min_code : FALLBACK_CPU_MIN_CODE;
}

struct ec_version {
	bool found;
	u8 major;
	u8 minor;
};

/* SMBIOS type 0, bytes 0x16/0x17: EC firmware major/minor release. */
static void ec_version_decode(const struct dmi_header *dh, void *priv)
{
	struct ec_version *v = priv;
	const u8 *raw = (const u8 *)dh;

	if (dh->type != 0 || v->found)
		return;
	v->found = true;
	if (dh->length < 0x18) {
		v->major = 0xff;
		v->minor = 0xff;
		return;
	}
	v->major = raw[0x16];
	v->minor = raw[0x17];
}

/* Module autoload; mec_check_host() then checks every field exactly. */
static const struct dmi_system_id mec_dmi_table[] __initconst = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "EliteMini Series"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "F7BSI"),
			DMI_EXACT_MATCH(DMI_BOARD_VERSION, "1.0"),
		},
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Venus series"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "F7BSC"),
			DMI_EXACT_MATCH(DMI_BOARD_VERSION, "Default string"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, mec_dmi_table);

static bool __init board_host_matches(const struct board *b)
{
	size_t i;

	for (i = 0; i < b->n_host; i++) {
		const char *s = dmi_get_system_info(b->host[i].field);

		if (!s || strcmp(s, b->host[i].value))
			return false;
	}
	return true;
}

/* Exact host and EC version gate, before any port access (PlatformProfiles.cs:68-81). */
int __init mec_check_host(void)
{
	struct ec_version ver = { };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		if (board_host_matches(&boards[i])) {
			mec_board = &boards[i];
			break;
		}
	}
	if (!mec_board) {
		pr_err("unsupported host: DMI product \"%s\", board \"%s\" version \"%s\"\n",
		       dmi_get_system_info(DMI_PRODUCT_NAME) ?: "",
		       dmi_get_system_info(DMI_BOARD_NAME) ?: "",
		       dmi_get_system_info(DMI_BOARD_VERSION) ?: "");
		return -ENODEV;
	}

	if (dmi_walk(ec_version_decode, &ver) || !ver.found) {
		pr_err("SMBIOS BIOS information (type 0) not found\n");
		return -ENODEV;
	}
	if (ver.major != mec_board->ec_major || ver.minor != mec_board->ec_minor) {
		pr_err("%s: EC firmware %u.%u, expected %u.%u\n", mec_board->name,
		       ver.major, ver.minor, mec_board->ec_major, mec_board->ec_minor);
		return -ENODEV;
	}
	return 0;
}
