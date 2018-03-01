#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <fts.h>
#include <selinux/selinux.h>
#include <selinux/context.h>
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/avc.h>
#include <openssl/sha.h>
#include <private/android_filesystem_config.h>
#include <log/log.h>
#include "policy.h"
#include "callbacks.h"
#include "selinux_internal.h"
#include "label_internal.h"
#include <fnmatch.h>
#include <limits.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <libgen.h>
#include <packagelistparser/packagelistparser.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * XXX Where should this configuration file be located?
 * Needs to be accessible by zygote and installd when
 * setting credentials for app processes and setting permissions
 * on app data directories.
 */
static char const * const seapp_contexts_split[] = {
	"/system/etc/selinux/plat_seapp_contexts",
	"/vendor/etc/selinux/nonplat_seapp_contexts"
};

static char const * const seapp_contexts_rootfs[] = {
	"/plat_seapp_contexts",
	"/nonplat_seapp_contexts"
};

static const struct selinux_opt seopts_file_split[] = {
    { SELABEL_OPT_PATH, "/system/etc/selinux/plat_file_contexts" },
    { SELABEL_OPT_PATH, "/vendor/etc/selinux/nonplat_file_contexts" }
};

static const struct selinux_opt seopts_file_rootfs[] = {
    { SELABEL_OPT_PATH, "/plat_file_contexts" },
    { SELABEL_OPT_PATH, "/nonplat_file_contexts" }
};

static const char *const sepolicy_file = "/sepolicy";

static const struct selinux_opt seopts_prop_split[] = {
    { SELABEL_OPT_PATH, "/system/etc/selinux/plat_property_contexts" },
    { SELABEL_OPT_PATH, "/vendor/etc/selinux/nonplat_property_contexts"}
};

static const struct selinux_opt seopts_prop_rootfs[] = {
    { SELABEL_OPT_PATH, "/plat_property_contexts" },
    { SELABEL_OPT_PATH, "/nonplat_property_contexts"}
};

static const struct selinux_opt seopts_service_split[] = {
    { SELABEL_OPT_PATH, "/system/etc/selinux/plat_service_contexts" },
    { SELABEL_OPT_PATH, "/vendor/etc/selinux/nonplat_service_contexts" }
};

static const struct selinux_opt seopts_service_rootfs[] = {
    { SELABEL_OPT_PATH, "/plat_service_contexts" },
    { SELABEL_OPT_PATH, "/nonplat_service_contexts" }
};

static const struct selinux_opt seopts_hwservice_split[] = {
    { SELABEL_OPT_PATH, "/system/etc/selinux/plat_hwservice_contexts" },
    { SELABEL_OPT_PATH, "/vendor/etc/selinux/nonplat_hwservice_contexts" }
};

static const struct selinux_opt seopts_hwservice_rootfs[] = {
    { SELABEL_OPT_PATH, "/plat_hwservice_contexts" },
    { SELABEL_OPT_PATH, "/nonplat_hwservice_contexts" }
};


static const struct selinux_opt seopts_vndservice =
    { SELABEL_OPT_PATH, "/vendor/etc/selinux/vndservice_contexts" };

static const struct selinux_opt seopts_vndservice_rootfs =
    { SELABEL_OPT_PATH, "/vndservice_contexts" };


enum levelFrom {
	LEVELFROM_NONE,
	LEVELFROM_APP,
	LEVELFROM_USER,
	LEVELFROM_ALL
};

#if DEBUG
static char const * const levelFromName[] = {
	"none",
	"app",
	"user",
	"all"
};
#endif

struct prefix_str {
	size_t len;
	char *str;
	char is_prefix;
};

static void free_prefix_str(struct prefix_str *p)
{
	if (!p)
		return;
	free(p->str);
}

struct seapp_context {
	/* input selectors */
	bool isSystemServer;
	bool isEphemeralAppSet;
	bool isEphemeralApp;
	bool isV2AppSet;
	bool isV2App;
	bool isOwnerSet;
	bool isOwner;
	struct prefix_str user;
	char *seinfo;
	struct prefix_str name;
	struct prefix_str path;
	bool isPrivAppSet;
	bool isPrivApp;
	int32_t minTargetSdkVersion;
	/* outputs */
	char *domain;
	char *type;
	char *level;
	enum levelFrom levelFrom;
};

static void free_seapp_context(struct seapp_context *s)
{
	if (!s)
		return;

	free_prefix_str(&s->user);
	free(s->seinfo);
	free_prefix_str(&s->name);
	free_prefix_str(&s->path);
	free(s->domain);
	free(s->type);
	free(s->level);
}

static bool seapp_contexts_dup = false;

static int seapp_context_cmp(const void *A, const void *B)
{
	const struct seapp_context *const *sp1 = (const struct seapp_context *const *) A;
	const struct seapp_context *const *sp2 = (const struct seapp_context *const *) B;
	const struct seapp_context *s1 = *sp1, *s2 = *sp2;
	bool dup;

	/* Give precedence to isSystemServer=true. */
	if (s1->isSystemServer != s2->isSystemServer)
		return (s1->isSystemServer ? -1 : 1);

	/* Give precedence to a specified isEphemeral= over an
	 * unspecified isEphemeral=. */
	if (s1->isEphemeralAppSet != s2->isEphemeralAppSet)
		return (s1->isEphemeralAppSet ? -1 : 1);

	/* Give precedence to a specified isV2= over an
	 * unspecified isV2=. */
	if (s1->isV2AppSet != s2->isV2AppSet)
		return (s1->isV2AppSet ? -1 : 1);


	/* Give precedence to a specified isOwner= over an unspecified isOwner=. */
	if (s1->isOwnerSet != s2->isOwnerSet)
		return (s1->isOwnerSet ? -1 : 1);

	/* Give precedence to a specified user= over an unspecified user=. */
	if (s1->user.str && !s2->user.str)
		return -1;
	if (!s1->user.str && s2->user.str)
		return 1;

	if (s1->user.str) {
		/* Give precedence to a fixed user= string over a prefix. */
		if (s1->user.is_prefix != s2->user.is_prefix)
			return (s2->user.is_prefix ? -1 : 1);

		/* Give precedence to a longer prefix over a shorter prefix. */
		if (s1->user.is_prefix && s1->user.len != s2->user.len)
			return (s1->user.len > s2->user.len) ? -1 : 1;
	}

	/* Give precedence to a specified seinfo= over an unspecified seinfo=. */
	if (s1->seinfo && !s2->seinfo)
		return -1;
	if (!s1->seinfo && s2->seinfo)
		return 1;

	/* Give precedence to a specified name= over an unspecified name=. */
	if (s1->name.str && !s2->name.str)
		return -1;
	if (!s1->name.str && s2->name.str)
		return 1;

	if (s1->name.str) {
		/* Give precedence to a fixed name= string over a prefix. */
		if (s1->name.is_prefix != s2->name.is_prefix)
			return (s2->name.is_prefix ? -1 : 1);

		/* Give precedence to a longer prefix over a shorter prefix. */
		if (s1->name.is_prefix && s1->name.len != s2->name.len)
			return (s1->name.len > s2->name.len) ? -1 : 1;
	}

	/* Give precedence to a specified path= over an unspecified path=. */
	if (s1->path.str && !s2->path.str)
		return -1;
	if (!s1->path.str && s2->path.str)
		return 1;

	if (s1->path.str) {
		/* Give precedence to a fixed path= string over a prefix. */
		if (s1->path.is_prefix != s2->path.is_prefix)
			return (s2->path.is_prefix ? -1 : 1);

		/* Give precedence to a longer prefix over a shorter prefix. */
		if (s1->path.is_prefix && s1->path.len != s2->path.len)
			return (s1->path.len > s2->path.len) ? -1 : 1;
	}

	/* Give precedence to a specified isPrivApp= over an unspecified isPrivApp=. */
	if (s1->isPrivAppSet != s2->isPrivAppSet)
		return (s1->isPrivAppSet ? -1 : 1);

	/* Give precedence to a higher minTargetSdkVersion= over a lower minTargetSdkVersion=.
	 * If unspecified, minTargetSdkVersion has a default value of 0.
	 */
	if (s1->minTargetSdkVersion > s2->minTargetSdkVersion)
		return -1;
	else if (s1->minTargetSdkVersion < s2->minTargetSdkVersion)
		return 1;

	/*
	 * Check for a duplicated entry on the input selectors.
	 * We already compared isSystemServer, isOwnerSet, and isOwner above.
	 * We also have already checked that both entries specify the same
	 * string fields, so if s1 has a non-NULL string, then so does s2.
	 */
	dup = (!s1->user.str || !strcmp(s1->user.str, s2->user.str)) &&
		(!s1->seinfo || !strcmp(s1->seinfo, s2->seinfo)) &&
		(!s1->name.str || !strcmp(s1->name.str, s2->name.str)) &&
		(!s1->path.str || !strcmp(s1->path.str, s2->path.str)) &&
		(s1->isPrivAppSet && s1->isPrivApp == s2->isPrivApp) &&
		(s1->isOwnerSet && s1->isOwner == s2->isOwner) &&
		(s1->isSystemServer && s1->isSystemServer == s2->isSystemServer) &&
		(s1->isV2AppSet && s1->isV2App == s2->isV2App) &&
		(s1->isEphemeralAppSet && s1->isEphemeralApp == s2->isEphemeralApp);

	if (dup) {
		seapp_contexts_dup = true;
		selinux_log(SELINUX_ERROR, "seapp_contexts:  Duplicated entry\n");
		if (s1->user.str)
			selinux_log(SELINUX_ERROR, " user=%s\n", s1->user.str);
		if (s1->seinfo)
			selinux_log(SELINUX_ERROR, " seinfo=%s\n", s1->seinfo);
		if (s1->name.str)
			selinux_log(SELINUX_ERROR, " name=%s\n", s1->name.str);
		if (s1->path.str)
			selinux_log(SELINUX_ERROR, " path=%s\n", s1->path.str);
	}

	/* Anything else has equal precedence. */
	return 0;
}

static struct seapp_context **seapp_contexts = NULL;
static int nspec = 0;

static void free_seapp_contexts(void)
{
	int n;

	if (!seapp_contexts)
		return;

	for (n = 0; n < nspec; n++)
		free_seapp_context(seapp_contexts[n]);

	free(seapp_contexts);
	seapp_contexts = NULL;
	nspec = 0;
}

static int32_t get_minTargetSdkVersion(const char *value)
{
	char *endptr;
	long minTargetSdkVersion;
	minTargetSdkVersion = strtol(value, &endptr, 10);
	if (('\0' != *endptr) || (minTargetSdkVersion < 0) || (minTargetSdkVersion > INT32_MAX)) {
		return -1; /* error parsing minTargetSdkVersion */
	} else {
		return (int32_t) minTargetSdkVersion;
	}
}

int selinux_android_seapp_context_reload(void)
{
	FILE *fp = NULL;
	char line_buf[BUFSIZ];
	char *token;
	unsigned lineno;
	struct seapp_context *cur;
	char *p, *name = NULL, *value = NULL, *saveptr;
	size_t i, len, files_len;
	int n, ret;
	const char *const *seapp_contexts_files;

	// Prefer files from /system & /vendor, fall back to files from /
	if (access(seapp_contexts_split[0], R_OK) != -1) {
		seapp_contexts_files = seapp_contexts_split;
		files_len = sizeof(seapp_contexts_split)/sizeof(seapp_contexts_split[0]);
	} else {
		seapp_contexts_files = seapp_contexts_rootfs;
		files_len = sizeof(seapp_contexts_rootfs)/sizeof(seapp_contexts_rootfs[0]);
	}

	free_seapp_contexts();

	nspec = 0;
	for (i = 0; i < files_len; i++) {
		fp = fopen(seapp_contexts_files[i], "re");
		if (!fp) {
			selinux_log(SELINUX_ERROR, "%s:  could not open seapp_contexts file: %s",
				    __FUNCTION__, seapp_contexts_files[i]);
			return -1;
		}
		while (fgets(line_buf, sizeof line_buf - 1, fp)) {
			p = line_buf;
			while (isspace(*p))
				p++;
			if (*p == '#' || *p == 0)
				continue;
			nspec++;
		}
		fclose(fp);
	}

	seapp_contexts = (struct seapp_context **) calloc(nspec, sizeof(struct seapp_context *));
	if (!seapp_contexts)
		goto oom;

	nspec = 0;
	for (i = 0; i < files_len; i++) {
		lineno = 1;
		fp = fopen(seapp_contexts_files[i], "re");
		if (!fp) {
			selinux_log(SELINUX_ERROR, "%s:  could not open seapp_contexts file: %s",
				    __FUNCTION__, seapp_contexts_files[i]);
			free_seapp_contexts();
			return -1;
		}
		while (fgets(line_buf, sizeof line_buf - 1, fp)) {
			len = strlen(line_buf);
			if (line_buf[len - 1] == '\n')
				line_buf[len - 1] = 0;
			p = line_buf;
			while (isspace(*p))
				p++;
			if (*p == '#' || *p == 0)
				continue;

			cur = (struct seapp_context *) calloc(1, sizeof(struct seapp_context));
			if (!cur)
				goto oom;

			token = strtok_r(p, " \t", &saveptr);
			if (!token) {
				free_seapp_context(cur);
				goto err;
			}

			while (1) {
				name = token;
				value = strchr(name, '=');
				if (!value) {
					free_seapp_context(cur);
					goto err;
				}
				*value++ = 0;

				if (!strcasecmp(name, "isSystemServer")) {
					if (!strcasecmp(value, "true"))
						cur->isSystemServer = true;
					else if (!strcasecmp(value, "false"))
						cur->isSystemServer = false;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "isEphemeralApp")) {
					cur->isEphemeralAppSet = true;
					if (!strcasecmp(value, "true"))
						cur->isEphemeralApp = true;
					else if (!strcasecmp(value, "false"))
						cur->isEphemeralApp = false;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "isV2App")) {
					cur->isV2AppSet = true;
					if (!strcasecmp(value, "true"))
						cur->isV2App = true;
					else if (!strcasecmp(value, "false"))
						cur->isV2App = false;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "isOwner")) {
					cur->isOwnerSet = true;
					if (!strcasecmp(value, "true"))
						cur->isOwner = true;
					else if (!strcasecmp(value, "false"))
						cur->isOwner = false;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "user")) {
					if (cur->user.str) {
						free_seapp_context(cur);
						goto err;
					}
					cur->user.str = strdup(value);
					if (!cur->user.str) {
						free_seapp_context(cur);
						goto oom;
					}
					cur->user.len = strlen(cur->user.str);
					if (cur->user.str[cur->user.len-1] == '*')
						cur->user.is_prefix = 1;
				} else if (!strcasecmp(name, "seinfo")) {
					if (cur->seinfo) {
						free_seapp_context(cur);
						goto err;
					}
					cur->seinfo = strdup(value);
					if (!cur->seinfo) {
						free_seapp_context(cur);
						goto oom;
					}
					if (strstr(value, ":")) {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "name")) {
					if (cur->name.str) {
						free_seapp_context(cur);
						goto err;
					}
					cur->name.str = strdup(value);
					if (!cur->name.str) {
						free_seapp_context(cur);
						goto oom;
					}
					cur->name.len = strlen(cur->name.str);
					if (cur->name.str[cur->name.len-1] == '*')
						cur->name.is_prefix = 1;
				} else if (!strcasecmp(name, "domain")) {
					if (cur->domain) {
						free_seapp_context(cur);
						goto err;
					}
					cur->domain = strdup(value);
					if (!cur->domain) {
						free_seapp_context(cur);
						goto oom;
					}
				} else if (!strcasecmp(name, "type")) {
					if (cur->type) {
						free_seapp_context(cur);
						goto err;
					}
					cur->type = strdup(value);
					if (!cur->type) {
						free_seapp_context(cur);
						goto oom;
					}
				} else if (!strcasecmp(name, "levelFromUid")) {
					if (cur->levelFrom) {
						free_seapp_context(cur);
						goto err;
					}
					if (!strcasecmp(value, "true"))
						cur->levelFrom = LEVELFROM_APP;
					else if (!strcasecmp(value, "false"))
						cur->levelFrom = LEVELFROM_NONE;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "levelFrom")) {
					if (cur->levelFrom) {
						free_seapp_context(cur);
						goto err;
					}
					if (!strcasecmp(value, "none"))
						cur->levelFrom = LEVELFROM_NONE;
					else if (!strcasecmp(value, "app"))
						cur->levelFrom = LEVELFROM_APP;
					else if (!strcasecmp(value, "user"))
						cur->levelFrom = LEVELFROM_USER;
					else if (!strcasecmp(value, "all"))
						cur->levelFrom = LEVELFROM_ALL;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "level")) {
					if (cur->level) {
						free_seapp_context(cur);
						goto err;
					}
					cur->level = strdup(value);
					if (!cur->level) {
						free_seapp_context(cur);
						goto oom;
					}
				} else if (!strcasecmp(name, "path")) {
					if (cur->path.str) {
						free_seapp_context(cur);
						goto err;
					}
					cur->path.str = strdup(value);
					if (!cur->path.str) {
						free_seapp_context(cur);
					goto oom;
					}
					cur->path.len = strlen(cur->path.str);
					if (cur->path.str[cur->path.len-1] == '*')
						cur->path.is_prefix = 1;
				} else if (!strcasecmp(name, "isPrivApp")) {
					cur->isPrivAppSet = true;
					if (!strcasecmp(value, "true"))
						cur->isPrivApp = true;
					else if (!strcasecmp(value, "false"))
						cur->isPrivApp = false;
					else {
						free_seapp_context(cur);
						goto err;
					}
				} else if (!strcasecmp(name, "minTargetSdkVersion")) {
					cur->minTargetSdkVersion = get_minTargetSdkVersion(value);
					if (cur->minTargetSdkVersion < 0) {
						free_seapp_context(cur);
						goto err;
					}
				} else {
					free_seapp_context(cur);
					goto err;
				}

				token = strtok_r(NULL, " \t", &saveptr);
				if (!token)
					break;
			}

			if (cur->name.str &&
			    (!cur->seinfo || !strcmp(cur->seinfo, "default"))) {
				selinux_log(SELINUX_ERROR, "%s:  No specific seinfo value specified with name=\"%s\", on line %u:  insecure configuration!\n",
					    seapp_contexts_files[i], cur->name.str, lineno);
				free_seapp_context(cur);
				goto err;
			}

			seapp_contexts[nspec] = cur;
			nspec++;
			lineno++;
		}
		fclose(fp);
		fp = NULL;
	}

	qsort(seapp_contexts, nspec, sizeof(struct seapp_context *),
	      seapp_context_cmp);

	if (seapp_contexts_dup)
		goto err_no_log;

#if DEBUG
	{
		int i;
		for (i = 0; i < nspec; i++) {
			cur = seapp_contexts[i];
			selinux_log(SELINUX_INFO, "%s:  isSystemServer=%s  isEphemeralApp=%s isV2App=%s isOwner=%s user=%s seinfo=%s "
					"name=%s path=%s isPrivApp=%s minTargetSdkVersion=%d -> domain=%s type=%s level=%s levelFrom=%s",
				__FUNCTION__,
				cur->isSystemServer ? "true" : "false",
				cur->isEphemeralAppSet ? (cur->isEphemeralApp ? "true" : "false") : "null",
				cur->isV2AppSet ? (cur->isV2App ? "true" : "false") : "null",
				cur->isOwnerSet ? (cur->isOwner ? "true" : "false") : "null",
				cur->user.str,
				cur->seinfo, cur->name.str, cur->path.str,
				cur->isPrivAppSet ? (cur->isPrivApp ? "true" : "false") : "null",
				cur->minTargetSdkVersion,
				cur->domain, cur->type, cur->level,
				levelFromName[cur->levelFrom]);
		}
	}
#endif

	ret = 0;

out:
	if (fp) {
		fclose(fp);
	}
	return ret;

err:
	selinux_log(SELINUX_ERROR, "%s:  Invalid entry on line %u\n",
		    seapp_contexts_files[i], lineno);
err_no_log:
	free_seapp_contexts();
	ret = -1;
	goto out;
oom:
	selinux_log(SELINUX_ERROR,
		    "%s:  Out of memory\n", __FUNCTION__);
	free_seapp_contexts();
	ret = -1;
	goto out;
}


static void seapp_context_init(void)
{
        selinux_android_seapp_context_reload();
}

static pthread_once_t once = PTHREAD_ONCE_INIT;

/*
 * Max id that can be mapped to category set uniquely
 * using the current scheme.
 */
#define CAT_MAPPING_MAX_ID (0x1<<16)

enum seapp_kind {
	SEAPP_TYPE,
	SEAPP_DOMAIN
};

#define PRIVILEGED_APP_STR ":privapp"
#define EPHEMERAL_APP_STR ":ephemeralapp"
#define V2_APP_STR ":v2"
#define TARGETSDKVERSION_STR ":targetSdkVersion="
static int32_t get_app_targetSdkVersion(const char *seinfo)
{
	char *substr = strstr(seinfo, TARGETSDKVERSION_STR);
	long targetSdkVersion;
	char *endptr;
	if (substr != NULL) {
		substr = substr + strlen(TARGETSDKVERSION_STR);
		if (substr != NULL) {
			targetSdkVersion = strtol(substr, &endptr, 10);
			if (('\0' != *endptr && ':' != *endptr)
					|| (targetSdkVersion < 0) || (targetSdkVersion > INT32_MAX)) {
				return -1; /* malformed targetSdkVersion value in seinfo */
			} else {
				return (int32_t) targetSdkVersion;
			}
		}
	}
	return 0; /* default to 0 when targetSdkVersion= is not present in seinfo */
}

static int seinfo_parse(char *dest, const char *src, size_t size)
{
	size_t len;
	char *p;

	if ((p = strchr(src, ':')) != NULL)
		len = p - src;
	else
		len = strlen(src);

	if (len > size - 1)
		return -1;

	strncpy(dest, src, len);
	dest[len] = '\0';

	return 0;
}

static int seapp_context_lookup(enum seapp_kind kind,
				uid_t uid,
				bool isSystemServer,
				const char *seinfo,
				const char *pkgname,
				const char *path,
				context_t ctx)
{
	struct passwd *pwd;
	bool isOwner;
	const char *username = NULL;
	struct seapp_context *cur = NULL;
	int i;
	size_t n;
	uid_t userid;
	uid_t appid;
	bool isPrivApp = false;
	bool isEphemeralApp = false;
	int32_t targetSdkVersion = 0;
	bool isV2App = false;
	char parsedseinfo[BUFSIZ];

	__selinux_once(once, seapp_context_init);

	if (seinfo) {
		if (seinfo_parse(parsedseinfo, seinfo, BUFSIZ))
			goto err;
		isPrivApp = strstr(seinfo, PRIVILEGED_APP_STR) ? true : false;
		isEphemeralApp = strstr(seinfo, EPHEMERAL_APP_STR) ? true : false;
		isV2App = strstr(seinfo, V2_APP_STR) ? true : false;
		targetSdkVersion = get_app_targetSdkVersion(seinfo);
		if (targetSdkVersion < 0) {
			selinux_log(SELINUX_ERROR,
					"%s:  Invalid targetSdkVersion passed for app with uid %d, seinfo %s, name %s\n",
					__FUNCTION__, uid, seinfo, pkgname);
			goto err;
		}
		seinfo = parsedseinfo;
	}

	userid = uid / AID_USER;
	isOwner = (userid == 0);
	appid = uid % AID_USER;
	if (appid < AID_APP) {
            /*
             * This code is Android specific, bionic guarantees that
             * calls to non-reentrant getpwuid() are thread safe.
             */
#ifndef __BIONIC__
#warning "This code assumes that getpwuid is thread safe, only true with Bionic!"
#endif
		pwd = getpwuid(appid);
		if (!pwd)
			goto err;

		username = pwd->pw_name;

	} else if (appid < AID_ISOLATED_START) {
		username = "_app";
		appid -= AID_APP;
	} else {
		username = "_isolated";
		appid -= AID_ISOLATED_START;
	}

	if (appid >= CAT_MAPPING_MAX_ID || userid >= CAT_MAPPING_MAX_ID)
		goto err;

	for (i = 0; i < nspec; i++) {
		cur = seapp_contexts[i];

		if (cur->isSystemServer != isSystemServer)
			continue;

		if (cur->isEphemeralAppSet && cur->isEphemeralApp != isEphemeralApp)
			continue;

		if (cur->isV2AppSet && cur->isV2App != isV2App)
			continue;

		if (cur->isOwnerSet && cur->isOwner != isOwner)
			continue;

		if (cur->user.str) {
			if (cur->user.is_prefix) {
				if (strncasecmp(username, cur->user.str, cur->user.len-1))
					continue;
			} else {
				if (strcasecmp(username, cur->user.str))
					continue;
			}
		}

		if (cur->seinfo) {
			if (!seinfo || strcasecmp(seinfo, cur->seinfo))
				continue;
		}

		if (cur->name.str) {
			if(!pkgname)
				continue;

			if (cur->name.is_prefix) {
				if (strncasecmp(pkgname, cur->name.str, cur->name.len-1))
					continue;
			} else {
				if (strcasecmp(pkgname, cur->name.str))
					continue;
			}
		}

		if (cur->isPrivAppSet && cur->isPrivApp != isPrivApp)
			continue;

		if (cur->minTargetSdkVersion > targetSdkVersion)
			continue;

		if (cur->path.str) {
			if (!path)
				continue;

			if (cur->path.is_prefix) {
				if (strncmp(path, cur->path.str, cur->path.len-1))
					continue;
			} else {
				if (strcmp(path, cur->path.str))
					continue;
			}
		}

		if (kind == SEAPP_TYPE && !cur->type)
			continue;
		else if (kind == SEAPP_DOMAIN && !cur->domain)
			continue;

		if (kind == SEAPP_TYPE) {
			if (context_type_set(ctx, cur->type))
				goto oom;
		} else if (kind == SEAPP_DOMAIN) {
			if (context_type_set(ctx, cur->domain))
				goto oom;
		}

		if (cur->levelFrom != LEVELFROM_NONE) {
			char level[255];
			switch (cur->levelFrom) {
			case LEVELFROM_APP:
				snprintf(level, sizeof level, "s0:c%u,c%u",
					 appid & 0xff,
					 256 + (appid>>8 & 0xff));
				break;
			case LEVELFROM_USER:
				snprintf(level, sizeof level, "s0:c%u,c%u",
					 512 + (userid & 0xff),
					 768 + (userid>>8 & 0xff));
				break;
			case LEVELFROM_ALL:
				snprintf(level, sizeof level, "s0:c%u,c%u,c%u,c%u",
					 appid & 0xff,
					 256 + (appid>>8 & 0xff),
					 512 + (userid & 0xff),
					 768 + (userid>>8 & 0xff));
				break;
			default:
				goto err;
			}
			if (context_range_set(ctx, level))
				goto oom;
		} else if (cur->level) {
			if (context_range_set(ctx, cur->level))
				goto oom;
		}

		break;
	}

	if (kind == SEAPP_DOMAIN && i == nspec) {
		/*
		 * No match.
		 * Fail to prevent staying in the zygote's context.
		 */
		selinux_log(SELINUX_ERROR,
			    "%s:  No match for app with uid %d, seinfo %s, name %s\n",
			    __FUNCTION__, uid, seinfo, pkgname);

		if (security_getenforce() == 1)
			goto err;
	}

	return 0;
err:
	return -1;
oom:
	return -2;
}

int selinux_android_setfilecon(const char *pkgdir,
				const char *pkgname,
				const char *seinfo,
				uid_t uid)
{
	char *orig_ctx_str = NULL;
	char *ctx_str = NULL;
	context_t ctx = NULL;
	int rc = -1;

	if (is_selinux_enabled() <= 0)
		return 0;

	rc = getfilecon(pkgdir, &ctx_str);
	if (rc < 0)
		goto err;

	ctx = context_new(ctx_str);
	orig_ctx_str = ctx_str;
	if (!ctx)
		goto oom;

	rc = seapp_context_lookup(SEAPP_TYPE, uid, 0, seinfo, pkgname, NULL, ctx);
	if (rc == -1)
		goto err;
	else if (rc == -2)
		goto oom;

	ctx_str = context_str(ctx);
	if (!ctx_str)
		goto oom;

	rc = security_check_context(ctx_str);
	if (rc < 0)
		goto err;

	if (strcmp(ctx_str, orig_ctx_str)) {
		rc = setfilecon(pkgdir, ctx_str);
		if (rc < 0)
			goto err;
	}

	rc = 0;
out:
	freecon(orig_ctx_str);
	context_free(ctx);
	return rc;
err:
	selinux_log(SELINUX_ERROR, "%s:  Error setting context for pkgdir %s, uid %d: %s\n",
		    __FUNCTION__, pkgdir, uid, strerror(errno));
	rc = -1;
	goto out;
oom:
	selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __FUNCTION__);
	rc = -1;
	goto out;
}

int selinux_android_setcon(const char *con)
{
	int ret = setcon(con);
	if (ret)
		return ret;
	/*
	  System properties must be reinitialized after setcon() otherwise the
	  previous property files will be leaked since mmap()'ed regions are not
	  closed as a result of setcon().
	*/
	return __system_properties_init();
}

int selinux_android_setcontext(uid_t uid,
			       bool isSystemServer,
			       const char *seinfo,
			       const char *pkgname)
{
	char *orig_ctx_str = NULL, *ctx_str;
	context_t ctx = NULL;
	int rc = -1;

	if (is_selinux_enabled() <= 0)
		return 0;

	rc = getcon(&ctx_str);
	if (rc)
		goto err;

	ctx = context_new(ctx_str);
	orig_ctx_str = ctx_str;
	if (!ctx)
		goto oom;

	rc = seapp_context_lookup(SEAPP_DOMAIN, uid, isSystemServer, seinfo, pkgname, NULL, ctx);
	if (rc == -1)
		goto err;
	else if (rc == -2)
		goto oom;

	ctx_str = context_str(ctx);
	if (!ctx_str)
		goto oom;

	rc = security_check_context(ctx_str);
	if (rc < 0)
		goto err;

	if (strcmp(ctx_str, orig_ctx_str)) {
		rc = selinux_android_setcon(ctx_str);
		if (rc < 0)
			goto err;
	}

	rc = 0;
out:
	freecon(orig_ctx_str);
	context_free(ctx);
	avc_netlink_close();
	return rc;
err:
	if (isSystemServer)
		selinux_log(SELINUX_ERROR,
				"%s:  Error setting context for system server: %s\n",
				__FUNCTION__, strerror(errno));
	else
		selinux_log(SELINUX_ERROR,
				"%s:  Error setting context for app with uid %d, seinfo %s: %s\n",
				__FUNCTION__, uid, seinfo, strerror(errno));

	rc = -1;
	goto out;
oom:
	selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __FUNCTION__);
	rc = -1;
	goto out;
}

static struct selabel_handle *fc_sehandle = NULL;
#define FC_DIGEST_SIZE SHA_DIGEST_LENGTH
static uint8_t fc_digest[FC_DIGEST_SIZE];

static bool compute_file_contexts_hash(uint8_t c_digest[], const struct selinux_opt *opts, unsigned nopts)
{
    int fd = -1;
    void *map = MAP_FAILED;
    bool ret = false;
    uint8_t *fc_data = NULL;
    size_t total_size = 0;
    struct stat sb;
    size_t i;

    for (i = 0; i < nopts; i++) {
        fd = open(opts[i].value, O_CLOEXEC | O_RDONLY);
        if (fd < 0) {
            selinux_log(SELINUX_ERROR, "SELinux:  Could not open %s:  %s\n",
                    opts[i].value, strerror(errno));
            goto cleanup;
        }

        if (fstat(fd, &sb) < 0) {
            selinux_log(SELINUX_ERROR, "SELinux:  Could not stat %s:  %s\n",
                    opts[i].value, strerror(errno));
            goto cleanup;
        }

        map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            selinux_log(SELINUX_ERROR, "SELinux:  Could not map %s:  %s\n",
                    opts[i].value, strerror(errno));
            goto cleanup;
        }

        fc_data = realloc(fc_data, total_size + sb.st_size);
        if (!fc_data) {
            selinux_log(SELINUX_ERROR, "SELinux: Count not re-alloc for %s:  %s\n",
                     opts[i].value, strerror(errno));
            goto cleanup;
        }

        memcpy(fc_data + total_size, map, sb.st_size);
        total_size += sb.st_size;

        /* reset everything for next file */
        munmap(map, sb.st_size);
        close(fd);
        map = MAP_FAILED;
        fd = -1;
    }

    SHA1(fc_data, total_size, c_digest);
    ret = true;

cleanup:
    if (map != MAP_FAILED)
        munmap(map, sb.st_size);
    if (fd >= 0)
        close(fd);
    free(fc_data);

    return ret;
}

static void file_context_init(void)
{
    if (!fc_sehandle)
        fc_sehandle = selinux_android_file_context_handle();
}



static pthread_once_t fc_once = PTHREAD_ONCE_INIT;

#define PKGTAB_SIZE 256
static struct pkg_info *pkgTab[PKGTAB_SIZE];

static unsigned int pkghash(const char *pkgname)
{
    unsigned int h = 7;
    for (; *pkgname; pkgname++) {
        h = h * 31 + *pkgname;
    }
    return h & (PKGTAB_SIZE - 1);
}

static bool pkg_parse_callback(pkg_info *info, void *userdata) {

    (void) userdata;

    unsigned int hash = pkghash(info->name);
    if (pkgTab[hash])
        info->private_data = pkgTab[hash];
    pkgTab[hash] = info;
    return true;
}

static void package_info_init(void)
{

    bool rc = packagelist_parse(pkg_parse_callback, NULL);
    if (!rc) {
        selinux_log(SELINUX_ERROR, "SELinux: Could NOT parse package list\n");
        return;
    }

#if DEBUG
    {
        unsigned int hash, buckets, entries, chainlen, longestchain;
        struct pkg_info *info = NULL;

        buckets = entries = longestchain = 0;
        for (hash = 0; hash < PKGTAB_SIZE; hash++) {
            if (pkgTab[hash]) {
                buckets++;
                chainlen = 0;
                for (info = pkgTab[hash]; info; info = (pkg_info *)info->private_data) {
                    chainlen++;
                    selinux_log(SELINUX_INFO, "%s:  name=%s uid=%u debuggable=%s dataDir=%s seinfo=%s\n",
                                __FUNCTION__,
                                info->name, info->uid, info->debuggable ? "true" : "false", info->data_dir, info->seinfo);
                }
                entries += chainlen;
                if (longestchain < chainlen)
                    longestchain = chainlen;
            }
        }
        selinux_log(SELINUX_INFO, "SELinux:  %d pkg entries and %d/%d buckets used, longest chain %d\n", entries, buckets, PKGTAB_SIZE, longestchain);
    }
#endif

}

static pthread_once_t pkg_once = PTHREAD_ONCE_INIT;

struct pkg_info *package_info_lookup(const char *name)
{
    struct pkg_info *info;
    unsigned int hash;

    __selinux_once(pkg_once, package_info_init);

    hash = pkghash(name);
    for (info = pkgTab[hash]; info; info = (pkg_info *)info->private_data) {
        if (!strcmp(name, info->name))
            return info;
    }
    return NULL;
}

/* The contents of these paths are encrypted on FBE devices until user
 * credentials are presented (filenames inside are mangled), so we need
 * to delay restorecon of those until vold explicitly requests it. */
// NOTE: these paths need to be kept in sync with vold
#define DATA_SYSTEM_CE_PREFIX "/data/system_ce/"
#define DATA_MISC_CE_PREFIX "/data/misc_ce/"

/* The path prefixes of package data directories. */
#define DATA_DATA_PATH "/data/data"
#define DATA_USER_PATH "/data/user"
#define DATA_USER_DE_PATH "/data/user_de"
#define EXPAND_USER_PATH "/mnt/expand/\?\?\?\?\?\?\?\?-\?\?\?\?-\?\?\?\?-\?\?\?\?-\?\?\?\?\?\?\?\?\?\?\?\?/user"
#define EXPAND_USER_DE_PATH "/mnt/expand/\?\?\?\?\?\?\?\?-\?\?\?\?-\?\?\?\?-\?\?\?\?-\?\?\?\?\?\?\?\?\?\?\?\?/user_de"
#define DATA_DATA_PREFIX DATA_DATA_PATH "/"
#define DATA_USER_PREFIX DATA_USER_PATH "/"
#define DATA_USER_DE_PREFIX DATA_USER_DE_PATH "/"

static int pkgdir_selabel_lookup(const char *pathname,
                                 const char *seinfo,
                                 uid_t uid,
                                 char **secontextp)
{
    char *pkgname = NULL, *end = NULL;
    struct pkg_info *info = NULL;
    char *secontext = *secontextp;
    context_t ctx = NULL;
    int rc = 0;

    /* Skip directory prefix before package name. */
    if (!strncmp(pathname, DATA_DATA_PREFIX, sizeof(DATA_DATA_PREFIX)-1)) {
        pathname += sizeof(DATA_DATA_PREFIX) - 1;
    } else if (!strncmp(pathname, DATA_USER_PREFIX, sizeof(DATA_USER_PREFIX)-1)) {
        pathname += sizeof(DATA_USER_PREFIX) - 1;
        while (isdigit(*pathname))
            pathname++;
        if (*pathname == '/')
            pathname++;
        else
            return 0;
    } else if (!strncmp(pathname, DATA_USER_DE_PREFIX, sizeof(DATA_USER_DE_PREFIX)-1)) {
        pathname += sizeof(DATA_USER_DE_PREFIX) - 1;
        while (isdigit(*pathname))
            pathname++;
        if (*pathname == '/')
            pathname++;
        else
            return 0;
    } else if (!fnmatch(EXPAND_USER_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME)) {
        pathname += sizeof(EXPAND_USER_PATH);
        while (isdigit(*pathname))
            pathname++;
        if (*pathname == '/')
            pathname++;
        else
            return 0;
    } else if (!fnmatch(EXPAND_USER_DE_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME)) {
        pathname += sizeof(EXPAND_USER_DE_PATH);
        while (isdigit(*pathname))
            pathname++;
        if (*pathname == '/')
            pathname++;
        else
            return 0;
    } else
        return 0;

    if (!(*pathname))
        return 0;

    pkgname = strdup(pathname);
    if (!pkgname)
        return -1;

    for (end = pkgname; *end && *end != '/'; end++)
        ;
    pathname = end;
    if (*end)
        pathname++;
    *end = '\0';

    if (!seinfo) {
        info = package_info_lookup(pkgname);
        if (!info) {
            selinux_log(SELINUX_WARNING, "SELinux:  Could not look up information for package %s, cannot restorecon %s.\n",
                        pkgname, pathname);
            free(pkgname);
            return -1;
        }
    }

    ctx = context_new(secontext);
    if (!ctx)
        goto err;

    rc = seapp_context_lookup(SEAPP_TYPE, info ? info->uid : uid, 0,
                              info ? info->seinfo : seinfo, info ? info->name : pkgname, pathname, ctx);
    if (rc < 0)
        goto err;

    secontext = context_str(ctx);
    if (!secontext)
        goto err;

    if (!strcmp(secontext, *secontextp))
        goto out;

    rc = security_check_context(secontext);
    if (rc < 0)
        goto err;

    freecon(*secontextp);
    *secontextp = strdup(secontext);
    if (!(*secontextp))
        goto err;

    rc = 0;

out:
    free(pkgname);
    context_free(ctx);
    return rc;
err:
    selinux_log(SELINUX_ERROR, "%s:  Error looking up context for path %s, pkgname %s, seinfo %s, uid %u: %s\n",
                __FUNCTION__, pathname, pkgname, info->seinfo, info->uid, strerror(errno));
    rc = -1;
    goto out;
}

#define RESTORECON_LAST "security.restorecon_last"

static int restorecon_sb(const char *pathname, const struct stat *sb,
                         bool nochange, bool verbose,
                         const char *seinfo, uid_t uid)
{
    char *secontext = NULL;
    char *oldsecontext = NULL;
    int rc = 0;

    if (selabel_lookup(fc_sehandle, &secontext, pathname, sb->st_mode) < 0)
        return 0;  /* no match, but not an error */

    if (lgetfilecon(pathname, &oldsecontext) < 0)
        goto err;

    /*
     * For subdirectories of /data/data or /data/user, we ignore selabel_lookup()
     * and use pkgdir_selabel_lookup() instead. Files within those directories
     * have different labeling rules, based off of /seapp_contexts, and
     * installd is responsible for managing these labels instead of init.
     */
    if (!strncmp(pathname, DATA_DATA_PREFIX, sizeof(DATA_DATA_PREFIX)-1) ||
        !strncmp(pathname, DATA_USER_PREFIX, sizeof(DATA_USER_PREFIX)-1) ||
        !strncmp(pathname, DATA_USER_DE_PREFIX, sizeof(DATA_USER_DE_PREFIX)-1) ||
        !fnmatch(EXPAND_USER_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME) ||
        !fnmatch(EXPAND_USER_DE_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME)) {
        if (pkgdir_selabel_lookup(pathname, seinfo, uid, &secontext) < 0)
            goto err;
    }

    if (strcmp(oldsecontext, secontext) != 0) {
        if (verbose)
            selinux_log(SELINUX_INFO,
                        "SELinux:  Relabeling %s from %s to %s.\n", pathname, oldsecontext, secontext);
        if (!nochange) {
            if (lsetfilecon(pathname, secontext) < 0)
                goto err;
        }
    }

    rc = 0;

out:
    freecon(oldsecontext);
    freecon(secontext);
    return rc;

err:
    selinux_log(SELINUX_ERROR,
                "SELinux: Could not set context for %s:  %s\n",
                pathname, strerror(errno));
    rc = -1;
    goto out;
}

#define SYS_PATH "/sys"
#define SYS_PREFIX SYS_PATH "/"

static int selinux_android_restorecon_common(const char* pathname_orig,
                                             const char *seinfo,
                                             uid_t uid,
                                             unsigned int flags)
{
    bool nochange = (flags & SELINUX_ANDROID_RESTORECON_NOCHANGE) ? true : false;
    bool verbose = (flags & SELINUX_ANDROID_RESTORECON_VERBOSE) ? true : false;
    bool recurse = (flags & SELINUX_ANDROID_RESTORECON_RECURSE) ? true : false;
    bool force = (flags & SELINUX_ANDROID_RESTORECON_FORCE) ? true : false;
    bool datadata = (flags & SELINUX_ANDROID_RESTORECON_DATADATA) ? true : false;
    bool skipce = (flags & SELINUX_ANDROID_RESTORECON_SKIPCE) ? true : false;
    bool cross_filesystems = (flags & SELINUX_ANDROID_RESTORECON_CROSS_FILESYSTEMS) ? true : false;
    bool issys;
    bool setrestoreconlast = true;
    struct stat sb;
    struct statfs sfsb;
    FTS *fts;
    FTSENT *ftsent;
    char *pathname = NULL, *pathdnamer = NULL, *pathdname, *pathbname;
    char * paths[2] = { NULL , NULL };
    int ftsflags = FTS_NOCHDIR | FTS_PHYSICAL;
    int error, sverrno;
    char xattr_value[FC_DIGEST_SIZE];
    ssize_t size;

    if (!cross_filesystems) {
        ftsflags |= FTS_XDEV;
    }

    if (is_selinux_enabled() <= 0)
        return 0;

    __selinux_once(fc_once, file_context_init);

    if (!fc_sehandle)
        return 0;

    /*
     * Convert passed-in pathname to canonical pathname by resolving realpath of
     * containing dir, then appending last component name.
     */
    pathbname = basename(pathname_orig);
    if (!strcmp(pathbname, "/") || !strcmp(pathbname, ".") || !strcmp(pathbname, "..")) {
        pathname = realpath(pathname_orig, NULL);
        if (!pathname)
            goto realpatherr;
    } else {
        pathdname = dirname(pathname_orig);
        pathdnamer = realpath(pathdname, NULL);
        if (!pathdnamer)
            goto realpatherr;
        if (!strcmp(pathdnamer, "/"))
            error = asprintf(&pathname, "/%s", pathbname);
        else
            error = asprintf(&pathname, "%s/%s", pathdnamer, pathbname);
        if (error < 0)
            goto oom;
    }

    paths[0] = pathname;
    issys = (!strcmp(pathname, SYS_PATH)
            || !strncmp(pathname, SYS_PREFIX, sizeof(SYS_PREFIX)-1)) ? true : false;

    if (!recurse) {
        if (lstat(pathname, &sb) < 0) {
            error = -1;
            goto cleanup;
        }

        error = restorecon_sb(pathname, &sb, nochange, verbose, seinfo, uid);
        goto cleanup;
    }

    /*
     * Ignore restorecon_last on /data/data or /data/user
     * since their labeling is based on seapp_contexts and seinfo
     * assignments rather than file_contexts and is managed by
     * installd rather than init.
     */
    if (!strncmp(pathname, DATA_DATA_PREFIX, sizeof(DATA_DATA_PREFIX)-1) ||
        !strncmp(pathname, DATA_USER_PREFIX, sizeof(DATA_USER_PREFIX)-1) ||
        !strncmp(pathname, DATA_USER_DE_PREFIX, sizeof(DATA_USER_DE_PREFIX)-1) ||
        !fnmatch(EXPAND_USER_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME) ||
        !fnmatch(EXPAND_USER_DE_PATH, pathname, FNM_LEADING_DIR|FNM_PATHNAME))
        setrestoreconlast = false;

    /* Also ignore on /sys since it is regenerated on each boot regardless. */
    if (issys)
        setrestoreconlast = false;

    /* Ignore files on in-memory filesystems */
    if (statfs(pathname, &sfsb) == 0) {
        if (sfsb.f_type == RAMFS_MAGIC || sfsb.f_type == TMPFS_MAGIC)
            setrestoreconlast = false;
    }

    if (setrestoreconlast) {
        size = getxattr(pathname, RESTORECON_LAST, xattr_value, sizeof fc_digest);
        if (!force && size == sizeof fc_digest && memcmp(fc_digest, xattr_value, sizeof fc_digest) == 0) {
            selinux_log(SELINUX_INFO,
                        "SELinux: Skipping restorecon_recursive(%s)\n",
                        pathname);
            error = 0;
            goto cleanup;
        }
    }

    fts = fts_open(paths, ftsflags, NULL);
    if (!fts) {
        error = -1;
        goto cleanup;
    }

    error = 0;
    while ((ftsent = fts_read(fts)) != NULL) {
        switch (ftsent->fts_info) {
        case FTS_DC:
            selinux_log(SELINUX_ERROR,
                        "SELinux:  Directory cycle on %s.\n", ftsent->fts_path);
            errno = ELOOP;
            error = -1;
            goto out;
        case FTS_DP:
            continue;
        case FTS_DNR:
            selinux_log(SELINUX_ERROR,
                        "SELinux:  Could not read %s: %s.\n", ftsent->fts_path, strerror(errno));
            fts_set(fts, ftsent, FTS_SKIP);
            continue;
        case FTS_NS:
            selinux_log(SELINUX_ERROR,
                        "SELinux:  Could not stat %s: %s.\n", ftsent->fts_path, strerror(errno));
            fts_set(fts, ftsent, FTS_SKIP);
            continue;
        case FTS_ERR:
            selinux_log(SELINUX_ERROR,
                        "SELinux:  Error on %s: %s.\n", ftsent->fts_path, strerror(errno));
            fts_set(fts, ftsent, FTS_SKIP);
            continue;
        case FTS_D:
            if (issys && !selabel_partial_match(fc_sehandle, ftsent->fts_path)) {
                fts_set(fts, ftsent, FTS_SKIP);
                continue;
            }

            if (skipce &&
                (!strncmp(ftsent->fts_path, DATA_SYSTEM_CE_PREFIX, sizeof(DATA_SYSTEM_CE_PREFIX)-1) ||
                 !strncmp(ftsent->fts_path, DATA_MISC_CE_PREFIX, sizeof(DATA_MISC_CE_PREFIX)-1))) {
                // Don't label anything below this directory.
                fts_set(fts, ftsent, FTS_SKIP);
                // but fall through and make sure we label the directory itself
            }

            if (!datadata &&
                (!strcmp(ftsent->fts_path, DATA_DATA_PATH) ||
                 !strncmp(ftsent->fts_path, DATA_USER_PREFIX, sizeof(DATA_USER_PREFIX)-1) ||
                 !strncmp(ftsent->fts_path, DATA_USER_DE_PREFIX, sizeof(DATA_USER_DE_PREFIX)-1) ||
                 !fnmatch(EXPAND_USER_PATH, ftsent->fts_path, FNM_LEADING_DIR|FNM_PATHNAME) ||
                 !fnmatch(EXPAND_USER_DE_PATH, ftsent->fts_path, FNM_LEADING_DIR|FNM_PATHNAME))) {
                // Don't label anything below this directory.
                fts_set(fts, ftsent, FTS_SKIP);
                // but fall through and make sure we label the directory itself
            }
            /* fall through */
        default:
            error |= restorecon_sb(ftsent->fts_path, ftsent->fts_statp, nochange, verbose, seinfo, uid);
            break;
        }
    }

    // Labeling successful. Mark the top level directory as completed.
    if (setrestoreconlast && !nochange && !error)
        setxattr(pathname, RESTORECON_LAST, fc_digest, sizeof fc_digest, 0);

out:
    sverrno = errno;
    (void) fts_close(fts);
    errno = sverrno;
cleanup:
    free(pathdnamer);
    free(pathname);
    return error;
oom:
    sverrno = errno;
    selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __FUNCTION__);
    errno = sverrno;
    error = -1;
    goto cleanup;
realpatherr:
    sverrno = errno;
    selinux_log(SELINUX_ERROR, "SELinux: Could not get canonical path for %s restorecon: %s.\n",
            pathname_orig, strerror(errno));
    errno = sverrno;
    error = -1;
    goto cleanup;
}

int selinux_android_restorecon(const char *file, unsigned int flags)
{
    return selinux_android_restorecon_common(file, NULL, -1, flags);
}

int selinux_android_restorecon_pkgdir(const char *pkgdir,
                                      const char *seinfo,
                                      uid_t uid,
                                      unsigned int flags)
{
    return selinux_android_restorecon_common(pkgdir, seinfo, uid, flags | SELINUX_ANDROID_RESTORECON_DATADATA);
}

static struct selabel_handle* selinux_android_file_context(const struct selinux_opt *opts,
                                                    unsigned nopts)
{
    struct selabel_handle *sehandle;
    struct selinux_opt fc_opts[nopts + 1];

    memcpy(fc_opts, opts, nopts*sizeof(struct selinux_opt));
    fc_opts[nopts].type = SELABEL_OPT_BASEONLY;
    fc_opts[nopts].value = (char *)1;

    sehandle = selabel_open(SELABEL_CTX_FILE, fc_opts, ARRAY_SIZE(fc_opts));
    if (!sehandle) {
        selinux_log(SELINUX_ERROR, "%s: Error getting file context handle (%s)\n",
                __FUNCTION__, strerror(errno));
        return NULL;
    }
    if (!compute_file_contexts_hash(fc_digest, opts, nopts)) {
        selabel_close(sehandle);
        return NULL;
    }

    selinux_log(SELINUX_INFO, "SELinux: Loaded file_contexts\n");

    return sehandle;
}

static bool selinux_android_opts_file_exists(const struct selinux_opt *opt)
{
    return (access(opt[0].value, R_OK) != -1);
}

struct selabel_handle* selinux_android_file_context_handle(void)
{
    if (selinux_android_opts_file_exists(seopts_file_split)) {
        return selinux_android_file_context(seopts_file_split,
                                            ARRAY_SIZE(seopts_file_split));
    } else {
        return selinux_android_file_context(seopts_file_rootfs,
                                            ARRAY_SIZE(seopts_file_rootfs));
    }
}
struct selabel_handle* selinux_android_prop_context_handle(void)
{
    struct selabel_handle* sehandle;
    const struct selinux_opt* seopts_prop;

    // Prefer files from /system & /vendor, fall back to files from /
    if (access(seopts_prop_split[0].value, R_OK) != -1) {
        seopts_prop = seopts_prop_split;
    } else {
        seopts_prop = seopts_prop_rootfs;
    }

    sehandle = selabel_open(SELABEL_CTX_ANDROID_PROP,
            seopts_prop, 2);
    if (!sehandle) {
        selinux_log(SELINUX_ERROR, "%s: Error getting property context handle (%s)\n",
                __FUNCTION__, strerror(errno));
        return NULL;
    }
    selinux_log(SELINUX_INFO, "SELinux: Loaded property_contexts from %s & %s.\n",
            seopts_prop[0].value, seopts_prop[1].value);

    return sehandle;
}

struct selabel_handle* selinux_android_service_open_context_handle(const struct selinux_opt* seopts_service,
                                                                   unsigned nopts)
{
    struct selabel_handle* sehandle;

    sehandle = selabel_open(SELABEL_CTX_ANDROID_SERVICE,
            seopts_service, nopts);

    if (!sehandle) {
        selinux_log(SELINUX_ERROR, "%s: Error getting service context handle (%s)\n",
                __FUNCTION__, strerror(errno));
        return NULL;
    }
    selinux_log(SELINUX_INFO, "SELinux: Loaded service_contexts from:\n");
    for (unsigned i = 0; i < nopts; i++) {
        selinux_log(SELINUX_INFO, "    %s\n", seopts_service[i].value);
    }
    return sehandle;
}

struct selabel_handle* selinux_android_service_context_handle(void)
{
    const struct selinux_opt* seopts_service;

    // Prefer files from /system & /vendor, fall back to files from /
    if (access(seopts_service_split[0].value, R_OK) != -1) {
        seopts_service = seopts_service_split;
    } else {
        seopts_service = seopts_service_rootfs;
    }

    // TODO(b/36866029) full treble devices can't load non-plat
    return selinux_android_service_open_context_handle(seopts_service, 2);
}

struct selabel_handle* selinux_android_hw_service_context_handle(void)
{
    const struct selinux_opt* seopts_service;
    if (access(seopts_hwservice_split[0].value, R_OK) != -1) {
        seopts_service = seopts_hwservice_split;
    } else {
        seopts_service = seopts_hwservice_rootfs;
    }

    return selinux_android_service_open_context_handle(seopts_service, 2);
}

struct selabel_handle* selinux_android_vendor_service_context_handle(void)
{
    const struct selinux_opt* seopts_service;
    if (access(seopts_vndservice.value, R_OK) != -1) {
        seopts_service = &seopts_vndservice;
    } else {
        seopts_service = &seopts_vndservice_rootfs;
    }

    return selinux_android_service_open_context_handle(seopts_service, 1);
}

void selinux_android_set_sehandle(const struct selabel_handle *hndl)
{
    fc_sehandle = (struct selabel_handle *) hndl;
}

int selinux_android_load_policy()
{
	int fd = -1;

	fd = open(sepolicy_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		selinux_log(SELINUX_ERROR, "SELinux:  Could not open %s:  %s\n",
				sepolicy_file, strerror(errno));
		return -1;
	}
	int ret = selinux_android_load_policy_from_fd(fd, sepolicy_file);
	close(fd);
	return ret;
}

int selinux_android_load_policy_from_fd(int fd, const char *description)
{
	int rc;
	struct stat sb;
	void *map = NULL;
	static int load_successful = 0;

	/*
	 * Since updating policy at runtime has been abolished
	 * we just check whether a policy has been loaded before
	 * and return if this is the case.
	 * There is no point in reloading policy.
	 */
	if (load_successful){
	  selinux_log(SELINUX_WARNING, "SELinux: Attempted reload of SELinux policy!/n");
	  return 0;
	}

	set_selinuxmnt(SELINUXMNT);
	if (fstat(fd, &sb) < 0) {
		selinux_log(SELINUX_ERROR, "SELinux:  Could not stat %s:  %s\n",
				description, strerror(errno));
		return -1;
	}
	map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		selinux_log(SELINUX_ERROR, "SELinux:  Could not map %s:  %s\n",
				description, strerror(errno));
		return -1;
	}

	rc = security_load_policy(map, sb.st_size);
	if (rc < 0) {
		selinux_log(SELINUX_ERROR, "SELinux:  Could not load policy:  %s\n",
				strerror(errno));
		munmap(map, sb.st_size);
		return -1;
	}

	munmap(map, sb.st_size);
	selinux_log(SELINUX_INFO, "SELinux: Loaded policy from %s\n", description);
	load_successful = 1;
	return 0;
}

int selinux_log_callback(int type, const char *fmt, ...)
{
    va_list ap;
    int priority;
    char *strp;

    switch(type) {
    case SELINUX_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
    case SELINUX_INFO:
        priority = ANDROID_LOG_INFO;
        break;
    default:
        priority = ANDROID_LOG_ERROR;
        break;
    }

    va_start(ap, fmt);
    if (vasprintf(&strp, fmt, ap) != -1) {
        LOG_PRI(priority, "SELinux", "%s", strp);
        LOG_EVENT_STRING(AUDITD_LOG_TAG, strp);
        free(strp);
    }
    va_end(ap);
    return 0;
}
