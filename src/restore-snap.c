/*
 * crun restore-snap — restore a libkrun VM from snapshot
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
#  error "restore-snap requires dlopen"
#endif

static char doc[] = "Restore a container from a libkrun snapshot";

struct restore_snap_options_s
{
  const char *image_path;
};

enum
{
  OPTION_IMAGE_PATH = 1000,
};

static struct restore_snap_options_s restore_snap_options;

static struct argp_option options[] = {
  { "image-path", OPTION_IMAGE_PATH, "FILE", 0,
    "Path to read the snapshot file from", 0 },
  {
    0,
  }
};

static char args_doc[] = "RESTORE-SNAP CONTAINER_ID";

static error_t
parse_opt (int key, char *arg, struct argp_state *state arg_unused)
{
  switch (key)
    {
    case OPTION_IMAGE_PATH:
      restore_snap_options.image_path = arg;
      return 0;
    case ARGP_KEY_NO_ARGS:
      libcrun_fail_with_error (0, "please specify a container ID");
    default:
      return ARGP_ERR_UNKNOWN;
    }
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

typedef int32_t (*krun_restore_fn) (int fd, uint32_t flags);

static int
dlopen_krun_and_restore (int fd, libcrun_error_t *err)
{
  void *handle;
  krun_restore_fn krun_restore;
  int32_t ret;

  handle = dlopen ("libkrun.so", RTLD_LAZY);
  if (handle == NULL)
    return crun_make_error (err, 0, "dlopen libkrun.so: %s", dlerror ());

  krun_restore = (krun_restore_fn) dlsym (handle, "krun_restore_vm");
  if (krun_restore == NULL)
    {
      dlclose (handle);
      return crun_make_error (err, 0, "libkrun missing krun_restore_vm symbol");
    }

  ret = krun_restore (fd, 0);
  if (ret < 0)
    {
      dlclose (handle);
      return crun_make_error (err, -ret, "krun_restore_vm(fd=%d) failed", fd);
    }

  dlclose (handle);
  return ret;
}

int
crun_command_restore_snap (struct crun_global_arguments *global_args, int argc,
                           char **argv, libcrun_error_t *err)
{
  int first_arg = 0, ret;

  libcrun_context_t crun_context = { 0 };

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, &restore_snap_options);
  crun_assert_n_args (argc - first_arg, 1, 1);

  ret = init_libcrun_context (&crun_context, argv[first_arg], global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  cleanup_free char *def_path = NULL;
  if (restore_snap_options.image_path == NULL)
    {
      ret = asprintf (&def_path, "/tmp/crun-snap-%s.snap", argv[first_arg]);
      if (UNLIKELY (ret < 0))
        OOM ();
      restore_snap_options.image_path = def_path;
    }

  int fd = open (restore_snap_options.image_path, O_RDONLY);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s` for restore",
                            restore_snap_options.image_path);

  ret = dlopen_krun_and_restore (fd, err);
  close (fd);

  if (LIKELY (ret == 0))
    printf ("Restored from %s (new ctx_id=%d)\n",
            restore_snap_options.image_path, (int) ret);

  return ret;
}
