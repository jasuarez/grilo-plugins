/*
 * Copyright (C) 2011 W. Michael Petullo.
 * Copyright (C) 2012 Igalia S.L.
 *
 * Contact: W. Michael Petullo <mike@flyn.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <grilo.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <libdmapsharing/dmap.h>

#include "grl-common.h"
#include "grl-dpap.h"
#include "grl-dpap-db.h"
#include "grl-dpap-record.h"
#include "grl-dpap-record-factory.h"

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT dmap_log_domain
GRL_LOG_DOMAIN_STATIC(dmap_log_domain);

/* --- Plugin information --- */

#define PLUGIN_ID   DPAP_PLUGIN_ID

#define SOURCE_ID_TEMPLATE   "grl-dpap-%s"
#define SOURCE_DESC_TEMPLATE _("A source for browsing the DPAP server '%s'")

/* --- Grilo DPAP Private --- */

#define GRL_DPAP_SOURCE_GET_PRIVATE(object)           \
  (G_TYPE_INSTANCE_GET_PRIVATE((object),              \
                               GRL_DPAP_SOURCE_TYPE,  \
                               GrlDpapSourcePrivate))

struct _GrlDpapSourcePrivate {
  DMAPMdnsBrowserService *service;
};

/* --- Data types --- */

static GrlDpapSource *grl_dpap_source_new (DMAPMdnsBrowserService *service);

static void grl_dpap_source_finalize (GObject *object);

gboolean grl_dpap_plugin_init (GrlRegistry *registry,
                               GrlPlugin *plugin,
                               GList *configs);

static const GList *grl_dpap_source_supported_keys (GrlSource *source);

static void grl_dpap_source_browse (GrlSource *source,
                                    GrlSourceBrowseSpec *bs);

static void grl_dpap_source_search (GrlSource *source,
                                    GrlSourceSearchSpec *ss);


static void grl_dpap_service_added_cb (DMAPMdnsBrowser *browser,
                                       DMAPMdnsBrowserService *service,
                                       GrlPlugin *plugin);

static void grl_dpap_service_removed_cb (DMAPMdnsBrowser *browser,
                                         const gchar *service_name,
                                         GrlPlugin *plugin);

/* ===================== Globals  ======================= */
static DMAPMdnsBrowser *browser;
/* Maps URIs to DBs */
static GHashTable *connections;
/* Map DPAP services to Grilo media sources */
static GHashTable *sources;

/* =================== DPAP Plugin ====================== */

gboolean
grl_dpap_plugin_init (GrlRegistry *registry,
                      GrlPlugin *plugin,
                      GList *configs)
{
  GError *error = NULL;

  GRL_LOG_DOMAIN_INIT (dmap_log_domain, "dmap");

  GRL_DEBUG ("dmap_plugin_init");

  /* Initialize i18n */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  browser     = dmap_mdns_browser_new (DMAP_MDNS_BROWSER_SERVICE_TYPE_DPAP);
  connections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  sources     = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  g_signal_connect (G_OBJECT (browser),
                    "service-added",
                    G_CALLBACK (grl_dpap_service_added_cb),
                    (gpointer) plugin);

  g_signal_connect (G_OBJECT (browser),
                    "service-removed",
                    G_CALLBACK (grl_dpap_service_removed_cb),
                    (gpointer) plugin);

  if (!dmap_mdns_browser_start (browser, &error)) {
    GRL_DEBUG ("error starting browser. code: %d message: %s",
               error->code,
               error->message);
    g_error_free (error);

    g_hash_table_unref (connections);
    g_hash_table_unref (sources);
    g_object_unref (browser);
    return FALSE;
  }

  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_dpap_plugin_init,
                     NULL,
                     PLUGIN_ID);

/* ================== DMAP GObject ====================== */

G_DEFINE_TYPE (GrlDpapSource,
               grl_dpap_source,
               GRL_TYPE_SOURCE);

static GrlDpapSource *
grl_dpap_source_new (DMAPMdnsBrowserService *service)
{
  gchar *source_desc;
  gchar *source_id;

  GrlDpapSource *source;

  GRL_DEBUG ("grl_dpap_source_new");

  source_desc = g_strdup_printf (SOURCE_DESC_TEMPLATE, service->name);
  source_id = g_strdup_printf (SOURCE_ID_TEMPLATE, service->name);

  source = g_object_new (GRL_DPAP_SOURCE_TYPE,
                         "source-id",   source_id,
                         "source-name", service->name,
                         "source-desc", source_desc,
                         NULL);

  source->priv->service = service;

  g_free (source_desc);
  g_free (source_id);

  return source;
}

static void
grl_dpap_source_class_init (GrlDpapSourceClass * klass)
{
  GrlSourceClass *source_class = GRL_SOURCE_CLASS (klass);

  source_class->browse = grl_dpap_source_browse;
  source_class->search = grl_dpap_source_search;
  source_class->supported_keys = grl_dpap_source_supported_keys;

  G_OBJECT_CLASS (source_class)->finalize = grl_dpap_source_finalize;

  g_type_class_add_private (klass, sizeof (GrlDpapSourcePrivate));
}

static void
grl_dpap_source_init (GrlDpapSource *source)
{
  source->priv = GRL_DPAP_SOURCE_GET_PRIVATE (source);
}

static void
grl_dpap_source_finalize (GObject *object)
{
  G_OBJECT_CLASS (grl_dpap_source_parent_class)->finalize (object);
}

/* ======================= Utilities ==================== */

static void
grl_dpap_do_browse (ResultCbAndArgsAndDb *cb_and_db)
{
  grl_dpap_db_browse(GRL_DPAP_DB(cb_and_db->db),
                     cb_and_db->cb.container,
                     cb_and_db->cb.source,
                     cb_and_db->cb.op_id,
                     cb_and_db->cb.skip,
                     cb_and_db->cb.count,
                     cb_and_db->cb.callback,
                     cb_and_db->cb.user_data);

  g_free (cb_and_db);
}

static void
grl_dpap_do_search (ResultCbAndArgsAndDb *cb_and_db)
{
  grl_dpap_db_search(GRL_DPAP_DB(cb_and_db->db),
                     cb_and_db->cb.source,
                     cb_and_db->cb.op_id,
                     (GHRFunc) cb_and_db->cb.predicate,
                     cb_and_db->cb.predicate_data,
                     cb_and_db->cb.callback,
                     cb_and_db->cb.user_data);

  g_free (cb_and_db);
}

static void
browse_connected_cb (DMAPConnection       *connection,
                     gboolean              result,
                     const char           *reason,
                     ResultCbAndArgsAndDb *cb_and_db)
{
  GError *error;

  /* NOTE: connection argument is required by API but ignored in this case. */
  if (!result) {
    error = g_error_new_literal (GRL_CORE_ERROR,
                                 GRL_CORE_ERROR_BROWSE_FAILED,
                                 reason);
    cb_and_db->cb.callback (cb_and_db->cb.source,
                            cb_and_db->cb.op_id,
                            NULL,
                            0,
                            cb_and_db->cb.user_data,
                            error);
    g_error_free (error);
  } else {
    grl_dpap_do_browse (cb_and_db);
  }
}

static void
search_connected_cb (DMAPConnection       *connection,
                     gboolean              result,
                     const char           *reason,
                     ResultCbAndArgsAndDb *cb_and_db)
{
  GError *error;

  /* NOTE: connection argument is required by API but ignored in this case. */
  if (!result) {
    error = g_error_new_literal (GRL_CORE_ERROR,
                                 GRL_CORE_ERROR_BROWSE_FAILED,
                                 reason);
    cb_and_db->cb.callback (cb_and_db->cb.source,
                            cb_and_db->cb.op_id,
                            NULL,
                            0,
                            cb_and_db->cb.user_data,
                            error);
    g_error_free (error);
  } else {
    grl_dpap_do_search (cb_and_db);
  }
}

static void
grl_dpap_service_added_cb (DMAPMdnsBrowser *browser,
                           DMAPMdnsBrowserService *service,
                           GrlPlugin *plugin)
{
  GrlRegistry   *registry = grl_registry_get_default ();
  GrlDpapSource *source   = grl_dpap_source_new (service);

  GRL_DEBUG (__FUNCTION__);

  g_object_add_weak_pointer (G_OBJECT (source), (gpointer *) &source);
  grl_registry_register_source (registry,
                                plugin,
                                GRL_SOURCE (source),
                                NULL);
  if (source != NULL) {
    g_hash_table_insert (sources, g_strdup (service->name), g_object_ref (source));
    g_object_remove_weak_pointer (G_OBJECT (source), (gpointer *) &source);
  }
}

static void
grl_dpap_service_removed_cb (DMAPMdnsBrowser *browser,
                             const gchar *service_name,
                             GrlPlugin *plugin)
{
  GrlRegistry   *registry = grl_registry_get_default ();
  GrlDpapSource *source   = g_hash_table_lookup (sources, service_name);

  GRL_DEBUG (__FUNCTION__);

  if (source) {
    grl_registry_unregister_source (registry, GRL_SOURCE (source), NULL);
    g_hash_table_remove (sources, service_name);
  }
}

static void
grl_dpap_connect (gchar *name, gchar *host, guint port, ResultCbAndArgsAndDb *cb_and_db, DMAPConnectionCallback callback)
{
  DMAPRecordFactory *factory;
  DMAPConnection *connection;

  factory = DMAP_RECORD_FACTORY (grl_dpap_record_factory_new ());
  connection = DMAP_CONNECTION (dpap_connection_new (name, host, port, DMAP_DB (cb_and_db->db), factory));
  dmap_connection_connect (connection, (DMAPConnectionCallback) callback, cb_and_db);
}

static gboolean
grl_dpap_match (GrlMedia *media, gpointer val, gpointer user_data)
{
  g_assert (GRL_IS_MEDIA_IMAGE (media));

  if (user_data == NULL)
    return TRUE;

  const char *title = grl_media_get_title (media);
  return strstr (title, user_data) != NULL;
}

/* ================== API Implementation ================ */

static const GList *
grl_dpap_source_supported_keys (GrlSource *source)
{
  static GList *keys = NULL;

  GRL_DEBUG (__func__);

  if (!keys)
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_HEIGHT,
                                      GRL_METADATA_KEY_THUMBNAIL,
                                      GRL_METADATA_KEY_WIDTH,
                                      GRL_METADATA_KEY_ID,
                                      GRL_METADATA_KEY_URL,
                                      NULL);

  return keys;
}

static void
grl_dpap_source_browse (GrlSource *source,
                        GrlSourceBrowseSpec *bs)
{
  GrlDpapSource *dmap_source = GRL_DPAP_SOURCE (source);
  gchar *url = grl_dmap_build_url (dmap_source->priv->service);

  GRL_DEBUG (__func__);

  ResultCbAndArgsAndDb *cb_and_db;

  cb_and_db = g_new (ResultCbAndArgsAndDb, 1);

  cb_and_db->cb.callback       = bs->callback;
  cb_and_db->cb.source         = bs->source;
  cb_and_db->cb.container      = bs->container;
  cb_and_db->cb.op_id          = bs->operation_id;
  cb_and_db->cb.skip           = grl_operation_options_get_skip (bs->options);
  cb_and_db->cb.count          = grl_operation_options_get_count (bs->options);
  cb_and_db->cb.user_data      = bs->user_data;

  if ((cb_and_db->db = g_hash_table_lookup (connections, url))) {
    /* Just call directly; already connected, already populated database. */
    browse_connected_cb (NULL, TRUE, NULL, cb_and_db);
  } else {
    /* Connect */
    cb_and_db->db = DMAP_DB (grl_dpap_db_new ());

    grl_dpap_connect (dmap_source->priv->service->name,
                      dmap_source->priv->service->host,
                      dmap_source->priv->service->port,
                      cb_and_db,
                      (DMAPConnectionCallback) browse_connected_cb);

    g_hash_table_insert (connections, g_strdup (url), cb_and_db->db);
  }

  g_free (url);
}

static void grl_dpap_source_search (GrlSource *source,
                                    GrlSourceSearchSpec *ss)
{
  GrlDpapSource *dmap_source = GRL_DPAP_SOURCE (source);

  ResultCbAndArgsAndDb *cb_and_db;
  DMAPMdnsBrowserService *service = dmap_source->priv->service;
  gchar *url = grl_dmap_build_url (service);

  cb_and_db = g_new (ResultCbAndArgsAndDb, 1);

  cb_and_db->cb.callback       = ss->callback;
  cb_and_db->cb.source         = ss->source;
  cb_and_db->cb.container      = NULL;
  cb_and_db->cb.op_id          = ss->operation_id;
  cb_and_db->cb.predicate      = (GHRFunc) grl_dpap_match;
  cb_and_db->cb.predicate_data = ss->text;
  cb_and_db->cb.user_data      = ss->user_data;

  if ((cb_and_db->db = g_hash_table_lookup (connections, url))) {
    /* Just call directly; already connected, already populated database */
    search_connected_cb (NULL, TRUE, NULL, cb_and_db);
  } else {
    /* Connect */
    cb_and_db->db = DMAP_DB (grl_dpap_db_new ());
    grl_dpap_connect (service->name, service->host, service->port, cb_and_db, (DMAPConnectionCallback) search_connected_cb);
    g_hash_table_insert (connections, g_strdup (url), cb_and_db->db);
  }

  g_free (url);
}