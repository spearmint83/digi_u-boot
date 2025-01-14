// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000-2010
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2001 Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Andreas Heppel <aheppel@sysgo.de>
 */

#include <common.h>
#include <bootstage.h>
#include <command.h>
#include <env.h>
#include <env_internal.h>
#include <log.h>
#include <sort.h>
#include <asm/global_data.h>
#include <linux/stddef.h>
#include <search.h>
#include <errno.h>
#include <malloc.h>
#include <u-boot/crc.h>
#include <dm/ofnode.h>

#ifdef CONFIG_ENV_AES_CCMP1
/* CCMP1 AES encryption support */
#include <nand.h>
#include "../board/digi/ccmp1/ta_ccmp1.h"
#endif

DECLARE_GLOBAL_DATA_PTR;

/************************************************************************
 * Default settings to be used when no valid environment is found
 */
#include <env_default.h>

struct hsearch_data env_htab = {
	.change_ok = env_flags_validate,
};

/*
 * Read an environment variable as a boolean
 * Return -1 if variable does not exist (default to true)
 */
int env_get_yesno(const char *var)
{
	char *s = env_get(var);

	if (s == NULL)
		return -1;
	return (*s == '1' || *s == 'y' || *s == 'Y' || *s == 't' || *s == 'T') ?
		1 : 0;
}

/*
 * Look up the variable from the default environment
 */
char *env_get_default(const char *name)
{
	char *ret_val;
	unsigned long really_valid = gd->env_valid;
	unsigned long real_gd_flags = gd->flags;

	/* Pretend that the image is bad. */
	gd->flags &= ~GD_FLG_ENV_READY;
	gd->env_valid = ENV_INVALID;
	ret_val = env_get(name);
	gd->env_valid = really_valid;
	gd->flags = real_gd_flags;
	return ret_val;
}

__weak void platform_default_environment(void)
{
	return;
}

void env_set_default(const char *s, int flags)
{
	if (sizeof(default_environment) > ENV_SIZE) {
		puts("*** Error - default environment is too large\n\n");
		return;
	}

	if (s) {
		if ((flags & H_INTERACTIVE) == 0) {
			printf("*** Warning - %s, "
				"using default environment\n\n", s);
		} else {
			puts(s);
		}
	} else {
		debug("Using default environment\n");
	}

	flags |= H_DEFAULT;
	if (himport_r(&env_htab, (char *)default_environment,
			sizeof(default_environment), '\0', flags, 0,
			0, NULL) == 0)
		pr_err("## Error: Environment import failed: errno = %d\n",
		       errno);

	/* Platform-specific actions on default environment */
	platform_default_environment();

	gd->flags |= GD_FLG_ENV_READY;
	gd->flags |= GD_FLG_ENV_DEFAULT;
}


/* [re]set individual variables to their value in the default environment */
int env_set_default_vars(int nvars, char * const vars[], int flags)
{
	/*
	 * Special use-case: import from default environment
	 * (and use \0 as a separator)
	 */
	flags |= H_NOCLEAR | H_DEFAULT;
	return himport_r(&env_htab, (const char *)default_environment,
				sizeof(default_environment), '\0',
				flags, 0, nvars, vars);
}

__weak int env_aes_cbc_crypt(env_t *env, const int enc)
{
	return 0;
}

/*
 * Check if CRC is valid and (if yes) import the environment.
 * Note that "buf" may or may not be aligned.
 */
int env_import(const char *buf, int check, int flags)
{
	env_t *ep = (env_t *)buf;
	int ret;
	char *p = NULL;
	char *env_data = ep->data;

	if (check) {
		uint32_t crc;

		memcpy(&crc, &ep->crc, sizeof(crc));

		if (crc32(0, ep->data, ENV_SIZE) != crc) {
			env_set_default("bad CRC", 0);
			return -ENOMSG; /* needed for env_load() */
		}
	}

	/* Decrypt the env if desired. */
	ret = env_aes_cbc_crypt(ep, 0);
	if (ret) {
#if defined(CONFIG_ENV_AES_CAAM_KEY) || defined(CONFIG_ENV_AES_CCMP1)
		if (himport_r(&env_htab, (char *)ep->data, ENV_SIZE,
				'\0', 0, 0, 0, NULL)) {
			printf("Environment is unencrypted!\n");
			printf("Resetting to defaults (read-only variables like MAC addresses will be kept).\n");
			gd->flags |= GD_FLG_ENV_READY;
			run_command("env default -a", 0);
			return 0;
		}
#endif
		pr_err("Failed to decrypt env!\n");
		env_set_default("!import failed", 0);
		return ret;
	} else {
		/* Verify that we can read some environment variable */
		while (*env_data) {
			p = strstr((char *)env_data, "baudrate");
			if (p != NULL)
				break;
			env_data += strlen(env_data) + 1;
		}
		/* if we haven't found anything exit with an error */
		if (p == NULL) {
			errno = EIO;
			goto err;
		}

		if (himport_r(&env_htab, (char *)ep->data, ENV_SIZE, '\0', 0, 0,
			0, NULL)) {
			gd->flags |= GD_FLG_ENV_READY;
			return 0;
		}
	}

err:
	pr_err("Cannot import environment: errno = %d\n", errno);

	env_set_default("import failed", 0);

	return -EIO;
}

#ifdef CONFIG_SYS_REDUNDAND_ENVIRONMENT
static unsigned char env_flags;

int env_check_redund(const char *buf1, int buf1_read_fail,
		     const char *buf2, int buf2_read_fail)
{
	int crc1_ok = 0, crc2_ok = 0;
	env_t *tmp_env1, *tmp_env2;

	tmp_env1 = (env_t *)buf1;
	tmp_env2 = (env_t *)buf2;

	if (buf1_read_fail && buf2_read_fail) {
		puts("*** Error - No Valid Environment Area found\n");
		return -EIO;
	} else if (buf1_read_fail || buf2_read_fail) {
		puts("*** Warning - some problems detected ");
		puts("reading environment; recovered successfully\n");
	}

	if (!buf1_read_fail)
		crc1_ok = crc32(0, tmp_env1->data, ENV_SIZE) ==
				tmp_env1->crc;
	if (!buf2_read_fail)
		crc2_ok = crc32(0, tmp_env2->data, ENV_SIZE) ==
				tmp_env2->crc;

	if (!crc1_ok && !crc2_ok) {
		return -ENOMSG; /* needed for env_load() */
	} else if (crc1_ok && !crc2_ok) {
		gd->env_valid = ENV_VALID;
	} else if (!crc1_ok && crc2_ok) {
		gd->env_valid = ENV_REDUND;
	} else {
		/* both ok - check serial */
		if (tmp_env1->flags == 255 && tmp_env2->flags == 0)
			gd->env_valid = ENV_REDUND;
		else if (tmp_env2->flags == 255 && tmp_env1->flags == 0)
			gd->env_valid = ENV_VALID;
		else if (tmp_env1->flags > tmp_env2->flags)
			gd->env_valid = ENV_VALID;
		else if (tmp_env2->flags > tmp_env1->flags)
			gd->env_valid = ENV_REDUND;
		else /* flags are equal - almost impossible */
			gd->env_valid = ENV_VALID;
	}

	return 0;
}

int env_import_redund(const char *buf1, int buf1_read_fail,
		      const char *buf2, int buf2_read_fail,
		      int flags)
{
	env_t *ep;
	int ret;

	ret = env_check_redund(buf1, buf1_read_fail, buf2, buf2_read_fail);

	if (ret == -EIO) {
		env_set_default("bad env area", 0);
		return -EIO;
	} else if (ret == -ENOMSG) {
#if defined(OLD_ENV_OFFSET_LOCATIONS)
		static int old_env_tries = OLD_ENV_OFFSET_LOCATIONS;

		/*
		 * Return error but don't reset the environment yet.
		 * We'll try to restore it from the old location.
		 */
		if (old_env_tries) {
			old_env_tries--;
			return -ENOMSG;
		}
#endif
		env_set_default("bad CRC", 0);
		return -ENOMSG;
	}

	if (gd->env_valid == ENV_VALID)
		ep = (env_t *)buf1;
	else
		ep = (env_t *)buf2;

	env_flags = ep->flags;

	return env_import((char *)ep, 0, flags);
}
#endif /* CONFIG_SYS_REDUNDAND_ENVIRONMENT */

/* Export the environment and generate CRC for it. */
int env_export(env_t *env_out)
{
	char *res;
	ssize_t	len;
#if defined(CONFIG_ENV_AES_CAAM_KEY) || defined(CONFIG_ENV_AES_CCMP1)
	int ret;
#endif

	res = (char *)env_out->data;
	len = hexport_r(&env_htab, '\0', 0, &res, ENV_SIZE, 0, NULL);
	if (len < 0) {
		pr_err("Cannot export environment: errno = %d\n", errno);
		return 1;
	}

	/* Encrypt the env if desired. */
#if defined(CONFIG_ENV_AES_CAAM_KEY) || defined(CONFIG_ENV_AES_CCMP1)
	ret = env_aes_cbc_crypt(env_out, 1);
	if (ret)
		return ret;
#endif

	env_out->crc = crc32(0, env_out->data, ENV_SIZE);

#ifdef CONFIG_SYS_REDUNDAND_ENVIRONMENT
	env_out->flags = ++env_flags; /* increase the serial */
#endif

	return 0;
}

void env_relocate(void)
{
#if defined(CONFIG_NEEDS_MANUAL_RELOC)
	env_reloc();
	env_fix_drivers();
	env_htab.change_ok += gd->reloc_off;
#endif
	if (gd->env_valid == ENV_INVALID) {
#if defined(CONFIG_ENV_IS_NOWHERE) || defined(CONFIG_SPL_BUILD)
		/* Environment not changable */
		env_set_default(NULL, 0);
#else
		bootstage_error(BOOTSTAGE_ID_NET_CHECKSUM);
		env_set_default("bad CRC", 0);
#endif
	} else {
		env_load();
	}
}

#ifdef CONFIG_AUTO_COMPLETE
int env_complete(char *var, int maxv, char *cmdv[], int bufsz, char *buf,
		 bool dollar_comp)
{
	struct env_entry *match;
	int found, idx;

	if (dollar_comp) {
		/*
		 * When doing $ completion, the first character should
		 * obviously be a '$'.
		 */
		if (var[0] != '$')
			return 0;

		var++;

		/*
		 * The second one, if present, should be a '{', as some
		 * configuration of the u-boot shell expand ${var} but not
		 * $var.
		 */
		if (var[0] == '{')
			var++;
		else if (var[0] != '\0')
			return 0;
	}

	idx = 0;
	found = 0;
	cmdv[0] = NULL;


	while ((idx = hmatch_r(var, idx, &match, &env_htab))) {
		int vallen = strlen(match->key) + 1;

		if (found >= maxv - 2 ||
		    bufsz < vallen + (dollar_comp ? 3 : 0))
			break;

		cmdv[found++] = buf;

		/* Add the '${' prefix to each var when doing $ completion. */
		if (dollar_comp) {
			strcpy(buf, "${");
			buf += 2;
			bufsz -= 3;
		}

		memcpy(buf, match->key, vallen);
		buf += vallen;
		bufsz -= vallen;

		if (dollar_comp) {
			/*
			 * This one is a bit odd: vallen already contains the
			 * '\0' character but we need to add the '}' suffix,
			 * hence the buf - 1 here. strcpy() will add the '\0'
			 * character just after '}'. buf is then incremented
			 * to account for the extra '}' we just added.
			 */
			strcpy(buf - 1, "}");
			buf++;
		}
	}

	qsort(cmdv, found, sizeof(cmdv[0]), strcmp_compar);

	if (idx)
		cmdv[found++] = dollar_comp ? "${...}" : "...";

	cmdv[found] = NULL;
	return found;
}
#endif

#ifdef CONFIG_ENV_IMPORT_FDT
void env_import_fdt(void)
{
	const char *path;
	struct ofprop prop;
	ofnode node;
	int res;

	path = env_get("env_fdt_path");
	if (!path || !path[0])
		return;

	node = ofnode_path(path);
	if (!ofnode_valid(node)) {
		printf("Warning: device tree node '%s' not found\n", path);
		return;
	}

	for (res = ofnode_get_first_property(node, &prop);
	     !res;
	     res = ofnode_get_next_property(&prop)) {
		const char *name, *val;

		val = ofnode_get_property_by_prop(&prop, &name, NULL);
		env_set(name, val);
	}
}
#endif
