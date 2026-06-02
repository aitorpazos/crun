/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2026 Telnyx, Inc.
 */

#ifndef RESTORE_SNAP_H
#define RESTORE_SNAP_H

#include "crun.h"

int crun_command_restore_snap (struct crun_global_arguments *global_args, int argc, char **argv, libcrun_error_t *error);

#endif
