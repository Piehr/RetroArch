/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
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

#include <stdlib.h>
#include <string.h>

#include <rhash.h>
#include <compat/strl.h>
#include <retro_endianness.h>
#include <streams/file_stream.h>

#include "configuration.h"
#include "movie.h"
#include "core.h"
#include "content.h"
#include "retroarch.h"
#include "runloop.h"
#include "msg_hash.h"
#include "verbosity.h"

#include "command.h"
#include "file_path_special.h"

struct bsv_movie
{
   RFILE *file;

   /* A ring buffer keeping track of positions
    * in the file for each frame. */
   size_t *frame_pos;
   size_t frame_mask;
   size_t frame_ptr;

   size_t min_file_pos;

   size_t state_size;
   uint8_t *state;

   bool playback;
   bool first_rewind;
   bool did_rewind;
};

struct bsv_state
{
   /* Movie playback/recording support. */
   char movie_path[PATH_MAX_LENGTH];
   bool movie_playback;
   bool eof_exit;

   /* Immediate playback/recording. */
   char movie_start_path[PATH_MAX_LENGTH];
   bool movie_start_recording;
   bool movie_start_playback;
   bool movie_end;
};

static bsv_movie_t     *bsv_movie_state_handle = NULL;
static struct bsv_state bsv_movie_state;

static bool bsv_movie_init_playback(bsv_movie_t *handle, const char *path)
{
   uint32_t state_size;
   uint32_t *content_crc_ptr = NULL;
   uint32_t header[4]        = {0};
   RFILE *file               = filestream_open(path, RFILE_MODE_READ, -1);

   if (!file)
   {
      RARCH_ERR("Could not open BSV file for playback, path : \"%s\".\n", path);
      return false;
   }

   handle->file              = file;
   handle->playback          = true;

   if (filestream_read(handle->file, header, 4) != 4)
   {
      RARCH_ERR("%s\n", msg_hash_to_str(MSG_COULD_NOT_READ_MOVIE_HEADER));
      return false;
   }

   /* Compatibility with old implementation that
    * used incorrect documentation. */
   if (swap_if_little32(header[MAGIC_INDEX]) != BSV_MAGIC
         && swap_if_big32(header[MAGIC_INDEX]) != BSV_MAGIC)
   {
      RARCH_ERR("%s\n", msg_hash_to_str(MSG_MOVIE_FILE_IS_NOT_A_VALID_BSV1_FILE));
      return false;
   }

   content_get_crc(&content_crc_ptr);

   if (swap_if_big32(header[CRC_INDEX]) != *content_crc_ptr)
      RARCH_WARN("%s.\n", msg_hash_to_str(MSG_CRC32_CHECKSUM_MISMATCH));

   state_size = swap_if_big32(header[STATE_SIZE_INDEX]);

   if (state_size)
   {
      retro_ctx_size_info_t info;
      retro_ctx_serialize_info_t serial_info;
      uint8_t *buf       = (uint8_t*)malloc(state_size);

      if (!buf)
         return false;

      handle->state      = buf;
      handle->state_size = state_size;

      if (filestream_read(handle->file, handle->state, state_size) != state_size)
      {
         RARCH_ERR("%s\n", msg_hash_to_str(MSG_COULD_NOT_READ_STATE_FROM_MOVIE));
         return false;
      }

      core_serialize_size( &info);

      if (info.size == state_size)
      {
         serial_info.data_const = handle->state;
         serial_info.size       = state_size;
         core_unserialize(&serial_info);
      }
      else
         RARCH_WARN("%s\n",
               msg_hash_to_str(MSG_MOVIE_FORMAT_DIFFERENT_SERIALIZER_VERSION));
   }

   handle->min_file_pos = sizeof(header) + state_size;

   return true;
}

static bool bsv_movie_init_record(bsv_movie_t *handle, const char *path)
{
   retro_ctx_size_info_t info;
   uint32_t state_size;
   uint32_t header[4]        = {0};
   uint32_t *content_crc_ptr = NULL;
   RFILE *file               = filestream_open(path, RFILE_MODE_WRITE, -1);

   if (!file)
   {
      RARCH_ERR("Could not open BSV file for recording, path : \"%s\".\n", path);
      return false;
   }

   handle->file              = file;

   content_get_crc(&content_crc_ptr);

   /* This value is supposed to show up as
    * BSV1 in a HEX editor, big-endian. */
   header[MAGIC_INDEX]      = swap_if_little32(BSV_MAGIC);
   header[CRC_INDEX]        = swap_if_big32(*content_crc_ptr);

   core_serialize_size(&info);

   state_size               = (unsigned)info.size;

   header[STATE_SIZE_INDEX] = swap_if_big32(state_size);

   filestream_write(handle->file, header, sizeof(uint32_t));

   handle->min_file_pos     = sizeof(header) + state_size;
   handle->state_size       = state_size;

   if (state_size)
   {
      retro_ctx_serialize_info_t serial_info;

      handle->state = (uint8_t*)malloc(state_size);
      if (!handle->state)
         return false;

      serial_info.data = handle->state;
      serial_info.size = state_size;

      core_serialize(&serial_info);

      filestream_write(handle->file, handle->state, state_size);
   }

   return true;
}

static void bsv_movie_free(bsv_movie_t *handle)
{
   if (!handle)
      return;

   filestream_close(handle->file);

   free(handle->state);
   free(handle->frame_pos);
   free(handle);
}

static bsv_movie_t *bsv_movie_init(const char *path,
      enum rarch_movie_type type)
{
   size_t *frame_pos   = NULL;
   bsv_movie_t *handle = (bsv_movie_t*)calloc(1, sizeof(*handle));

   if (!handle)
      return NULL;

   if (type == RARCH_MOVIE_PLAYBACK)
   {
      if (!bsv_movie_init_playback(handle, path))
         goto error;
   }
   else if (!bsv_movie_init_record(handle, path))
      goto error;

   /* Just pick something really large 
    * ~1 million frames rewind should do the trick. */
   if (!(frame_pos = (size_t*)calloc((1 << 20), sizeof(size_t))))
      goto error; 

   handle->frame_pos       = frame_pos;

   handle->frame_pos[0]    = handle->min_file_pos;
   handle->frame_mask      = (1 << 20) - 1;

   return handle;

error:
   bsv_movie_free(handle);
   return NULL;
}

/* Used for rewinding while playback/record. */
void bsv_movie_set_frame_start(void)
{
   if (bsv_movie_state_handle)
      bsv_movie_state_handle->frame_pos[bsv_movie_state_handle->frame_ptr] 
         = filestream_tell(bsv_movie_state_handle->file);
}

void bsv_movie_set_frame_end(void)
{
   if (!bsv_movie_state_handle)
      return;

   bsv_movie_state_handle->frame_ptr    = 
      (bsv_movie_state_handle->frame_ptr + 1) & bsv_movie_state_handle->frame_mask;

   bsv_movie_state_handle->first_rewind = 
      !bsv_movie_state_handle->did_rewind;
   bsv_movie_state_handle->did_rewind   = false;
}

static void bsv_movie_frame_rewind(bsv_movie_t *handle)
{
   handle->did_rewind = true;

   if (     (handle->frame_ptr <= 1) 
         && (handle->frame_pos[0] == handle->min_file_pos))
   {
      /* If we're at the beginning... */
      handle->frame_ptr = 0;
      filestream_seek(handle->file, handle->min_file_pos, SEEK_SET);
   }
   else
   {
      /* First time rewind is performed, the old frame is simply replayed.
       * However, playing back that frame caused us to read data, and push
       * data to the ring buffer.
       *
       * Sucessively rewinding frames, we need to rewind past the read data,
       * plus another. */
      handle->frame_ptr = (handle->frame_ptr -
            (handle->first_rewind ? 1 : 2)) & handle->frame_mask;
      filestream_seek(handle->file, handle->frame_pos[handle->frame_ptr], SEEK_SET);
   }

   if (filestream_tell(handle->file) <= (long)handle->min_file_pos)
   {
      /* We rewound past the beginning. */

      if (!handle->playback)
      {
         retro_ctx_serialize_info_t serial_info;

         /* If recording, we simply reset
          * the starting point. Nice and easy. */

         filestream_seek(handle->file, 4 * sizeof(uint32_t), SEEK_SET);

         serial_info.data = handle->state;
         serial_info.size = handle->state_size;

         core_serialize(&serial_info);

         filestream_write(handle->file, handle->state, handle->state_size);
      }
      else
         filestream_seek(handle->file, handle->min_file_pos, SEEK_SET);
   }
}

static void bsv_movie_init_state(void)
{
   settings_t *settings = config_get_ptr();

   if (bsv_movie_ctl(BSV_MOVIE_CTL_START_PLAYBACK, NULL))
   {
      if (!(bsv_movie_init_handle(bsv_movie_state.movie_start_path,
                  RARCH_MOVIE_PLAYBACK)))
      {
         RARCH_ERR("%s: \"%s\".\n",
               msg_hash_to_str(MSG_FAILED_TO_LOAD_MOVIE_FILE),
               bsv_movie_state.movie_start_path);
         retroarch_fail(1, "event_init_movie()");
      }

      bsv_movie_state.movie_playback = true;
      runloop_msg_queue_push(msg_hash_to_str(MSG_STARTING_MOVIE_PLAYBACK),
            2, 180, false);
      RARCH_LOG("%s.\n", msg_hash_to_str(MSG_STARTING_MOVIE_PLAYBACK));

      configuration_set_uint(settings, settings->rewind_granularity, 1);
   }
   else if (bsv_movie_state.movie_start_recording)
   {
      char msg[256];
      snprintf(msg, sizeof(msg),
            "%s \"%s\".",
            msg_hash_to_str(MSG_STARTING_MOVIE_RECORD_TO),
            bsv_movie_state.movie_start_path);

      if (!(bsv_movie_init_handle(bsv_movie_state.movie_start_path,
                  RARCH_MOVIE_RECORD)))
      {
         runloop_msg_queue_push(
               msg_hash_to_str(MSG_FAILED_TO_START_MOVIE_RECORD),
               1, 180, true);
         RARCH_ERR("%s.\n", msg_hash_to_str(MSG_FAILED_TO_START_MOVIE_RECORD));
         return;
      }

      runloop_msg_queue_push(msg, 1, 180, true);
      RARCH_LOG("%s \"%s\".\n",
            msg_hash_to_str(MSG_STARTING_MOVIE_RECORD_TO),
            bsv_movie_state.movie_start_path);

      configuration_set_uint(settings, settings->rewind_granularity, 1);
   }
}

bool bsv_movie_get_input(int16_t *bsv_data)
{
   if (filestream_read(bsv_movie_state_handle->file, bsv_data, 1) != 1)
      return false;

   *bsv_data = swap_if_big16(*bsv_data);

   return true;
}

bool bsv_movie_is_playback_on(void)
{
   return bsv_movie_state_handle && bsv_movie_state.movie_playback;
}

bool bsv_movie_is_playback_off(void)
{
   return bsv_movie_state_handle && !bsv_movie_state.movie_playback;
}

bool bsv_movie_ctl(enum bsv_ctl_state state, void *data)
{
   switch (state)
   {
      case BSV_MOVIE_CTL_IS_INITED:
         return bsv_movie_state_handle;
      case BSV_MOVIE_CTL_START_RECORDING:
         return bsv_movie_state.movie_start_recording;
      case BSV_MOVIE_CTL_SET_START_RECORDING:
         bsv_movie_state.movie_start_recording = true;
         break;
      case BSV_MOVIE_CTL_UNSET_START_RECORDING:
         bsv_movie_state.movie_start_recording = false;
         break;
      case BSV_MOVIE_CTL_START_PLAYBACK:
         return bsv_movie_state.movie_start_playback;
      case BSV_MOVIE_CTL_SET_START_PLAYBACK:
         bsv_movie_state.movie_start_playback = true;
         break;
      case BSV_MOVIE_CTL_UNSET_START_PLAYBACK:
         bsv_movie_state.movie_start_playback = false;
         break;
      case BSV_MOVIE_CTL_SET_END_EOF:
         bsv_movie_state.eof_exit = true;
         break;
      case BSV_MOVIE_CTL_END_EOF:
         return bsv_movie_state.movie_end && bsv_movie_state.eof_exit;
      case BSV_MOVIE_CTL_SET_END:
         bsv_movie_state.movie_end = true;
         break;
      case BSV_MOVIE_CTL_UNSET_END:
         bsv_movie_state.movie_end = false;
         break;
      case BSV_MOVIE_CTL_UNSET_PLAYBACK:
         bsv_movie_state.movie_playback = false;
         break;
      case BSV_MOVIE_CTL_DEINIT:
         if (bsv_movie_state_handle)
            bsv_movie_free(bsv_movie_state_handle);
         bsv_movie_state_handle = NULL;
         break;
      case BSV_MOVIE_CTL_INIT:
         bsv_movie_init_state();
         break;
      case BSV_MOVIE_CTL_FRAME_REWIND:
         bsv_movie_frame_rewind(bsv_movie_state_handle);
         break;
      case BSV_MOVIE_CTL_SET_INPUT:
         {
            int16_t *bsv_data = (int16_t*)data;

            *bsv_data = swap_if_big16(*bsv_data);
            filestream_write(bsv_movie_state_handle->file, bsv_data, 1);
         }
         break;
      case BSV_MOVIE_CTL_NONE:
      default:
         return false;
   }

   return true;
}

void bsv_movie_set_path(const char *path)
{
   strlcpy(bsv_movie_state.movie_path,
         path, sizeof(bsv_movie_state.movie_path));
}

void bsv_movie_set_start_path(const char *path)
{
   strlcpy(bsv_movie_state.movie_start_path, path,
         sizeof(bsv_movie_state.movie_start_path));
}

bool bsv_movie_init_handle(const char *path,
      enum rarch_movie_type type)
{
   bsv_movie_state_handle = bsv_movie_init(path, type);
   if (!bsv_movie_state_handle)
      return false;
   return true;
}

/* Checks if movie is being played back. */
static bool bsv_movie_check_movie_playback(void)
{
   runloop_msg_queue_push(
         msg_hash_to_str(MSG_MOVIE_PLAYBACK_ENDED), 2, 180, false);
   RARCH_LOG("%s\n", msg_hash_to_str(MSG_MOVIE_PLAYBACK_ENDED));

   command_event(CMD_EVENT_BSV_MOVIE_DEINIT, NULL);

   bsv_movie_state.movie_end      = false;
   bsv_movie_state.movie_playback = false;

   return true;
}

/* Checks if movie is being recorded. */
static bool runloop_check_movie_record(void)
{
   if (!bsv_movie_state_handle)
      return false;

   runloop_msg_queue_push(
         msg_hash_to_str(MSG_MOVIE_RECORD_STOPPED), 2, 180, true);
   RARCH_LOG("%s\n", msg_hash_to_str(MSG_MOVIE_RECORD_STOPPED));

   command_event(CMD_EVENT_BSV_MOVIE_DEINIT, NULL);

   return true;
}

static bool runloop_check_movie_init(void)
{
   char msg[128], path[PATH_MAX_LENGTH];
   settings_t *settings       = config_get_ptr();

   msg[0] = path[0]           = '\0';

   configuration_set_uint(settings, settings->rewind_granularity, 1);

   if (settings->ints.state_slot > 0)
      snprintf(path, sizeof(path), "%s%d",
            bsv_movie_state.movie_path, settings->ints.state_slot);
   else
      strlcpy(path, bsv_movie_state.movie_path, sizeof(path));

   strlcat(path,
         file_path_str(FILE_PATH_BSV_EXTENSION),
         sizeof(path));

   snprintf(msg, sizeof(msg), "%s \"%s\".",
         msg_hash_to_str(MSG_STARTING_MOVIE_RECORD_TO),
         path);

   bsv_movie_init_handle(path, RARCH_MOVIE_RECORD);

   if (!bsv_movie_state_handle)
      return false;

   if (bsv_movie_state_handle)
   {
      runloop_msg_queue_push(msg, 2, 180, true);
      RARCH_LOG("%s \"%s\".\n",
            msg_hash_to_str(MSG_STARTING_MOVIE_RECORD_TO),
            path);
   }
   else
   {
      runloop_msg_queue_push(
            msg_hash_to_str(MSG_FAILED_TO_START_MOVIE_RECORD),
            2, 180, true);
      RARCH_ERR("%s\n",
            msg_hash_to_str(MSG_FAILED_TO_START_MOVIE_RECORD));
   }

   return true;
}

bool bsv_movie_check(void)
{
   if (bsv_movie_state_handle && bsv_movie_state.movie_playback)
   {
      if (!bsv_movie_state.movie_end)
         return false;
      return bsv_movie_check_movie_playback();
   }

   if (!bsv_movie_state_handle)
   {
      if (bsv_movie_state_handle)
         return false;
      return runloop_check_movie_init();
   }

   return runloop_check_movie_record();
}
