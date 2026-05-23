/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2026 Telnyx, Inc.
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <config.h>
#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>

#include "crun.h"
#include "libcrun/utils.h"
#include "delete.h"

enum
{
  OPTION_FROM = 1000,
  OPTION_COUNT,
  OPTION_TTL,
  OPTION_SHARE_NETWORK,
  OPTION_SHARE_IPC,
  OPTION_CRIU,
};

static const char *from_id = NULL;
static int count = 1;
static int ttl = 0;
static bool share_network = false;
static bool share_ipc = false;
static bool use_criu = false;

static struct argp_option options[]
    = { { "from", OPTION_FROM, "ID", 0, "parent container ID to fork from", 0 },
        { "count", OPTION_COUNT, "N", 0, "number of children to fork", 0 },
        { "ttl", OPTION_TTL, "SECONDS", 0, "auto-delete after N seconds (0 = keep)", 0 },
        { "share-network", OPTION_SHARE_NETWORK, 0, 0, "share parent's network namespace", 0 },
        { "share-ipc", OPTION_SHARE_IPC, 0, 0, "share parent's IPC namespace", 0 },
        { "criu", OPTION_CRIU, 0, 0, "use CRIU for memory COW", 0 },
        { 0 } };

static char doc[] = "OCI runtime";
static char args_doc[] = "fork [OPTION]... PREFIX";

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case OPTION_FROM:
      from_id = argp_mandatory_argument (arg, state);
      break;

    case OPTION_COUNT:
      count = parse_int_or_fail (argp_mandatory_argument (arg, state), "count");
      if (count < 1)
        libcrun_fail_with_error (0, "count must be >= 1");
      break;

    case OPTION_TTL:
      ttl = parse_int_or_fail (argp_mandatory_argument (arg, state), "ttl");
      if (ttl < 0)
        libcrun_fail_with_error (0, "ttl must be >= 0");
      break;

    case OPTION_SHARE_NETWORK:
      share_network = true;
      break;

    case OPTION_SHARE_IPC:
      share_ipc = true;
      break;

    case OPTION_CRIU:
      use_criu = true;
      break;

    case ARGP_KEY_NO_ARGS:
      libcrun_fail_with_error (0, "please specify a prefix (e.g. worker)");

    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

/* Forward declaration.  */
extern int crun_command_split (struct crun_global_arguments *, int, char **, libcrun_error_t *);

int
crun_command_fork (struct crun_global_arguments *global_args, int argc, char **argv, libcrun_error_t *err)
{
  (void) err;
  int first_arg;
  int ret;

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, NULL);
  crun_assert_n_args (argc - first_arg, 1, 1);

  const char *prefix = argv[first_arg];

  if (from_id == NULL)
    libcrun_fail_with_error (0, "specify the parent with --from");

  int succeeded = 0;
  int i;
  for (i = 0; i < count; i++)
    {
      cleanup_free char *child_name = NULL;
      ret = asprintf (&child_name, "%s%d", prefix, i);
      if (UNLIKELY (ret < 0))
        OOM ();

      size_t split_argc = 3;
      if (share_network)
        split_argc++;
      if (share_ipc)
        split_argc++;
      if (use_criu)
        split_argc++;

      char **split_argv = xmalloc0 ((split_argc + 1) * sizeof (char *));
      size_t pos = 0;
      split_argv[pos++] = xstrdup (argv[0] ? argv[0] : "crun");
      split_argv[pos++] = xstrdup ("--from");
      split_argv[pos++] = xstrdup (from_id);

      if (share_network)
        split_argv[pos++] = xstrdup ("--share-network");
      if (share_ipc)
        split_argv[pos++] = xstrdup ("--share-ipc");
      if (use_criu)
        split_argv[pos++] = xstrdup ("--criu");

      split_argv[pos++] = xstrdup (child_name);
      split_argv[pos] = NULL;

      libcrun_error_t split_err = NULL;
      ret = crun_command_split (global_args, (int) pos, split_argv, &split_err);

      size_t j;
      for (j = 0; j < pos; j++)
        free (split_argv[j]);
      free (split_argv);

      if (UNLIKELY (ret < 0))
        {
          libcrun_error_t *tmp_err = &split_err;
          libcrun_error_write_warning_and_release (stderr, &tmp_err);
          continue;
        }

      succeeded++;

      if (ttl > 0)
        {
          pid_t watcher = fork ();
          if (watcher == 0)
            {
              sleep (ttl);
              cleanup_free char *del_argv0 = xstrdup (argv[0]);
              cleanup_free char *del_id = xstrdup (child_name);
              char *del_argv[] = { del_argv0, del_id, NULL };
              libcrun_error_t del_err = NULL;
              crun_command_delete (global_args, 2, del_argv, &del_err);
              if (del_err)
                libcrun_error_release (&del_err);
              _exit (0);
            }
          else if (watcher < 0)
            {
              libcrun_warning ("fork ttl watcher for `%s` failed: %s", child_name, strerror (errno));
            }
        }
    }

  fprintf (stdout, "forked %d/%d children from `%s` (prefix `%s`)\n", succeeded, count, from_id, prefix);

  return succeeded == count ? 0 : 1;
}
