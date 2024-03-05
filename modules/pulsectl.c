/* This entire file is licensed under GNU General Public License v3.0
 *
 * Copyright 2022 Sfwbar maintainers
 */

#include <glib.h>
#include "../src/module.h"
#include "../src/basewidget.h"
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

gboolean invalid;

typedef struct _pulse_info {
  guint32 idx, client;
  gchar *name;
  gboolean mute;
  pa_cvolume cvol;
  gdouble vol;
  gchar *icon, *form, *port, *monitor, *description;
  pa_channel_map cmap;
} pulse_info;

typedef struct _pulse_channel {
  gint iface_idx;
  gint chan_num;
  gchar *channel;
  gchar *device;
} pulse_channel_t;

typedef struct _pulse_interface {
  const gchar *prefix;
  gchar *default_name;
  gchar *name;
  gboolean fixed;
  GList *list;
  module_queue_t queue;
  pa_operation *(*set_volume)(pa_context *, uint32_t, const pa_cvolume *,
      pa_context_success_cb_t, void *);
  pa_operation *(*set_mute)(pa_context *, uint32_t, int,
      pa_context_success_cb_t, void *);
} pulse_interface_t;

pa_mainloop_api *papi;
gint64 sfwbar_module_signature = 0x73f4d956a1;
guint16 sfwbar_module_version = 2;

static pa_context *pctx;
static pulse_interface_t pulse_interfaces[];

static void* pulse_channel_dup_dummy ( void *d )
{
  return d;
}

static void pulse_channel_free ( pulse_channel_t *channel )
{
  g_free(channel->channel);
  g_free(channel->device);
  g_free(channel);
}

static gboolean pulse_channel_comp ( pulse_channel_t *c1, pulse_channel_t *c2 )
{
  return (!g_strcmp0(c1->channel, c2->channel) &&
      !g_strcmp0(c1->device, c2->device) && c1->iface_idx==c2->iface_idx);
}

static void *pulse_channel_get_str ( pulse_channel_t *channel, gchar *prop )
{
  if(!g_ascii_strcasecmp(prop, "interface"))
    return g_strdup(channel->iface_idx<3?
        pulse_interfaces[channel->iface_idx].prefix:"none");
  if(!g_ascii_strcasecmp(prop, "channel"))
    return g_strdup(channel->channel);
  if(!g_ascii_strcasecmp(prop, "device"))
    return g_strdup(channel->device);
  if(!g_ascii_strcasecmp(prop, "ChannelNumber"))
    return g_strdup_printf("%d", channel->chan_num);
  return NULL;
}

static module_queue_t channel_q = {
  .free = (void (*)(void *))pulse_channel_free,
  .duplicate = (void *(*)(void *))pulse_channel_dup_dummy,
  .get_str = (void *(*)(void *, gchar *))pulse_channel_get_str,
  .compare = (gboolean (*)(const void *, const void *))pulse_channel_comp,
};

static void *pulse_remove_get_str ( gchar *name, gchar *prop )
{
  if(!g_ascii_strcasecmp(prop, "RemovedDevice"))
    return g_strdup(name);
  return NULL;
}

static module_queue_t remove_q = {
  .free = g_free,
  .duplicate = (void *(*)(void *))g_strdup,
  .get_str = (void *(*)(void *, gchar *))pulse_remove_get_str,
  .compare = (gboolean (*)(const void *, const void *))g_strcmp0,
};

static pulse_info *pulse_info_from_idx ( pulse_interface_t *iface, guint32 idx,
    gboolean new )
{
  GList *iter;
  pulse_info *info;

  for(iter=iface->list; iter; iter=g_list_next(iter))
    if(((pulse_info *)iter->data)->idx == idx)
      return iter->data;

  if(new)
  {
    info = g_malloc0(sizeof(pulse_info));
    iface->list = g_list_prepend(iface->list, info);
    return info;
  }
  else
    return NULL;
}

static pulse_interface_t pulse_interfaces[] = {
  {
    .prefix = "sink",
    .default_name = "default",
    .list = NULL,
    .set_volume = pa_context_set_sink_volume_by_index,
    .set_mute = pa_context_set_sink_mute_by_index,
  },
  {
    .prefix = "source",
    .default_name = "default",
    .list = NULL,
    .set_volume = pa_context_set_source_volume_by_index,
    .set_mute = pa_context_set_source_mute_by_index,
  },
  {
    .prefix = "client",
    .default_name = "default",
    .list = NULL,
    .set_volume = pa_context_set_sink_input_volume,
    .set_mute = pa_context_set_sink_input_mute,
  }
};

pulse_interface_t *pulse_interface_get ( gchar *name, gchar **ptr )
{
  gint i;

  for(i=0; i<3; i++)
    if(g_str_has_prefix(name, pulse_interfaces[i].prefix))
    {
      if(ptr)
        *ptr = name + strlen(pulse_interfaces[i].prefix) + 1;
      return &pulse_interfaces[i];
    }

  return NULL;
}

static void pulse_set_name ( gchar *ifname, const gchar *name, gboolean fixed )
{
  pulse_interface_t *iface;

  if( !(iface = pulse_interface_get(ifname, NULL)) )
    return;

  if(!fixed && iface->fixed)
    return;

  iface->fixed = fixed;
  g_free(iface->name);
  iface->name = g_strdup(name);
}

static void pulse_remove_device ( pulse_interface_t *iface, guint32 idx )
{
  GList *iter;
  pulse_info *info;
  gchar *tmp;

  for(iter=iface->list; iter; iter=g_list_next(iter))
    if(((pulse_info *)iter->data)->idx == idx)
      break;
  if(!iter)
    return;

  info = iter->data;
  iface->list = g_list_delete_link(iface->list, iter);

  if(info->name)
  {
    tmp = g_strdup_printf("@pulse-%s-%d", iface->prefix, idx);
    module_queue_append(&remove_q, tmp);
    g_free(tmp);
  }
  g_free(info->name);
  g_free(info->icon);
  g_free(info->form);
  g_free(info->port);
  g_free(info->monitor);
  g_free(info->description);
  g_free(info);
}

static void pulse_device_advertise ( gint iface_idx,
    const pa_channel_map *cmap, gint idx )
{
  pulse_channel_t *channel;
  gint i;

  for(i=0; i<cmap->channels; i++)
  {
    channel = g_malloc0(sizeof(pulse_channel_t));
    channel->iface_idx = iface_idx;
    channel->chan_num = i;
    channel->channel = g_strdup(pa_channel_position_to_string(cmap->map[i]));
    channel->device = g_strdup_printf("@pulse-%s-%d",
        pulse_interfaces[iface_idx].prefix, idx );
    module_queue_append(&channel_q, channel);
  }
}

static void pulse_sink_cb ( pa_context *ctx, const pa_sink_info *pinfo,
    gint eol, void *d )
{
  pulse_info *info;
  gboolean new;

  if(!pinfo)
    return;
  new = !pulse_info_from_idx(&pulse_interfaces[0], pinfo->index, FALSE);

  info = pulse_info_from_idx(&pulse_interfaces[0], pinfo->index, TRUE);

  g_free(info->name);
  info->name = g_strdup(pinfo->name);
  g_free(info->icon);
  info->icon = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_ICON_NAME));
  g_free(info->form);
  info->form = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_FORM_FACTOR));
  g_free(info->port);
  info->port = g_strdup(pinfo->active_port?pinfo->active_port->name:NULL);
  g_free(info->monitor);
  info->monitor = g_strdup(pinfo->monitor_source_name);
  g_free(info->description);
  info->description = g_strdup(pinfo->description);
  info->idx = pinfo->index;
  info->cvol = pinfo->volume;
  info->vol = 100.0 * pa_cvolume_avg(&info->cvol)/PA_VOLUME_NORM;
  info->mute = pinfo->mute;
  info->cmap = pinfo->channel_map;

  if(new)
    pulse_device_advertise(0, &pinfo->channel_map, pinfo->index);

  g_main_context_invoke(NULL, (GSourceFunc)base_widget_emit_trigger,
      (gpointer)g_intern_static_string("pulse"));
}

static void pulse_client_cb ( pa_context *ctx, const pa_client_info *cinfo,
    int eol, void *data )
{
  pulse_info *info;
  gboolean change = FALSE;
  GList *iter;

  if(!cinfo)
    return;

  for(iter=pulse_interfaces[2].list; iter; iter=g_list_next(iter))
  {
    info = iter->data;
    if(info->client==cinfo->index && g_strcmp0(info->description, cinfo->name))
    {
      g_free(info->description);
      info->description = g_strdup(cinfo->name);
      change = TRUE;
    }
  }

  if(change)
    g_main_context_invoke(NULL, (GSourceFunc)base_widget_emit_trigger,
        (gpointer)g_intern_static_string("pulse"));
}

static void pulse_sink_input_cb ( pa_context *ctx,
    const pa_sink_input_info *pinfo, gint eol, void *d )
{
  pulse_info *info;
  gboolean new;

  if(!pinfo)
    return;

  new = !pulse_info_from_idx(&pulse_interfaces[2], pinfo->index, FALSE);
  info = pulse_info_from_idx(&pulse_interfaces[2], pinfo->index, TRUE);
  g_free(info->name);
  info->name = g_strdup(pinfo->name);
  g_free(info->icon);
  info->icon = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_ICON_NAME));
  g_free(info->form);
  info->form = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_FORM_FACTOR));
  info->idx = pinfo->index;
  info->cvol = pinfo->volume;
  info->vol = 100.0 * pa_cvolume_avg(&info->cvol)/PA_VOLUME_NORM;
  info->mute = pinfo->mute;
  info->cmap = pinfo->channel_map;
  info->client = pinfo->client;
  g_main_context_invoke(NULL, (GSourceFunc)base_widget_emit_trigger,
      (gpointer)g_intern_static_string("pulse"));

  if(new)
    pulse_device_advertise(2, &pinfo->channel_map, pinfo->index);

  pa_operation_unref(pa_context_get_client_info(ctx, pinfo->client,
        pulse_client_cb, (void *)info ));
}

static void pulse_source_cb ( pa_context *ctx, const pa_source_info *pinfo,
    gint eol, void *d )
{
  pulse_info *info;

  if(!pinfo)
    return;
  info = pulse_info_from_idx(&pulse_interfaces[1], pinfo->index, TRUE);

  g_free(info->name);
  info->name = g_strdup(pinfo->name);
  g_free(info->icon);
  info->icon = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_ICON_NAME));
  g_free(info->form);
  info->form = g_strdup(pa_proplist_gets(pinfo->proplist,
        PA_PROP_DEVICE_FORM_FACTOR));
  g_free(info->port);
  info->port = g_strdup(pinfo->active_port?pinfo->active_port->name:"Unknown");
  g_free(info->monitor);
  info->monitor = g_strdup(pinfo->monitor_of_sink_name);
  g_free(info->description);
  info->description = g_strdup(pinfo->description);
  info->idx = pinfo->index;
  info->cvol = pinfo->volume;
  info->vol = 100.0 * pa_cvolume_avg(&info->cvol)/PA_VOLUME_NORM;
  info->mute = pinfo->mute;
  g_main_context_invoke(NULL, (GSourceFunc)base_widget_emit_trigger,
      (gpointer)g_intern_static_string("pulse"));
}

static void pulse_server_cb ( pa_context *ctx, const pa_server_info *info,
    void *d )
{
  pulse_set_name("sink", info->default_sink_name, FALSE);
  pulse_set_name("source", info->default_source_name, FALSE);
  pa_operation_unref(
      pa_context_get_sink_info_list(ctx, pulse_sink_cb, NULL));
  pa_operation_unref(
      pa_context_get_source_info_list(ctx, pulse_source_cb, NULL));
  pa_operation_unref(
      pa_context_get_sink_input_info_list(ctx, pulse_sink_input_cb, NULL));
}

static void pulse_subscribe_cb ( pa_context *ctx,
    pa_subscription_event_type_t type, guint idx, void *d )
{
  if((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
  {
    switch(type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
    {
      case PA_SUBSCRIPTION_EVENT_SINK:
        pulse_remove_device(&pulse_interfaces[0], idx);
        break;
      case PA_SUBSCRIPTION_EVENT_SOURCE:
        pulse_remove_device(&pulse_interfaces[1], idx);
        break;
      case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        pulse_remove_device(&pulse_interfaces[2], idx);
        break;
    }
  }
  if(!(type & PA_SUBSCRIPTION_EVENT_CHANGE))
    return;

  switch(type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
  {
    case PA_SUBSCRIPTION_EVENT_SERVER:
      pa_operation_unref(
          pa_context_get_server_info(ctx,pulse_server_cb,NULL));
      break;
    case PA_SUBSCRIPTION_EVENT_SINK:
      pa_operation_unref(
        pa_context_get_sink_info_by_index(ctx, idx, pulse_sink_cb, NULL));
      break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
      pa_operation_unref(
        pa_context_get_sink_input_info(ctx, idx, pulse_sink_input_cb, NULL));
      break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
      pa_operation_unref(
        pa_context_get_source_info_by_index(ctx, idx, pulse_source_cb, NULL));
      break;
    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
      break;
    case PA_SUBSCRIPTION_EVENT_CLIENT:
      pa_operation_unref(
        pa_context_get_client_info(ctx, idx, pulse_client_cb, NULL));
      break;
  }
}

static void pulse_state_cb ( pa_context *ctx, gpointer data )
{
  pa_context_state_t state;
  state = pa_context_get_state(ctx);

  if(state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
    papi->quit(papi,0);
  else if(state == PA_CONTEXT_READY)
  {
    pa_operation_unref(
        pa_context_get_server_info(ctx,pulse_server_cb,NULL));
    pa_context_set_subscribe_callback(ctx,pulse_subscribe_cb,NULL);
    pa_operation_unref(
      pa_context_subscribe(ctx,PA_SUBSCRIPTION_MASK_SERVER |
          PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT |
          PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT,
          NULL, NULL));
  }
}

gboolean sfwbar_module_init ( void )
{
  pa_glib_mainloop *ploop;

  channel_q.trigger = g_intern_static_string("pulse_channel");
  remove_q.trigger = g_intern_static_string("pulse_removed");
  ploop = pa_glib_mainloop_new(g_main_context_get_thread_default());
  papi = pa_glib_mainloop_get_api(ploop);
  pctx = pa_context_new(papi, "sfwbar");
  pa_context_connect(pctx, NULL, PA_CONTEXT_NOFAIL, NULL);
  pa_context_set_state_callback(pctx, pulse_state_cb, NULL);
  return TRUE;
}

void sfwbar_module_invalidate ( void )
{
  invalid = TRUE;
}

static pulse_info *pulse_addr_parse ( gchar *addr, pulse_interface_t *iface,
    gint *cidx )
{
  pa_channel_position_t cpos;
  pulse_info *info;
  GList *iter;
  gchar *device, *channel;
  gint i;
  gchar *ptr;

  if(cidx)
    *cidx = 0;

  if(addr)
  {
    device = g_strdup(addr);
    if( (channel = strchr(device, ':')) )
      *channel = '\0';
  }
  else
  {
    device = NULL;
    channel = NULL;
  }

  if(device && g_str_has_prefix(device, "@pulse-"))
  {
    if( device && (ptr = strrchr(device, '-')) )
      info = pulse_info_from_idx(iface, atoi(ptr+1), FALSE);
    else
      info = NULL;
  }
  else
  {
    for(iter=iface->list; iter; iter=g_list_next(iter))
      if(!g_strcmp0(((pulse_info *)iter->data)->name,
            device?device:iface->name))
        break;
    info = iter?iter->data:NULL;
  }

  if(cidx && info && channel)
  {
    cpos = pa_channel_position_from_string(channel+1);
    for(i=0; i<info->cmap.channels; i++)
      if(info->cmap.map[i] == cpos)
        *cidx = i+1;
  }
  g_free(device);

  return info;
}

static void *pulse_expr_func ( void **params, void *widget, void *event )
{
  pulse_info *info;
  pulse_interface_t *iface;
  gchar *cmd;
  gint cidx;

  if(!params || !params[0])
    return g_strdup("");

  if( !(iface = pulse_interface_get(params[0], &cmd)) )
    return g_strdup("");

  if( !(info = pulse_addr_parse(params[1], iface, &cidx)) )
    return g_strdup("");

  if(!g_ascii_strcasecmp(cmd, "volume"))
    return g_strdup_printf("%f",
        cidx?info->cvol.values[cidx-1]*100.0/PA_VOLUME_NORM:info->vol);
  if(!g_ascii_strcasecmp(cmd, "mute"))
    return g_strdup_printf("%d",info->mute);
  if(!g_ascii_strcasecmp(cmd, "icon"))
    return g_strdup(info->icon?info->icon:"");
  if(!g_ascii_strcasecmp(cmd, "form"))
    return g_strdup(info->form?info->form:"");
  if(!g_ascii_strcasecmp(cmd, "port"))
    return g_strdup(info->port?info->port:"");
  if(!g_ascii_strcasecmp(cmd, "monitor"))
    return g_strdup(info->monitor?info->monitor:"");
  if(!g_ascii_strcasecmp(cmd, "description"))
    return g_strdup(info->description?info->description:"");
  if(!g_ascii_strcasecmp(cmd, "count"))
    return g_strdup_printf("%d", iface?g_list_length(iface->list):0);

  return g_strdup_printf("invalid query: %s", cmd);
}

static void *pulse_channel_func ( void **params, void *widget, void *event )
{
  gchar *result;

  if(!params || !params[0])
    return g_strdup("");

  if( (result = module_queue_get_string(&channel_q, params[0])) )
    return result;
  if( (result = module_queue_get_string(&remove_q, params[0])) )
    return result;

  return g_strdup("");
}

static pa_cvolume *pulse_adjust_volume ( pulse_info *info, gint cidx,
    gchar *vstr )
{
  gint vdelta;

  if(!vstr)
    return &(info->cvol);

  while(*vstr==' ')
    vstr++;

  vdelta = g_ascii_strtod(vstr, NULL)*PA_VOLUME_NORM/100;
  if(*vstr!='+' && *vstr!='-')
    vdelta -= cidx?info->cvol.values[cidx-1]:pa_cvolume_avg(&info->cvol);

  if(!cidx)
  {
    if(vdelta > 0)
      pa_cvolume_inc_clamp(&info->cvol, vdelta, PA_VOLUME_UI_MAX);
    else
      pa_cvolume_dec(&info->cvol, -vdelta);
  }
  else
    info->cvol.values[cidx-1] = CLAMP((gint)(info->cvol.values[cidx-1]) +
        vdelta, 0, PA_VOLUME_UI_MAX);

  return &info->cvol;
}

static gboolean pulse_mute_parse ( gchar *cmd, gboolean mute )
{
  while(*cmd==' ')
    cmd++;

  if(!g_ascii_strcasecmp(cmd, "toggle"))
    return !mute;
  else if(!g_ascii_strcasecmp(cmd, "true"))
    return TRUE;
  else if(!g_ascii_strcasecmp(cmd, "false"))
    return FALSE;

  return mute;
}

static pa_operation *pulse_client_set_sink ( pulse_info *info, gchar *dest )
{
  pulse_info *dinfo;

  if(!info || !dest || !info->client)
    return NULL;

  while(*dest == ' ')
    dest++;

  if( !(dinfo = pulse_addr_parse(dest, &pulse_interfaces[0], NULL)) )
    return NULL;

  return pa_context_move_sink_input_by_index(pctx, info->idx, dinfo->idx,
      NULL, NULL);
}

static void pulse_action ( gchar *cmd, gchar *name, void *d1,
    void *d2, void *d3, void *d4 )
{
  pulse_info *info;
  pulse_interface_t *iface;
  pa_operation *op;
  gint cidx;
  gchar *command;

  if( !(iface = pulse_interface_get(cmd, &command)) )
    return;

  if( !(info = pulse_addr_parse(name, iface, &cidx)) )
    return;

  if(!g_ascii_strncasecmp(command, "volume", 6))
    op = iface->set_volume(pctx, info->idx,
        pulse_adjust_volume(info, cidx, command+6), NULL, NULL);
  else if(!g_ascii_strncasecmp(command, "mute", 4))
    op = iface->set_mute(pctx, info->idx,
        pulse_mute_parse(command+4, info->mute), NULL, NULL);
  else if(!g_ascii_strncasecmp(command, "set-sink", 8))
    op = pulse_client_set_sink(info, command+8);

  else
    op = NULL;

  if(op)
    pa_operation_unref(op);
}

static void pulse_channel_ack_action ( gchar *cmd, gchar *name, void *d1,
    void *d2, void *d3, void *d4 )
{
  module_queue_remove(&channel_q);
}

static void pulse_channel_ack_removed_action ( gchar *cmd, gchar *name,
    void *d1, void *d2, void *d3, void *d4 )
{
  module_queue_remove(&remove_q);
}

static void pulse_set_sink_action ( gchar *sink, gchar *dummy, void *d1,
    void *d2, void *d3, void *d4 )
{
  pulse_set_name("sink", sink, TRUE);
}

static void pulse_set_source_action ( gchar *source, gchar *dummy, void *d1,
    void *d2, void *d3, void *d4 )
{
  pulse_set_name("source", source, TRUE);
}

ModuleExpressionHandlerV1 handler1 = {
  .flags = 0,
  .name = "Pulse",
  .parameters = "Ss",
  .function = pulse_expr_func
};

ModuleExpressionHandlerV1 handler2 = {
  .flags = 0,
  .name = "PulseChannel",
  .parameters = "S",
  .function = pulse_channel_func
};

ModuleExpressionHandlerV1 *sfwbar_expression_handlers[] = {
  &handler1,
  &handler2,
  NULL
};

ModuleActionHandlerV1 act_handler1 = {
  .name = "PulseCmd",
  .function = pulse_action
};

ModuleActionHandlerV1 act_handler2 = {
  .name = "PulseSetDefaultSink",
  .function = pulse_set_sink_action
};

ModuleActionHandlerV1 act_handler3 = {
  .name = "PulseSetDefaultSource",
  .function = pulse_set_source_action
};

ModuleActionHandlerV1 act_handler4 = {
  .name = "PulseChannelAck",
  .function = pulse_channel_ack_action
};

ModuleActionHandlerV1 act_handler5 = {
  .name = "PulseChannelAckRemoved",
  .function = pulse_channel_ack_removed_action
};

ModuleActionHandlerV1 *sfwbar_action_handlers[] = {
  &act_handler1,
  &act_handler2,
  &act_handler3,
  &act_handler4,
  &act_handler5,
  NULL
};
