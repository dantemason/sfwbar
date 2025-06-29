/* This entire file is licensed under GNU General Public License v3.0
 *
 * Copyright 2024- sfwbar maintainers
 */

#include <gdk/gdkwayland.h>
#include "workspace.h"
#include "wayland.h"
#include "ext-workspace-v1.h"
#include "gui/monitor.h"

#define EXT_WORKSPACE_VERSION 1

static struct ext_workspace_manager_v1 *workspace_manager;
static GList *workspace_groups, *workspaces_to_focus;

typedef struct _ew_group {
  GPtrArray *outputs;
  GPtrArray *workspaces;
} ew_group_t;

/* API */
static void ew_set_workspace(workspace_t *ws)
{
  if(!workspace_manager)
    return;

  if(!ws->id || ws->id == PAGER_PIN_ID)
  {
    if(!ws->name)
      g_warning("Workspace: ext-workspace: unnamed pin datected");
    else if(!workspace_groups)
      g_warning("Workspace: ext-workspace: create is not supported by compositor");
    else
    {
      workspaces_to_focus = g_list_prepend(workspaces_to_focus,
          g_strdup(ws->name));
      ext_workspace_group_handle_v1_create_workspace(
          workspace_groups->data, ws->name);
      ext_workspace_manager_v1_commit(workspace_manager);
    }
  }
  else if(ws->state & WS_CAP_ACTIVATE)
  {
    g_debug("Workspace: ext-workspace: activating workspace %s", ws->name);
    ext_workspace_handle_v1_activate(ws->id);
    ext_workspace_manager_v1_commit(workspace_manager);
  }
  else
    g_warning("Workspace: ext-workspace: activation not supported by compositor");
}

static gboolean ew_get_can_create ( void )
{
  return !!workspace_groups;
}

static gboolean ew_check_monitor ( void *wsid, gchar *name )
{
  workspace_t *ws;
  GdkMonitor *monitor;
  ew_group_t *group;

  if( !(monitor = monitor_from_name(name)) )
    return TRUE;

  if( !(ws = workspace_from_id(wsid)) )
    return FALSE;

  if( !(group = ws->data) )
    return TRUE;  // in case the compositor doesn't support groups

  return g_ptr_array_find(group->outputs,
      gdk_wayland_monitor_get_wl_output(monitor), NULL);
}

static struct workspace_api ew_api_impl = {
  .set_workspace = ew_set_workspace,
  .get_geom = NULL,
  .get_can_create = ew_get_can_create,
  .check_monitor = ew_check_monitor,
};

/* Workspace */
static void ew_workspace_handle_name(void *data,
    struct ext_workspace_handle_v1 *workspace, const char *name)
{
  workspace_t *ws = data;
  GList *item;

  workspace_set_name(ws, name);

  if( !(item=g_list_find_custom(workspaces_to_focus, name,
          (GCompareFunc)g_strcmp0)) )
    return;

  g_free(item->data);
  workspaces_to_focus = g_list_remove(workspaces_to_focus, item);
  workspace_activate(ws);
}

static void ew_workspace_handle_coordinates(void *data,
    struct ext_workspace_handle_v1 *workspace,
    struct wl_array *coordinates)
{
}

static void ew_workspace_handle_state(void *data,
    struct ext_workspace_handle_v1 *workspace, uint32_t state)
{
  workspace_mod_state(workspace, WS_STATE_FOCUSED,
      !!(state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE));
  workspace_mod_state(workspace, WS_STATE_URGENT,
      !!(state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT));
  workspace_mod_state(workspace, WS_STATE_VISIBLE,
      !(state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN));
}

static void ew_workspace_handle_capabilities(void *data,
    struct ext_workspace_handle_v1 *workspace,
    uint32_t capabilities)
{
  uint32_t wscaps = 0;

  if(capabilities &
      EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE)
    wscaps |= WS_CAP_ACTIVATE;

  workspace_set_caps(data, wscaps);
}

static void ew_workspace_handle_remove(void *data,
		struct ext_workspace_handle_v1 *workspace)
{
  workspace_t *ws = data;

  ext_workspace_handle_v1_destroy(workspace);

  if(!ws)
    return;

  g_debug("Workspace: ext-workspace: workspace '%s' destroyed", ws->name);
  workspace_unref(ws->id);
}

static void ew_workspace_handle_id ( void *data,
    struct ext_workspace_handle_v1 *workspace,
    const char *id )
{
}

static const struct ext_workspace_handle_v1_listener
    ew_workspace_impl =
{
  .id = ew_workspace_handle_id,
  .name = ew_workspace_handle_name,
  .coordinates = ew_workspace_handle_coordinates,
  .state = ew_workspace_handle_state,
  .capabilities = ew_workspace_handle_capabilities,
  .removed = ew_workspace_handle_remove,
};

/* Group */
static void ew_workspace_group_handle_capabilities(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group,
    uint32_t capabilities)
{
  if(capabilities &
      EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE)
    if(!g_list_find(workspace_groups, workspace_group))
      workspace_groups = g_list_prepend(workspace_groups, workspace_group);
}

static void ew_workspace_group_handle_output_enter(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group,
    struct wl_output *output)
{
  ew_group_t *group = data;

  if(g_ptr_array_find(group->outputs, output, NULL))
    return;
  g_debug("Workspace: group %p, add output: %p", group, output);
  g_ptr_array_add(group->outputs, output);
  g_ptr_array_foreach(group->workspaces, (GFunc)workspace_commit, NULL);
}

static void ew_workspace_group_handle_output_leave(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group,
    struct wl_output *output)
{
  ew_group_t *group = data;

  g_debug("Workspace: group %p, remove output: %p", group, output);
  g_ptr_array_remove(group->outputs, output);
  g_ptr_array_foreach(group->workspaces, (GFunc)workspace_commit, NULL);
}

static void ew_workspace_group_handle_workspace_enter(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group,
    struct ext_workspace_handle_v1 *workspace)
{
  ew_group_t *group = data;
  workspace_t *ws;

  if( !(ws = workspace_from_id(workspace)) )
    return;

  g_debug("Workspace: group %p, add workspace: %s", group, ws->name);
  ws->data = group;
  g_ptr_array_add(group->workspaces, workspace);
  workspace_commit(ws);
}

static void ew_workspace_group_handle_workspace_leave(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group,
    struct ext_workspace_handle_v1 *workspace)
{
  ew_group_t *group = data;
  workspace_t *ws;

  g_ptr_array_remove(group->workspaces, workspace);
  if( !(ws = workspace_from_id(workspace)) || ws->data != group )
    return;

  g_debug("Workspace: group %p, remove workspace: %s", group, ws->name);
  ws->data = NULL;
  workspace_commit(ws);
}

static void ew_workspace_group_handle_remove(void *data,
    struct ext_workspace_group_handle_v1 *workspace_group)
{
  ew_group_t *group = data;
  workspace_groups = g_list_remove(workspace_groups, workspace_group);
  ext_workspace_group_handle_v1_destroy(workspace_group);

  g_ptr_array_unref(group->outputs);
  g_ptr_array_unref(group->workspaces);
}

static const struct ext_workspace_group_handle_v1_listener
	ew_workspace_group_impl =
{
  .capabilities = ew_workspace_group_handle_capabilities,
  .output_enter = ew_workspace_group_handle_output_enter,
  .output_leave = ew_workspace_group_handle_output_leave,
  .workspace_enter = ew_workspace_group_handle_workspace_enter,
  .workspace_leave = ew_workspace_group_handle_workspace_leave,
  .removed = ew_workspace_group_handle_remove,
};

/* Manager */
static void ew_workspace_manager_handle_workspace_group ( void *data,
    struct ext_workspace_manager_v1 *workspace_manager,
    struct ext_workspace_group_handle_v1 *workspace_group)
{
  ew_group_t *group = g_malloc0(sizeof(ew_group_t));
  group->outputs = g_ptr_array_new();
  group->workspaces = g_ptr_array_new();
  ext_workspace_group_handle_v1_add_listener(
    workspace_group, &ew_workspace_group_impl, group);
}

static void ew_workspace_manager_handle_workspace ( void *data,
    struct ext_workspace_manager_v1 *workspace_manager,
    struct ext_workspace_handle_v1 *workspace)
{
  workspace_t *ws;

  ws = workspace_new(workspace);
  workspace_mod_state(workspace, WS_STATE_VISIBLE, TRUE);

  ext_workspace_handle_v1_add_listener(workspace, &ew_workspace_impl, ws);
}

static void ew_workspace_manager_handle_done(void *data,
    struct ext_workspace_manager_v1 *workspace_manager)
{
  g_list_foreach(workspace_get_list(), (GFunc)workspace_commit, NULL);
}

static void ew_workspace_manager_handle_finished(void *data,
    struct ext_workspace_manager_v1 *manager)
{
  ext_workspace_manager_v1_destroy(manager);
  workspace_manager = NULL;
}

static const struct ext_workspace_manager_v1_listener
  workspace_manager_impl =
{
  .workspace_group = ew_workspace_manager_handle_workspace_group,
  .workspace = ew_workspace_manager_handle_workspace,
  .done = ew_workspace_manager_handle_done,
  .finished = ew_workspace_manager_handle_finished,
};

/* Public API */
void ew_init( void )
{
  if(workspace_api_check())
  {
    g_info("Workspace: Not using ext-workspace: custom IPC priority");
    return;
  }

  if( !(workspace_manager = wayland_iface_register(
          ext_workspace_manager_v1_interface.name,
          EXT_WORKSPACE_VERSION,
          EXT_WORKSPACE_VERSION,
          &ext_workspace_manager_v1_interface)) )
    return;

  workspace_api_register(&ew_api_impl);

  ext_workspace_manager_v1_add_listener(workspace_manager,
      &workspace_manager_impl, NULL);
}
