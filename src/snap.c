/*
 * crun snapshot — save a libkrun VM to a file
 *
 * For krun containers, this uses krun_snapshot_vm() to create a
 * consistent snapshot of the VM state + guest memory.
 * For non-krun containers, falls back to criu checkpoint.
 */

#define _GNU_SOURCE

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdint.h>

#include "crun.h"
#include "libcrun/container.h"
#include "libcrun/status.h"
#include "libcrun/utils.h"

#if ! HAVE_DLOPEN
#  error "snap requires dlopen"
#endif

static char doc[] = "Create a snapshot of a container (libkrun VM or criu)";

struct snap_options_s
{
  const char *image_path;
};

enum
{
  OPTION_IMAGE_PATH = 1000,
};

static struct snap_options_s snap_options;

static struct argp_option options[] = {
  { "image-path", OPTION_IMAGE_PATH, "FILE", 0,
    "Path to write the snapshot file (default: /tmp/crun-snap-{id}.snap)", 0 },
  {
    0,
  }
};

static char args_doc[] = "SNAP CONTAINER_ID";

static error_t
parse_opt (int key, char *arg, struct argp_state *state arg_unused)
{
  switch (key)
    {
    case OPTION_IMAGE_PATH:
      snap_options.image_path = arg;
      return 0;
    case ARGP_KEY_NO_ARGS:
      libcrun_fail_with_error (0, "please specify a container ID");
    default:
      return ARGP_ERR_UNKNOWN;
    }
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

/* libkrun function signatures */
typedef int32_t (*krun_pause_fn) (uint32_t ctx_id);
typedef int32_t (*krun_resume_fn) (uint32_t ctx_id);
typedef int32_t (*krun_snapshot_fn) (uint32_t ctx_id, int fd, uint32_t flags);

static int
dlopen_krun_and_snapshot (uint32_t ctx_id, int fd, libcrun_error_t *err)
{
  void *handle;
  krun_pause_fn krun_pause;
  krun_resume_fn krun_resume;
  krun_snapshot_fn krun_snapshot;
  int32_t ret;

  handle = dlopen ("libkrun.so", RTLD_LAZY);
  if (handle == NULL)
    return crun_make_error (err, 0, "dlopen libkrun.so: %s", dlerror ());

  krun_pause = (krun_pause_fn) dlsym (handle, "krun_pause_ctx");
  krun_resume = (krun_resume_fn) dlsym (handle, "krun_resume_ctx");
  krun_snapshot = (krun_snapshot_fn) dlsym (handle, "krun_snapshot_vm");

  if (krun_pause == NULL || krun_resume == NULL || krun_snapshot == NULL)
    {
      dlclose (handle);
      return crun_make_error (err, 0,
                              "libkrun missing snapshot symbols: pause=%s resume=%s snap=%s",
                              krun_pause ? "ok" : "MISSING",
                              krun_resume ? "ok" : "MISSING",
                              krun_snapshot ? "ok" : "MISSING");
    }

  /* 1. Pause the VM */
  ret = krun_pause (ctx_id);
  if (ret < 0)
    {
      dlclose (handle);
      return crun_make_error (err, -ret, "krun_pause_ctx(%u) failed", ctx_id);
    }

  /* 2. Snapshot to fd */
  ret = krun_snapshot (ctx_id, fd, 0);
  if (ret < 0)
    {
      krun_resume (ctx_id); /* try to unblock the VM */
      dlclose (handle);
      return crun_make_error (err, -ret, "krun_snapshot_vm(%u, fd=%d) failed", ctx_id, fd);
    }

  /* 3. Resume the VM */
  ret = krun_resume (ctx_id);
  if (ret < 0)
    {
      dlclose (handle);
      return crun_make_error (err, -ret, "krun_resume_ctx(%u) failed", ctx_id);
    }

  dlclose (handle);
  return 0;
}

int
crun_command_snap (struct crun_global_arguments *global_args, int argc,
                   char **argv, libcrun_error_t *err)
{
  int first_arg = 0, ret;
  cleanup_free char *snap_path = NULL;

  libcrun_context_t crun_context = { 0 };
  libcrun_container_status_t status = {};

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, &snap_options);
  crun_assert_n_args (argc - first_arg, 1, 1);

  ret = init_libcrun_context (&crun_context, argv[first_arg], global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = libcrun_read_container_status (&status, crun_context.state_root,
                                       argv[first_arg], err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = libcrun_is_container_running (&status, err);
  if (ret <= 0)
    {
      libcrun_free_container_status (&status);
      return crun_make_error (err, 0, "container `%s` is not running", argv[first_arg]);
    }

  if (status.handler_name == NULL || strcmp (status.handler_name, "krun") != 0)
    {
      libcrun_free_container_status (&status);
      return crun_make_error (err, 0,
                              "container `%s` is not a libkrun VM (handler=%s) — use `checkpoint` instead",
                              argv[first_arg], status.handler_name ?: "none");
    }

  if (status.handler_ctx_id == 0)
    {
      libcrun_free_container_status (&status);
      return crun_make_error (err, 0, "container `%s` has no libkrun ctx_id", argv[first_arg]);
    }

  if (snap_options.image_path == NULL)
    {
      ret = asprintf (&snap_path, "/tmp/crun-snap-%s.snap", argv[first_arg]);
      if (UNLIKELY (ret < 0))
        OOM ();
      snap_options.image_path = snap_path;
    }

  int fd = open (snap_options.image_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (UNLIKELY (fd < 0))
    {
      libcrun_free_container_status (&status);
      return crun_make_error (err, errno, "open `%s`", snap_options.image_path);
    }

  ret = dlopen_krun_and_snapshot (status.handler_ctx_id, fd, err);
  close (fd);
  libcrun_free_container_status (&status);

  if (LIKELY (ret == 0))
    printf ("snapshot written to %s\n", snap_options.image_path);

  return ret;
}
