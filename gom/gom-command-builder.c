/* gom-command-builder.c
 *
 * Copyright (C) 2011 Christian Hergert <chris@dronelabs.com>
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>

#include "gom-adapter.h"
#include "gom-command.h"
#include "gom-command-builder.h"
#include "gom-filter.h"
#include "gom-resource.h"

G_DEFINE_TYPE(GomCommandBuilder, gom_command_builder, G_TYPE_OBJECT)

struct _GomCommandBuilderPrivate
{
   GomAdapter *adapter;
   GomFilter *filter;
   GType resource_type;
   guint limit;
   guint offset;
   gchar *m2m_table;
   GType m2m_type;
};

enum
{
   PROP_0,
   PROP_ADAPTER,
   PROP_FILTER,
   PROP_LIMIT,
   PROP_M2M_TABLE,
   PROP_M2M_TYPE,
   PROP_OFFSET,
   PROP_RESOURCE_TYPE,
   LAST_PROP
};

static GParamSpec *gParamSpecs[LAST_PROP];

static gboolean
is_mapped (GParamSpec *pspec)
{
   gboolean ret;

   /*
    * TODO: Make this better, like let classes opt in to what
    *       fields they want mapped.
    */
   ret = (pspec->owner_type != GOM_TYPE_RESOURCE);
   return ret;
}

static void
add_fields (GString          *str,
            GomResourceClass *klass)
{
   GParamSpec **pspecs;
   gboolean mapped = FALSE;
   guint n_pspecs;
   guint i;

   pspecs = g_object_class_list_properties(G_OBJECT_CLASS(klass), &n_pspecs);
   for (i = 0; i < n_pspecs; i++) {
      if (is_mapped(pspecs[i])) {
         if (mapped) {
            g_string_append(str, ", ");
         }
         klass = g_type_class_peek(pspecs[i]->owner_type);
         g_string_append_printf(str, "'%s'.'%s' AS '%s'",
                                klass->table,
                                pspecs[i]->name,
                                pspecs[i]->name);
         mapped = TRUE;
      }
   }

   g_string_append(str, " ");
}

static void
add_from (GString          *str,
          GomResourceClass *klass)
{
   g_string_append_printf(str, " FROM '%s' ", klass->table);
}

static void
add_joins (GString          *str,
           GomResourceClass *klass)
{
   GomResourceClass *parent = klass;

   while ((parent = g_type_class_peek_parent(parent))) {
      if (G_TYPE_FROM_CLASS(parent) == GOM_TYPE_RESOURCE) {
         break;
      }
      g_string_append_printf(str, " JOIN '%s' ON '%s'.'%s' = '%s'.'%s' ",
                             parent->table,
                             klass->table, klass->primary_key,
                             parent->table, parent->primary_key);
   }
}

static void
add_m2m (GString          *str,
         GomResourceClass *klass,
         const gchar      *m2m_table,
         GType             m2m_type)
{
   GomResourceClass *m2m_klass;
   gchar *prefix = NULL;

   if (!m2m_table) {
      return;
   }

   g_assert(g_type_is_a(m2m_type, GOM_TYPE_RESOURCE));
   g_assert(m2m_type != GOM_TYPE_RESOURCE);

   g_string_append_printf(str, " JOIN '%s' ON '%s'.'%s' = '%s'.'%s:%s' ",
                          m2m_table,
                          klass->table, klass->primary_key,
                          m2m_table, klass->table, klass->primary_key);

   /*
    * TODO: We should make this join all the table hierarchy (with special
    *       generated table name using AS) so we can query on any of the
    *       joined fields.
    */

   do {
      m2m_klass = g_type_class_ref(m2m_type);
      if (!prefix) {
         prefix = g_strdup(m2m_klass->table);
      }
      g_string_append_printf(str, " JOIN '%s' AS '%s_%s' ON '%s_%s'.'%s' = '%s'.'%s:%s' ",
                             m2m_klass->table,
                             m2m_table, m2m_klass->table,
                             m2m_table, m2m_klass->table, m2m_klass->primary_key,
                             m2m_table, prefix, klass->primary_key);
      g_type_class_unref(m2m_klass);
   } while ((m2m_type = g_type_parent(m2m_type)) != GOM_TYPE_RESOURCE);

   g_free(prefix);
}

static void
build_map (GHashTable  *table_map,
           GType        type,
           const gchar *m2m_table)
{
   GomResourceClass *klass;
   const gchar *prefix;
   gchar *key;
   gchar *value;

   g_assert(table_map);
   g_assert(g_type_is_a(type, GOM_TYPE_RESOURCE));
   g_assert(type != GOM_TYPE_RESOURCE);

   prefix = g_type_name(type);

   do {
      klass = g_type_class_ref(type);
      key = g_strdup_printf("%s.%s", prefix, klass->table);
      value = g_strdup_printf("%s_%s", m2m_table, klass->table);
      g_hash_table_replace(table_map, key, value);
      g_type_class_unref(klass);
   } while ((type = g_type_parent(type)) != GOM_TYPE_RESOURCE);
}

static void
add_where (GString     *str,
           GType        m2m_type,
           const gchar *m2m_table,
           GomFilter   *filter)
{
   GHashTable *table_map = NULL;
   gchar *sql;

   if (filter) {
      if (m2m_type) {
         table_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
         build_map(table_map, m2m_type, m2m_table);
      }
      sql = gom_filter_get_sql(filter, table_map);
      g_string_append_printf(str, " WHERE %s ", sql);
      g_free(sql);
      if (table_map) {
         g_hash_table_destroy(table_map);
      }
   }
}

static void
add_limit (GString *str,
           guint    limit)
{
   if (limit) {
      g_string_append_printf(str, " LIMIT %u ", limit);
   }
}

static void
add_offset (GString *str,
            guint    offset)
{
   if (offset) {
      g_string_append_printf(str, " OFFSET %u ", offset);
   }
}

static void
bind_params (GomCommand *command,
             GomFilter  *filter)
{
   GValueArray *values;
   guint i;

   if (filter) {
      values = gom_filter_get_values(filter);
      for (i = 0; i < values->n_values; i++) {
         gom_command_set_param(command, i, g_value_array_get_nth(values, i));
      }
      g_value_array_free(values);
   }
}

/**
 * gom_command_builder_build_select:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Builds a #GomCommand that will select all the rows matching the current
 * query params.
 *
 * Returns: (transfer full): A #GomCommand.
 */
GomCommand *
gom_command_builder_build_select (GomCommandBuilder *builder)
{
   GomCommandBuilderPrivate *priv;
   GomResourceClass *klass;
   GomCommand *command;
   GString *str;

   g_return_val_if_fail(GOM_IS_COMMAND_BUILDER(builder), NULL);

   priv = builder->priv;

   klass = g_type_class_ref(priv->resource_type);

   str = g_string_new("SELECT ");
   add_fields(str, klass);
   add_from(str, klass);
   add_joins(str, klass);
   add_m2m(str, klass, priv->m2m_table, priv->m2m_type);
   add_where(str, priv->m2m_type, priv->m2m_table, priv->filter);
   add_limit(str, priv->limit);
   add_offset(str, priv->offset);

   command = g_object_new(GOM_TYPE_COMMAND,
                          "adapter", priv->adapter,
                          "sql", str->str,
                          NULL);

   bind_params(command, priv->filter);

   g_type_class_unref(klass);
   g_string_free(str, TRUE);

   return command;
}

/**
 * gom_command_builder_build_count:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Builds a new command that will count the number of rows matching the
 * current query parameters.
 *
 * Returns: (transfer full): A #GomCommand.
 */
GomCommand *
gom_command_builder_build_count (GomCommandBuilder *builder)
{
   GomCommandBuilderPrivate *priv;
   GomResourceClass *klass;
   GomCommand *command;
   GString *str;

   g_return_val_if_fail(GOM_IS_COMMAND_BUILDER(builder), NULL);

   priv = builder->priv;

   klass = g_type_class_ref(priv->resource_type);

   str = g_string_new(NULL);
   g_string_append_printf(str, "SELECT COUNT('%s'.'%s') ",
                          klass->table, klass->primary_key);
   add_from(str, klass);
   add_joins(str, klass);
   add_m2m(str, klass, priv->m2m_table, priv->m2m_type);
   add_where(str, priv->m2m_type, priv->m2m_table, priv->filter);
   add_limit(str, priv->limit);
   add_offset(str, priv->offset);

   command = g_object_new(GOM_TYPE_COMMAND,
                          "adapter", priv->adapter,
                          "sql", str->str,
                          NULL);

   bind_params(command, priv->filter);

   g_type_class_unref(klass);
   g_string_free(str, TRUE);

   return command;
}

/**
 * gom_command_builder_build_delete:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Builds a new #GomCommand to delete the rows matching the current query
 * params.
 *
 * Returns: (transfer full): A #GomCommand.
 */
GomCommand *
gom_command_builder_build_delete (GomCommandBuilder *builder)
{
   GomCommandBuilderPrivate *priv;
   GomResourceClass *klass;
   GomCommand *command;
   GString *str;

   g_return_val_if_fail(GOM_IS_COMMAND_BUILDER(builder), NULL);

   priv = builder->priv;

   klass = g_type_class_ref(priv->resource_type);

   str = g_string_new("DELETE ");
   add_from(str, klass);
   add_where(str, priv->m2m_type, priv->m2m_table, priv->filter);
   g_string_append(str, ";");

   command = g_object_new(GOM_TYPE_COMMAND,
                          "adapter", priv->adapter,
                          "sql", str->str,
                          NULL);

   bind_params(command, priv->filter);

   g_type_class_unref(klass);
   g_string_free(str, TRUE);

   return command;
}

static gboolean
do_prop_on_insert (GParamSpec       *pspec,
                   GomResourceClass *klass,
                   GType             resource_type)
{
#define IS_TOPLEVEL(t)        (g_type_parent((t)) == GOM_TYPE_RESOURCE)
#define IS_PRIMARY_KEY(p)     (!g_strcmp0((p)->name, klass->primary_key))
#define BELONGS_TO_TYPE(p, t) ((p)->owner_type == (t))

   return ((is_mapped(pspec)) &&
           (!(IS_TOPLEVEL(resource_type) && IS_PRIMARY_KEY(pspec))) &&
           (!(!IS_PRIMARY_KEY(pspec) && !BELONGS_TO_TYPE(pspec, resource_type))));

#undef IS_TOPLEVEL
#undef IS_PRIMARY_KEY
#undef BELONGS_TO_TYPE
}

/**
 * gom_command_builder_build_insert:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Builds a new #GomCommand that will insert the parameters of the resource
 * into the underlying database.
 *
 * Returns: (transfer full): A #GomCommand.
 */
GomCommand *
gom_command_builder_build_insert (GomCommandBuilder *builder,
                                  GomResource       *resource)
{
   GomCommandBuilderPrivate *priv;
   GomResourceClass *klass;
   GomCommand *command = NULL;
   GParamSpec **pspecs = NULL;
   gboolean did_pspec = FALSE;
   GString *str = NULL;
   guint n_pspecs = 0;
   guint i = 0;
   guint idx = 0;

   g_return_val_if_fail(GOM_IS_COMMAND_BUILDER(builder), NULL);

   priv = builder->priv;

   klass = g_type_class_ref(priv->resource_type);

   pspecs = g_object_class_list_properties(G_OBJECT_CLASS(klass), &n_pspecs);

   str = g_string_new("INSERT INTO ");
   g_string_append_printf(str, "%s (", klass->table);

   for (i = 0; i < n_pspecs; i++) {
      if (do_prop_on_insert(pspecs[i], klass, priv->resource_type)) {
         if (did_pspec) {
            g_string_append(str, ", ");
         }
         g_string_append_printf(str, "'%s'", pspecs[i]->name);
         did_pspec = TRUE;
      }
   }

   g_string_append(str, ") VALUES (");

   did_pspec = FALSE;

   for (i = 0; i < n_pspecs; i++) {
      if (do_prop_on_insert(pspecs[i], klass, priv->resource_type)) {
         if (did_pspec) {
            g_string_append(str, ", ");
         }
         g_string_append(str, "?");
         did_pspec = TRUE;
      }
   }

   g_string_append(str, ");");

   command = g_object_new(GOM_TYPE_COMMAND,
                          "adapter", priv->adapter,
                          "sql", str->str,
                          NULL);

   for (i = 0; i < n_pspecs; i++) {
      if (do_prop_on_insert(pspecs[i], klass, priv->resource_type)) {
         GValue value = { 0 };

         g_value_init(&value, pspecs[i]->value_type);
         g_object_get_property(G_OBJECT(resource), pspecs[i]->name, &value);
         gom_command_set_param(command, idx++, &value);
         g_value_unset(&value);
      }
   }

   g_type_class_unref(klass);
   if (str) {
      g_string_free(str, TRUE);
   }
   g_free(pspecs);

   return command;
}

/**
 * gom_command_builder_build_update:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Builds a new #GomCommand that will update the contents stored for @resource
 * in the underlying database.
 *
 * Returns: (transfer full): A #GomCommand.
 */
GomCommand *
gom_command_builder_build_update (GomCommandBuilder *builder,
                                  GomResource       *resource)
{
   GomCommandBuilderPrivate *priv;
   GomResourceClass *klass;
   GomCommand *command = NULL;
   GParamSpec **pspecs = NULL;
   gboolean did_pspec = FALSE;
   GString *str = NULL;
   guint n_pspecs = 0;
   guint i = 0;
   guint idx = 0;

   g_return_val_if_fail(GOM_IS_COMMAND_BUILDER(builder), NULL);

   priv = builder->priv;

   klass = g_type_class_ref(priv->resource_type);

   pspecs = g_object_class_list_properties(G_OBJECT_CLASS(klass), &n_pspecs);

   str = g_string_new("UPDATE ");
   g_string_append_printf(str, "%s SET ", klass->table);

   for (i = 0; i < n_pspecs; i++) {
      if (do_prop_on_insert(pspecs[i], klass, priv->resource_type)) {
         if (did_pspec) {
            g_string_append(str, ", ");
         }
         g_string_append_printf(str, "'%s' = ?", pspecs[i]->name);
         did_pspec = TRUE;
      }
   }

   g_string_append_printf(str, " WHERE '%s'.'%s' = ?;",
                          klass->table, klass->primary_key);

   command = g_object_new(GOM_TYPE_COMMAND,
                          "adapter", priv->adapter,
                          "sql", str->str,
                          NULL);

   for (i = 0; i < n_pspecs; i++) {
      if (do_prop_on_insert(pspecs[i], klass, priv->resource_type)) {
         GValue value = { 0 };

         g_value_init(&value, pspecs[i]->value_type);
         g_object_get_property(G_OBJECT(resource), pspecs[i]->name, &value);
         gom_command_set_param(command, idx++, &value);
         g_value_unset(&value);
      }
   }

   {
      GParamSpec *pspec;
      GValue value = { 0 };

      pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(resource),
                                           klass->primary_key);
      g_assert(pspec);

      g_value_init(&value, pspec->value_type);
      g_object_get_property(G_OBJECT(resource), pspec->name, &value);
      gom_command_set_param(command, idx++, &value);
      g_value_unset(&value);
   }

   g_type_class_unref(klass);

   if (str) {
      g_string_free(str, TRUE);
   }

   g_free(pspecs);

   return command;
}

/**
 * gom_command_builder_finalize:
 * @object: (in): A #GomCommandBuilder.
 *
 * Finalizer for a #GomCommandBuilder instance.  Frees any resources held by
 * the instance.
 */
static void
gom_command_builder_finalize (GObject *object)
{
   GomCommandBuilderPrivate *priv = GOM_COMMAND_BUILDER(object)->priv;

   g_clear_object(&priv->adapter);
   g_clear_object(&priv->filter);
   g_free(priv->m2m_table);

   G_OBJECT_CLASS(gom_command_builder_parent_class)->finalize(object);
}

/**
 * gom_command_builder_get_property:
 * @object: (in): A #GObject.
 * @prop_id: (in): The property identifier.
 * @value: (out): The given property.
 * @pspec: (in): A #ParamSpec.
 *
 * Get a given #GObject property.
 */
static void
gom_command_builder_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
   GomCommandBuilder *builder = GOM_COMMAND_BUILDER(object);

   switch (prop_id) {
   case PROP_ADAPTER:
      g_value_set_object(value, builder->priv->adapter);
      break;
   case PROP_FILTER:
      g_value_set_object(value, builder->priv->filter);
      break;
   case PROP_LIMIT:
      g_value_set_uint(value, builder->priv->limit);
      break;
   case PROP_M2M_TABLE:
      g_value_set_string(value, builder->priv->m2m_table);
      break;
   case PROP_M2M_TYPE:
      g_value_set_gtype(value, builder->priv->m2m_type);
      break;
   case PROP_OFFSET:
      g_value_set_uint(value, builder->priv->offset);
      break;
   case PROP_RESOURCE_TYPE:
      g_value_set_gtype(value, builder->priv->resource_type);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
   }
}

/**
 * gom_command_builder_set_property:
 * @object: (in): A #GObject.
 * @prop_id: (in): The property identifier.
 * @value: (in): The given property.
 * @pspec: (in): A #ParamSpec.
 *
 * Set a given #GObject property.
 */
static void
gom_command_builder_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
   GomCommandBuilder *builder = GOM_COMMAND_BUILDER(object);

   switch (prop_id) {
   case PROP_ADAPTER:
      builder->priv->adapter = g_value_dup_object(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_FILTER:
      g_clear_object(&builder->priv->filter);
      builder->priv->filter = g_value_dup_object(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_LIMIT:
      builder->priv->limit = g_value_get_uint(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_M2M_TABLE:
      builder->priv->m2m_table = g_value_dup_string(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_M2M_TYPE:
      builder->priv->m2m_type = g_value_get_gtype(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_OFFSET:
      builder->priv->offset = g_value_get_uint(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   case PROP_RESOURCE_TYPE:
      builder->priv->resource_type = g_value_get_gtype(value);
      g_object_notify_by_pspec(object, pspec);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
   }
}

/**
 * gom_command_builder_class_init:
 * @klass: (in): A #GomCommandBuilderClass.
 *
 * Initializes the #GomCommandBuilderClass and prepares the vtable.
 */
static void
gom_command_builder_class_init (GomCommandBuilderClass *klass)
{
   GObjectClass *object_class;

   object_class = G_OBJECT_CLASS(klass);
   object_class->finalize = gom_command_builder_finalize;
   object_class->get_property = gom_command_builder_get_property;
   object_class->set_property = gom_command_builder_set_property;
   g_type_class_add_private(object_class, sizeof(GomCommandBuilderPrivate));

   gParamSpecs[PROP_ADAPTER] =
      g_param_spec_object("adapter",
                          _("Adapter"),
                          _("The GomAdapter."),
                          GOM_TYPE_ADAPTER,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
   g_object_class_install_property(object_class, PROP_ADAPTER,
                                   gParamSpecs[PROP_ADAPTER]);

   gParamSpecs[PROP_FILTER] =
      g_param_spec_object("filter",
                          _("Filter"),
                          _("The filter for the command."),
                          GOM_TYPE_FILTER,
                          G_PARAM_READWRITE);
   g_object_class_install_property(object_class, PROP_FILTER,
                                   gParamSpecs[PROP_FILTER]);

   gParamSpecs[PROP_LIMIT] =
      g_param_spec_uint("limit",
                        _("Limit"),
                        _("The maximum number or results."),
                        0,
                        G_MAXUINT,
                        0,
                        G_PARAM_READWRITE);
   g_object_class_install_property(object_class, PROP_LIMIT,
                                   gParamSpecs[PROP_LIMIT]);

   gParamSpecs[PROP_M2M_TABLE] =
      g_param_spec_string("m2m-table",
                          _("Many-to-many table"),
                          _("The table to use for many-to-many queries."),
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
   g_object_class_install_property(object_class, PROP_M2M_TABLE,
                                   gParamSpecs[PROP_M2M_TABLE]);

   gParamSpecs[PROP_M2M_TYPE] =
      g_param_spec_gtype("m2m-type",
                          _("Many-to-many type"),
                          _("The type for the join within m2m-table."),
                          GOM_TYPE_RESOURCE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
   g_object_class_install_property(object_class, PROP_M2M_TYPE,
                                   gParamSpecs[PROP_M2M_TYPE]);

   gParamSpecs[PROP_OFFSET] =
      g_param_spec_uint("offset",
                        _("Offset"),
                        _("The number of results to skip."),
                        0,
                        G_MAXUINT,
                        0,
                        G_PARAM_READWRITE);
   g_object_class_install_property(object_class, PROP_OFFSET,
                                   gParamSpecs[PROP_OFFSET]);

   gParamSpecs[PROP_RESOURCE_TYPE] =
      g_param_spec_gtype("resource-type",
                         _("Resource Type"),
                         _("The resource type to query for."),
                         GOM_TYPE_RESOURCE,
                         G_PARAM_READWRITE);
   g_object_class_install_property(object_class, PROP_RESOURCE_TYPE,
                                   gParamSpecs[PROP_RESOURCE_TYPE]);
}

/**
 * gom_command_builder_init:
 * @builder: (in): A #GomCommandBuilder.
 *
 * Initializes the newly created #GomCommandBuilder instance.
 */
static void
gom_command_builder_init (GomCommandBuilder *builder)
{
   builder->priv =
      G_TYPE_INSTANCE_GET_PRIVATE(builder,
                                  GOM_TYPE_COMMAND_BUILDER,
                                  GomCommandBuilderPrivate);
}