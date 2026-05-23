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
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <json-c/json.h>

#include "crun.h"
#include "libcrun/container.h"
#include "libcrun/status.h"
#include "libcrun/utils.h"

enum
{
  OPTION_FROM = 1000,
  OPTION_CONSOLE_SOCKET,
  OPTION_PID_FILE,
  OPTION_NO_SUBREAPER,
  OPTION_NO_NEW_KEYRING,
  OPTION_PRESERVE_FDS,
  OPTION_NO_PIVOT
};

static const char *bundle = NULL;
static const char *from_id = NULL;

static libcrun_context_t crun_context;

static struct argp_option options[]
    = { { "from", OPTION_FROM, "ID", 0, "parent container ID to split from (COW)", 0 },
        { "bundle", 'b', "DIR", 0, "container bundle (default \".\")", 0 },
        { "config", 'f', "FILE", 0, "override the config file name", 0 },
        { "console-socket", OPTION_CONSOLE_SOCKET, "SOCK", 0,
          "path to a socket that will receive the ptmx end of the tty", 0 },
        { "preserve-fds", OPTION_PRESERVE_FDS, "N", 0, "pass additional FDs to the container", 0 },
        { "no-pivot", OPTION_NO_PIVOT, 0, 0, "do not use pivot_root", 0 },
        { "pid-file", OPTION_PID_FILE, "FILE", 0, "where to write the PID of the container", 0 },
        { "no-subreaper", OPTION_NO_SUBREAPER, 0, 0, "do not create a subreaper process (ignored)", 0 },
        { "no-new-keyring", OPTION_NO_NEW_KEYRING, 0, 0, "keep the same session key", 0 },
        {
            0,
        } };

static char doc[] = "OCI runtime";

static char args_doc[] = "split [OPTION]... CONTAINER";

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'b':
      bundle = crun_context.bundle = argp_mandatory_argument (arg, state);
      break;

    case 'f':
      break;

    case OPTION_FROM:
      from_id = argp_mandatory_argument (arg, state);
      break;

    case OPTION_CONSOLE_SOCKET:
      crun_context.console_socket = argp_mandatory_argument (arg, state);
      break;

    case OPTION_PRESERVE_FDS:
      crun_context.preserve_fds = parse_int_or_fail (argp_mandatory_argument (arg, state), "preserve-fds");
      break;

    case OPTION_NO_SUBREAPER:
      break;

    case OPTION_NO_PIVOT:
      crun_context.no_pivot = true;
      break;

    case OPTION_NO_NEW_KEYRING:
      crun_context.no_new_keyring = true;
      break;

    case OPTION_PID_FILE:
      crun_context.pid_file = argp_mandatory_argument (arg, state);
      break;

    case ARGP_KEY_NO_ARGS:
      libcrun_fail_with_error (0, "please specify a ID for the container");

    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

static int
setup_overlayfs_rootfs (const char *parent_rootfs, const char *overlay_rootfs, const char *upperdir, const char *workdir,
                        libcrun_error_t *err)
{
  int ret;

  ret = crun_ensure_directory (overlay_rootfs, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (upperdir, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (workdir, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  cleanup_free char *opts = NULL;
  ret = asprintf (&opts, "lowerdir=%s,upperdir=%s,workdir=%s", parent_rootfs, upperdir, workdir);
  if (UNLIKELY (ret < 0))
    OOM ();

  ret = mount ("overlay", overlay_rootfs, "overlay", MS_NOATIME, opts);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "mount overlayfs for split container at `%s`", overlay_rootfs);

  return 0;
}

static int
copy_config_with_new_rootfs (const char *parent_config_path, const char *child_config_path, const char *child_rootfs,
                             libcrun_error_t *err)
{
  json_object *jobj = NULL;
  int ret;

  jobj = json_object_from_file (parent_config_path);
  if (UNLIKELY (jobj == NULL))
    return crun_make_error (err, EINVAL, "parse parent config `%s`", parent_config_path);

  json_object *root_obj;
  if (json_object_object_get_ex (jobj, "root", &root_obj))
    {
      json_object *path_obj = json_object_new_string (child_rootfs);
      json_object_object_add (root_obj, "path", path_obj);
    }

  ret = json_object_to_file_ext (child_config_path, jobj, JSON_C_TO_STRING_PRETTY);
  json_object_put (jobj);

  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "write child config `%s`", child_config_path);

  return 0;
}

static int
write_split_status (const char *state_root, const char *child_id, const char *from_id, const char *overlay_rootfs,
                    libcrun_error_t *err)
{
  cleanup_free char *status_file = NULL;
  int ret;

  /* Build the path to the status file manually.  Status dir was already
     created during container creation.  */
  ret = libcrun_get_state_directory (&status_file, state_root, child_id, err);
  if (UNLIKELY (ret < 0))
    return ret;

  cleanup_free char *extra_file = NULL;
  ret = append_paths (&extra_file, err, status_file, "status.extra", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  json_object *jobj = json_object_new_object ();

  json_object_object_add (jobj, "split-from", json_object_new_string (from_id));
  json_object_object_add (jobj, "cow-rootfs", json_object_new_boolean (true));
  json_object_object_add (jobj, "overlay-rootfs", json_object_new_string (overlay_rootfs));

  const char *json_str = json_object_to_json_string_ext (jobj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

  int fd = open (extra_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (UNLIKELY (fd < 0))
    {
      json_object_put (jobj);
      return crun_make_error (err, errno, "open split status `%s`", extra_file);
    }

  size_t len = strlen (json_str);
  ssize_t written = write (fd, json_str, len);
  close (fd);
  json_object_put (jobj);

  if (UNLIKELY (written < 0 || (size_t) written != len))
    return crun_make_error (err, errno, "write split status `%s`", extra_file);

  return 0;
}

int
crun_command_split (struct crun_global_arguments *global_args, int argc, char **argv, libcrun_error_t *err)
{
  cleanup_container libcrun_container_t *container = NULL;
  cleanup_free char *parent_bundle = NULL;
  cleanup_free char *parent_config = NULL;
  cleanup_free char *child_state_dir = NULL;
  cleanup_free char *child_bundle = NULL;
  cleanup_free char *child_config = NULL;
  cleanup_free char *parent_rootfs = NULL;
  cleanup_free char *child_overlay_rootfs = NULL;
  cleanup_free char *upperdir = NULL;
  cleanup_free char *workdir = NULL;
  cleanup_free char *bundle_cleanup = NULL;
  const char *config_file = "config.json";
  int ret;
  int first_arg;

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, &crun_context);
  crun_assert_n_args (argc - first_arg, 1, 1);

  const char *child_id = argv[first_arg];

  if (from_id == NULL)
    libcrun_fail_with_error (0, "specify the parent container with --from");

  /* Resolve bundle to absolute path.  */
  if (bundle == NULL)
    {
      bundle_cleanup = getcwd (NULL, 0);
      if (UNLIKELY (bundle_cleanup == NULL))
        libcrun_fail_with_error (errno, "getcwd failed");
      bundle = bundle_cleanup;
    }
  else if (bundle[0] != '/')
    {
      bundle_cleanup = realpath (bundle, NULL);
      if (UNLIKELY (bundle_cleanup == NULL))
        libcrun_fail_with_error (errno, "realpath `%s` failed", bundle);
      bundle = bundle_cleanup;
    }

  /* Read parent container status to get its rootfs and bundle.  */
  libcrun_container_status_t parent_status = { 0 };
  ret = libcrun_read_container_status (&parent_status, global_args->root, from_id, err);
  if (UNLIKELY (ret < 0))
    return ret;

  parent_bundle = xstrdup (parent_status.bundle);
  parent_rootfs = xstrdup (parent_status.rootfs);
  libcrun_free_container_status (&parent_status);

  /* Build child bundle.  */
  ret = append_paths (&child_bundle, err, bundle, child_id, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (child_bundle, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_overlay_rootfs, err, child_bundle, "rootfs", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  /* Child state dir holds overlay upper/work.  */
  ret = libcrun_get_state_directory (&child_state_dir, global_args->root, child_id, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (child_state_dir, 0700, false, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&upperdir, err, child_state_dir, "overlay-upper", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&workdir, err, child_state_dir, "overlay-work", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  /* Mount overlayfs so that the child container rootfs is a single
     unified directory.  The mount is done in the host namespace and
     will be picked up by the container's MS_BIND setup.  */
  ret = setup_overlayfs_rootfs (parent_rootfs, child_overlay_rootfs, upperdir, workdir, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&parent_config, err, parent_bundle, config_file, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_config, err, child_bundle, config_file, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = copy_config_with_new_rootfs (parent_config, child_config, child_overlay_rootfs, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  ret = init_libcrun_context (&crun_context, child_id, global_args, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  crun_context.bundle = child_bundle;

  container = libcrun_container_load_from_file (child_config, err);
  if (container == NULL)
    {
      ret = -1;
      goto fail_unmount;
    }

  ret = libcrun_container_create (&crun_context, container, 0, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  ret = write_split_status (global_args->root, child_id, from_id, child_overlay_rootfs, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  return 0;

fail_unmount:
  umount2 (child_overlay_rootfs, MNT_DETACH);
  return ret;
}
