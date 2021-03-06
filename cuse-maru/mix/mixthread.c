/*  cuse-maru - CUSE implementation of Open Sound System using libmaru.
 *  Copyright (C) 2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2012 - Agnes Heyer
 *
 *  cuse-maru is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  cuse-maru is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with cuse-maru.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cuse-mix.h"
#include "mixthread.h"
#include "utils.h"
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>

static pthread_t g_thread;

static bool write_all(int fd, const void *data_, size_t size)
{
   const uint8_t *data = data_;

   while (size)
   {
      ssize_t ret = write(fd, data, size);
      if (ret <= 0)
         return false;

      data += ret;
      size -= ret;
   }

   return true;
}

static void mix_streams(const struct epoll_event *events, size_t num_events,
      int16_t *mix_buffer,
      size_t fragsize)
{
   size_t samples = fragsize / (g_state.format.bits / 8);

   float tmp_mix_buffer_f[samples] AUDIO_ALIGNED;
   int16_t tmp_mix_buffer_i[samples] AUDIO_ALIGNED;
   float mix_buffer_f[samples] AUDIO_ALIGNED;

   memset(mix_buffer_f, 0, sizeof(mix_buffer_f));

   for (unsigned i = 0; i < num_events; i++)
   {
      memset(tmp_mix_buffer_f, 0, sizeof(tmp_mix_buffer_f));

      struct stream_info *info = events[i].data.ptr;

      // We were pinged, clear eventfd.
      if (!info)
      {
         uint64_t dummy;
         eventfd_read(g_state.ping_fd, &dummy);
         continue;
      }

      if (info->src)
      {
         size_t has_read = resampler_process(info->src,
               tmp_mix_buffer_f,
               samples / info->channels);

         info->write_cnt += has_read;
      }
      else
      {
         ssize_t has_read = maru_fifo_read(info->fifo, tmp_mix_buffer_i, fragsize);

         if (has_read > 0)
            info->write_cnt += has_read;

         audio_convert_s16_to_float(tmp_mix_buffer_f, tmp_mix_buffer_i, samples);
      }

      stream_poll_signal(info);

      if (maru_fifo_read_notify_ack(info->fifo) != LIBMARU_SUCCESS)
      {
         epoll_ctl(g_state.epfd, EPOLL_CTL_DEL,
               maru_fifo_read_notify_fd(info->fifo), NULL);

         eventfd_write(info->sync_fd, 1);
      }

      audio_mix_volume(mix_buffer_f, tmp_mix_buffer_f, info->volume_f, samples);
   }

   audio_convert_float_to_s16(mix_buffer, mix_buffer_f, samples);
}

static void *thread_entry(void *data)
{
   (void)data;

   int16_t *mix_buffer = malloc(g_state.format.fragsize);
   if (!mix_buffer)
   {
      fprintf(stderr, "Failed to allocate mixbuffer.\n");
      exit(1);
   }

   for (;;)
   {
      struct epoll_event events[MAX_STREAMS];

      int ret = epoll_wait(g_state.epfd, events, MAX_STREAMS, -1);
      if (ret < 0)
      {
         if (errno == EINTR)
            continue;

         perror("epoll_wait");
         exit(1);
      }

      mix_streams(events, ret, mix_buffer, g_state.format.fragsize);

      if (!write_all(g_state.dev, mix_buffer, g_state.format.fragsize))
      {
         fprintf(stderr, "write_all failed!\n");
         exit(1);
      }

      memset(mix_buffer, 0, g_state.format.fragsize);
   }

   free(mix_buffer);
   return NULL;
}

bool start_mix_thread(void)
{
   if (pthread_create(&g_thread, NULL, thread_entry, NULL) < 0)
   {
      perror("pthread_create");
      return false;
   }

   return true;
}

