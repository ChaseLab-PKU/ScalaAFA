/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"

#include "spdk/vhost.h"

static const char *g_pid_path = NULL;

static void
vhost_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -S <path>                 directory where to create vhost sockets (default: pwd)\n");
}

static void
save_pid(const char *pid_path)
{
	FILE *pid_file;

	pid_file = fopen(pid_path, "w");
	if (pid_file == NULL) {
		fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	fprintf(pid_file, "%d\n", getpid());
	fclose(pid_file);
}

static int
vhost_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case 'S':
		spdk_vhost_set_socket_path(arg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
mythread_init(void *arg)
{
	char *str = arg;
	int tmp1 = spdk_env_get_core_count();
	int tmp2 = spdk_env_get_current_core();

	printf("%s %d %d", str, tmp1, tmp2);
}

static void
vhost_started(void *arg1)
{
	int tmp1 = spdk_env_get_core_count();
	int tmp2 = spdk_env_get_first_core();
	printf("\n%d %d\n", tmp1, tmp2);


	struct spdk_cpuset cpu_mask = {};
	struct spdk_thread *t;

	spdk_cpuset_set_cpu(&cpu_mask, spdk_env_get_last_core(), true);

	t= spdk_thread_create("mythread", &cpu_mask);
	spdk_thread_send_msg(t, mythread_init, "infos");
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "vhost";
	opts.reactor_mask = "0x3";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:S:", NULL,
				      vhost_parse_arg, vhost_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, vhost_started, NULL);

	spdk_app_fini();

	return rc;
}
