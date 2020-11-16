/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 *   Copyright (C) 2020 John Crispin <john@phrozen.org> 
 */

#include "usync.h"

#define USYNC_TMP	"/tmp/usync.tmp"
#define USYNC_LATEST	"/etc/usync/usync.active"

enum {
	CONFIG_UUID,
	__CONFIG_MAX,
};

static const struct blobmsg_policy config_policy[__CONFIG_MAX] = {
	[CONFIG_UUID] = { .name = "uuid", .type = BLOBMSG_TYPE_INT32 },
};

static struct blob_attr *config_tb[__CONFIG_MAX];
static struct blob_buf cfg;

static time_t uuid_applied;
time_t uuid_latest = 0;
time_t uuid_active = 0;

static time_t
config_load(const char *path)
{
	blob_buf_init(&cfg, 0);
	if (!blobmsg_add_json_from_file(&cfg, path)) {
		ULOG_ERR("failed to load %s\n", path);
		return 0;
	}

	memset(config_tb, 0, sizeof(config_tb));
	blobmsg_parse(config_policy, __CONFIG_MAX, config_tb, blob_data(cfg.head), blob_len(cfg.head));
	if (config_tb[CONFIG_UUID])
		return blobmsg_get_u32(config_tb[CONFIG_UUID]);

	return 0;
}

static void
apply_run_cb(time_t uuid)
{
	char str[64];

	ULOG_ERR("running verify task\n");

	sprintf(str, "/etc/usync/usync.cfg.%010ld", uuid);
	execlp("/usr/sbin/usync_apply.sh", "/usr/sbin/usync_apply.sh", str, NULL);
	exit(1);
}

static void
apply_complete_cb(int ret)
{
	if (ret) {
		ULOG_ERR("apply task returned %d\n", ret);
		config_init(0);
		proto_send_heartbeat();
		return;
	}
	uuid_active = uuid_applied = uuid_latest;
	ULOG_INFO("applied cfg:%ld\n", uuid_latest);
	proto_send_heartbeat();
}

struct task apply_task = {
	.run_time = 10,
	.run = apply_run_cb,
	.complete = apply_complete_cb,
};

static void
config_apply(void)
{
	if (uuid_latest && (uuid_latest == uuid_applied))
		return;
	ULOG_INFO("applying cfg:%ld\n", uuid_latest);
	task_run(&apply_task, uuid_latest);
}

void
config_init(int apply)
{
	char path[PATH_MAX] = { };
	char link[PATH_MAX] = { };
	struct stat s;
	glob_t gl;

	uuid_active = 0;

	snprintf(path, PATH_MAX, "%s/usync.cfg.*", USYNC_CONFIG);
	if (glob(path, 0, NULL, &gl))
                return;
	if (!gl.gl_pathc)
		goto out;

	uuid_latest = config_load(gl.gl_pathv[gl.gl_pathc - 1]);

	if (apply)
		config_apply();

	snprintf(path, PATH_MAX, "%s/usync.active", USYNC_CONFIG);
	if (readlink(path, link, PATH_MAX) < 0) {
		ULOG_INFO("no active symlink found\n");
		goto out;
	}

	snprintf(path, PATH_MAX, "%s/%s", USYNC_CONFIG, basename(link));
	if (stat(path, &s)) {
		ULOG_INFO("active config not found\n");
		goto out;
	}

	uuid_active = config_load(path);

out:
	globfree(&gl);
	ULOG_INFO("config_init latest:%ld active:%ld\n", uuid_latest, uuid_active);
}

static void
verify_run_cb(time_t uuid)
{
	char str[64];

	ULOG_ERR("running verify task\n");

	sprintf(str, "%010ld", uuid);
	execlp("/usr/sbin/usync_verify.sh", "/usr/sbin/usync_verify.sh", USYNC_TMP, str, NULL);
	exit(1);
}

static void
verify_complete_cb(int ret)
{
	if (ret) {
		ULOG_ERR("verify task returned %d\n", ret);
		proto_send_heartbeat();
		return;
	}
	config_init(1);
}

struct task verify_task = {
	.run_time = 10,
	.run = verify_run_cb,
	.complete = verify_complete_cb,
};

int
config_verify(struct blob_attr *attr)
{
	static struct blob_attr *tb[__CONFIG_MAX];
	FILE *fp = NULL;
	char *cfg;
	int ret = -1;

	blobmsg_parse(config_policy, __CONFIG_MAX, tb, blobmsg_data(attr), blobmsg_data_len(attr));
	if (!tb[CONFIG_UUID])
		return -1;

	cfg = blobmsg_format_json(attr, true);
	if (!cfg)
		goto err;

	fp = fopen(USYNC_TMP, "w+");
	if (!fp)
		goto err;
	if (fwrite(cfg, strlen(cfg), 1, fp) != 1)
		goto err;
	fclose(fp);
	fp = NULL;

	uuid_latest = (time_t)blobmsg_get_u32(tb[CONFIG_UUID]);
	ret = 0;

err:
	if (cfg)
		free(cfg);
	if (fp)
		fclose(fp);

	if (!ret &&
	    (!uuid_active || uuid_active != uuid_latest))
		task_run(&verify_task, uuid_latest);

	return 0;
}


