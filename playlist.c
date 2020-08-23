/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libretro.h>
#include <boolean.h>
#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <compat/posix_string.h>
#include <string/stdstring.h>
#include <streams/interface_stream.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <formats/jsonsax_full.h>
#include <array/rbuf.h>

#include "playlist.h"
#include "verbosity.h"
#include "file_path_special.h"
#include "core_info.h"

#ifndef PLAYLIST_ENTRIES
#define PLAYLIST_ENTRIES 6
#endif

#define WINDOWS_PATH_DELIMITER '\\'
#define POSIX_PATH_DELIMITER '/'

#ifdef _WIN32
#define LOCAL_FILE_SYSTEM_PATH_DELIMITER WINDOWS_PATH_DELIMITER
#define USING_WINDOWS_FILE_SYSTEM
#else
#define LOCAL_FILE_SYSTEM_PATH_DELIMITER POSIX_PATH_DELIMITER
#define USING_POSIX_FILE_SYSTEM
#endif

struct content_playlist
{
   char *default_core_path;
   char *default_core_name;
   char *base_content_directory;

   struct playlist_entry *entries;

   playlist_config_t config;  /* size_t alignment */

   enum playlist_label_display_mode label_display_mode;
   enum playlist_thumbnail_mode right_thumbnail_mode;
   enum playlist_thumbnail_mode left_thumbnail_mode;
   enum playlist_sort_mode sort_mode;

   bool modified;
   bool old_format;
   bool compressed;
   bool cached_external;
};

typedef struct
{
   struct playlist_entry *current_entry;
   char *current_meta_string;
   char *current_items_string;
   char **current_entry_val;
   char **current_meta_val;
   int *current_entry_int_val;
   unsigned *current_entry_uint_val;
   struct string_list **current_entry_string_list_val;
   enum playlist_label_display_mode *current_meta_label_display_mode_val;
   enum playlist_thumbnail_mode *current_meta_thumbnail_mode_val;
   enum playlist_sort_mode *current_meta_sort_mode_val;
   intfstream_t *file;
   playlist_t *playlist;
   JSON_Parser parser;     /* ptr alignment */
   JSON_Writer writer;     /* ptr alignment */

   unsigned array_depth;
   unsigned object_depth;

   bool in_items;
   bool in_subsystem_roms;
   bool capacity_exceeded;
   bool out_of_memory;
} JSONContext;

/* TODO/FIXME - global state - perhaps move outside this file */
static playlist_t *playlist_cached = NULL;

typedef int (playlist_sort_fun_t)(
      const struct playlist_entry *a,
      const struct playlist_entry *b);

/* TODO/FIXME - hack for allowing the explore view to switch 
 * over to a playlist item */
void playlist_set_cached_external(playlist_t* pl)
{
   playlist_free_cached();
   if (!pl)
      return;

   playlist_cached = pl;
   playlist_cached->cached_external = true;
}

/* Convenience function: copies specified playlist
 * path to specified playlist configuration object */
void playlist_config_set_path(playlist_config_t *config, const char *path)
{
   if (!config)
      return;

   if (!string_is_empty(path))
      strlcpy(config->path, path, sizeof(config->path));
   else
      config->path[0] = '\0';
}

/* Convenience function: copies base content directory
 * path to specified playlist configuration object.
 * Also sets autofix_paths boolean, depending on base 
 * content directory value */
void playlist_config_set_base_content_directory(
      playlist_config_t* config, const char* path)
{
   if (!config)
      return;

   config->autofix_paths = !string_is_empty(path);
   if (config->autofix_paths)
      strlcpy(config->base_content_directory, path,
            sizeof(config->base_content_directory));
   else
      config->base_content_directory[0] = '\0';
}


/* Creates a copy of the specified playlist configuration.
 * Returns false in the event of an error */
bool playlist_config_copy(const playlist_config_t *src,
      playlist_config_t *dst)
{
   if (!src || !dst)
      return false;

   strlcpy(dst->path, src->path, sizeof(dst->path));
   strlcpy(dst->base_content_directory, src->base_content_directory,
         sizeof(dst->base_content_directory));

   dst->capacity            = src->capacity;
   dst->old_format          = src->old_format;
   dst->compress            = src->compress;
   dst->fuzzy_archive_match = src->fuzzy_archive_match;
   dst->autofix_paths       = src->autofix_paths;

   return true;
}

/* Returns internal playlist configuration object
 * of specified playlist.
 * Returns NULL it the event of an error. */
playlist_config_t *playlist_get_config(playlist_t *playlist)
{
   if (!playlist)
      return NULL;

   return &playlist->config;
}

static void path_replace_base_path_and_convert_to_local_file_system(
      char *out_path, const char *in_path,
      const char *in_oldrefpath, const char *in_refpath,
      size_t size)
{
   size_t in_oldrefpath_length = strlen(in_oldrefpath);
   size_t in_refpath_length    = strlen(in_refpath);

   /* If entry path is inside playlist base path,
    * replace it with new base content directory */
   if (string_starts_with_size(in_path, in_oldrefpath, in_oldrefpath_length))
   {
      memcpy(out_path, in_refpath, in_refpath_length);
      memcpy(out_path + in_refpath_length, in_path + in_oldrefpath_length,
            strlen(in_path) - in_oldrefpath_length + 1);

#ifdef USING_WINDOWS_FILE_SYSTEM
      /* If we are running under a Windows filesystem,
       * '/' characters are not allowed anywhere. 
       * We replace with '\' and hope for the best... */
      string_replace_all_chars(out_path,
            POSIX_PATH_DELIMITER, WINDOWS_PATH_DELIMITER);
#endif

#ifdef USING_POSIX_FILE_SYSTEM
      /* Under POSIX filesystem, we replace '\' characters with '/' */
      string_replace_all_chars(out_path,
            WINDOWS_PATH_DELIMITER, POSIX_PATH_DELIMITER);
#endif
   }
   else
      strlcpy(out_path, in_path, size);
}

/**
 * playlist_path_equal:
 * @real_path           : 'Real' search path, generated by path_resolve_realpath()
 * @entry_path          : Existing playlist entry 'path' value
 *
 * Returns 'true' if real_path matches entry_path
 * (Taking into account relative paths, case insensitive
 * filesystems, 'incomplete' archive paths)
 **/
static bool playlist_path_equal(const char *real_path,
      const char *entry_path, const playlist_config_t *config)
{
   bool real_path_is_compressed;
   bool entry_real_path_is_compressed;
   char entry_real_path[PATH_MAX_LENGTH];

   entry_real_path[0] = '\0';

   /* Sanity check */
   if (string_is_empty(real_path)  ||
       string_is_empty(entry_path) ||
       !config)
      return false;

   /* Get entry 'real' path */
   strlcpy(entry_real_path, entry_path, sizeof(entry_real_path));
   path_resolve_realpath(entry_real_path, sizeof(entry_real_path), true);

   if (string_is_empty(entry_real_path))
      return false;

   /* First pass comparison */
#ifdef _WIN32
   /* Handle case-insensitive operating systems*/
   if (string_is_equal_noncase(real_path, entry_real_path))
      return true;
#else
   if (string_is_equal(real_path, entry_real_path))
      return true;
#endif

#ifdef RARCH_INTERNAL
   /* If fuzzy matching is disabled, we can give up now */
   if (!config->fuzzy_archive_match)
      return false;
#endif

   /* If we reach this point, we have to work
    * harder...
    * Need to handle a rather awkward archive file
    * case where:
    * - playlist path contains a properly formatted
    *   [archive_path][delimiter][rom_file]
    * - search path is just [archive_path]
    * ...or vice versa.
    * This pretty much always happens when a playlist
    * is generated via scan content (which handles the
    * archive paths correctly), but the user subsequently
    * loads an archive file via the command line or some
    * external launcher (where the [delimiter][rom_file]
    * part is almost always omitted) */
   real_path_is_compressed       = path_is_compressed_file(real_path);
   entry_real_path_is_compressed = path_is_compressed_file(entry_real_path);

   if ((real_path_is_compressed  && !entry_real_path_is_compressed) ||
       (!real_path_is_compressed && entry_real_path_is_compressed))
   {
      const char *compressed_path_a  = real_path_is_compressed ? real_path       : entry_real_path;
      const char *full_path          = real_path_is_compressed ? entry_real_path : real_path;
      const char *delim              = path_get_archive_delim(full_path);

      if (delim)
      {
         char compressed_path_b[PATH_MAX_LENGTH] = {0};
         unsigned len = (unsigned)(1 + delim - full_path);

         strlcpy(compressed_path_b, full_path,
               (len < PATH_MAX_LENGTH ? len : PATH_MAX_LENGTH) * sizeof(char));

#ifdef _WIN32
         /* Handle case-insensitive operating systems*/
         if (string_is_equal_noncase(compressed_path_a, compressed_path_b))
            return true;
#else
         if (string_is_equal(compressed_path_a, compressed_path_b))
            return true;
#endif
      }
   }

   return false;
}

/**
 * playlist_core_path_equal:
 * @real_core_path  : 'Real' search path, generated by path_resolve_realpath()
 * @entry_core_path : Existing playlist entry 'core path' value
 * @config          : Playlist config parameters
 *
 * Returns 'true' if real_core_path matches entry_core_path
 * (Taking into account relative paths, case insensitive
 * filesystems)
 **/
static bool playlist_core_path_equal(const char *real_core_path, const char *entry_core_path, const playlist_config_t *config)
{
   char entry_real_core_path[PATH_MAX_LENGTH];

   entry_real_core_path[0] = '\0';

   /* Sanity check */
   if (string_is_empty(real_core_path) || string_is_empty(entry_core_path))
      return false;

   /* Get entry 'real' core path */
   strlcpy(entry_real_core_path, entry_core_path, sizeof(entry_real_core_path));
   if (!string_is_equal(entry_real_core_path, FILE_PATH_DETECT) &&
       !string_is_equal(entry_real_core_path, FILE_PATH_BUILTIN))
      path_resolve_realpath(entry_real_core_path, sizeof(entry_real_core_path), true);

   if (string_is_empty(entry_real_core_path))
      return false;

#ifdef _WIN32
   /* Handle case-insensitive operating systems*/
   if (string_is_equal_noncase(real_core_path, entry_real_core_path))
      return true;
#else
   if (string_is_equal(real_core_path, entry_real_core_path))
      return true;
#endif

   if (config->autofix_paths &&
       core_info_core_file_id_is_equal(real_core_path, entry_core_path))
      return true;

   return false;
}

uint32_t playlist_get_size(playlist_t *playlist)
{
   if (!playlist)
      return 0;
   return (uint32_t)RBUF_LEN(playlist->entries);
}

char *playlist_get_conf_path(playlist_t *playlist)
{
   if (!playlist)
      return NULL;
   return playlist->config.path;
}

/**
 * playlist_get_index:
 * @playlist            : Playlist handle.
 * @idx                 : Index of playlist entry.
 * @path                : Path of playlist entry.
 * @core_path           : Core path of playlist entry.
 * @core_name           : Core name of playlist entry.
 *
 * Gets values of playlist index:
 **/
void playlist_get_index(playlist_t *playlist,
      size_t idx,
      const struct playlist_entry **entry)
{
   if (!playlist || !entry || (idx >= RBUF_LEN(playlist->entries)))
      return;

   *entry = &playlist->entries[idx];
}

/**
 * playlist_free_entry:
 * @entry               : Playlist entry handle.
 *
 * Frees playlist entry.
 **/
static void playlist_free_entry(struct playlist_entry *entry)
{
   if (!entry)
      return;

   if (entry->path)
      free(entry->path);
   if (entry->label)
      free(entry->label);
   if (entry->core_path)
      free(entry->core_path);
   if (entry->core_name)
      free(entry->core_name);
   if (entry->db_name)
      free(entry->db_name);
   if (entry->crc32)
      free(entry->crc32);
   if (entry->subsystem_ident)
      free(entry->subsystem_ident);
   if (entry->subsystem_name)
      free(entry->subsystem_name);
   if (entry->runtime_str)
      free(entry->runtime_str);
   if (entry->last_played_str)
      free(entry->last_played_str);
   if (entry->subsystem_roms)
      string_list_free(entry->subsystem_roms);

   entry->path      = NULL;
   entry->label     = NULL;
   entry->core_path = NULL;
   entry->core_name = NULL;
   entry->db_name   = NULL;
   entry->crc32     = NULL;
   entry->subsystem_ident = NULL;
   entry->subsystem_name = NULL;
   entry->runtime_str = NULL;
   entry->last_played_str = NULL;
   entry->subsystem_roms = NULL;
   entry->runtime_status = PLAYLIST_RUNTIME_UNKNOWN;
   entry->runtime_hours = 0;
   entry->runtime_minutes = 0;
   entry->runtime_seconds = 0;
   entry->last_played_year = 0;
   entry->last_played_month = 0;
   entry->last_played_day = 0;
   entry->last_played_hour = 0;
   entry->last_played_minute = 0;
   entry->last_played_second = 0;
}

/**
 * playlist_delete_index:
 * @playlist            : Playlist handle.
 * @idx                 : Index of playlist entry.
 *
 * Delete the entry at the index:
 **/
void playlist_delete_index(playlist_t *playlist,
      size_t idx)
{
   size_t len;
   struct playlist_entry *entry_to_delete;

   if (!playlist)
      return;

   len = RBUF_LEN(playlist->entries);
   if (idx >= len)
      return;

   /* Free unwanted entry */
   entry_to_delete = (struct playlist_entry *)(playlist->entries + idx);
   if (entry_to_delete)
      playlist_free_entry(entry_to_delete);

   /* Shift remaining entries to fill the gap */
   memmove(playlist->entries + idx, playlist->entries + idx + 1,
         (len - 1 - idx) * sizeof(struct playlist_entry));

   RBUF_RESIZE(playlist->entries, len - 1);

   playlist->modified = true;
}

/**
 * playlist_delete_by_path:
 * @playlist            : Playlist handle.
 * @search_path         : Content path.
 *
 * Deletes all entries with content path
 * matching 'search_path'
 **/
void playlist_delete_by_path(playlist_t *playlist,
      const char *search_path)
{
   size_t i = 0;
   char real_search_path[PATH_MAX_LENGTH];

   real_search_path[0] = '\0';

   if (!playlist || string_is_empty(search_path))
      return;

   /* Get 'real' search path */
   strlcpy(real_search_path, search_path, sizeof(real_search_path));
   path_resolve_realpath(real_search_path, sizeof(real_search_path), true);

   while (i < RBUF_LEN(playlist->entries))
   {
      if (!playlist_path_equal(real_search_path, playlist->entries[i].path,
            &playlist->config))
      {
         i++;
         continue;
      }

      /* Paths are equal - delete entry */
      playlist_delete_index(playlist, i);

      /* Entries are shifted up by the delete
       * operation - *do not* increment i */
   }
}

void playlist_get_index_by_path(playlist_t *playlist,
      const char *search_path,
      const struct playlist_entry **entry)
{
   size_t i, len;
   char real_search_path[PATH_MAX_LENGTH];

   real_search_path[0] = '\0';

   if (!playlist || !entry || string_is_empty(search_path))
      return;

   /* Get 'real' search path */
   strlcpy(real_search_path, search_path, sizeof(real_search_path));
   path_resolve_realpath(real_search_path, sizeof(real_search_path), true);

   for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
   {
      if (!playlist_path_equal(real_search_path, playlist->entries[i].path,
               &playlist->config))
         continue;

      *entry = &playlist->entries[i];

      break;
   }
}

bool playlist_entry_exists(playlist_t *playlist,
      const char *path)
{
   size_t i, len;
   char real_search_path[PATH_MAX_LENGTH];

   real_search_path[0] = '\0';

   if (!playlist || string_is_empty(path))
      return false;

   /* Get 'real' search path */
   strlcpy(real_search_path, path, sizeof(real_search_path));
   path_resolve_realpath(real_search_path, sizeof(real_search_path), true);

   for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
   {
      if (playlist_path_equal(real_search_path, playlist->entries[i].path,
               &playlist->config))
         return true;
   }

   return false;
}

void playlist_update(playlist_t *playlist, size_t idx,
      const struct playlist_entry *update_entry)
{
   struct playlist_entry *entry = NULL;

   if (!playlist || idx >= RBUF_LEN(playlist->entries))
      return;

   entry            = &playlist->entries[idx];

   if (update_entry->path && (update_entry->path != entry->path))
   {
      if (entry->path)
         free(entry->path);
      entry->path        = strdup(update_entry->path);
      playlist->modified = true;
   }

   if (update_entry->label && (update_entry->label != entry->label))
   {
      if (entry->label)
         free(entry->label);
      entry->label       = strdup(update_entry->label);
      playlist->modified = true;
   }

   if (update_entry->core_path && (update_entry->core_path != entry->core_path))
   {
      if (entry->core_path)
         free(entry->core_path);
      entry->core_path   = NULL;
      entry->core_path   = strdup(update_entry->core_path);
      playlist->modified = true;
   }

   if (update_entry->core_name && (update_entry->core_name != entry->core_name))
   {
      if (entry->core_name)
         free(entry->core_name);
      entry->core_name   = strdup(update_entry->core_name);
      playlist->modified = true;
   }

   if (update_entry->db_name && (update_entry->db_name != entry->db_name))
   {
      if (entry->db_name)
         free(entry->db_name);
      entry->db_name     = strdup(update_entry->db_name);
      playlist->modified = true;
   }

   if (update_entry->crc32 && (update_entry->crc32 != entry->crc32))
   {
      if (entry->crc32)
         free(entry->crc32);
      entry->crc32       = strdup(update_entry->crc32);
      playlist->modified = true;
   }
}

void playlist_update_runtime(playlist_t *playlist, size_t idx,
      const struct playlist_entry *update_entry,
      bool register_update)
{
   struct playlist_entry *entry = NULL;

   if (!playlist || idx >= RBUF_LEN(playlist->entries))
      return;

   entry            = &playlist->entries[idx];

   if (update_entry->path && (update_entry->path != entry->path))
   {
      if (entry->path)
         free(entry->path);
      entry->path        = NULL;
      entry->path        = strdup(update_entry->path);
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->core_path && (update_entry->core_path != entry->core_path))
   {
      if (entry->core_path)
         free(entry->core_path);
      entry->core_path   = NULL;
      entry->core_path   = strdup(update_entry->core_path);
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->runtime_status != entry->runtime_status)
   {
      entry->runtime_status = update_entry->runtime_status;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->runtime_hours != entry->runtime_hours)
   {
      entry->runtime_hours = update_entry->runtime_hours;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->runtime_minutes != entry->runtime_minutes)
   {
      entry->runtime_minutes = update_entry->runtime_minutes;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->runtime_seconds != entry->runtime_seconds)
   {
      entry->runtime_seconds = update_entry->runtime_seconds;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_year != entry->last_played_year)
   {
      entry->last_played_year = update_entry->last_played_year;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_month != entry->last_played_month)
   {
      entry->last_played_month = update_entry->last_played_month;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_day != entry->last_played_day)
   {
      entry->last_played_day = update_entry->last_played_day;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_hour != entry->last_played_hour)
   {
      entry->last_played_hour = update_entry->last_played_hour;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_minute != entry->last_played_minute)
   {
      entry->last_played_minute = update_entry->last_played_minute;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_second != entry->last_played_second)
   {
      entry->last_played_second = update_entry->last_played_second;
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->runtime_str && (update_entry->runtime_str != entry->runtime_str))
   {
      if (entry->runtime_str)
         free(entry->runtime_str);
      entry->runtime_str = NULL;
      entry->runtime_str = strdup(update_entry->runtime_str);
      playlist->modified = playlist->modified || register_update;
   }

   if (update_entry->last_played_str && (update_entry->last_played_str != entry->last_played_str))
   {
      if (entry->last_played_str)
         free(entry->last_played_str);
      entry->last_played_str = NULL;
      entry->last_played_str = strdup(update_entry->last_played_str);
      playlist->modified = playlist->modified || register_update;
   }
}

bool playlist_push_runtime(playlist_t *playlist,
      const struct playlist_entry *entry)
{
   size_t i, len;
   char real_path[PATH_MAX_LENGTH];
   char real_core_path[PATH_MAX_LENGTH];

   if (!playlist || !entry)
      return false;

   if (string_is_empty(entry->core_path))
   {
      RARCH_ERR("cannot push NULL or empty core path into the playlist.\n");
      return false;
   }

   real_path[0]      = '\0';
   real_core_path[0] = '\0';

   /* Get 'real' path */
   if (!string_is_empty(entry->path))
   {
      strlcpy(real_path, entry->path, sizeof(real_path));
      path_resolve_realpath(real_path, sizeof(real_path), true);
   }

   /* Get 'real' core path */
   strlcpy(real_core_path, entry->core_path, sizeof(real_core_path));
   if (!string_is_equal(real_core_path, FILE_PATH_DETECT) &&
       !string_is_equal(real_core_path, FILE_PATH_BUILTIN))
      path_resolve_realpath(real_core_path, sizeof(real_core_path), true);

   if (string_is_empty(real_core_path))
   {
      RARCH_ERR("cannot push NULL or empty core path into the playlist.\n");
      return false;
   }

   len = RBUF_LEN(playlist->entries);
   for (i = 0; i < len; i++)
   {
      struct playlist_entry tmp;
      const char *entry_path = playlist->entries[i].path;
      bool equal_path        =
         (string_is_empty(real_path) && string_is_empty(entry_path)) ||
         playlist_path_equal(real_path, entry_path, &playlist->config);

      /* Core name can have changed while still being the same core.
       * Differentiate based on the core path only. */
      if (!equal_path)
         continue;

      if (!playlist_core_path_equal(real_core_path, playlist->entries[i].core_path, &playlist->config))
         continue;

      /* If top entry, we don't want to push a new entry since
       * the top and the entry to be pushed are the same. */
      if (i == 0)
         return false;

      /* Seen it before, bump to top. */
      tmp = playlist->entries[i];
      memmove(playlist->entries + 1, playlist->entries,
            i * sizeof(struct playlist_entry));
      playlist->entries[0] = tmp;

      goto success;
   }

   if (playlist->config.capacity == 0)
      return false;

   if (len == playlist->config.capacity)
   {
      struct playlist_entry *last_entry = &playlist->entries[len - 1];
      playlist_free_entry(last_entry);
      len--;
   }
   else
   {
      /* Allocate memory to fit one more item and resize the buffer */
      if (!RBUF_TRYFIT(playlist->entries, len + 1))
         return false; /* out of memory */
      RBUF_RESIZE(playlist->entries, len + 1);
   }

   if (playlist->entries)
   {
      memmove(playlist->entries + 1, playlist->entries,
            len * sizeof(struct playlist_entry));

      playlist->entries[0].path            = NULL;
      playlist->entries[0].core_path       = NULL;

      if (!string_is_empty(real_path))
         playlist->entries[0].path      = strdup(real_path);
      if (!string_is_empty(real_core_path))
         playlist->entries[0].core_path = strdup(real_core_path);

      playlist->entries[0].runtime_status = entry->runtime_status;
      playlist->entries[0].runtime_hours = entry->runtime_hours;
      playlist->entries[0].runtime_minutes = entry->runtime_minutes;
      playlist->entries[0].runtime_seconds = entry->runtime_seconds;
      playlist->entries[0].last_played_year = entry->last_played_year;
      playlist->entries[0].last_played_month = entry->last_played_month;
      playlist->entries[0].last_played_day = entry->last_played_day;
      playlist->entries[0].last_played_hour = entry->last_played_hour;
      playlist->entries[0].last_played_minute = entry->last_played_minute;
      playlist->entries[0].last_played_second = entry->last_played_second;

      playlist->entries[0].runtime_str        = NULL;
      playlist->entries[0].last_played_str    = NULL;

      if (!string_is_empty(entry->runtime_str))
         playlist->entries[0].runtime_str     = strdup(entry->runtime_str);
      if (!string_is_empty(entry->last_played_str))
         playlist->entries[0].last_played_str = strdup(entry->last_played_str);
   }

success:
   playlist->modified = true;

   return true;
}

/**
 * playlist_resolve_path:
 * @mode      : PLAYLIST_LOAD or PLAYLIST_SAVE
 * @path      : The path to be modified
 *
 * Resolves the path of an item, such as the content path or path to the core, to a format
 * appropriate for saving or loading depending on the @mode parameter
 *
 * Can be platform specific. File paths for saving can be abbreviated to avoid saving absolute
 * paths, as the base directory (home or application dir) may change after each subsequent
 * install (iOS)
**/
void playlist_resolve_path(enum playlist_file_mode mode,
      char *path, size_t len)
{
#ifdef HAVE_COCOATOUCH
   char tmp[PATH_MAX_LENGTH];

   if (mode == PLAYLIST_LOAD)
   {
      fill_pathname_expand_special(tmp, path, sizeof(tmp));
      strlcpy(path, tmp, len);
   }
   else
   {
      /* iOS needs to call realpath here since the call
       * above fails due to possibly buffer related issues.
       * Try to expand the path to ensure that it gets saved
       * correctly. The path can be abbreviated if saving to
       * a playlist from another playlist (ex: content history to favorites)
       */
      char tmp2[PATH_MAX_LENGTH];
      fill_pathname_expand_special(tmp, path, sizeof(tmp));
      realpath(tmp, tmp2);
      fill_pathname_abbreviate_special(path, tmp2, len);
   }
#else
   if (mode == PLAYLIST_LOAD)
      return;

   path_resolve_realpath(path, len, true);
#endif
}

/**
 * playlist_push:
 * @playlist        	   : Playlist handle.
 *
 * Push entry to top of playlist.
 **/
bool playlist_push(playlist_t *playlist,
      const struct playlist_entry *entry)
{
   size_t i, len;
   char real_path[PATH_MAX_LENGTH];
   char real_core_path[PATH_MAX_LENGTH];
   const char *core_name = entry->core_name;
   bool entry_updated    = false;

   real_path[0] = '\0';
   real_core_path[0] = '\0';

   if (!playlist || !entry)
      return false;

   if (string_is_empty(entry->core_path))
   {
      RARCH_ERR("cannot push NULL or empty core path into the playlist.\n");
      return false;
   }

   /* Get 'real' path */
   if (!string_is_empty(entry->path))
   {
      strlcpy(real_path, entry->path, sizeof(real_path));
      playlist_resolve_path(PLAYLIST_SAVE, real_path, sizeof(real_path));
   }

   /* Get 'real' core path */
   strlcpy(real_core_path, entry->core_path, sizeof(real_core_path));
   if (!string_is_equal(real_core_path, FILE_PATH_DETECT) &&
       !string_is_equal(real_core_path, FILE_PATH_BUILTIN))
       playlist_resolve_path(PLAYLIST_SAVE, real_core_path,
             sizeof(real_core_path));

   if (string_is_empty(real_core_path))
   {
      RARCH_ERR("cannot push NULL or empty core path into the playlist.\n");
      return false;
   }

   if (string_is_empty(core_name))
   {
      static char base_path[255] = {0};
      fill_pathname_base_noext(base_path, real_core_path, sizeof(base_path));
      core_name = base_path;

      if (string_is_empty(core_name))
      {
         RARCH_ERR("cannot push NULL or empty core name into the playlist.\n");
         return false;
      }
   }

   len = RBUF_LEN(playlist->entries);
   for (i = 0; i < len; i++)
   {
      struct playlist_entry tmp;
      const char *entry_path = playlist->entries[i].path;
      bool equal_path        =
         (string_is_empty(real_path) && string_is_empty(entry_path)) ||
         playlist_path_equal(real_path, entry_path, &playlist->config);

      /* Core name can have changed while still being the same core.
       * Differentiate based on the core path only. */
      if (!equal_path)
         continue;

      if (!playlist_core_path_equal(real_core_path, playlist->entries[i].core_path, &playlist->config))
         continue;

      if (     !string_is_empty(entry->subsystem_ident)
            && !string_is_empty(playlist->entries[i].subsystem_ident)
            && !string_is_equal(playlist->entries[i].subsystem_ident, entry->subsystem_ident))
         continue;

      if (      string_is_empty(entry->subsystem_ident)
            && !string_is_empty(playlist->entries[i].subsystem_ident))
         continue;

      if (    !string_is_empty(entry->subsystem_ident)
            && string_is_empty(playlist->entries[i].subsystem_ident))
         continue;

      if (     !string_is_empty(entry->subsystem_name)
            && !string_is_empty(playlist->entries[i].subsystem_name)
            && !string_is_equal(playlist->entries[i].subsystem_name, entry->subsystem_name))
         continue;

      if (      string_is_empty(entry->subsystem_name)
            && !string_is_empty(playlist->entries[i].subsystem_name))
         continue;

      if (     !string_is_empty(entry->subsystem_name)
            &&  string_is_empty(playlist->entries[i].subsystem_name))
         continue;

      if (entry->subsystem_roms)
      {
         unsigned j;
         const struct string_list *roms = playlist->entries[i].subsystem_roms;
         bool                   unequal = false;

         if (entry->subsystem_roms->size != roms->size)
            continue;

         for (j = 0; j < entry->subsystem_roms->size; j++)
         {
            char real_rom_path[PATH_MAX_LENGTH];

            real_rom_path[0] = '\0';

            if (!string_is_empty(entry->subsystem_roms->elems[j].data))
            {
               strlcpy(real_rom_path, entry->subsystem_roms->elems[j].data, sizeof(real_rom_path));
               path_resolve_realpath(real_rom_path, sizeof(real_rom_path), true);
            }

            if (!playlist_path_equal(real_rom_path, roms->elems[j].data,
                     &playlist->config))
            {
               unequal = true;
               break;
            }
         }

         if (unequal)
            continue;
      }

      /* If content was previously loaded via file browser
       * or command line, certain entry values will be missing.
       * If we are now loading the same content from a playlist,
       * fill in any blanks */
      if (!playlist->entries[i].label && !string_is_empty(entry->label))
      {
         playlist->entries[i].label   = strdup(entry->label);
         entry_updated                = true;
      }
      if (!playlist->entries[i].crc32 && !string_is_empty(entry->crc32))
      {
         playlist->entries[i].crc32   = strdup(entry->crc32);
         entry_updated                = true;
      }
      if (!playlist->entries[i].db_name && !string_is_empty(entry->db_name))
      {
         playlist->entries[i].db_name = strdup(entry->db_name);
         entry_updated                = true;
      }

      /* If top entry, we don't want to push a new entry since
       * the top and the entry to be pushed are the same. */
      if (i == 0)
      {
         if (entry_updated)
            goto success;

         return false;
      }

      /* Seen it before, bump to top. */
      tmp = playlist->entries[i];
      memmove(playlist->entries + 1, playlist->entries,
            i * sizeof(struct playlist_entry));
      playlist->entries[0] = tmp;

      goto success;
   }

   if (playlist->config.capacity == 0)
      return false;

   if (len == playlist->config.capacity)
   {
      struct playlist_entry *last_entry = &playlist->entries[len - 1];
      playlist_free_entry(last_entry);
      len--;
   }
   else
   {
      /* Allocate memory to fit one more item and resize the buffer */
      if (!RBUF_TRYFIT(playlist->entries, len + 1))
         return false; /* out of memory */
      RBUF_RESIZE(playlist->entries, len + 1);
   }

   if (playlist->entries)
   {
      memmove(playlist->entries + 1, playlist->entries,
            len * sizeof(struct playlist_entry));

      playlist->entries[0].path               = NULL;
      playlist->entries[0].label              = NULL;
      playlist->entries[0].core_path          = NULL;
      playlist->entries[0].core_name          = NULL;
      playlist->entries[0].db_name            = NULL;
      playlist->entries[0].crc32              = NULL;
      playlist->entries[0].subsystem_ident    = NULL;
      playlist->entries[0].subsystem_name     = NULL;
      playlist->entries[0].runtime_str        = NULL;
      playlist->entries[0].last_played_str    = NULL;
      playlist->entries[0].subsystem_roms     = NULL;
      playlist->entries[0].runtime_status     = PLAYLIST_RUNTIME_UNKNOWN;
      playlist->entries[0].runtime_hours      = 0;
      playlist->entries[0].runtime_minutes    = 0;
      playlist->entries[0].runtime_seconds    = 0;
      playlist->entries[0].last_played_year   = 0;
      playlist->entries[0].last_played_month  = 0;
      playlist->entries[0].last_played_day    = 0;
      playlist->entries[0].last_played_hour   = 0;
      playlist->entries[0].last_played_minute = 0;
      playlist->entries[0].last_played_second = 0;
      if (!string_is_empty(real_path))
         playlist->entries[0].path            = strdup(real_path);
      if (!string_is_empty(entry->label))
         playlist->entries[0].label           = strdup(entry->label);
      if (!string_is_empty(real_core_path))
         playlist->entries[0].core_path       = strdup(real_core_path);
      if (!string_is_empty(core_name))
         playlist->entries[0].core_name       = strdup(core_name);
      if (!string_is_empty(entry->db_name))
         playlist->entries[0].db_name         = strdup(entry->db_name);
      if (!string_is_empty(entry->crc32))
         playlist->entries[0].crc32           = strdup(entry->crc32);
      if (!string_is_empty(entry->subsystem_ident))
         playlist->entries[0].subsystem_ident = strdup(entry->subsystem_ident);
      if (!string_is_empty(entry->subsystem_name))
         playlist->entries[0].subsystem_name  = strdup(entry->subsystem_name);

      if (entry->subsystem_roms)
      {
         union string_list_elem_attr attributes = {0};

         playlist->entries[0].subsystem_roms    = string_list_new();

         for (i = 0; i < entry->subsystem_roms->size; i++)
            string_list_append(playlist->entries[0].subsystem_roms, entry->subsystem_roms->elems[i].data, attributes);
      }
   }

success:
   playlist->modified = true;

   return true;
}

static JSON_Writer_HandlerResult JSONOutputHandler(JSON_Writer writer, const char *pBytes, size_t length)
{
   JSONContext *context = (JSONContext*)JSON_Writer_GetUserData(writer);

   (void)writer; /* unused */
   return intfstream_write(context->file, pBytes, length) == length ? JSON_Writer_Continue : JSON_Writer_Abort;
}

static void JSONLogError(JSONContext *pCtx)
{
   if (pCtx->parser && JSON_Parser_GetError(pCtx->parser) != JSON_Error_AbortedByHandler)
   {
      JSON_Error error            = JSON_Parser_GetError(pCtx->parser);
      JSON_Location errorLocation = { 0, 0, 0 };

      (void)JSON_Parser_GetErrorLocation(pCtx->parser, &errorLocation);
      RARCH_WARN("Error: Invalid JSON at line %d, column %d (input byte %d) - %s.\n",
            (int)errorLocation.line + 1,
            (int)errorLocation.column + 1,
            (int)errorLocation.byte,
            JSON_ErrorString(error));
   }
   else if (pCtx->writer && JSON_Writer_GetError(pCtx->writer) != JSON_Error_AbortedByHandler)
   {
      RARCH_WARN("Error: could not write output - %s.\n", JSON_ErrorString(JSON_Writer_GetError(pCtx->writer)));
   }
}

void playlist_write_runtime_file(playlist_t *playlist)
{
   size_t i, len;
   intfstream_t *file  = NULL;
   JSONContext context = {0};

   if (!playlist || !playlist->modified)
      return;

   file = intfstream_open_file(playlist->config.path,
         RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
   {
      RARCH_ERR("Failed to write to playlist file: %s\n", playlist->config.path);
      return;
   }

   context.writer = JSON_Writer_Create(NULL);
   context.file   = file;

   if (!context.writer)
   {
      RARCH_ERR("Failed to create JSON writer\n");
      goto end;
   }

   JSON_Writer_SetOutputEncoding(context.writer, JSON_UTF8);
   JSON_Writer_SetOutputHandler(context.writer, &JSONOutputHandler);
   JSON_Writer_SetUserData(context.writer, &context);

   JSON_Writer_WriteStartObject(context.writer);
   JSON_Writer_WriteNewLine(context.writer);
   JSON_Writer_WriteSpace(context.writer, 2);
   JSON_Writer_WriteString(context.writer, "version",
         STRLEN_CONST("version"), JSON_UTF8);
   JSON_Writer_WriteColon(context.writer);
   JSON_Writer_WriteSpace(context.writer, 1);
   JSON_Writer_WriteString(context.writer, "1.0",
         STRLEN_CONST("1.0"), JSON_UTF8);
   JSON_Writer_WriteComma(context.writer);
   JSON_Writer_WriteNewLine(context.writer);
   JSON_Writer_WriteSpace(context.writer, 2);
   JSON_Writer_WriteString(context.writer, "items",
         STRLEN_CONST("items"), JSON_UTF8);
   JSON_Writer_WriteColon(context.writer);
   JSON_Writer_WriteSpace(context.writer, 1);
   JSON_Writer_WriteStartArray(context.writer);
   JSON_Writer_WriteNewLine(context.writer);

   for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
   {
      JSON_Writer_WriteSpace(context.writer, 4);
      JSON_Writer_WriteStartObject(context.writer);

      JSON_Writer_WriteNewLine(context.writer);
      JSON_Writer_WriteSpace(context.writer, 6);
      JSON_Writer_WriteString(context.writer, "path",
            STRLEN_CONST("path"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      JSON_Writer_WriteSpace(context.writer, 1);
      JSON_Writer_WriteString(context.writer,
            playlist->entries[i].path
            ? playlist->entries[i].path
            : "",
            playlist->entries[i].path
            ? strlen(playlist->entries[i].path)
            : 0,
            JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);

      JSON_Writer_WriteNewLine(context.writer);
      JSON_Writer_WriteSpace(context.writer, 6);
      JSON_Writer_WriteString(context.writer, "core_path",
            STRLEN_CONST("core_path"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      JSON_Writer_WriteSpace(context.writer, 1);
      JSON_Writer_WriteString(context.writer,
            playlist->entries[i].core_path
            ? playlist->entries[i].core_path
            : "",
            playlist->entries[i].core_path
            ? strlen(playlist->entries[i].core_path)
            : 0,
            JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      JSON_Writer_WriteNewLine(context.writer);

      {
         char tmp[32] = {0};

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].runtime_hours);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "runtime_hours",
               STRLEN_CONST("runtime_hours"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].runtime_minutes);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "runtime_minutes",
               STRLEN_CONST("runtime_minutes"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].runtime_seconds);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "runtime_seconds",
               STRLEN_CONST("runtime_seconds"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_year);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_year",
               STRLEN_CONST("last_played_year"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_month);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_month",
               STRLEN_CONST("last_played_month"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_day);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_day",
               STRLEN_CONST("last_played_day"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp,
               strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_hour);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_hour",
               STRLEN_CONST("last_played_hour"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_minute);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_minute",
               STRLEN_CONST("last_played_minute"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp, strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         JSON_Writer_WriteNewLine(context.writer);

         memset(tmp, 0, sizeof(tmp));

         snprintf(tmp, sizeof(tmp), "%u", playlist->entries[i].last_played_second);

         JSON_Writer_WriteSpace(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "last_played_second",
               STRLEN_CONST("last_played_second"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         JSON_Writer_WriteSpace(context.writer, 1);
         JSON_Writer_WriteNumber(context.writer, tmp,
               strlen(tmp), JSON_UTF8);
         JSON_Writer_WriteNewLine(context.writer);
      }

      JSON_Writer_WriteSpace(context.writer, 4);
      JSON_Writer_WriteEndObject(context.writer);

      if (i < len - 1)
         JSON_Writer_WriteComma(context.writer);

      JSON_Writer_WriteNewLine(context.writer);
   }

   JSON_Writer_WriteSpace(context.writer, 2);
   JSON_Writer_WriteEndArray(context.writer);
   JSON_Writer_WriteNewLine(context.writer);
   JSON_Writer_WriteEndObject(context.writer);
   JSON_Writer_WriteNewLine(context.writer);
   JSON_Writer_Free(context.writer);

   playlist->modified        = false;
   playlist->old_format      = false;
   playlist->compressed      = false;

   RARCH_LOG("[Playlist]: Written to playlist file: %s\n", playlist->config.path);
end:
   intfstream_close(file);
   free(file);
}

/* No-op versions of JSON whitespace writers,
 * used when generating compressed output */
static JSON_Status JSON_CALL JSON_Writer_WriteNewLine_NULL(JSON_Writer writer)
{
   return JSON_Success;
}

static JSON_Status JSON_CALL JSON_Writer_WriteSpace_NULL(JSON_Writer writer, size_t numberOfSpaces)
{
   return JSON_Success;
}

void playlist_write_file(playlist_t *playlist)
{
   size_t i, len;
   intfstream_t *file = NULL;
   bool compressed    = false;

   /* Playlist will be written if any of the
    * following are true:
    * > 'modified' flag is set
    * > Current playlist format (old/new) does not
    *   match requested
    * > Current playlist compression status does
    *   not match requested */
   if (!playlist ||
       !(playlist->modified ||
#if defined(HAVE_ZLIB)
        (playlist->compressed != playlist->config.compress) ||
#endif
        (playlist->old_format != playlist->config.old_format)))
      return;

#if defined(HAVE_ZLIB)
   if (playlist->config.compress)
      file = intfstream_open_rzip_file(playlist->config.path,
            RETRO_VFS_FILE_ACCESS_WRITE);
   else
#endif
      file = intfstream_open_file(playlist->config.path,
            RETRO_VFS_FILE_ACCESS_WRITE,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
   {
      RARCH_ERR("Failed to write to playlist file: %s\n", playlist->config.path);
      return;
   }

   /* Get current file compression state */
   compressed = intfstream_is_compressed(file);

#ifdef RARCH_INTERNAL
   if (playlist->config.old_format)
   {
      for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
         intfstream_printf(file, "%s\n%s\n%s\n%s\n%s\n%s\n",
               playlist->entries[i].path      ? playlist->entries[i].path      : "",
               playlist->entries[i].label     ? playlist->entries[i].label     : "",
               playlist->entries[i].core_path ? playlist->entries[i].core_path : "",
               playlist->entries[i].core_name ? playlist->entries[i].core_name : "",
               playlist->entries[i].crc32     ? playlist->entries[i].crc32     : "",
               playlist->entries[i].db_name   ? playlist->entries[i].db_name   : ""
               );

      /* Add metadata lines
       * > We add these at the end of the file to prevent
       *   breakage if the playlist is loaded with an older
       *   version of RetroArch */
      intfstream_printf(
            file,
            "default_core_path = \"%s\"\n"
            "default_core_name = \"%s\"\n"
            "label_display_mode = \"%d\"\n"
            "thumbnail_mode = \"%d|%d\"\n"
            "sort_mode = \"%d\"\n",
            playlist->default_core_path ? playlist->default_core_path : "",
            playlist->default_core_name ? playlist->default_core_name : "",
            playlist->label_display_mode,
            playlist->right_thumbnail_mode, playlist->left_thumbnail_mode,
            playlist->sort_mode);

      playlist->old_format = true;
   }
   else
#endif
   {
      char uint_str[4];
      JSONContext context = {0};

      /* Assign JSON whitespace functions
      * > When compressing playlists, human readability
      *   is not a factor - can skip all indentation
      *   and new line characters
      * > Create these function pointers locally to
      *   ensure thread safety */
      JSON_Status (JSON_CALL *json_write_new_line)(JSON_Writer writer) =
            compressed ?
                  JSON_Writer_WriteNewLine_NULL :
                  JSON_Writer_WriteNewLine;
      JSON_Status (JSON_CALL *json_write_space)(JSON_Writer writer, size_t numberOfSpaces) =
            compressed ?
               JSON_Writer_WriteSpace_NULL :
               JSON_Writer_WriteSpace;

      context.writer = JSON_Writer_Create(NULL);
      context.file   = file;

      if (!context.writer)
      {
         RARCH_ERR("Failed to create JSON writer\n");
         goto end;
      }

      JSON_Writer_SetOutputEncoding(context.writer, JSON_UTF8);
      JSON_Writer_SetOutputHandler(context.writer, &JSONOutputHandler);
      JSON_Writer_SetUserData(context.writer, &context);

      JSON_Writer_WriteStartObject(context.writer);
      json_write_new_line(context.writer);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "version",
            STRLEN_CONST("version"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteString(context.writer, "1.4",
            STRLEN_CONST("1.4"), JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "default_core_path",
            STRLEN_CONST("default_core_path"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteString(context.writer,
            playlist->default_core_path
            ? playlist->default_core_path
            : "",
            playlist->default_core_path
            ? strlen(playlist->default_core_path)
            : 0,
            JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "default_core_name",
            STRLEN_CONST("default_core_name"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteString(context.writer,
            playlist->default_core_name
            ? playlist->default_core_name
            : "",
            playlist->default_core_name
            ? strlen(playlist->default_core_name)
            : 0,
            JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      if (!string_is_empty(playlist->base_content_directory))
      {
         json_write_space(context.writer, 2);
         JSON_Writer_WriteString(context.writer, "base_content_directory",
            STRLEN_CONST("base_content_directory"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer,
            playlist->base_content_directory,
            strlen(playlist->base_content_directory),
            JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);
         json_write_new_line(context.writer);
      }

      uint_str[0] = '\0';
      snprintf(uint_str, sizeof(uint_str), "%u", playlist->label_display_mode);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "label_display_mode",
            STRLEN_CONST("label_display_mode"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteNumber(context.writer, uint_str,
            strlen(uint_str), JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      uint_str[0] = '\0';
      snprintf(uint_str, sizeof(uint_str), "%u", playlist->right_thumbnail_mode);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "right_thumbnail_mode",
            STRLEN_CONST("right_thumbnail_mode"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteNumber(context.writer, uint_str,
            strlen(uint_str), JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      uint_str[0] = '\0';
      snprintf(uint_str, sizeof(uint_str), "%u", playlist->left_thumbnail_mode);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "left_thumbnail_mode",
            STRLEN_CONST("left_thumbnail_mode"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteNumber(context.writer, uint_str,
            strlen(uint_str), JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      uint_str[0] = '\0';
      snprintf(uint_str, sizeof(uint_str), "%u", playlist->sort_mode);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "sort_mode",
            STRLEN_CONST("sort_mode"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteNumber(context.writer, uint_str,
            strlen(uint_str), JSON_UTF8);
      JSON_Writer_WriteComma(context.writer);
      json_write_new_line(context.writer);

      json_write_space(context.writer, 2);
      JSON_Writer_WriteString(context.writer, "items",
            STRLEN_CONST("items"), JSON_UTF8);
      JSON_Writer_WriteColon(context.writer);
      json_write_space(context.writer, 1);
      JSON_Writer_WriteStartArray(context.writer);
      json_write_new_line(context.writer);

      for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
      {
         json_write_space(context.writer, 4);
         JSON_Writer_WriteStartObject(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "path",
               STRLEN_CONST("path"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer,
               playlist->entries[i].path
               ? playlist->entries[i].path
               : "",
               playlist->entries[i].path
               ? strlen(playlist->entries[i].path)
               : 0,
               JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "label",
               STRLEN_CONST("label"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer,
               playlist->entries[i].label
               ? playlist->entries[i].label
               : "",
               playlist->entries[i].label
               ? strlen(playlist->entries[i].label)
               : 0,
               JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "core_path",
               STRLEN_CONST("core_path"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer,
               playlist->entries[i].core_path
               ? playlist->entries[i].core_path
               : "",
               playlist->entries[i].core_path
               ? strlen(playlist->entries[i].core_path)
               : 0,
               JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "core_name",
               STRLEN_CONST("core_name"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer,
               playlist->entries[i].core_name
               ? playlist->entries[i].core_name
               : "",
               playlist->entries[i].core_name
               ? strlen(playlist->entries[i].core_name)
               : 0,
               JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "crc32",
               STRLEN_CONST("crc32"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer, playlist->entries[i].crc32 ? playlist->entries[i].crc32 : "",
               playlist->entries[i].crc32
               ? strlen(playlist->entries[i].crc32)
               : 0,
               JSON_UTF8);
         JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
         json_write_space(context.writer, 6);
         JSON_Writer_WriteString(context.writer, "db_name",
               STRLEN_CONST("db_name"), JSON_UTF8);
         JSON_Writer_WriteColon(context.writer);
         json_write_space(context.writer, 1);
         JSON_Writer_WriteString(context.writer, playlist->entries[i].db_name ? playlist->entries[i].db_name : "",
               playlist->entries[i].db_name
               ? strlen(playlist->entries[i].db_name)
               : 0,
               JSON_UTF8);

         if (!string_is_empty(playlist->entries[i].subsystem_ident))
         {
            JSON_Writer_WriteComma(context.writer);
            json_write_new_line(context.writer);
            json_write_space(context.writer, 6);
            JSON_Writer_WriteString(context.writer, "subsystem_ident",
                  STRLEN_CONST("subsystem_ident"), JSON_UTF8);
            JSON_Writer_WriteColon(context.writer);
            json_write_space(context.writer, 1);
            JSON_Writer_WriteString(context.writer, playlist->entries[i].subsystem_ident ? playlist->entries[i].subsystem_ident : "",
                  playlist->entries[i].subsystem_ident
                  ? strlen(playlist->entries[i].subsystem_ident)
                  : 0,
                  JSON_UTF8);
         }

         if (!string_is_empty(playlist->entries[i].subsystem_name))
         {
            JSON_Writer_WriteComma(context.writer);
            json_write_new_line(context.writer);
            json_write_space(context.writer, 6);
            JSON_Writer_WriteString(context.writer, "subsystem_name",
                  STRLEN_CONST("subsystem_name"), JSON_UTF8);
            JSON_Writer_WriteColon(context.writer);
            json_write_space(context.writer, 1);
            JSON_Writer_WriteString(context.writer,
                  playlist->entries[i].subsystem_name
                  ? playlist->entries[i].subsystem_name
                  : "",
                  playlist->entries[i].subsystem_name
                  ? strlen(playlist->entries[i].subsystem_name)
                  : 0, JSON_UTF8);
         }

         if (  playlist->entries[i].subsystem_roms &&
               playlist->entries[i].subsystem_roms->size > 0)
         {
            unsigned j;

            JSON_Writer_WriteComma(context.writer);
            json_write_new_line(context.writer);
            json_write_space(context.writer, 6);
            JSON_Writer_WriteString(context.writer, "subsystem_roms",
                  STRLEN_CONST("subsystem_roms"), JSON_UTF8);
            JSON_Writer_WriteColon(context.writer);
            json_write_space(context.writer, 1);
            JSON_Writer_WriteStartArray(context.writer);
            json_write_new_line(context.writer);

            for (j = 0; j < playlist->entries[i].subsystem_roms->size; j++)
            {
               const struct string_list *roms = playlist->entries[i].subsystem_roms;
               json_write_space(context.writer, 8);
               JSON_Writer_WriteString(context.writer,
                     !string_is_empty(roms->elems[j].data)
                     ? roms->elems[j].data
                     : "",
                     !string_is_empty(roms->elems[j].data)
                     ? strlen(roms->elems[j].data)
                     : 0,
                     JSON_UTF8);

               if (j < playlist->entries[i].subsystem_roms->size - 1)
               {
                  JSON_Writer_WriteComma(context.writer);
                  json_write_new_line(context.writer);
               }
            }

            json_write_new_line(context.writer);
            json_write_space(context.writer, 6);
            JSON_Writer_WriteEndArray(context.writer);
         }

         json_write_new_line(context.writer);

         json_write_space(context.writer, 4);
         JSON_Writer_WriteEndObject(context.writer);

         if (i < len - 1)
            JSON_Writer_WriteComma(context.writer);

         json_write_new_line(context.writer);
      }

      json_write_space(context.writer, 2);
      JSON_Writer_WriteEndArray(context.writer);
      json_write_new_line(context.writer);
      JSON_Writer_WriteEndObject(context.writer);
      json_write_new_line(context.writer);
      JSON_Writer_Free(context.writer);

      playlist->old_format = false;
   }

   playlist->modified   = false;
   playlist->compressed = compressed;

   RARCH_LOG("[Playlist]: Written to playlist file: %s\n", playlist->config.path);
end:
   intfstream_close(file);
   free(file);
}

/**
 * playlist_free:
 * @playlist            : Playlist handle.
 *
 * Frees playlist handle.
 */
void playlist_free(playlist_t *playlist)
{
   size_t i, len;

   if (!playlist)
      return;

   if (playlist->default_core_path)
      free(playlist->default_core_path);
   playlist->default_core_path = NULL;

   if (playlist->default_core_name)
      free(playlist->default_core_name);
   playlist->default_core_name = NULL;

   if (playlist->base_content_directory)
      free(playlist->base_content_directory);
   playlist->base_content_directory = NULL;

   if (playlist->entries)
   {
      for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
      {
         struct playlist_entry *entry = &playlist->entries[i];

         if (entry)
            playlist_free_entry(entry);
      }

      RBUF_FREE(playlist->entries);
   }

   free(playlist);
}

/**
 * playlist_clear:
 * @playlist        	   : Playlist handle.
 *
 * Clears all playlist entries in playlist.
 **/
void playlist_clear(playlist_t *playlist)
{
   size_t i, len;
   if (!playlist)
      return;

   for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
   {
      struct playlist_entry *entry = &playlist->entries[i];

      if (entry)
         playlist_free_entry(entry);
   }
   RBUF_CLEAR(playlist->entries);
}

/**
 * playlist_size:
 * @playlist        	   : Playlist handle.
 *
 * Gets size of playlist.
 * Returns: size of playlist.
 **/
size_t playlist_size(playlist_t *playlist)
{
   if (!playlist)
      return 0;
   return RBUF_LEN(playlist->entries);
}

/**
 * playlist_capacity:
 * @playlist        	   : Playlist handle.
 *
 * Gets maximum capacity of playlist.
 * Returns: maximum capacity of playlist.
 **/
size_t playlist_capacity(playlist_t *playlist)
{
   if (!playlist)
      return 0;
   return playlist->config.capacity;
}

static JSON_Parser_HandlerResult JSONStartArrayHandler(JSON_Parser parser)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);

   pCtx->array_depth++;

   if (pCtx->object_depth == 1)
   {
      if (string_is_equal(pCtx->current_meta_string, "items") && pCtx->array_depth == 1)
         pCtx->in_items = true;
   }
   else if (pCtx->object_depth == 2)
   {
      if (pCtx->array_depth == 2)
         if (string_is_equal(pCtx->current_items_string, "subsystem_roms"))
            pCtx->in_subsystem_roms = true;
   }

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONEndArrayHandler(JSON_Parser parser)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);

   retro_assert(pCtx->array_depth > 0);

   pCtx->array_depth--;

   if (pCtx->object_depth == 1)
   {
      if (pCtx->in_items && string_is_equal(pCtx->current_meta_string, "items") && pCtx->array_depth == 0)
      {
         free(pCtx->current_meta_string);
         pCtx->current_meta_string = NULL;
         pCtx->in_items = false;

         if (pCtx->current_items_string)
         {
            free(pCtx->current_items_string);
            pCtx->current_items_string = NULL;
         }
      }
   }
   else if (pCtx->object_depth == 2)
   {
      if (pCtx->in_subsystem_roms && string_is_equal(pCtx->current_items_string, "subsystem_roms") && pCtx->array_depth == 1)
         pCtx->in_subsystem_roms = false;
   }

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONStartObjectHandler(JSON_Parser parser)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);

   pCtx->object_depth++;

   if (pCtx->in_items && pCtx->object_depth == 2)
   {
      if ((pCtx->array_depth == 1) && !pCtx->capacity_exceeded)
      {
         size_t len = RBUF_LEN(pCtx->playlist->entries);
         if (len < pCtx->playlist->config.capacity)
         {
            /* Allocate memory to fit one more item but don't resize the
             * buffer just yet, wait until JSONEndObjectHandler for that */
            if (!RBUF_TRYFIT(pCtx->playlist->entries, len + 1))
            {
               pCtx->out_of_memory     = true;
               return JSON_Parser_Abort;
            }
            pCtx->current_entry = &pCtx->playlist->entries[len];
            memset(pCtx->current_entry, 0, sizeof(*pCtx->current_entry));
         }
         else
         {
            /* Hit max item limit.
             * Note: We can't just abort here, since there may
             * be more metadata to read at the end of the file... */
            RARCH_WARN("JSON file contains more entries than current playlist capacity. Excess entries will be discarded.\n");
            pCtx->capacity_exceeded  = true;
            pCtx->current_entry      = NULL;
            /* In addition, since we are discarding excess entries,
             * the playlist must be flagged as being modified
             * (i.e. the playlist is not the same as when it was
             * last saved to disk...) */
            pCtx->playlist->modified = true;
         }
      }
   }

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONEndObjectHandler(JSON_Parser parser)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);

   if (pCtx->in_items && pCtx->object_depth == 2)
   {
      if ((pCtx->array_depth == 1) && !pCtx->capacity_exceeded)
         RBUF_RESIZE(pCtx->playlist->entries,
               RBUF_LEN(pCtx->playlist->entries) + 1);
   }

   retro_assert(pCtx->object_depth > 0);

   pCtx->object_depth--;

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONStringHandler(JSON_Parser parser, char *pValue, size_t length, JSON_StringAttributes attributes)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);
   (void)attributes; /* unused */

   if (pCtx->in_items && pCtx->in_subsystem_roms && pCtx->object_depth == 2 && pCtx->array_depth == 2)
   {
      if (pCtx->current_entry_string_list_val && length && !string_is_empty(pValue))
      {
         union string_list_elem_attr attr = {0};

         if (!*pCtx->current_entry_string_list_val)
            *pCtx->current_entry_string_list_val = string_list_new();

         string_list_append(*pCtx->current_entry_string_list_val, pValue, attr);
      }
   }
   else if (pCtx->in_items && pCtx->object_depth == 2)
   {
      if (pCtx->array_depth == 1)
      {
         if (pCtx->current_entry_val && length && !string_is_empty(pValue))
         {
            if (*pCtx->current_entry_val)
               free(*pCtx->current_entry_val);
            *pCtx->current_entry_val = strdup(pValue);
         }
      }
   }
   else if (pCtx->object_depth == 1)
   {
      if (pCtx->array_depth == 0)
      {
         if (pCtx->current_meta_val && length && !string_is_empty(pValue))
         {
            /* handle any top-level playlist metadata here */
#if 0
            RARCH_LOG("[Playlist]: Found meta: %s = %s\n", pCtx->current_meta_string, pValue);
#endif

            free(pCtx->current_meta_string);
            pCtx->current_meta_string = NULL;

            if (*pCtx->current_meta_val)
               free(*pCtx->current_meta_val);

            *pCtx->current_meta_val = strdup(pValue);
         }
      }
   }

   pCtx->current_entry_val = NULL;
   pCtx->current_meta_val  = NULL;

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONNumberHandler(JSON_Parser parser, char *pValue, size_t length, JSON_StringAttributes attributes)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);
   (void)attributes; /* unused */

   if (pCtx->in_items && pCtx->object_depth == 2)
   {
      if (pCtx->array_depth == 1)
      {
         if (pCtx->current_entry_int_val && length && !string_is_empty(pValue))
            *pCtx->current_entry_int_val = (int)strtoul(pValue, NULL, 10);
         else if (pCtx->current_entry_uint_val && length && !string_is_empty(pValue))
            *pCtx->current_entry_uint_val = (unsigned)strtoul(pValue, NULL, 10);
      }
   }
   else if (pCtx->object_depth == 1)
   {
      if (pCtx->array_depth == 0)
      {
         if (pCtx->current_meta_string && length && !string_is_empty(pValue))
         {
            /* handle any top-level playlist metadata here */
#if 0
            RARCH_LOG("[Playlist]: Found meta: %s = %s\n", pCtx->current_meta_string, pValue);
#endif

            free(pCtx->current_meta_string);
            pCtx->current_meta_string = NULL;

            if (pCtx->current_meta_label_display_mode_val)
               *pCtx->current_meta_label_display_mode_val = (enum playlist_label_display_mode)strtoul(pValue, NULL, 10);
            else if (pCtx->current_meta_thumbnail_mode_val)
               *pCtx->current_meta_thumbnail_mode_val = (enum playlist_thumbnail_mode)strtoul(pValue, NULL, 10);
            else if (pCtx->current_meta_sort_mode_val)
               *pCtx->current_meta_sort_mode_val = (enum playlist_sort_mode)strtoul(pValue, NULL, 10);
         }
      }
   }

   pCtx->current_entry_int_val               = NULL;
   pCtx->current_entry_uint_val              = NULL;
   pCtx->current_meta_label_display_mode_val = NULL;
   pCtx->current_meta_thumbnail_mode_val     = NULL;
   pCtx->current_meta_sort_mode_val          = NULL;

   return JSON_Parser_Continue;
}

static JSON_Parser_HandlerResult JSONObjectMemberHandler(JSON_Parser parser, char *pValue, size_t length, JSON_StringAttributes attributes)
{
   JSONContext *pCtx = (JSONContext*)JSON_Parser_GetUserData(parser);
   (void)attributes; /* unused */

   if (pCtx->in_items && pCtx->object_depth == 2)
   {
      if (pCtx->array_depth == 1)
      {
         if (pCtx->current_entry_val)
         {
            /* something went wrong */
            RARCH_WARN("JSON parsing failed at line %d.\n", __LINE__);
            return JSON_Parser_Abort;
         }

         if (length)
         {
            if (!string_is_empty(pValue))
            {
               if (!string_is_empty(pCtx->current_items_string))
                  free(pCtx->current_items_string);
               pCtx->current_items_string = strdup(pValue);
            }

            if (!pCtx->capacity_exceeded)
            {
               if (string_is_equal(pValue, "path"))
                  pCtx->current_entry_val = &pCtx->current_entry->path;
               else if (string_is_equal(pValue, "label"))
                  pCtx->current_entry_val = &pCtx->current_entry->label;
               else if (string_is_equal(pValue, "core_path"))
                  pCtx->current_entry_val = &pCtx->current_entry->core_path;
               else if (string_is_equal(pValue, "core_name"))
                  pCtx->current_entry_val = &pCtx->current_entry->core_name;
               else if (string_is_equal(pValue, "crc32"))
                  pCtx->current_entry_val = &pCtx->current_entry->crc32;
               else if (string_is_equal(pValue, "db_name"))
                  pCtx->current_entry_val = &pCtx->current_entry->db_name;
               else if (string_starts_with_size(pValue, "subsystem_", STRLEN_CONST("subsystem_")))
               {
                  if (string_is_equal(pValue, "subsystem_ident"))
                     pCtx->current_entry_val = &pCtx->current_entry->subsystem_ident;
                  else if (string_is_equal(pValue, "subsystem_name"))
                     pCtx->current_entry_val = &pCtx->current_entry->subsystem_name;
                  else if (string_is_equal(pValue, "subsystem_roms"))
                     pCtx->current_entry_string_list_val = &pCtx->current_entry->subsystem_roms;
               }
               else if (string_starts_with_size(pValue, "runtime_", STRLEN_CONST("runtime_")))
               {
                  if (string_is_equal(pValue, "runtime_hours"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->runtime_hours;
                  else if (string_is_equal(pValue, "runtime_minutes"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->runtime_minutes;
                  else if (string_is_equal(pValue, "runtime_seconds"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->runtime_seconds;
               }
               else if (string_starts_with_size(pValue, "last_played_", STRLEN_CONST("last_played_")))
               {
                  if (string_is_equal(pValue, "last_played_year"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_year;
                  else if (string_is_equal(pValue, "last_played_month"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_month;
                  else if (string_is_equal(pValue, "last_played_day"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_day;
                  else if (string_is_equal(pValue, "last_played_hour"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_hour;
                  else if (string_is_equal(pValue, "last_played_minute"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_minute;
                  else if (string_is_equal(pValue, "last_played_second"))
                     pCtx->current_entry_uint_val = &pCtx->current_entry->last_played_second;
               }
            }
            else
            {
               pCtx->current_entry_val             = NULL;
               pCtx->current_entry_uint_val        = NULL;
               pCtx->current_entry_string_list_val = NULL;
            }
         }
      }
   }
   else if (pCtx->object_depth == 1)
   {
      if (pCtx->array_depth == 0)
      {
         if (pCtx->current_meta_val)
         {
            /* something went wrong */
            RARCH_WARN("JSON parsing failed at line %d.\n", __LINE__);
            return JSON_Parser_Abort;
         }

         if (length)
         {
            if (pCtx->current_meta_string)
               free(pCtx->current_meta_string);
            pCtx->current_meta_string = strdup(pValue);

            if (string_is_equal(pValue, "default_core_path"))
               pCtx->current_meta_val = &pCtx->playlist->default_core_path;
            else if (string_is_equal(pValue, "default_core_name"))
               pCtx->current_meta_val = &pCtx->playlist->default_core_name;
            else if (string_is_equal(pValue, "label_display_mode"))
               pCtx->current_meta_label_display_mode_val = &pCtx->playlist->label_display_mode;
            else if (string_is_equal(pValue, "right_thumbnail_mode"))
               pCtx->current_meta_thumbnail_mode_val = &pCtx->playlist->right_thumbnail_mode;
            else if (string_is_equal(pValue, "left_thumbnail_mode"))
               pCtx->current_meta_thumbnail_mode_val = &pCtx->playlist->left_thumbnail_mode;
            else if (string_is_equal(pValue, "sort_mode"))
               pCtx->current_meta_sort_mode_val = &pCtx->playlist->sort_mode;
            else if (string_is_equal(pValue, "base_content_directory"))
               pCtx->current_meta_val = &pCtx->playlist->base_content_directory;

         }
      }
   }

   return JSON_Parser_Continue;
}

static void get_old_format_metadata_value(
      char *metadata_line, char *value, size_t len)
{
   char *end   = NULL;
   char *start = strchr(metadata_line, '\"');

   if (!start)
      return;

   start++;
   end         = strchr(start, '\"');

   if (!end)
      return;

   *end        = '\0';
   strlcpy(value, start, len);
}

static bool playlist_read_file(playlist_t *playlist)
{
   unsigned i;
   int test_char;
   bool res = true;

#if defined(HAVE_ZLIB)
      /* Always use RZIP interface when reading playlists
       * > this will automatically handle uncompressed
       *   data */
   intfstream_t *file   = intfstream_open_rzip_file(
         playlist->config.path,
         RETRO_VFS_FILE_ACCESS_READ);
#else
   intfstream_t *file   = intfstream_open_file(
         playlist->config.path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
#endif

   /* If playlist file does not exist,
    * create an empty playlist instead */
   if (!file)
      return true;

   playlist->compressed = intfstream_is_compressed(file);

   /* Detect format of playlist
    * > Read file until we find the first printable
    *   non-whitespace ASCII character */
   do
   {
      test_char = intfstream_getc(file);

      if (test_char == EOF) /* read error or end of file */
         goto end;
   }while (!isgraph(test_char) || test_char > 0x7F);

   if (test_char == '{')
   {
      /* New playlist format detected */
#if 0
      RARCH_LOG("[Playlist]: New playlist format detected.\n");
#endif
      playlist->old_format = false;
   }
   else
   {
      /* old playlist format detected */
#if 0
      RARCH_LOG("[Playlist]: Old playlist format detected.\n");
#endif
      playlist->old_format = true;
   }

   /* Reset file to start */
   intfstream_rewind(file);

   if (!playlist->old_format)
   {
      JSONContext context = {0};
      context.parser      = JSON_Parser_Create(NULL);
      context.file        = file;
      context.playlist    = playlist;

      if (!context.parser)
      {
         RARCH_ERR("Failed to create JSON parser\n");
         goto end;
      }

#if 0
      JSON_Parser_SetTrackObjectMembers(context.parser, JSON_True);
#endif
      JSON_Parser_SetAllowBOM(context.parser, JSON_True);
      JSON_Parser_SetAllowComments(context.parser, JSON_True);
      JSON_Parser_SetAllowSpecialNumbers(context.parser, JSON_True);
      JSON_Parser_SetAllowHexNumbers(context.parser, JSON_True);
      JSON_Parser_SetAllowUnescapedControlCharacters(context.parser, JSON_True);
      JSON_Parser_SetReplaceInvalidEncodingSequences(context.parser, JSON_True);

#if 0
      JSON_Parser_SetNullHandler(context.parser,          &JSONNullHandler);
      JSON_Parser_SetBooleanHandler(context.parser,       &JSONBooleanHandler);
      JSON_Parser_SetSpecialNumberHandler(context.parser, &JSONSpecialNumberHandler);
      JSON_Parser_SetArrayItemHandler(context.parser,     &JSONArrayItemHandler);
#endif

      JSON_Parser_SetNumberHandler(context.parser,        &JSONNumberHandler);
      JSON_Parser_SetStringHandler(context.parser,        &JSONStringHandler);
      JSON_Parser_SetStartObjectHandler(context.parser,   &JSONStartObjectHandler);
      JSON_Parser_SetEndObjectHandler(context.parser,     &JSONEndObjectHandler);
      JSON_Parser_SetObjectMemberHandler(context.parser,  &JSONObjectMemberHandler);
      JSON_Parser_SetStartArrayHandler(context.parser,    &JSONStartArrayHandler);
      JSON_Parser_SetEndArrayHandler(context.parser,      &JSONEndArrayHandler);
      JSON_Parser_SetUserData(context.parser, &context);

      while (!intfstream_eof(file))
      {
         char chunk[4096] = {0};
         int64_t length = intfstream_read(file, chunk, sizeof(chunk));

         if (!length && !intfstream_eof(file))
         {
            RARCH_WARN("Could not read JSON input.\n");
            goto json_cleanup;
         }

         if (!JSON_Parser_Parse(context.parser, chunk,
                  (size_t)length, JSON_False))
         {
            if (context.out_of_memory)
            {
               RARCH_WARN("Ran out of memory while parsing JSON playlist\n");
               res = false;
               goto json_cleanup;
            }
            /* Note: Chunk may not be null-terminated.
             * It is therefore dangerous to print its contents.
             * Setting a size limit here mitigates the issue, but
             * in general this is not good practice...
             * Addendum: RARCH_WARN() actually limits the printed
             * buffer size anyway, so this warning message is most
             * likely worthless... */
            RARCH_WARN("Error parsing chunk:\n---snip---\n%.*s\n---snip---\n", 4096, chunk);
            JSONLogError(&context);
            goto json_cleanup;
         }
      }

      if (!JSON_Parser_Parse(context.parser, NULL, 0, JSON_True))
      {
         RARCH_WARN("Error parsing JSON.\n");
         JSONLogError(&context);
         goto json_cleanup;
      }

json_cleanup:

      JSON_Parser_Free(context.parser);

      if (context.current_meta_string)
         free(context.current_meta_string);

      if (context.current_items_string)
         free(context.current_items_string);
   }
   else
   {
      size_t len = RBUF_LEN(playlist->entries);
      char line_buf[PLAYLIST_ENTRIES][PATH_MAX_LENGTH] = {{0}};

      /* Unnecessary, but harmless */
      for (i = 0; i < PLAYLIST_ENTRIES; i++)
         line_buf[i][0] = '\0';

      /* Read playlist entries */
      while (len < playlist->config.capacity)
      {
         size_t i;
         size_t lines_read = 0;

         /* Attempt to read the next 'PLAYLIST_ENTRIES'
          * lines from the file */
         for (i = 0; i < PLAYLIST_ENTRIES; i++)
         {
            *line_buf[i] = '\0';

            if (intfstream_gets(file, line_buf[i], sizeof(line_buf[i])))
            {
               /* Ensure line is NUL terminated, regardless of
                * Windows or Unix line endings */
               string_replace_all_chars(line_buf[i], '\r', '\0');
               string_replace_all_chars(line_buf[i], '\n', '\0');

               lines_read++;
            }
            else
               break;
         }

         /* If a 'full set' of lines were read, then this
          * is a valid playlist entry */
         if (lines_read >= PLAYLIST_ENTRIES)
         {
            struct playlist_entry* entry;

            if (!RBUF_TRYFIT(playlist->entries, len + 1))
            {
               res = false; /* out of memory */
               goto end;
            }
            RBUF_RESIZE(playlist->entries, len + 1);
            entry = &playlist->entries[len++];

            memset(entry, 0, sizeof(*entry));

            /* path */
            if (!string_is_empty(line_buf[0]))
               entry->path      = strdup(line_buf[0]);

            /* label */
            if (!string_is_empty(line_buf[1]))
               entry->label     = strdup(line_buf[1]);

            /* core_path */
            if (!string_is_empty(line_buf[2]))
               entry->core_path = strdup(line_buf[2]);

            /* core_name */
            if (!string_is_empty(line_buf[3]))
               entry->core_name = strdup(line_buf[3]);

            /* crc32 */
            if (!string_is_empty(line_buf[4]))
               entry->crc32     = strdup(line_buf[4]);

            /* db_name */
            if (!string_is_empty(line_buf[5]))
               entry->db_name   = strdup(line_buf[5]);
         }
         /* If fewer than 'PLAYLIST_ENTRIES' lines were
          * read, then this is metadata */
         else
         {
            char default_core_path[PATH_MAX_LENGTH];
            char default_core_name[PATH_MAX_LENGTH];

            default_core_path[0] = '\0';
            default_core_name[0] = '\0';

            /* Get default_core_path */
            if (lines_read < 1)
               break;

            if (strncmp("default_core_path",
                     line_buf[0],
                     STRLEN_CONST("default_core_path")) == 0)
               get_old_format_metadata_value(
                     line_buf[0], default_core_path, sizeof(default_core_path));

            /* Get default_core_name */
            if (lines_read < 2)
               break;

            if (strncmp("default_core_name",
                     line_buf[1],
                     STRLEN_CONST("default_core_name")) == 0)
               get_old_format_metadata_value(
                     line_buf[1], default_core_name, sizeof(default_core_name));

            /* > Populate default core path/name, if required
             *   (if one is empty, the other should be ignored) */
            if (!string_is_empty(default_core_path) &&
                !string_is_empty(default_core_name))
            {
               playlist->default_core_path = strdup(default_core_path);
               playlist->default_core_name = strdup(default_core_name);
            }

            /* Get label_display_mode */
            if (lines_read < 3)
               break;

            if (strncmp("label_display_mode",
                     line_buf[2],
                     STRLEN_CONST("label_display_mode")) == 0)
            {
               unsigned display_mode;
               char display_mode_str[4] = {0};

               get_old_format_metadata_value(
                     line_buf[2], display_mode_str, sizeof(display_mode_str));

               display_mode = string_to_unsigned(display_mode_str);

               if (display_mode <= LABEL_DISPLAY_MODE_KEEP_REGION_AND_DISC_INDEX)
                  playlist->label_display_mode = (enum playlist_label_display_mode)display_mode;
            }

            /* Get thumbnail modes */
            if (lines_read < 4)
               break;

            if (strncmp("thumbnail_mode",
                     line_buf[3],
                     STRLEN_CONST("thumbnail_mode")) == 0)
            {
               char thumbnail_mode_str[8]          = {0};
               struct string_list *thumbnail_modes = NULL;

               get_old_format_metadata_value(
                     line_buf[3], thumbnail_mode_str, sizeof(thumbnail_mode_str));

               thumbnail_modes = string_split(thumbnail_mode_str, "|");

               if (thumbnail_modes)
               {
                  if (thumbnail_modes->size == 2)
                  {
                     unsigned thumbnail_mode;

                     /* Right thumbnail mode */
                     thumbnail_mode = string_to_unsigned(thumbnail_modes->elems[0].data);
                     if (thumbnail_mode <= PLAYLIST_THUMBNAIL_MODE_BOXARTS)
                        playlist->right_thumbnail_mode = (enum playlist_thumbnail_mode)thumbnail_mode;

                     /* Left thumbnail mode */
                     thumbnail_mode = string_to_unsigned(thumbnail_modes->elems[1].data);
                     if (thumbnail_mode <= PLAYLIST_THUMBNAIL_MODE_BOXARTS)
                        playlist->left_thumbnail_mode = (enum playlist_thumbnail_mode)thumbnail_mode;
                  }

                  string_list_free(thumbnail_modes);
               }
            }

            /* Get sort_mode */
            if (lines_read < 5)
               break;

            if (strncmp("sort_mode",
                     line_buf[4],
                     STRLEN_CONST("sort_mode")) == 0)
            {
               unsigned sort_mode;
               char sort_mode_str[4] = {0};

               get_old_format_metadata_value(
                     line_buf[4], sort_mode_str, sizeof(sort_mode_str));

               sort_mode = string_to_unsigned(sort_mode_str);

               if (sort_mode <= PLAYLIST_SORT_MODE_OFF)
                  playlist->sort_mode = (enum playlist_sort_mode)sort_mode;
            }

            /* All metadata parsed -> end of file */
            break;
         }
      }
   }

end:
   intfstream_close(file);
   free(file);
   return res;
}

void playlist_free_cached(void)
{
   if (playlist_cached && !playlist_cached->cached_external)
      playlist_free(playlist_cached);
   playlist_cached = NULL;
}

playlist_t *playlist_get_cached(void)
{
   if (playlist_cached)
      return playlist_cached;
   return NULL;
}

bool playlist_init_cached(const playlist_config_t *config)
{
   playlist_t *playlist = playlist_init(config);
   if (!playlist)
      return false;

   /* If playlist format/compression state
    * does not match requested settings, update
    * file on disk immediately */
   if (
#if defined(HAVE_ZLIB)
       (playlist->compressed != playlist->config.compress) ||
#endif
       (playlist->old_format != playlist->config.old_format))
      playlist_write_file(playlist);

   playlist_cached      = playlist;
   return true;
}

/**
 * playlist_init:
 * @config            	: Playlist configuration object.
 *
 * Creates and initializes a playlist.
 *
 * Returns: handle to new playlist if successful, otherwise NULL
 **/
playlist_t *playlist_init(const playlist_config_t *config)
{
   playlist_t           *playlist = (playlist_t*)malloc(sizeof(*playlist));
   if (!playlist)
      goto error;

   /* Set initial values */
   playlist->modified               = false;
   playlist->old_format             = false;
   playlist->compressed             = false;
   playlist->cached_external        = false;
   playlist->default_core_name      = NULL;
   playlist->default_core_path      = NULL;
   playlist->base_content_directory = NULL;
   playlist->entries                = NULL;
   playlist->label_display_mode     = LABEL_DISPLAY_MODE_DEFAULT;
   playlist->right_thumbnail_mode   = PLAYLIST_THUMBNAIL_MODE_DEFAULT;
   playlist->left_thumbnail_mode    = PLAYLIST_THUMBNAIL_MODE_DEFAULT;
   playlist->sort_mode              = PLAYLIST_SORT_MODE_DEFAULT;

   /* Cache configuration parameters */
   if (!playlist_config_copy(config, &playlist->config))
      goto error;

   /* Attempt to read any existing playlist file */
   if (!playlist_read_file(playlist))
      goto error;

   /* Try auto-fixing paths if enabled, and playlist
    * base content directory is different */
   if (config->autofix_paths && !string_is_equal(playlist->base_content_directory, config->base_content_directory))
   {
      if (!string_is_empty(playlist->base_content_directory))
      {
         size_t i, j, len;
         char tmp_entry_path[PATH_MAX_LENGTH];

         for (i = 0, len = RBUF_LEN(playlist->entries); i < len; i++)
         {
            struct playlist_entry* entry = &playlist->entries[i];

            if (!entry || string_is_empty(entry->path))
               continue;

            /* Fix entry path */
            tmp_entry_path[0] = '\0';
            path_replace_base_path_and_convert_to_local_file_system(
               tmp_entry_path, entry->path,
               playlist->base_content_directory, playlist->config.base_content_directory,
               sizeof(tmp_entry_path));

            free(entry->path);
            entry->path = strdup(tmp_entry_path);

            /* Fix subsystem roms paths*/
            if (entry->subsystem_roms && (entry->subsystem_roms->size > 0))
            {
               struct string_list* subsystem_roms_new_paths = string_list_new();
               union string_list_elem_attr attributes = { 0 };

               if (!subsystem_roms_new_paths)
                  goto error;

               for (j = 0; j < entry->subsystem_roms->size; j++)
               {
                  const char* subsystem_rom_path = entry->subsystem_roms->elems[j].data;

                  if (string_is_empty(subsystem_rom_path))
                     continue;

                  tmp_entry_path[0] = '\0';
                  path_replace_base_path_and_convert_to_local_file_system(
                     tmp_entry_path, subsystem_rom_path,
                     playlist->base_content_directory, playlist->config.base_content_directory,
                     sizeof(tmp_entry_path));
                  string_list_append(subsystem_roms_new_paths, tmp_entry_path, attributes);
               }

               string_list_free(entry->subsystem_roms);
               entry->subsystem_roms = subsystem_roms_new_paths;
            }
         }
      }

      /* Update playlist base content directory*/
      if (playlist->base_content_directory)
         free(playlist->base_content_directory);
      playlist->base_content_directory = strdup(playlist->config.base_content_directory);

      /* Save playlist */
      playlist->modified = true;
      playlist_write_file(playlist);
   }

   return playlist;

error:
   playlist_free(playlist);
   return NULL;
}

static int playlist_qsort_func(const struct playlist_entry *a,
      const struct playlist_entry *b)
{
   char *a_str            = NULL;
   char *b_str            = NULL;
   char *a_fallback_label = NULL;
   char *b_fallback_label = NULL;
   int ret                = 0;

   if (!a || !b)
      goto end;

   a_str                  = a->label;
   b_str                  = b->label;

   /* It is quite possible for playlist labels
    * to be blank. If that is the case, have to use
    * filename as a fallback (this is slow, but we
    * have no other option...) */
   if (string_is_empty(a_str))
   {
      a_fallback_label = (char*)calloc(PATH_MAX_LENGTH, sizeof(char));

      if (!a_fallback_label)
         goto end;

      if (!string_is_empty(a->path))
         fill_short_pathname_representation(a_fallback_label, a->path, PATH_MAX_LENGTH * sizeof(char));
      /* If filename is also empty, use core name
       * instead -> this matches the behaviour of
       * menu_displaylist_parse_playlist() */
      else if (!string_is_empty(a->core_name))
         strlcpy(a_fallback_label, a->core_name, PATH_MAX_LENGTH * sizeof(char));

      /* If both filename and core name are empty,
       * then have to compare an empty string
       * -> again, this is to match the behaviour of
       * menu_displaylist_parse_playlist() */

      a_str = a_fallback_label;
   }

   if (string_is_empty(b_str))
   {
      b_fallback_label = (char*)calloc(PATH_MAX_LENGTH, sizeof(char));

      if (!b_fallback_label)
         goto end;

      if (!string_is_empty(b->path))
         fill_short_pathname_representation(b_fallback_label, b->path, PATH_MAX_LENGTH * sizeof(char));
      else if (!string_is_empty(b->core_name))
         strlcpy(b_fallback_label, b->core_name, PATH_MAX_LENGTH * sizeof(char));

      b_str = b_fallback_label;
   }

   ret = strcasecmp(a_str, b_str);

end:

   a_str = NULL;
   b_str = NULL;

   if (a_fallback_label)
   {
      free(a_fallback_label);
      a_fallback_label = NULL;
   }

   if (b_fallback_label)
   {
      free(b_fallback_label);
      b_fallback_label = NULL;
   }

   return ret;
}

void playlist_qsort(playlist_t *playlist)
{
   /* Avoid inadvertent sorting if 'sort mode'
    * has been set explicitly to PLAYLIST_SORT_MODE_OFF */
   if (!playlist ||
       (playlist->sort_mode == PLAYLIST_SORT_MODE_OFF) ||
       !playlist->entries)
      return;

   qsort(playlist->entries, RBUF_LEN(playlist->entries),
         sizeof(struct playlist_entry),
         (int (*)(const void *, const void *))playlist_qsort_func);
}

void command_playlist_push_write(
      playlist_t *playlist,
      const struct playlist_entry *entry)
{
   if (!playlist)
      return;

   if (playlist_push(playlist, entry))
      playlist_write_file(playlist);
}

void command_playlist_update_write(
      playlist_t *plist,
      size_t idx,
      const struct playlist_entry *entry)
{
   playlist_t *playlist = plist ? plist : playlist_get_cached();

   if (!playlist)
      return;

   playlist_update(
         playlist,
         idx,
         entry);

   playlist_write_file(playlist);
}

bool playlist_index_is_valid(playlist_t *playlist, size_t idx,
      const char *path, const char *core_path)
{
   if (!playlist)
      return false;

   if (idx >= RBUF_LEN(playlist->entries))
      return false;

   return string_is_equal(playlist->entries[idx].path, path) &&
          string_is_equal(path_basename(playlist->entries[idx].core_path), path_basename(core_path));
}

bool playlist_entries_are_equal(
      const struct playlist_entry *entry_a,
      const struct playlist_entry *entry_b,
      const playlist_config_t *config)
{
   char real_path_a[PATH_MAX_LENGTH];
   char real_core_path_a[PATH_MAX_LENGTH];

   real_path_a[0]      = '\0';
   real_core_path_a[0] = '\0';

   /* Sanity check */
   if (!entry_a || !entry_b || !config)
      return false;

   if (string_is_empty(entry_a->path) &&
       string_is_empty(entry_a->core_path) &&
       string_is_empty(entry_b->path) &&
       string_is_empty(entry_b->core_path))
      return true;

   /* Check content paths */
   if (!string_is_empty(entry_a->path))
   {
      strlcpy(real_path_a, entry_a->path, sizeof(real_path_a));
      path_resolve_realpath(real_path_a, sizeof(real_path_a), true);
   }

   if (!playlist_path_equal(
         real_path_a, entry_b->path, config))
      return false;

   /* Check core paths */
   if (!string_is_empty(entry_a->core_path))
   {
      strlcpy(real_core_path_a, entry_a->core_path, sizeof(real_core_path_a));
      if (!string_is_equal(real_core_path_a, FILE_PATH_DETECT) &&
          !string_is_equal(real_core_path_a, FILE_PATH_BUILTIN))
         path_resolve_realpath(real_core_path_a, sizeof(real_core_path_a), true);
   }

   return playlist_core_path_equal(real_core_path_a, entry_b->core_path, config);
}

void playlist_get_crc32(playlist_t *playlist, size_t idx,
      const char **crc32)
{
   if (!playlist || idx >= RBUF_LEN(playlist->entries))
      return;

   if (crc32)
      *crc32 = playlist->entries[idx].crc32;
}

void playlist_get_db_name(playlist_t *playlist, size_t idx,
      const char **db_name)
{
   if (!playlist || idx >= RBUF_LEN(playlist->entries))
      return;

   if (db_name)
   {
      if (!string_is_empty(playlist->entries[idx].db_name))
         *db_name = playlist->entries[idx].db_name;
      else
      {
         const char *conf_path_basename = path_basename(playlist->config.path);

         /* Only use file basename if this is a 'collection' playlist
          * (i.e. ignore history/favourites) */
         if (
                  !string_is_empty(conf_path_basename)
               && !string_ends_with_size(conf_path_basename, "_history.lpl",
                        strlen(conf_path_basename), STRLEN_CONST("_history.lpl"))
               && !string_is_equal(conf_path_basename,
                        FILE_PATH_CONTENT_FAVORITES)
            )
            *db_name = conf_path_basename;
      }
   }
}

char *playlist_get_default_core_path(playlist_t *playlist)
{
   if (!playlist)
      return NULL;
   return playlist->default_core_path;
}

char *playlist_get_default_core_name(playlist_t *playlist)
{
   if (!playlist)
      return NULL;
   return playlist->default_core_name;
}

enum playlist_label_display_mode playlist_get_label_display_mode(playlist_t *playlist)
{
   if (!playlist)
      return LABEL_DISPLAY_MODE_DEFAULT;
   return playlist->label_display_mode;
}

enum playlist_thumbnail_mode playlist_get_thumbnail_mode(
      playlist_t *playlist, enum playlist_thumbnail_id thumbnail_id)
{
   if (!playlist)
      return PLAYLIST_THUMBNAIL_MODE_DEFAULT;

   if (thumbnail_id == PLAYLIST_THUMBNAIL_RIGHT)
      return playlist->right_thumbnail_mode;
   else if (thumbnail_id == PLAYLIST_THUMBNAIL_LEFT)
      return playlist->left_thumbnail_mode;

   /* Fallback */
   return PLAYLIST_THUMBNAIL_MODE_DEFAULT;
}

enum playlist_sort_mode playlist_get_sort_mode(playlist_t *playlist)
{
   if (!playlist)
      return PLAYLIST_SORT_MODE_DEFAULT;
   return playlist->sort_mode;
}

void playlist_set_default_core_path(playlist_t *playlist, const char *core_path)
{
   char real_core_path[PATH_MAX_LENGTH];

   if (!playlist || string_is_empty(core_path))
      return;

   real_core_path[0] = '\0';

   /* Get 'real' core path */
   strlcpy(real_core_path, core_path, sizeof(real_core_path));
   if (!string_is_equal(real_core_path, FILE_PATH_DETECT) &&
       !string_is_equal(real_core_path, FILE_PATH_BUILTIN))
       playlist_resolve_path(PLAYLIST_SAVE,
             real_core_path, sizeof(real_core_path));

   if (string_is_empty(real_core_path))
      return;

   if (!string_is_equal(playlist->default_core_path, real_core_path))
   {
      if (playlist->default_core_path)
         free(playlist->default_core_path);
      playlist->default_core_path = strdup(real_core_path);
      playlist->modified = true;
   }
}

void playlist_set_default_core_name(
      playlist_t *playlist, const char *core_name)
{
   if (!playlist || string_is_empty(core_name))
      return;

   if (!string_is_equal(playlist->default_core_name, core_name))
   {
      if (playlist->default_core_name)
         free(playlist->default_core_name);
      playlist->default_core_name = strdup(core_name);
      playlist->modified = true;
   }
}

void playlist_set_label_display_mode(playlist_t *playlist,
      enum playlist_label_display_mode label_display_mode)
{
   if (!playlist)
      return;

   if (playlist->label_display_mode != label_display_mode)
   {
      playlist->label_display_mode = label_display_mode;
      playlist->modified = true;
   }
}

void playlist_set_thumbnail_mode(
      playlist_t *playlist, enum playlist_thumbnail_id thumbnail_id,
      enum playlist_thumbnail_mode thumbnail_mode)
{
   if (!playlist)
      return;

   switch (thumbnail_id)
   {
      case PLAYLIST_THUMBNAIL_RIGHT:
         playlist->right_thumbnail_mode = thumbnail_mode;
         playlist->modified             = true;
         break;
      case PLAYLIST_THUMBNAIL_LEFT:
         playlist->left_thumbnail_mode = thumbnail_mode;
         playlist->modified            = true;
         break;
   }
}

void playlist_set_sort_mode(playlist_t *playlist,
      enum playlist_sort_mode sort_mode)
{
   if (!playlist)
      return;

   if (playlist->sort_mode != sort_mode)
   {
      playlist->sort_mode = sort_mode;
      playlist->modified  = true;
   }
}

/* Returns true if specified entry has a valid
 * core association (i.e. a non-empty string
 * other than DETECT) */
bool playlist_entry_has_core(const struct playlist_entry *entry)
{
   if (!entry                                              ||
       string_is_empty(entry->core_path)                   ||
       string_is_empty(entry->core_name)                   ||
       string_is_equal(entry->core_path, FILE_PATH_DETECT) ||
       string_is_equal(entry->core_name, FILE_PATH_DETECT))
      return false;

   return true;
}

/* Fetches core info object corresponding to the
 * currently associated core of the specified
 * playlist entry.
 * Returns NULL if entry does not have a valid
 * core association */
core_info_t *playlist_entry_get_core_info(const struct playlist_entry* entry)
{
   core_info_ctx_find_t core_info;

   if (!playlist_entry_has_core(entry))
      return NULL;

   /* Search for associated core */
   core_info.inf  = NULL;
   core_info.path = entry->core_path;

   if (core_info_find(&core_info))
      return core_info.inf;

   return NULL;
}

/* Fetches core info object corresponding to the
 * currently associated default core of the
 * specified playlist.
 * Returns NULL if playlist does not have a valid
 * default core association */
core_info_t *playlist_get_default_core_info(playlist_t* playlist)
{
   core_info_ctx_find_t core_info;

   if (!playlist ||
       string_is_empty(playlist->default_core_path) ||
       string_is_empty(playlist->default_core_name) ||
       string_is_equal(playlist->default_core_path, FILE_PATH_DETECT) ||
       string_is_equal(playlist->default_core_name, FILE_PATH_DETECT))
      return NULL;

   /* Search for associated core */
   core_info.inf  = NULL;
   core_info.path = playlist->default_core_path;

   if (core_info_find(&core_info))
      return core_info.inf;

   return NULL;
}

