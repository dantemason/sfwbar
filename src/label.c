/* This entire file is licensed under GNU General Public License v3.0
 *
 * Copyright 2022- sfwbar maintainers
 */

#include "sfwbar.h"
#include "basewidget.h"
#include "label.h"

G_DEFINE_TYPE_WITH_CODE (Label, label, BASE_WIDGET_TYPE, G_ADD_PRIVATE (Label))

static void label_update_value ( GtkWidget *self )
{
  LabelPrivate *priv;
  gchar *value;

  g_return_if_fail(IS_LABEL(self));
  priv = label_get_instance_private(LABEL(self));

  value = base_widget_get_value(self);
  if(value && pango_parse_markup(value, -1, 0, NULL, NULL, NULL, NULL))
    gtk_label_set_markup(GTK_LABEL(priv->label), value);
  else
    gtk_label_set_text(GTK_LABEL(priv->label), value);
}

static GtkWidget *label_get_child ( GtkWidget *self )
{
  LabelPrivate *priv;

  g_return_val_if_fail(IS_LABEL(self),NULL);
  priv = label_get_instance_private(LABEL(self));

  return priv->label;
}

static GtkWidget *label_mirror ( GtkWidget *src )
{
  g_return_val_if_fail(IS_LABEL(src), NULL);
  return label_new();
}

static void label_class_init ( LabelClass *kclass )
{
  BASE_WIDGET_CLASS(kclass)->update_value = label_update_value;
  BASE_WIDGET_CLASS(kclass)->get_child = label_get_child;
  BASE_WIDGET_CLASS(kclass)->mirror = label_mirror;
}

static void label_init ( Label *self )
{
}

GtkWidget *label_new ( void )
{
  GtkWidget *self;
  LabelPrivate *priv;

  self = GTK_WIDGET(g_object_new(label_get_type(), NULL));
  priv = label_get_instance_private(LABEL(self));

  priv->label = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(priv->label),PANGO_ELLIPSIZE_END);
  gtk_container_add(GTK_CONTAINER(self),priv->label);

  return self;
}
