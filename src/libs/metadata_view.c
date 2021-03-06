/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <sys/param.h>
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif

#define SHOW_FLAGS 1

DT_MODULE(1)

typedef enum dt_metadata_pref_cols_t
{
  DT_METADATA_PREF_COL_INDEX = 0, // index
  DT_METADATA_PREF_COL_NAME_L,    // displayed name
  DT_METADATA_PREF_COL_VISIBLE,   // visibility
  DT_METADATA_PREF_NUM_COLS
} dt_metadata_pref_cols_t;

typedef struct dt_lib_metadata_view_t
{
  GtkWidget *grid;
  GList *metadata;
  GObject *filmroll_event;
} dt_lib_metadata_view_t;

typedef struct dt_lib_metadata_info_t
{
  int index;          // md_xx value or index inserted by lua
  int order;          // display order
  char *name;         // metadata name
  char *value;        // metadata value
  char *tooltip;      // tooltip
  gboolean visible;
} dt_lib_metadata_info_t;

enum
{
  /* internal */
  md_internal_filmroll = 0,
  md_internal_imgid,
  md_internal_groupid,
  md_internal_filename,
  md_internal_version,
  md_internal_fullpath,
  md_internal_local_copy,
  md_internal_import_timestamp,
  md_internal_change_timestamp,
  md_internal_export_timestamp,
  md_internal_print_timestamp,
#if SHOW_FLAGS
  md_internal_flags,
#endif

  /* exif */
  md_exif_model,
  md_exif_maker,
  md_exif_lens,
  md_exif_aperture,
  md_exif_exposure,
  md_exif_exposure_bias,
  md_exif_focal_length,
  md_exif_focus_distance,
  md_exif_iso,
  md_exif_datetime,
  md_exif_width,
  md_exif_height,

  /* size of final image */
  md_width,
  md_height,

  /* xmp */
  md_xmp_metadata,

  /* geotagging */
  md_geotagging_lat = md_xmp_metadata + DT_METADATA_NUMBER,
  md_geotagging_lon,
  md_geotagging_ele,

  /* tags */
  md_tag_names,
  md_categories,

  /* entries, do not touch! */
  md_size
};

static const char *_labels[] = {
  /* internal */
  N_("filmroll"),
  N_("image id"),
  N_("group id"),
  N_("filename"),
  N_("version"),
  N_("full path"),
  N_("local copy"),
  N_("import timestamp"),
  N_("change timestamp"),
  N_("export timestamp"),
  N_("print timestamp"),
#if SHOW_FLAGS
  N_("flags"),
#endif

  /* exif */
  N_("model"),
  N_("maker"),
  N_("lens"),
  N_("aperture"),
  N_("exposure"),
  N_("exposure bias"),
  N_("focal length"),
  N_("focus distance"),
  N_("ISO"),
  N_("datetime"),
  N_("width"),
  N_("height"),
  N_("export width"),
  N_("export height"),

  /* xmp */
  //FIXME: reserve DT_METADATA_NUMBER places
  "","","","","","","",

  /* geotagging */
  N_("latitude"),
  N_("longitude"),
  N_("elevation"),

  /* tags */
  N_("tags"),
  N_("categories"),
};

static gboolean _dndactive = FALSE;

const char *name(dt_lib_module_t *self)
{
  return _("image information");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 299;
}

static gboolean _is_metadata_ui(const int i)
{
  // internal metadata ar not to be shown on the ui
  if(i >= md_xmp_metadata && i < md_xmp_metadata + DT_METADATA_NUMBER)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    return !(dt_metadata_get_type(keyid) == DT_METADATA_TYPE_INTERNAL);
  }
  else return TRUE;
}

static const char *_get_label(const int i)
{
  if(i >= md_xmp_metadata && i < md_xmp_metadata + DT_METADATA_NUMBER)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i - md_xmp_metadata);
    return(dt_metadata_get_name(keyid));
  }
  else return _labels[i];
}

#define NODATA_STRING "-"

// initialize the metadata queue
static void _lib_metadata_init_queue(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  d->metadata = NULL;
  for(int i = md_size - 1; i >= 0; i--)
  {
    dt_lib_metadata_info_t *m = g_malloc0(sizeof(dt_lib_metadata_info_t));
    m->name = (char *)_get_label(i);
    m->value = g_strdup(NODATA_STRING);
    m->index = m->order = i;
    m->visible = _is_metadata_ui(i);
    d->metadata = g_list_prepend(d->metadata, m);
  }
}

// helper which eliminates non-printable characters from a string
// strings which are already in valid UTF-8 are retained.
static void _filter_non_printable(char *string, size_t length)
{
  /* explicitly tell the validator to ignore the trailing nulls, otherwise this fails */
  if(g_utf8_validate(string, -1, 0)) return;

  unsigned char *str = (unsigned char *)string;
  int n = 0;

  while(*str != '\000' && n < length)
  {
    if((*str < 0x20) || (*str >= 0x7f)) *str = '.';

    str++;
    n++;
  }
}

static dt_lib_metadata_info_t *_get_metadata_per_index(const int index, dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  for(GList *meta = d->metadata; meta; meta = g_list_next(meta))
  {
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    if(m->index == index)
    {
      return m;
    }
  }
  return NULL;
}

/* helper function for updating a metadata value */
static void _metadata_update_value(const int i, const char *value, dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  dt_lib_metadata_info_t *m = _get_metadata_per_index(i, self);
  if(m)
  {
    if(m->value) g_free(m->value);
    m->value = g_strdup(str);
    GtkWidget *w_value = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, m->order);
    gtk_label_set_text(GTK_LABEL(w_value), str);
    const char *tooltip = m->tooltip ? m->tooltip : m->value;
    gtk_widget_set_tooltip_text(GTK_WIDGET(w_value), tooltip);
  }
}

static void _metadata_update_tooltip(const int i, const char *tooltip, dt_lib_module_t *self)
{
  dt_lib_metadata_info_t *m = _get_metadata_per_index(i, self);
  if(m)
  {
    if(m->tooltip) g_free(m->tooltip);
    m->tooltip = g_strdup(tooltip);
  }
}

static void _metadata_update_timestamp(const int i, const time_t *value, dt_lib_module_t *self)
{
  char datetime[200];
  struct tm tm_val;
  // just %c is too long and includes a time zone that we don't know from exif
  const size_t datetime_len = strftime(datetime, sizeof(datetime), "%a %x %X", localtime_r(value, &tm_val));
  if(datetime_len > 0)
  {
    const gboolean valid_utf = g_utf8_validate(datetime, datetime_len, NULL);
    if(valid_utf)
    {
      _metadata_update_value(i, datetime, self);
    }
    else
    {
      GError *error = NULL;
      gchar *local_datetime = g_locale_to_utf8(datetime,datetime_len,NULL,NULL, &error);
      if(local_datetime)
      {
        _metadata_update_value(i, local_datetime, self);
        g_free(local_datetime);
      }
      else
      {
        _metadata_update_value(i, NODATA_STRING, self);
        fprintf(stderr, "[metadata timestamp] could not convert '%s' to UTF-8: %s\n", datetime, error->message);
        g_error_free(error);
      }
    }
  }
  else
    _metadata_update_value(i, NODATA_STRING, self);
}

static gint _lib_metadata_sort_order(gconstpointer a, gconstpointer b)
{
  dt_lib_metadata_info_t *ma = (dt_lib_metadata_info_t *)a;
  dt_lib_metadata_info_t *mb = (dt_lib_metadata_info_t *)b;
  return ma->order - mb->order;
}

#ifdef USE_LUA
static int lua_update_metadata(lua_State*L);
#endif
/* update all values to reflect mouse over image id or no data at all */
static void _metadata_view_update_values(dt_lib_module_t *self)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(mouse_over_id == -1)
  {
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    {
      mouse_over_id = darktable.develop->image_storage.id;
    }
    else
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images LIMIT 1",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) mouse_over_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }

  if(mouse_over_id >= 0)
  {
    char value[512];
    char pathname[PATH_MAX] = { 0 };

    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    if(!img) goto fill_minuses;
    if(img->film_id == -1)
    {
      dt_image_cache_read_release(darktable.image_cache, img);
      goto fill_minuses;
    }

    /* update all metadata */

    dt_image_film_roll(img, value, sizeof(value));
    char tooltip[512];
    snprintf(tooltip, sizeof(tooltip), _("double click to jump to film roll\n%s"), value);
    _metadata_update_tooltip(md_internal_filmroll, tooltip, self);
    _metadata_update_value(md_internal_filmroll, value, self);

    snprintf(value, sizeof(value), "%d", img->id);
    _metadata_update_value(md_internal_imgid, value, self);

    snprintf(value, sizeof(value), "%d", img->group_id);
    _metadata_update_value(md_internal_groupid, value, self);

    _metadata_update_value(md_internal_filename, img->filename, self);

    snprintf(value, sizeof(value), "%d", img->version);
    _metadata_update_value(md_internal_version, value, self);

    gboolean from_cache = FALSE;
    dt_image_full_path(img->id, pathname, sizeof(pathname), &from_cache);
    _metadata_update_value(md_internal_fullpath, pathname, self);

    g_strlcpy(value, (img->flags & DT_IMAGE_LOCAL_COPY) ? _("yes") : _("no"), sizeof(value));
    _metadata_update_value(md_internal_local_copy, value, self);

    if (img->import_timestamp >=0)
      _metadata_update_timestamp(md_internal_import_timestamp, &img->import_timestamp, self);
    else
      _metadata_update_value(md_internal_import_timestamp, NODATA_STRING, self);

    if (img->change_timestamp >=0)
      _metadata_update_timestamp(md_internal_change_timestamp, &img->change_timestamp, self);
    else
      _metadata_update_value(md_internal_change_timestamp, NODATA_STRING, self);

    if (img->export_timestamp >=0)
      _metadata_update_timestamp(md_internal_export_timestamp, &img->export_timestamp, self);
    else
      _metadata_update_value(md_internal_export_timestamp, NODATA_STRING, self);

    if (img->print_timestamp >=0)
      _metadata_update_timestamp(md_internal_print_timestamp, &img->print_timestamp, self);
    else
      _metadata_update_value(md_internal_print_timestamp, NODATA_STRING, self);

    // TODO: decide if this should be removed for a release. maybe #ifdef'ing to only add it to git compiles?

    // the bits of the flags
#if SHOW_FLAGS
    {
      #define EMPTY_FIELD '.'
      #define FALSE_FIELD '.'
      #define TRUE_FIELD '!'

      char *flags_tooltip = NULL;
      char *flag_descriptions[] = { N_("unused"),
                                    N_("unused/deprecated"),
                                    N_("ldr"),
                                    N_("raw"),
                                    N_("hdr"),
                                    N_("marked for deletion"),
                                    N_("auto-applying presets applied"),
                                    N_("legacy flag. set for all new images"),
                                    N_("local copy"),
                                    N_("has .txt"),
                                    N_("has .wav"),
                                    N_("monochrome")
      };
      char *tooltip_parts[15] = { 0 };
      int next_tooltip_part = 0;

      memset(value, EMPTY_FIELD, sizeof(value));

      int stars = img->flags & 0x7;
      char *star_string = NULL;
      if(stars == 6)
      {
        value[0] = 'x';
        tooltip_parts[next_tooltip_part++] = _("image rejected");
      }
      else
      {
        value[0] = '0' + stars;
        tooltip_parts[next_tooltip_part++] = star_string = g_strdup_printf(ngettext("image has %d star", "image has %d stars", stars), stars);
      }


      if(img->flags & 8)
      {
        value[1] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[0]);
      }
      else
        value[1] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_THUMBNAIL_DEPRECATED)
      {
        value[2] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[1]);
      }
      else
        value[2] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_LDR)
      {
        value[3] = 'l';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[2]);
      }

      if(img->flags & DT_IMAGE_RAW)
      {
        value[4] = 'r';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[3]);
      }

      if(img->flags & DT_IMAGE_HDR)
      {
        value[5] = 'h';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[4]);
      }

      if(img->flags & DT_IMAGE_REMOVE)
      {
        value[6] = 'd';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[5]);
      }

      if(img->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)
      {
        value[7] = 'a';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[6]);
      }

      if(img->flags & DT_IMAGE_NO_LEGACY_PRESETS)
      {
        value[8] = 'p';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[7]);
      }

      if(img->flags & DT_IMAGE_LOCAL_COPY)
      {
        value[9] = 'c';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[8]);
      }

      if(img->flags & DT_IMAGE_HAS_TXT)
      {
        value[10] = 't';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[9]);
      }

      if(img->flags & DT_IMAGE_HAS_WAV)
      {
        value[11] = 'w';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[10]);
      }

      if(dt_image_monochrome_flags(img))
      {
        value[12] = 'm';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[11]);
      }

      static const struct
      {
        char *tooltip;
        char flag;
      } loaders[] =
      {
        { N_("unknown"), EMPTY_FIELD},
        { N_("tiff"), 't'},
        { N_("png"), 'p'},
        { N_("j2k"), 'J'},
        { N_("jpeg"), 'j'},
        { N_("exr"), 'e'},
        { N_("rgbe"), 'R'},
        { N_("pfm"), 'P'},
        { N_("GraphicsMagick"), 'g'},
        { N_("rawspeed"), 'r'},
        { N_("netpnm"), 'n'},
        { N_("avif"), 'a'},
      };

      const int loader = (unsigned int)img->loader < sizeof(loaders) / sizeof(*loaders) ? img->loader : 0;
      value[13] = loaders[loader].flag;
      char *loader_tooltip = g_strdup_printf(_("loader: %s"), _(loaders[loader].tooltip));
      tooltip_parts[next_tooltip_part++] = loader_tooltip;

      value[14] = '\0';

      flags_tooltip = g_strjoinv("\n", tooltip_parts);
      g_free(loader_tooltip);
      _metadata_update_tooltip(md_internal_flags, flags_tooltip, self);
      _metadata_update_value(md_internal_flags, value, self);

      g_free(star_string);
      g_free(flags_tooltip);

      #undef EMPTY_FIELD
      #undef FALSE_FIELD
      #undef TRUE_FIELD
    }
#endif // SHOW_FLAGS

    /* EXIF */
    _metadata_update_value(md_exif_model, img->camera_alias, self);
    _metadata_update_value(md_exif_lens, img->exif_lens, self);
    _metadata_update_value(md_exif_maker, img->camera_maker, self);

    snprintf(value, sizeof(value), "f/%.1f", img->exif_aperture);
    _metadata_update_value(md_exif_aperture, value, self);

    char *exposure_str = dt_util_format_exposure(img->exif_exposure);
    _metadata_update_value(md_exif_exposure, exposure_str, self);
    g_free(exposure_str);

    if(isnan(img->exif_exposure_bias))
    {
      _metadata_update_value(md_exif_exposure_bias, NODATA_STRING, self);
    }
    else
    {
      snprintf(value, sizeof(value), _("%+.2f EV"), img->exif_exposure_bias);
      _metadata_update_value(md_exif_exposure_bias, value, self);
    }

    snprintf(value, sizeof(value), "%.0f mm", img->exif_focal_length);
    _metadata_update_value(md_exif_focal_length, value, self);

    if(isnan(img->exif_focus_distance) || fpclassify(img->exif_focus_distance) == FP_ZERO)
    {
      _metadata_update_value(md_exif_focus_distance, NODATA_STRING, self);
    }
    else
    {
      snprintf(value, sizeof(value), "%.2f m", img->exif_focus_distance);
      _metadata_update_value(md_exif_focus_distance, value, self);
    }

    snprintf(value, sizeof(value), "%.0f", img->exif_iso);
    _metadata_update_value(md_exif_iso, value, self);

    struct tm tt_exif = { 0 };
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
      &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
    {
      tt_exif.tm_year -= 1900;
      tt_exif.tm_mon--;
      tt_exif.tm_isdst = -1;
      const time_t exif_timestamp = mktime(&tt_exif);
      _metadata_update_timestamp(md_exif_datetime, &exif_timestamp, self);
    }
    else
      _metadata_update_value(md_exif_datetime, img->exif_datetime_taken, self);

    if(((img->p_width != img->width) || (img->p_height != img->height))  &&
       (img->p_width || img->p_height))
    {
      snprintf(value, sizeof(value), "%d (%d)", img->p_height, img->height);
      _metadata_update_value(md_exif_height, value, self);
      snprintf(value, sizeof(value), "%d (%d) ",img->p_width, img->width);
      _metadata_update_value(md_exif_width, value, self);
    }
    else {
    snprintf(value, sizeof(value), "%d", img->height);
    _metadata_update_value(md_exif_height, value, self);
    snprintf(value, sizeof(value), "%d", img->width);
    _metadata_update_value(md_exif_width, value, self);
    }

    if(img->verified_size)
    {
      snprintf(value, sizeof(value), "%d", img->final_height);
    _metadata_update_value(md_height, value, self);
      snprintf(value, sizeof(value), "%d", img->final_width);
    _metadata_update_value(md_width, value, self);
    }
    else
    {
      _metadata_update_value(md_height, NODATA_STRING, self);
      _metadata_update_value(md_width, NODATA_STRING, self);
    }
    /* XMP */
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
      const gchar *key = dt_metadata_get_key(keyid);
      const gboolean hidden = dt_metadata_get_type(keyid) == DT_METADATA_TYPE_INTERNAL;
      if(hidden)
      {
        g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      else
      {
        GList *res = dt_metadata_get(img->id, key, NULL);
        if(res)
        {
          g_strlcpy(value, (char *)res->data, sizeof(value));
          _filter_non_printable(value, sizeof(value));
          g_list_free_full(res, &g_free);
        }
        else
          g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      _metadata_update_value(md_xmp_metadata+i, value, self);
    }

    /* geotagging */
    /* latitude */
    if(isnan(img->geoloc.latitude))
    {
      _metadata_update_value(md_geotagging_lat, NODATA_STRING, self);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *latitude = dt_util_latitude_str(img->geoloc.latitude);
        _metadata_update_value(md_geotagging_lat, latitude, self);
        g_free(latitude);
      }
      else
      {
        const gchar NS = img->geoloc.latitude < 0 ? 'S' : 'N';
        snprintf(value, sizeof(value), "%c %09.6f", NS, fabs(img->geoloc.latitude));
        _metadata_update_value(md_geotagging_lat, value, self);
      }
    }
    /* longitude */
    if(isnan(img->geoloc.longitude))
    {
      _metadata_update_value(md_geotagging_lon, NODATA_STRING, self);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *longitude = dt_util_longitude_str(img->geoloc.longitude);
        _metadata_update_value(md_geotagging_lon, longitude, self);
        g_free(longitude);
      }
      else
      {
        const gchar EW = img->geoloc.longitude < 0 ? 'W' : 'E';
        snprintf(value, sizeof(value), "%c %010.6f", EW, fabs(img->geoloc.longitude));
        _metadata_update_value(md_geotagging_lon, value, self);
      }
    }
    /* elevation */
    if(isnan(img->geoloc.elevation))
    {
      _metadata_update_value(md_geotagging_ele, NODATA_STRING, self);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *elevation = dt_util_elevation_str(img->geoloc.elevation);
        _metadata_update_value(md_geotagging_ele, elevation, self);
        g_free(elevation);
      }
      else
      {
        snprintf(value, sizeof(value), "%.2f %s", img->geoloc.elevation, _("m"));
        _metadata_update_value(md_geotagging_ele, value, self);
      }
    }

    /* tags */
    GList *tags = NULL;
    char *tagstring = NULL;
    char *categoriesstring = NULL;
    if(dt_tag_get_attached(mouse_over_id, &tags, TRUE))
    {
      gint length = 0;
      for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
      {
        const char *tagname = ((dt_tag_t *)taglist->data)->leave;
        if (!(((dt_tag_t *)taglist->data)->flags & DT_TF_CATEGORY))
        {
          // tags - just keywords
          length = length + strlen(tagname) + 2;
          if(length < 45)
            tagstring = dt_util_dstrcat(tagstring, "%s, ", tagname);
          else
          {
            tagstring = dt_util_dstrcat(tagstring, "\n%s, ", tagname);
            length = strlen(tagname) + 2;
          }
        }
        else
        {
          // categories - needs parent category to make sense
          char *category = g_strdup(((dt_tag_t *)taglist->data)->tag);
          char *catend = g_strrstr(category, "|");
          if (catend)
          {
            catend[0] = '\0';
            char *catstart = g_strrstr(category, "|");
            catstart = catstart ? catstart + 1 : category;
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s: %s " : "%s: %s ",
                  catstart, ((dt_tag_t *)taglist->data)->leave);
          }
          else
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s" : "%s",
                  ((dt_tag_t *)taglist->data)->leave);
          g_free(category);
        }
      }
      if(tagstring) tagstring[strlen(tagstring)-2] = '\0';
    }
    _metadata_update_value(md_tag_names, tagstring ? tagstring : NODATA_STRING, self);
    _metadata_update_value(md_categories, categoriesstring ? categoriesstring : NODATA_STRING, self);

    g_free(tagstring);
    g_free(categoriesstring);
    dt_tag_free_result(&tags);

    /* release img */
    dt_image_cache_read_release(darktable.image_cache, img);

#ifdef USE_LUA
    dt_lua_async_call_alien(lua_update_metadata,
        0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",mouse_over_id,LUA_ASYNC_DONE);
#endif
  }

  return;

/* reset */
fill_minuses:
  for(int k = 0; k < md_size; k++) _metadata_update_value(k, NODATA_STRING, self);
#ifdef USE_LUA
  dt_lua_async_call_alien(lua_update_metadata,
      0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",-1,LUA_ASYNC_DONE);
#endif
}

static void _jump_to()
{
  int32_t imgid = dt_control_get_mouse_over_id();
  if(imgid == -1)
  {
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);

    if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(imgid != -1)
  {
    char path[512];
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    dt_image_film_roll_directory(img, path, sizeof(path));
    dt_image_cache_read_release(darktable.image_cache, img);
    char collect[1024];
    snprintf(collect, sizeof(collect), "1:0:0:%s$", path);
    dt_collection_deserialize(collect);
  }
}

static gboolean _filmroll_clicked(GtkWidget *widget, GdkEventButton *event, gpointer null)
{
  if(event->type != GDK_2BUTTON_PRESS) return FALSE;
  _jump_to();
  return TRUE;
}

static gboolean _jump_to_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  _jump_to();
  return TRUE;
}

/* callback for the mouse over image change signal */
static void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(dt_control_running()) _metadata_view_update_values(self);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "jump to film roll"), GDK_KEY_j, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_jump_to_accel), (gpointer)self, NULL);
  dt_accel_connect_lib(self, "jump to film roll", closure);
}

static char *_get_current_configuration(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  char *pref = NULL;

  d->metadata = g_list_sort(d->metadata, _lib_metadata_sort_order);
  for(GList *meta = d->metadata; meta; meta= g_list_next(meta))
  {
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    if(_is_metadata_ui(m->index))
      pref = dt_util_dstrcat(pref, "%s%s,", m->visible ? "" : "|", m->name);
  }
  if(pref)
  {
    pref[strlen(pref) - 1] = '\0';
  }
  return pref;
}

static void _lib_metadata_refill_grid(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  d->metadata = g_list_sort(d->metadata, _lib_metadata_sort_order);

  int j = 0;
  // initialize the grid with metadata queue content
  for(GList *meta = d->metadata; meta; meta = g_list_next(meta))
  {
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    GtkWidget *w_name = gtk_grid_get_child_at(GTK_GRID(d->grid), 0, j);
    gtk_label_set_text(GTK_LABEL(w_name), _(m->name));
    gtk_widget_set_tooltip_text(w_name, _(m->name));
    GtkWidget *w_value = gtk_grid_get_child_at(GTK_GRID(d->grid), 1, j);
    gtk_label_set_text(GTK_LABEL(w_value), m->value);
    const char *tooltip = m->tooltip ? m->tooltip : m->value;
    gtk_widget_set_tooltip_text(w_value, tooltip);

    const int i = m->index;
    gtk_label_set_ellipsize(GTK_LABEL(w_value),
                            i == md_exif_model || i == md_exif_lens || i == md_exif_maker
                            ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_MIDDLE);
    if(i == md_internal_filmroll)
    {
      // film roll jump to:
      if(d->filmroll_event)
        g_signal_handlers_disconnect_by_func(d->filmroll_event, G_CALLBACK(_filmroll_clicked), NULL);
      g_signal_connect(G_OBJECT(w_value), "button-press-event", G_CALLBACK(_filmroll_clicked), NULL);
      d->filmroll_event = G_OBJECT(w_value);
    }

    gtk_widget_set_visible(w_name, m->visible);
    gtk_widget_set_visible(w_value, m->visible);
    j++;
  }
}

static void _lib_metadata_setup_grid(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;

  int j = 0;
  // initialize the grid with metadata queue content
  for(GList *meta = d->metadata; meta; meta = g_list_next(meta))
  {
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    GtkWidget *w_name = gtk_label_new(_(m->name));
    gtk_widget_set_halign(w_name, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(w_name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(w_name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(w_name, _(m->name));

    GtkWidget *w_value= gtk_label_new(m->value);
    gtk_widget_set_name(w_value, "brightbg");
    gtk_label_set_selectable(GTK_LABEL(w_value), TRUE);
    gtk_widget_set_halign(w_value, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(w_value), 0.0f);

    gtk_grid_attach(GTK_GRID(d->grid), w_name, 0, j, 1, 1);
    gtk_grid_attach(GTK_GRID(d->grid), w_value, 1, j, 1, 1);
    j++;
  }
}

static void _apply_preferences(const char *prefs_list, dt_lib_module_t *self)
{
  if(!prefs_list || !prefs_list[0]) return;
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;

  GList *prefs = dt_util_str_to_glist(",", prefs_list);
  int k = 0;
  for(GList *pref = prefs; pref; pref = g_list_next(pref))
  {
    const char *name = (char *)pref->data;
    gboolean visible = TRUE;
    if(name)
    {
      if(name[0] == '|')
      {
        name++;
        visible = FALSE;
      }
      for(GList *meta = d->metadata; meta; meta= g_list_next(meta))
      {
        dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
        if(name && !g_strcmp0(name, m->name))
        {
          m->order = k;
          m->visible = visible;
          break;
        }
      }
    }
    else continue;
    k++;
  }
  g_list_free_full(prefs, g_free);

  _lib_metadata_refill_grid(self);
}

static void _save_preferences(dt_lib_module_t *self)
{
  char *pref = _get_current_configuration(self);
  dt_conf_set_string("plugins/lighttable/metadata_view/visible", pref);
  g_free(pref);
}

static void _select_toggled_callback(GtkCellRendererToggle *cell_renderer, gchar *path_str, gpointer user_data)
{
  GtkListStore *store = (GtkListStore *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gboolean selected;

  gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path);
  gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, DT_METADATA_PREF_COL_VISIBLE, &selected, -1);
  gtk_list_store_set(store, &iter, DT_METADATA_PREF_COL_VISIBLE, !selected, -1);

  gtk_tree_path_free(path);
}

static void _drag_data_inserted(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
  _dndactive = TRUE;
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("metadata settings"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(300));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(w), FALSE);
  gtk_box_pack_start(GTK_BOX(area), w, TRUE, TRUE, 0);

  GtkListStore *store = gtk_list_store_new(DT_METADATA_PREF_NUM_COLS,
                                           G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkTreeModel *model = GTK_TREE_MODEL(store);

  d->metadata = g_list_sort(d->metadata, _lib_metadata_sort_order);
  for(GList *meta = d->metadata; meta; meta= g_list_next(meta))
  {
    GtkTreeIter iter;
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    if(!_is_metadata_ui(m->index))
      continue;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
                       DT_METADATA_PREF_COL_INDEX, m->index,
                       DT_METADATA_PREF_COL_NAME_L, _(m->name),
                       DT_METADATA_PREF_COL_VISIBLE, m->visible,
                       -1);
  }

  GtkWidget *view = gtk_tree_view_new_with_model(model);
  g_object_unref(model);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("metadata"), renderer,
                                                    "text", DT_METADATA_PREF_COL_NAME_L, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  GtkWidget *header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header,
                _("drag and drop one row at a time until you get the desired order"
                "\nuntick to hide metadata which are not of interest for you"
                "\nif different settings are needed, use presets"));
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_select_toggled_callback), store);
  column = gtk_tree_view_column_new_with_attributes(_("visible"), renderer,
                                                    "active", DT_METADATA_PREF_COL_VISIBLE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  // drag & drop
  gtk_tree_view_set_reorderable(GTK_TREE_VIEW(view), TRUE);
  g_signal_connect(G_OBJECT(model), "row-inserted", G_CALLBACK(_drag_data_inserted), NULL);

  gtk_container_add(GTK_CONTAINER(w), view);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  int i = 0;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while(valid)
    {
      gboolean visible;
      uint32_t index;
      gtk_tree_model_get(model, &iter,
                         DT_METADATA_PREF_COL_INDEX, &index,
                         DT_METADATA_PREF_COL_VISIBLE, &visible,
                         -1);
      for(GList *meta = d->metadata; meta; meta= g_list_next(meta))
      {
        dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
        if(m->index == index)
        {
          m->order = i;
          m->visible = visible;
          break;
        }
      }
      i++;
      valid = gtk_tree_model_iter_next(model, &iter);
    }

    _lib_metadata_refill_grid(self);
    _save_preferences(self);
  }
  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void init_presets(dt_lib_module_t *self)
{
}

void *get_params(dt_lib_module_t *self, int *size)
{
  *size = 0;
  char *params = _get_current_configuration(self);
  if(params)
    *size =strlen(params) + 1;
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  _apply_preferences(params, self);
  _save_preferences(self);
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui */
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)g_malloc0(sizeof(dt_lib_metadata_view_t));
  self->data = (void *)d;

  _lib_metadata_init_queue(self);

  GtkWidget *child_grid_window = gtk_grid_new();
  d->grid = child_grid_window;
  gtk_grid_set_column_spacing(GTK_GRID(child_grid_window), DT_PIXEL_APPLY_DPI(5));

  self->widget = dt_ui_scroll_wrap(child_grid_window, 200, "plugins/lighttable/metadata_view/windowheight");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  gtk_widget_show_all(d->grid);
  gtk_widget_set_no_show_all(d->grid, TRUE);
  _lib_metadata_setup_grid(self);
  char *pref = dt_conf_get_string("plugins/lighttable/metadata_view/visible");
  if (strlen(pref))
    _apply_preferences(pref, self);
  else
    gui_reset(self);
  g_free(pref);

  /* lets signup for mouse over image change signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* lets signup for develop image changed signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for develop initialize to update info of current
     image in darkroom when enter */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for tags changes */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for metadata changes */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_UPDATE,
                            G_CALLBACK(_mouse_over_image_callback), self);
}

static void _free_metadata_queue(dt_lib_metadata_info_t *m)
{
  if(m->value) g_free(m->value);
  if(m->tooltip) g_free(m->tooltip);
  g_free(m);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  g_list_free_full(d->metadata,  (GDestroyNotify)_free_metadata_queue);
  g_free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;

  for(GList *meta = d->metadata; meta; meta= g_list_next(meta))
  {
    dt_lib_metadata_info_t *m = (dt_lib_metadata_info_t *)meta->data;
    m->order = m->index;
    m->visible = _is_metadata_ui(m->index);
  }
  _lib_metadata_refill_grid(self);
  _save_preferences(self);
}

#ifdef USE_LUA
static int lua_update_values(lua_State*L)
{
  dt_lib_module_t *self = lua_touserdata(L, 1);
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,2);
  lua_getfield(L,3,"values");
  lua_getfield(L,3,"indexes");
  lua_pushnil(L);
  while(lua_next(L, 4) != 0)
  {
    lua_getfield(L,5,lua_tostring(L,-2));
    int index = lua_tointeger(L,-1);
    _metadata_update_value(index,luaL_checkstring(L,7),self);
    lua_pop(L,2);
  }
  return 0;
}
static int lua_update_metadata(lua_State*L)
{
  dt_lib_module_t *self = lua_touserdata(L, 1);
  int32_t imgid = lua_tointeger(L,2);
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,4,"callbacks");
  lua_getfield(L,4,"values");
  lua_pushnil(L);
  while(lua_next(L, 5) != 0)
  {
    lua_pushvalue(L,-1);
    luaA_push(L,dt_lua_image_t,&imgid);
    lua_call(L,1,1);
    lua_pushvalue(L,7);
    lua_pushvalue(L,9);
    lua_settable(L,6);
    lua_pop(L, 2);
  }
  lua_pushcfunction(L,lua_update_values);
  dt_lua_gtk_wrap(L);
  lua_pushlightuserdata(L,self);
  lua_call(L,1,0);
  return 0;
}

static int lua_register_info(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  const char* key = luaL_checkstring(L,1);
  luaL_checktype(L,2,LUA_TFUNCTION);
  {
    lua_getfield(L,-1,"callbacks");
    lua_pushstring(L,key);
    lua_pushvalue(L,2);
    lua_settable(L,5);
    lua_pop(L,1);
  }
  {
    lua_getfield(L,-1,"values");
    lua_pushstring(L,key);
    lua_pushstring(L,NODATA_STRING);
    lua_settable(L,5);
    lua_pop(L,1);
  }
  {
    dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
    dt_lib_metadata_info_t *m = g_malloc0(sizeof(dt_lib_metadata_info_t));
    m->name = (char *)key;
    m->value = g_strdup(NODATA_STRING);
    const int index = g_list_length(d->metadata);
    m->index = m->order = index;
    m->visible = TRUE;

    GtkWidget *w_name = gtk_label_new(_(m->name));
    gtk_widget_set_halign(w_name, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(w_name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(w_name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(w_name, _(m->name));

    gboolean validated = g_utf8_validate(m->value, -1, NULL);
    const gchar *str = validated ? m->value : NODATA_STRING;

    GtkWidget *w_value= gtk_label_new(str);
    gtk_widget_set_name(w_value, "brightbg");
    gtk_label_set_selectable(GTK_LABEL(w_value), TRUE);
    gtk_widget_set_halign(w_value, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(w_value), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(w_value), PANGO_ELLIPSIZE_MIDDLE);
    gtk_grid_attach(GTK_GRID(d->grid), w_name, 0, index, 1, 1);
    gtk_grid_attach(GTK_GRID(d->grid), w_value, 1, index, 1, 1);

    d->metadata = g_list_append(d->metadata, m);

    {
      lua_getfield(L,-1,"indexes");
      lua_pushstring(L,key);
      lua_pushinteger(L,index);
      lua_settable(L,5);
      lua_pop(L,1);
    }
    // apply again preferences because it's already done
    char *pref = dt_conf_get_string("plugins/lighttable/metadata_view/visible");
    _apply_preferences(pref, self);
    g_free(pref);
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_info,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_info");

  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_newtable(L);
  lua_setfield(L,-2,"callbacks");
  lua_newtable(L);
  lua_setfield(L,-2,"values");
  lua_newtable(L);
  lua_setfield(L,-2,"indexes");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
