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
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/mount.h>

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

typedef int32_t (*krun_create_ctx_fn) (void);
typedef int32_t (*krun_restore_fn) (int fd, uint32_t ctx_id);
typedef int32_t (*krun_start_fn) (uint32_t ctx_id);
typedef int32_t (*krun_add_virtiofs2_fn) (uint32_t ctx_id, const char *tag, const char *path, uint64_t shm_size);
typedef int32_t (*krun_set_exec_fn) (uint32_t ctx_id, const char *exec_path,
                                     const char *const argv[], const char *const envp[]);
typedef int32_t (*krun_set_workdir_fn) (uint32_t ctx_id, const char *workdir);

static int
mkdir_p_local (const char *path, mode_t mode)
{
  char *tmp = xstrdup (path);
  char *p;

  for (p = tmp + 1; *p; p++)
    if (*p == '/')
      {
        *p = '\0';
        if (mkdir (tmp, mode) < 0 && errno != EEXIST)
          {
            int saved = errno;
            free (tmp);
            errno = saved;
            return -1;
          }
        *p = '/';
      }

  if (mkdir (tmp, mode) < 0 && errno != EEXIST)
    {
      int saved = errno;
      free (tmp);
      errno = saved;
      return -1;
    }

  free (tmp);
  return 0;
}

static int
materialize_restore_bind_mounts (runtime_spec_schema_config_schema *def, const char *rootfs, libcrun_error_t *err)
{
  size_t i;

  if (def == NULL || def->mounts == NULL)
    return 0;

  for (i = 0; i < def->mounts_len; i++)
    {
      runtime_spec_schema_defs_mount *mnt = def->mounts[i];
      cleanup_free char *target = NULL;
      cleanup_free char *parent = NULL;
      char *slash;

      if (mnt == NULL || mnt->type == NULL || mnt->source == NULL || mnt->destination == NULL)
        continue;
      if (strcmp (mnt->type, "bind") != 0 || mnt->destination[0] != '/')
        continue;

      if (asprintf (&target, "%s%s", rootfs, mnt->destination) < 0)
        return crun_make_error (err, errno, "building restored bind target path");
      parent = xstrdup (target);
      slash = strrchr (parent, '/');
      if (slash != NULL && slash != parent)
        {
          *slash = '\0';
          if (mkdir_p_local (parent, 0755) < 0)
            return crun_make_error (err, errno, "creating restored bind parent `%s`", parent);
        }
      if (mkdir_p_local (target, 0755) < 0)
        return crun_make_error (err, errno, "creating restored bind target `%s`", target);

      if (mount (mnt->source, target, NULL, MS_BIND | MS_REC, NULL) < 0)
        {
          if (errno == EBUSY)
            continue;
          return crun_make_error (err, errno, "bind mounting `%s` to restored root `%s`", mnt->source, target);
        }
      fprintf (stderr, "materialized restored bind mount %s -> %s\n", mnt->source, target);
    }
  return 0;
}
typedef int32_t (*krun_set_log_level_fn) (uint32_t level);

static const char *
getenv_from_envp (char **envp, const char *name)
{
  size_t n = strlen (name);
  if (envp == NULL)
    return NULL;
  for (size_t i = 0; envp[i] != NULL; i++)
    if (strncmp (envp[i], name, n) == 0 && envp[i][n] == '=')
      return envp[i] + n + 1;
  return NULL;
}

static int
resolve_exec_path (char **out, const char *rootfs, const char *arg0, char **envp,
                   libcrun_error_t *err)
{
  if (arg0 == NULL || arg0[0] == '\0')
    return crun_make_error (err, 0, "container process has empty argv[0]");

  if (arg0[0] == '/')
    {
      *out = xstrdup (arg0);
      return 0;
    }

  const char *path_env = getenv_from_envp (envp, "PATH");
  if (path_env == NULL || path_env[0] == '\0')
    path_env = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

  cleanup_free char *paths = xstrdup (path_env);
  for (char *saveptr = NULL, *dir = strtok_r (paths, ":", &saveptr); dir != NULL;
       dir = strtok_r (NULL, ":", &saveptr))
    {
      cleanup_free char *guest_candidate = NULL;
      cleanup_free char *host_candidate = NULL;
      if (asprintf (&guest_candidate, "%s/%s", dir[0] ? dir : ".", arg0) < 0)
        OOM ();
      if (guest_candidate[0] == '/')
        {
          if (asprintf (&host_candidate, "%s%s", rootfs, guest_candidate) < 0)
            OOM ();
        }
      else
        {
          if (asprintf (&host_candidate, "%s/%s", rootfs, guest_candidate) < 0)
            OOM ();
        }
      if (access (host_candidate, X_OK) == 0)
        {
          *out = xstrdup (guest_candidate);
          return 0;
        }
    }

  /* Match crun's custom-handler fallback: if the executable cannot be
     resolved in the host rootfs, still pass argv[0] through to the guest. */
  *out = xstrdup (arg0);
  return 0;
}

static int
configure_krun_context_from_oci (void *handle, int32_t ctx_id, libcrun_container_t *container,
                                 const char *rootfs, libcrun_error_t *err)
{
  runtime_spec_schema_config_schema *def = container->container_def;
  krun_add_virtiofs2_fn krun_add_virtiofs2;
  krun_set_exec_fn krun_set_exec;
  krun_set_workdir_fn krun_set_workdir;
  int32_t ret;

  if (def == NULL || def->process == NULL || def->process->args == NULL || def->process->args_len == 0)
    return crun_make_error (err, 0, "container config missing process.args");

  krun_add_virtiofs2 = (krun_add_virtiofs2_fn) dlsym (handle, "krun_add_virtiofs2");
  krun_set_exec = (krun_set_exec_fn) dlsym (handle, "krun_set_exec");
  krun_set_workdir = (krun_set_workdir_fn) dlsym (handle, "krun_set_workdir");
  if (krun_add_virtiofs2 == NULL || krun_set_exec == NULL || krun_set_workdir == NULL)
    return crun_make_error (err, 0, "libkrun missing configure symbols: virtiofs=%s exec=%s workdir=%s",
                            krun_add_virtiofs2 ? "ok" : "MISSING",
                            krun_set_exec ? "ok" : "MISSING",
                            krun_set_workdir ? "ok" : "MISSING");

  ret = krun_add_virtiofs2 ((uint32_t) ctx_id, "/dev/root", rootfs, 512 * 1024 * 1024ULL);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, -ret, "could not add restored virtiofs root `%s`", rootfs);

  cleanup_free char *exec_path = NULL;
  ret = resolve_exec_path (&exec_path, rootfs, def->process->args[0], def->process->env, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = krun_set_exec ((uint32_t) ctx_id, exec_path,
                       (const char *const *) def->process->args,
                       (const char *const *) def->process->env);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, -ret, "could not set restored execution arguments");

  if (def->process->cwd != NULL)
    {
      ret = krun_set_workdir ((uint32_t) ctx_id, def->process->cwd);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, -ret, "could not set restored working directory");
    }

  fprintf (stderr, "configured restored krun ctx_id=%d rootfs=%s exec=%s\n", ctx_id, rootfs, exec_path);
  return 0;
}

static int
load_restore_container (const char *state_root, const char *id,
                        libcrun_container_t **container_out, char **rootfs_out,
                        libcrun_error_t *err)
{
  cleanup_container_status libcrun_container_status_t status = { 0 };
  cleanup_free char *config_path = NULL;
  cleanup_free char *rootfs_path = NULL;
  int ret;

  ret = libcrun_read_container_status (&status, state_root, id, err);
  if (UNLIKELY (ret < 0))
    return ret;
  if (status.bundle == NULL)
    return crun_make_error (err, 0, "container status for `%s` has no bundle", id);

  ret = append_paths (&config_path, err, status.bundle, "config.json", NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  libcrun_container_t *container = libcrun_container_load_from_file (config_path, err);
  if (UNLIKELY (container == NULL))
    return -1;

  const char *root = status.rootfs;
  if (root == NULL || root[0] == '\0')
    root = container->container_def->root != NULL ? container->container_def->root->path : "rootfs";
  if (root == NULL || root[0] == '\0')
    root = "rootfs";

  if (root[0] == '/')
    rootfs_path = xstrdup (root);
  else
    {
      ret = append_paths (&rootfs_path, err, status.bundle, root, NULL);
      if (UNLIKELY (ret < 0))
        {
          libcrun_container_free (container);
          return ret;
        }
    }

  *container_out = container;
  *rootfs_out = rootfs_path;
  rootfs_path = NULL;
  return 0;
}

static int
dlopen_krun_restore_and_start (int fd, int notify_fd, const char *state_root,
                               const char *container_id, libcrun_error_t *err)
{
  void *handle;
  krun_create_ctx_fn krun_create_ctx;
  krun_restore_fn krun_restore;
  krun_start_fn krun_start_enter;
  krun_set_log_level_fn krun_set_log_level;
  int32_t ctx_id, ret;
  cleanup_free char *rootfs = NULL;
  libcrun_container_t *container = NULL;

  handle = dlopen ("libkrun.so", RTLD_LAZY);
  if (handle == NULL)
    handle = dlopen ("libkrun.so.1", RTLD_LAZY);
  if (handle == NULL)
    return crun_make_error (err, 0, "dlopen libkrun.so/libkrun.so.1: %s", dlerror ());

  krun_create_ctx = (krun_create_ctx_fn) dlsym (handle, "krun_create_ctx");
  krun_restore = (krun_restore_fn) dlsym (handle, "krun_restore_vm");
  krun_start_enter = (krun_start_fn) dlsym (handle, "krun_start_enter");
  krun_set_log_level = (krun_set_log_level_fn) dlsym (handle, "krun_set_log_level");
  if (krun_create_ctx == NULL || krun_restore == NULL || krun_start_enter == NULL)
    {
      dlclose (handle);
      return crun_make_error (err, 0,
                              "libkrun missing symbols: create=%s restore=%s start=%s",
                              krun_create_ctx ? "ok" : "MISSING",
                              krun_restore ? "ok" : "MISSING",
                              krun_start_enter ? "ok" : "MISSING");
    }

  if (krun_set_log_level != NULL)
    krun_set_log_level (2);

  ret = load_restore_container (state_root, container_id, &container, &rootfs, err);
  if (UNLIKELY (ret < 0))
    {
      dlclose (handle);
      return ret;
    }

  ctx_id = krun_create_ctx ();
  if (ctx_id < 0)
    {
      libcrun_container_free (container);
      dlclose (handle);
      return crun_make_error (err, -ctx_id, "krun_create_ctx failed");
    }

  ret = materialize_restore_bind_mounts (container->container_def, rootfs, err);
  if (UNLIKELY (ret < 0))
    {
      libcrun_container_free (container);
      dlclose (handle);
      return ret;
    }

  ret = krun_restore (fd, (uint32_t) ctx_id);
  if (ret < 0)
    {
      libcrun_container_free (container);
      dlclose (handle);
      return crun_make_error (err, -ret, "krun_restore_vm(fd=%d ctx=%d) failed", fd, ctx_id);
    }

  /* Re-apply the target OCI/rootfs configuration after restoring the VMM
     state.  The snapshot contains guest/device state from the source node;
     configuring virtiofs before krun_restore_vm can leave the restored guest
     polling the source control mount.  Applying it after restore gives the
     target node's bundle/control paths to the restored context before start. */
  ret = configure_krun_context_from_oci (handle, ctx_id, container, rootfs, err);
  libcrun_container_free (container);
  if (UNLIKELY (ret < 0))
    {
      dlclose (handle);
      return ret;
    }

  fprintf (stderr, "restored snapshot into reconfigured ctx_id=%d via krun_restore_vm, now entering VM...\n", ctx_id);
  if (notify_fd >= 0)
    {
      char msg[64];
      int len = snprintf (msg, sizeof (msg), "OK %d\n", ctx_id);
      (void) write (notify_fd, msg, len);
      close (notify_fd);
    }

  ret = krun_start_enter ((uint32_t) ctx_id);
  // krun_start_enter does not return on success
  dlclose (handle);
  return crun_make_error (err, -ret, "krun_start_enter failed");
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

  int notify_pipe[2];
  if (UNLIKELY (pipe2 (notify_pipe, O_CLOEXEC) < 0))
    {
      close (fd);
      return crun_make_error (err, errno, "pipe restore-snap notify");
    }

  pid_t child = fork ();
  if (UNLIKELY (child < 0))
    {
      close (fd);
      close (notify_pipe[0]);
      close (notify_pipe[1]);
      return crun_make_error (err, errno, "fork restore-snap VM process");
    }

  if (child == 0)
    {
      close (notify_pipe[0]);
      ret = dlopen_krun_restore_and_start (fd, notify_pipe[1], crun_context.state_root,
                                           argv[first_arg], err);
      close (fd);
      close (notify_pipe[1]);
      _exit (ret == 0 ? 0 : 125);
    }

  close (fd);
  close (notify_pipe[1]);

  char notify_buf[64] = { 0 };
  fd_set rfds;
  FD_ZERO (&rfds);
  FD_SET (notify_pipe[0], &rfds);
  struct timeval timeout = { .tv_sec = 60, .tv_usec = 0 };
  ret = select (notify_pipe[0] + 1, &rfds, NULL, NULL, &timeout);
  if (UNLIKELY (ret < 0))
    {
      int saved_errno = errno;
      close (notify_pipe[0]);
      return crun_make_error (err, saved_errno, "select restore-snap notify");
    }
  if (UNLIKELY (ret == 0))
    {
      close (notify_pipe[0]);
      return crun_make_error (err, 0, "timed out waiting for krun_restore_vm completion");
    }
  ssize_t n = read (notify_pipe[0], notify_buf, sizeof (notify_buf) - 1);
  close (notify_pipe[0]);
  if (UNLIKELY (n <= 0))
    {
      int status;
      if (waitpid (child, &status, WNOHANG) == child)
        return crun_make_error (err, 0, "restore-snap child exited before restore completion");
      return crun_make_error (err, 0, "restore-snap child closed notify pipe before restore completion");
    }
  notify_buf[n] = '\0';
  if (UNLIKELY (strncmp (notify_buf, "OK ", 3) != 0))
    return crun_make_error (err, 0, "unexpected restore-snap notify: %s", notify_buf);

  /* Give krun_start_enter a small chance to fail after the restore ACK.  If it
     exits immediately, report failure instead of writing a false running PID. */
  usleep (250000);
  int status;
  if (waitpid (child, &status, WNOHANG) == child)
    return crun_make_error (err, 0, "restore-snap VM exited immediately after start (status=%d)", status);

  /* crun create leaves exec.fifo behind while the placeholder init waits in
     "created" state.  A restored libkrun VM is already running in `child`, so
     remove/write the fifo before rewriting status; otherwise `crun state` keeps
     reporting "created" even with a live VM pid. */
  {
    cleanup_free char *exec_fifo_path = NULL;
    if (asprintf (&exec_fifo_path, "%s/%s/exec.fifo", crun_context.state_root, argv[first_arg]) >= 0)
      {
        int efd = open (exec_fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        (void) unlink (exec_fifo_path);
        if (efd >= 0)
          {
            char b = 0;
            (void) write (efd, &b, 1);
            close (efd);
          }
      }
  }

  cleanup_container_status libcrun_container_status_t container_status = { 0 };
  ret = libcrun_read_container_status (&container_status, crun_context.state_root, argv[first_arg], err);
  if (UNLIKELY (ret < 0))
    return ret;
  container_status.pid = child;
  container_status.detached = true;
  ret = libcrun_write_container_status (crun_context.state_root, argv[first_arg], &container_status, err);
  if (UNLIKELY (ret < 0))
    return ret;

  fprintf (stderr, "restore-snap launched vm pid=%d\n", child);

  return 0;
}
