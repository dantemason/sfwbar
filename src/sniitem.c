/* This entire file is licensed under GNU General Public License v3.0
 *
 * Copyright 2022- sfwbar maintainers
 */

#include <gio/gio.h>
#include "gui/scaleimage.h"
#include "sni.h"

struct sni_prop_wrapper {
  guint prop;
  sni_item_t *sni;
};

static GList *sni_items;
static guint pix_counter;
static GList *sni_listeners;

static gchar *sni_properties[] = { "Category", "Id", "Title", "Status",
  "IconName", "OverlayIconName", "AttentionIconName", "AttentionMovieName",
  "IconAccessibleDesc", "AttnAccessibleDesc", "XAyatanaLabel",
  "XAyatanaLabelGuide", "IconThemePath", "IconPixmap", "OverlayIconPixmap",
  "AttentionIconPixmap", "WindowId", "ToolTip", "ItemIsMenu", "Menu",
  "XAyatanaOrderingIndex" };

#define LISTENER_CALL(method, sni) { \
  for(GList *li=sni_listeners; li; li=li->next) \
    if(SNI_LISTENER(li->data)->method) \
      SNI_LISTENER(li->data)->method(sni, \
          SNI_LISTENER(li->data)->data); \
}

void sni_listener_register ( sni_listener_t *listener, void *data )
{
  sni_listener_t *copy;
  GList *iter;

  if(!listener)
    return;

  copy = g_memdup(listener, sizeof(sni_listener_t));
  copy->data = data;
  sni_listeners = g_list_append(sni_listeners, copy);

  if(copy->sni_new)
  {
    for(iter=sni_item_get_list(); iter; iter=g_list_next(iter))
      copy->sni_new(iter->data, copy->data);
  }
}

void sni_listener_remove ( void *data )
{
  GList *iter;

  for(iter=sni_listeners; iter; iter=g_list_next(iter))
    if(SNI_LISTENER(iter->data)->data == data)
      break;
  if(iter)
    sni_listeners = g_list_remove(sni_listeners, iter->data);
}

gchar *sni_item_icon ( sni_item_t *item )
{
  if(!item->string[SNI_PROP_STATUS])
    return NULL;
  if(item->string[SNI_PROP_STATUS][0]=='N')
  {
    if(item->string[SNI_PROP_ATTN] && item->string[SNI_PROP_ATTN][0])
      return item->string[SNI_PROP_ATTN];
    return item->string[SNI_PROP_ATTNPIX];
  }
  if(item->string[SNI_PROP_ICON] && item->string[SNI_PROP_ICON][0])
    return item->string[SNI_PROP_ICON];
  return item->string[SNI_PROP_ICONPIX];
}

gchar *sni_item_tooltip ( sni_item_t *item )
{
  if(item->tooltip && item->tooltip[0])
    return item->tooltip;
  if(!item->string[SNI_PROP_STATUS])
    return NULL;
  if(item->string[SNI_PROP_STATUS][0]!='N' && item->string[SNI_PROP_ICONACC] &&
      item->string[SNI_PROP_ICONACC][0])
    return item->string[SNI_PROP_ICONACC];
  if(item->string[SNI_PROP_STATUS][0]=='N' && item->string[SNI_PROP_ATTNACC] &&
      item->string[SNI_PROP_ATTNACC][0])
    return item->string[SNI_PROP_ATTNACC];
  return NULL;
}

static gchar *sni_item_get_pixbuf ( GVariant *v )
{
  GVariant *img,*child;
  cairo_surface_t *cs;
  GdkPixbuf *res;
  gint32 x,y;
  guint32 *ptr;
  gsize len, i;
  gchar *name;

  if(!v || !g_variant_check_format_string(v, "a(iiay)", FALSE) ||
      g_variant_n_children(v) < 1)
    return NULL;

  child = g_variant_get_child_value(v, 0);

  g_variant_get(child, "(ii@ay)", &x, &y, &img);
  ptr = (guint32 *)g_variant_get_fixed_array(img, &len, sizeof(guchar));

  if(!len || !ptr || len != x*y*4)
  {
    g_variant_unref(img);
    g_variant_unref(child);
    return NULL;
  }

  ptr = g_memdup2(ptr, len);
  g_variant_unref(img);
  g_variant_unref(child);
  for(i=0; i<x*y; i++)
    ptr[i] = g_ntohl(ptr[i]);

  cs = cairo_image_surface_create_for_data((guchar *)ptr, CAIRO_FORMAT_ARGB32,
      x, y, cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, x));
  res = gdk_pixbuf_get_from_surface(cs, 0, 0, x, y);
  cairo_surface_destroy(cs);
  g_free(ptr);

  name = g_strdup_printf("<pixbufcache/>sni-%u", pix_counter++);
  scale_image_cache_insert(name, res);

  return name;
}

gchar *sni_item_get_tooltip ( GVariant *v )
{
  gchar *header, *body;

  g_variant_get(v, "(&s@a(iiay)&s&s)", NULL, NULL, &header, &body);
  if(header && *header && (!body || !*body))
    return g_strdup(header);
  else if ((!header || !*header) && body && *body)
    return g_strdup(body);
  else if(header && *header && body && *body)
    return g_strconcat(header, "\n", body, NULL);
  else
    return NULL;
}

void sni_item_prop_cb ( GDBusConnection *con, GAsyncResult *res,
    struct sni_prop_wrapper *wrap)
{
  GVariant *result, *inner;

  wrap->sni->ref--;

  if( (result = g_dbus_connection_call_finish(con, res, NULL)) )
  {
    g_variant_get(result, "(v)", &inner);
    g_variant_unref(result);
  }

  if(!result || !inner)
  {
    g_free(wrap);
    return;
  }

  if(wrap->prop<=SNI_PROP_THEME &&
      g_variant_is_of_type(inner,G_VARIANT_TYPE_STRING))
  {
    g_free(wrap->sni->string[wrap->prop]);
    g_variant_get(inner, "s", &(wrap->sni->string[wrap->prop]));
    g_debug("sni %s: property %s = %s", wrap->sni->dest,
        sni_properties[wrap->prop], wrap->sni->string[wrap->prop]);
  }
  else if(wrap->prop>=SNI_PROP_ICONPIX && wrap->prop<=SNI_PROP_ATTNPIX)
  {
    scale_image_cache_remove(wrap->sni->string[wrap->prop]);
    g_clear_pointer(&(wrap->sni->string[wrap->prop]), g_free);
    wrap->sni->string[wrap->prop] = sni_item_get_pixbuf(inner);
    g_debug("sni %s: property %s received", wrap->sni->dest,
        sni_properties[wrap->prop]);
  }
  else if(wrap->prop == SNI_PROP_MENU &&
      g_variant_is_of_type(inner,G_VARIANT_TYPE_OBJECT_PATH))
  {
    g_free(wrap->sni->menu_path);
    g_variant_get(inner, "o", &(wrap->sni->menu_path));
    sni_menu_init(wrap->sni);
    g_debug("sni %s: property %s = %s", wrap->sni->dest,
        sni_properties[wrap->prop], wrap->sni->menu_path);
  }
  else if(wrap->prop == SNI_PROP_ISMENU)
  {
    g_variant_get(inner, "b", &(wrap->sni->menu));
    g_debug("sni %s: property %s = %d", wrap->sni->dest,
        sni_properties[wrap->prop], wrap->sni->menu);
  }
  else if(wrap->prop == SNI_PROP_ORDER)
  {
    g_variant_get(inner, "u", &(wrap->sni->order));
    g_debug("sni %s: property %s = %u", wrap->sni->dest,
        sni_properties[wrap->prop], wrap->sni->order);
  }
  else if(wrap->prop == SNI_PROP_TOOLTIP)
  {
    g_free(wrap->sni->tooltip);
    wrap->sni->tooltip = sni_item_get_tooltip(inner);
    g_debug("sni %s: property %s = %s", wrap->sni->dest,
        sni_properties[wrap->prop], wrap->sni->tooltip);
  }

  g_variant_unref(inner);
  LISTENER_CALL(sni_invalidate, wrap->sni);
  g_free(wrap);
}

void sni_item_get_prop ( GDBusConnection *con, sni_item_t *sni,
    guint prop )
{
  struct sni_prop_wrapper *wrap;

  wrap = g_malloc(sizeof(struct sni_prop_wrapper));
  wrap->prop = prop;
  wrap->sni = sni;
  wrap->sni->ref++;

  g_dbus_connection_call(con, sni->dest, sni->path,
    "org.freedesktop.DBus.Properties", "Get",
    g_variant_new("(ss)", sni->iface, sni_properties[prop]), NULL,
    G_DBUS_CALL_FLAGS_NONE, -1, sni->cancel,
    (GAsyncReadyCallback)sni_item_prop_cb, wrap);
}

void sni_item_signal_cb (GDBusConnection *con, const gchar *sender,
         const gchar *path, const gchar *interface, const gchar *signal,
         GVariant *parameters, gpointer data)
{
  g_debug("sni: received signal %s from %s", signal, sender);
  if(!g_strcmp0(signal, "NewTitle"))
    sni_item_get_prop(con, data,  SNI_PROP_TITLE);
  else if(!g_strcmp0(signal, "NewStatus"))
    sni_item_get_prop(con, data, SNI_PROP_STATUS);
  else if(!g_strcmp0(signal, "NewToolTip"))
    sni_item_get_prop(con, data, SNI_PROP_TOOLTIP);
  else if(!g_strcmp0(signal, "NewIconThemePath"))
    sni_item_get_prop(con, data, SNI_PROP_THEME);
  else if(!g_strcmp0(signal, "NewIcon"))
  {
    sni_item_get_prop(con, data, SNI_PROP_ICON);
    sni_item_get_prop(con, data, SNI_PROP_ICONACC);
    sni_item_get_prop(con, data, SNI_PROP_ICONPIX);
  }
  else if(!g_strcmp0(signal, "NewOverlayIcon"))
  {
    sni_item_get_prop(con, data, SNI_PROP_OVLAY);
    sni_item_get_prop(con, data, SNI_PROP_OVLAYPIX);
  }
  else if(!g_strcmp0(signal, "NewAttentionIcon"))
  {
    sni_item_get_prop(con, data, SNI_PROP_ATTN);
    sni_item_get_prop(con, data, SNI_PROP_ATTNACC);
    sni_item_get_prop(con, data, SNI_PROP_ATTNPIX);
  }
  else if(!g_strcmp0(signal, "XAyatanaNewLabel"))
    sni_item_get_prop(con, data, SNI_PROP_LABEL);
}

sni_item_t *sni_item_new (GDBusConnection *con, gchar *iface,
    const gchar *uid)
{
  sni_item_t *sni;
  gchar *path;
  guint i;

  sni = g_malloc0(sizeof(sni_item_t));
  sni->uid = g_strdup(uid);
  sni->cancel = g_cancellable_new();
  sni->menu = TRUE;
  path = strchr(uid,'/');
  if(path!=NULL)
  {
    sni->dest = g_strndup(uid, path-uid);
    sni->path = g_strdup(path);
  }
  else
  {
    sni->path = g_strdup("/StatusNotifierItem");
    sni->dest = g_strdup(uid);
  }
  sni->iface = g_strdup(iface);
  sni->signal = g_dbus_connection_signal_subscribe(con, sni->dest,
      sni->iface, NULL, sni->path, NULL, 0, sni_item_signal_cb, sni, NULL);
  sni_items = g_list_append(sni_items, sni);
  LISTENER_CALL(sni_new, sni);
  for(i=0; i<SNI_PROP_MAX; i++)
    sni_item_get_prop(con, sni, i);

  return sni;
}

void sni_item_free ( sni_item_t *sni )
{
  gint i;

  g_dbus_connection_signal_unsubscribe(sni_get_connection(), sni->signal);
  LISTENER_CALL(sni_destroy, sni);
  g_cancellable_cancel(sni->cancel);
  g_object_unref(sni->cancel);
  for(i=0; i<3; i++)
    scale_image_cache_remove(sni->string[SNI_PROP_ICONPIX+i]);
  for(i=0; i<SNI_MAX_STRING; i++)
    g_free(sni->string[i]);

  gtk_widget_destroy(sni->menu_obj);
  g_free(sni->menu_path);
  g_free(sni->tooltip);
  g_free(sni->uid);
  g_free(sni->path);
  g_free(sni->dest);
  g_free(sni->iface);
  g_free(sni);
}

GList *sni_item_get_list ( void )
{
  return sni_items;
}
