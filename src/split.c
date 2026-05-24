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
#include <sys/wait.h>
#include <json-c/json.h>

#include "crun.h"
#include "libcrun/container.h"
#include "libcrun/status.h"
#include "libcrun/utils.h"

enum
{
  OPTION_FROM = 1000,
  OPTION_SHARE_NETWORK,
  OPTION_SHARE_IPC,
  OPTION_SHARE_UTS,
  OPTION_SHARE_PID,
  OPTION_SHARE_USER,
  OPTION_SHARE_CGROUP,
  OPTION_CRIU,
};

static const char *bundle = NULL;
static const char *from_id = NULL;
static bool share_network = false;
static bool share_ipc = false;
static bool share_uts = false;
static bool share_pid = false;
static bool share_user = false;
static bool share_cgroup = false;
static bool use_criu = false;

static libcrun_context_t crun_context;

static struct argp_option options[]
    = { { "from", OPTION_FROM, "ID", 0, "parent container ID to split from", 0 },
        { "share-network", OPTION_SHARE_NETWORK, 0, 0, "share parent's network namespace (setns)", 0 },
        { "share-ipc", OPTION_SHARE_IPC, 0, 0, "share parent's IPC namespace (setns)", 0 },
        { "share-uts", OPTION_SHARE_UTS, 0, 0, "share parent's UTS namespace (setns)", 0 },
        { "share-pid", OPTION_SHARE_PID, 0, 0, "share parent's PID namespace (setns)", 0 },
        { "share-user", OPTION_SHARE_USER, 0, 0, "share parent's user namespace (setns)", 0 },
        { "share-cgroup", OPTION_SHARE_CGROUP, 0, 0, "share parent's cgroup namespace (setns)", 0 },
        { "criu", OPTION_CRIU, 0, 0, "use CRIU checkpoint/restore for COW memory", 0 },
        { "bundle", 'b', "DIR", 0, "container bundle (default \".\")", 0 },
        { "config", 'f', "FILE", 0, "override the config file name", 0 },
        {
            0,
        } };

static char doc[] = "OCI runtime";

static char args_doc[] = "CONTAINER";

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  (void) arg;

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

    case OPTION_SHARE_NETWORK:
      share_network = true;
      break;

    case OPTION_SHARE_IPC:
      share_ipc = true;
      break;

    case OPTION_SHARE_UTS:
      share_uts = true;
      break;

    case OPTION_SHARE_PID:
      share_pid = true;
      break;

    case OPTION_SHARE_USER:
      share_user = true;
      break;

    case OPTION_SHARE_CGROUP:
      share_cgroup = true;
      break;

    case OPTION_CRIU:
      use_criu = true;
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
set_namespace_path (json_object *linux_obj, const char *type, const char *path)
{
  json_object *ns_array;
  if (! json_object_object_get_ex (linux_obj, "namespaces", &ns_array))
    {
      ns_array = json_object_new_array ();
      json_object_object_add (linux_obj, "namespaces", ns_array);
    }

  size_t len = json_object_array_length (ns_array);
  size_t i;
  for (i = 0; i < len; i++)
    {
      json_object *ns = json_object_array_get_idx (ns_array, i);
      json_object *type_obj;
      if (json_object_object_get_ex (ns, "type", &type_obj))
        {
          if (strcmp (json_object_get_string (type_obj), type) == 0)
            {
              json_object_object_add (ns, "path", json_object_new_string (path));
              return 0;
            }
        }
    }

  json_object *ns = json_object_new_object ();
  json_object_object_add (ns, "type", json_object_new_string (type));
  json_object_object_add (ns, "path", json_object_new_string (path));
  json_object_array_add (ns_array, ns);
  return 0;
}

static int
copy_config_with_new_rootfs_and_namespaces (const char *parent_config_path, const char *child_config_path,
                                            const char *child_rootfs, pid_t parent_pid,
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

  if (parent_pid > 0)
    {
      json_object *linux_obj;
      if (json_object_object_get_ex (jobj, "linux", &linux_obj))
        {
          if (share_network)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/net", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "network", ns_path);
            }
          if (share_ipc)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/ipc", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "ipc", ns_path);
            }
          if (share_uts)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/uts", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "uts", ns_path);
            }
          if (share_pid)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/pid", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "pid", ns_path);
            }
          if (share_user)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/user", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "user", ns_path);
            }
          if (share_cgroup)
            {
              cleanup_free char *ns_path = NULL;
              ret = asprintf (&ns_path, "/proc/%d/ns/cgroup", parent_pid);
              if (ret >= 0)
                set_namespace_path (linux_obj, "cgroup", ns_path);
            }
        }
    }

  ret = json_object_to_file_ext (child_config_path, jobj, JSON_C_TO_STRING_PRETTY);
  json_object_put (jobj);

  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "write child config `%s`", child_config_path);

  return 0;
}

static int
write_split_status (const char *state_root, const char *child_id, const char *from_id, const char *overlay_rootfs,
                    bool criu_used, libcrun_error_t *err)
{
  cleanup_free char *state_dir = NULL;
  int ret;

  ret = libcrun_get_state_directory (&state_dir, state_root, child_id, err);
  if (UNLIKELY (ret < 0))
    return ret;

  cleanup_free char *extra_file = NULL;
  ret = append_paths (&extra_file, err, state_dir, "status.extra", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  json_object *jobj = json_object_new_object ();

  json_object_object_add (jobj, "split-from", json_object_new_string (from_id));
  json_object_object_add (jobj, "cow-rootfs", json_object_new_boolean (overlay_rootfs != NULL));
  if (overlay_rootfs)
    json_object_object_add (jobj, "overlay-rootfs", json_object_new_string (overlay_rootfs));
  json_object_object_add (jobj, "criu", json_object_new_boolean (criu_used));

  if (share_network)
    json_object_object_add (jobj, "share-network", json_object_new_boolean (true));
  if (share_ipc)
    json_object_object_add (jobj, "share-ipc", json_object_new_boolean (true));
  if (share_uts)
    json_object_object_add (jobj, "share-uts", json_object_new_boolean (true));
  if (share_pid)
    json_object_object_add (jobj, "share-pid", json_object_new_boolean (true));
  if (share_user)
    json_object_object_add (jobj, "share-user", json_object_new_boolean (true));
  if (share_cgroup)
    json_object_object_add (jobj, "share-cgroup", json_object_new_boolean (true));

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

#if HAVE_CRIU
static int
split_container_via_criu (struct crun_global_arguments *global_args,
                            const char *from_id, const char *child_id,
                            const char *parent_bundle, const char *parent_rootfs,
                            const char *child_bundle, const char *child_config,
                            const char *child_state_dir,
                            libcrun_error_t *err)
{
  cleanup_free char *checkpoint_dir = NULL;
  cleanup_free char *child_rootfs = NULL;
  cleanup_free char *parent_config = NULL;
  int ret;

  ret = append_paths (&checkpoint_dir, err, child_bundle, ".split-criu", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (checkpoint_dir, 0700, false, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_rootfs, err, child_bundle, "rootfs", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (child_rootfs, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  /* Copy parent's rootfs into child bundle.  CRIU restore needs matching
     rootfs content but can run from a different path (criu_set_root).  */
  pid_t cp_pid = fork ();
  if (cp_pid == 0)
    {
      execlp ("cp", "cp", "-aT", parent_rootfs, child_rootfs, NULL);
      _exit (127);
    }
  if (cp_pid < 0)
    return crun_make_error (err, errno, "fork for cp");

  int wstatus;
  if (waitpid (cp_pid, &wstatus, 0) < 0)
    return crun_make_error (err, errno, "waitpid for cp");

  if (! WIFEXITED (wstatus) || WEXITSTATUS (wstatus) != 0)
    return crun_make_error (err, 0, "cp -aT `%s` -> `%s` failed", parent_rootfs, child_rootfs);

  ret = append_paths (&parent_config, err, parent_bundle, "config.json", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = copy_config_with_new_rootfs_and_namespaces (parent_config, child_config, child_rootfs, 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  /* Checkpoint parent (leave it running).  */
  libcrun_checkpoint_restore_t cr_chkpt = { 0 };
  cr_chkpt.image_path = checkpoint_dir;
  cr_chkpt.leave_running = true;
  cr_chkpt.manage_cgroups_mode = -1;

  libcrun_context_t parent_ctx = { 0 };
  ret = init_libcrun_context (&parent_ctx, from_id, global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  parent_ctx.bundle = parent_bundle;
  ret = libcrun_container_checkpoint (&parent_ctx, from_id, &cr_chkpt, err);
  if (UNLIKELY (ret < 0))
    return ret;

  /* Restore into child container.  */
  libcrun_context_t child_ctx = { 0 };
  ret = init_libcrun_context (&child_ctx, child_id, global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  child_ctx.bundle = child_bundle;

  if (chdir (child_bundle) < 0)
    return crun_make_error (err, errno, "chdir to child bundle `%s`", child_bundle);

  libcrun_checkpoint_restore_t cr_restore = { 0 };
  cr_restore.image_path = checkpoint_dir;
  cr_restore.manage_cgroups_mode = -1;
  cr_restore.detach = true;

  ret = libcrun_container_restore (&child_ctx, child_id, &cr_restore, err);
  if (UNLIKELY (ret < 0))
    {
      /* Do not delete checkpoint dir on failure; keep for debugging.  */
      return ret;
    }

  return 0;
}
#else
static int
split_container_via_criu (struct crun_global_arguments *global_args arg_unused, const char *from_id arg_unused,
                            const char *child_id arg_unused, const char *parent_bundle arg_unused,
                            const char *parent_rootfs arg_unused, const char *child_bundle arg_unused,
                            const char *child_config arg_unused, const char *child_state_dir arg_unused,
                            libcrun_error_t *err)
{
  return crun_make_error (err, 0, "crun was compiled without CRIU support");
}
#endif

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

  /* Reset per-invocation static state for reuse by fork(2) caller.  */
  bundle = NULL;

  argp_parse (&run_argp, argc, argv, 0, &first_arg, &crun_context);
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
  pid_t parent_pid = parent_status.pid;
  libcrun_free_container_status (&parent_status);

  /* Build child bundle and state dir (used for both overlay and criu paths).  */
  ret = append_paths (&child_bundle, err, bundle, child_id, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (child_bundle, 0755, true, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_overlay_rootfs, err, child_bundle, "rootfs", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_state_dir, err, global_args->root, child_id, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  if (use_criu)
    {
      ret = append_paths (&child_config, err, child_bundle, config_file, NULL);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = split_container_via_criu (global_args, from_id, child_id,
                                      parent_bundle, parent_rootfs,
                                      child_bundle, child_config,
                                      child_state_dir, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = write_split_status (global_args->root, child_id, from_id, NULL, true, err);
      if (UNLIKELY (ret < 0))
        return ret;

      return 0;
    }

  /* Non-CRIU path: overlayfs for COW storage.  */
  ret = append_paths (&upperdir, err, child_bundle, ".split-overlay-upper", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&workdir, err, child_bundle, ".split-overlay-work", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = setup_overlayfs_rootfs (parent_rootfs, child_overlay_rootfs, upperdir, workdir, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&parent_config, err, parent_bundle, config_file, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = append_paths (&child_config, err, child_bundle, config_file, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = copy_config_with_new_rootfs_and_namespaces (parent_config, child_config, child_overlay_rootfs, parent_pid, err);
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

  ret = libcrun_container_start (&crun_context, child_id, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  ret = write_split_status (global_args->root, child_id, from_id, child_overlay_rootfs, false, err);
  if (UNLIKELY (ret < 0))
    goto fail_unmount;

  return 0;

fail_unmount:
  umount2 (child_overlay_rootfs, MNT_DETACH);
  return ret;
}
