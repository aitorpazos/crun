/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019 Giuseppe Scrivano <giuseppe@scrivano.org>
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

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "crun.h"
#include "libcrun/container.h"
#include "libcrun/utils.h"
#include "libcrun/status.h"

static char doc[] = "OCI runtime";

enum
{
  OPTION_CONSOLE_SOCKET = 1000,
  OPTION_PID_FILE,
  OPTION_NO_SUBREAPER,
  OPTION_NO_NEW_KEYRING,
  OPTION_PRESERVE_FDS
};

struct list_options_s
{
  bool quiet;
  int format;
};

enum
{
  LIST_TABLE = 100,
  LIST_JSON,
  LIST_TREE,
};

static struct list_options_s list_options;

static struct argp_option options[]
    = { { "quiet", 'q', 0, 0, "show only IDs", 0 },
        { "format", 'f', "FORMAT", 0, "select one of: table or json (default: \"table\")", 0 },
        { "tree", 't', 0, 0, "show container tree with parent/child relationships", 0 },
        {
            0,
        } };

static char args_doc[] = "list";

static error_t
parse_opt (int key, char *arg, struct argp_state *state arg_unused)
{
  switch (key)
    {
    case 'q':
      list_options.quiet = true;
      break;
    case 'f':
      if (strcmp (arg, "table") == 0)
        list_options.format = LIST_TABLE;
      else if (strcmp (arg, "json") == 0)
        list_options.format = LIST_JSON;
      else
        error (EXIT_FAILURE, 0, "invalid format `%s`", arg);
      break;

    case 't':
      list_options.format = LIST_TREE;
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

int
crun_command_list (struct crun_global_arguments *global_args, int argc, char **argv, libcrun_error_t *err)
{
  int first_arg;
  int ret, max_length = 4;
  libcrun_context_t crun_context = {
    0,
  };
  libcrun_container_list_t *list = NULL, *it;

  list_options.format = LIST_TABLE;

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, &list_options);
  crun_assert_n_args (argc - first_arg, 0, 0);

  ret = init_libcrun_context (&crun_context, argv[first_arg], global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (list_options.format == LIST_JSON)
    return libcrun_write_json_containers_list (&crun_context, stdout, err);

  ret = libcrun_get_containers_list (&list, crun_context.state_root, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (list_options.format == LIST_TREE)
    {
      /* Build tree from parent links in status files */
      typedef struct tree_node_s {
        const char *name;
        const char *parent;
        struct tree_node_s *next;
      } tree_node_t;
      tree_node_t *tree_nodes = NULL;
      int tree_count = 0;
      for (it = list; it; it = it->next)
        tree_count++;
      if (tree_count > 0)
        {
          tree_nodes = calloc (tree_count, sizeof (tree_node_t));
          int idx = 0;
          for (it = list; it; it = it->next)
            {
              tree_nodes[idx].name = it->name;
              idx++;
            }
          /* Second pass: read parent from status */
          idx = 0;
          for (it = list; it; it = it->next)
            {
              libcrun_container_status_t status;
              if (libcrun_read_container_status (&status, crun_context.state_root, it->name, err) >= 0)
                {
                  tree_nodes[idx].parent = status.parent ? strdup (status.parent) : NULL;
                  libcrun_free_container_status (&status);
                }
              idx++;
            }
          /* Print tree */
          for (idx = 0; idx < tree_count; idx++)
            {
              if (tree_nodes[idx].parent == NULL)
                {
                  printf ("%s\n", tree_nodes[idx].name);
                  /* Print children */
                  for (int j = 0; j < tree_count; j++)
                    if (tree_nodes[j].parent && strcmp (tree_nodes[j].parent, tree_nodes[idx].name) == 0)
                      printf ("  +- %s\n", tree_nodes[j].name);
                }
            }
          for (idx = 0; idx < tree_count; idx++)
            free ((char *) tree_nodes[idx].parent);
          free (tree_nodes);
        }
      libcrun_free_containers_list (list);
      return 0;
    }

  for (it = list; it; it = it->next)
    {
      int l = strlen (it->name);
      if (l > max_length)
        max_length = l;
    }

  max_length++;

  if (! list_options.quiet)
    printf ("%-*s%-10s%-8s %-39s %-30s %s\n", max_length, "NAME", "PID", "STATUS", "BUNDLE PATH", "CREATED", "OWNER");

  for (it = list; it; it = it->next)
    {
      libcrun_container_status_t status;

      ret = libcrun_read_container_status (&status, crun_context.state_root, it->name, err);
      if (UNLIKELY (ret < 0))
        {
          libcrun_error_write_warning_and_release (stderr, &err);
          continue;
        }
      if (list_options.quiet)
        printf ("%s\n", it->name);
      else
        {
          int running = 0;
          int pid = status.pid;
          const char *container_status = NULL;

          ret = libcrun_get_container_state_string (it->name, &status, crun_context.state_root, &container_status,
                                                    &running, err);
          if (UNLIKELY (ret < 0))
            {
              libcrun_error_write_warning_and_release (stderr, &err);
              continue;
            }

          if (! running)
            pid = 0;

          printf ("%-*s%-10d%-8s %-39s %-30s %s\n", max_length, it->name, pid, container_status, status.bundle, status.created, status.owner);
        }

      libcrun_free_container_status (&status);
    }

  libcrun_free_containers_list (list);
  return ret >= 0 ? 0 : ret;
}
