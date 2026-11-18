/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "game_input.h"
#include "log.h"

void
GameInput::init(int iframe, char *ibits, int isize, int offset)
{
   ASSERT(isize);
   ASSERT(isize <= GAMEINPUT_MAX_BYTES);
   frame = iframe;
   size = isize;
   memset(bits, 0, sizeof(bits));
   if (ibits) {
      memcpy(bits + (offset * isize), ibits, isize);
   }
}

void
GameInput::init(int iframe, char *ibits, int isize)
{
   ASSERT(isize);
   ASSERT(isize <= GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS);
   frame = iframe;
   size = isize;
   memset(bits, 0, sizeof(bits));
   if (ibits) {
      memcpy(bits, ibits, isize);
   }
}

void
GameInput::desc(char *buf, size_t buf_size, bool show_frame) const
{
   ASSERT(size);
   int pos = 0;
   if (buf_size == 0) {
      return;
   }
   if (show_frame) {
      pos = snprintf(buf, buf_size, "(frame:%d size:%d ", frame, size);
   } else {
      pos = snprintf(buf, buf_size, "(size:%d ", size);
   }
   if (pos < 0 || (size_t)pos >= buf_size) {
      buf[buf_size - 1] = '\0';
      return;
   }

   for (int i = 0; i < size * 8; i++) {
      if (value(i)) {
         int n = snprintf(buf + pos, buf_size - (size_t)pos, "%2d ", i);
         if (n < 0 || (size_t)(pos + n) >= buf_size) {
            break;
         }
         pos += n;
      }
   }
   if ((size_t)pos + 1 < buf_size) {
      buf[pos++] = ')';
   }
   buf[(size_t)pos < buf_size ? (size_t)pos : (buf_size - 1)] = '\0';
}

void
GameInput::log(char *prefix, bool show_frame) const
{
   char buf[1024];
   int c = snprintf(buf, sizeof(buf), "%s", prefix);
   if (c >= 0 && (size_t)c < sizeof(buf)) {
      desc(buf + c, sizeof(buf) - (size_t)c, show_frame);
      size_t len = strlen(buf);
      if (len + 1 < sizeof(buf)) {
         buf[len] = '\n';
         buf[len + 1] = '\0';
      }
   }
   Log("%s", buf);
}

bool
GameInput::equal(GameInput &other, bool bitsonly)
{
   if (!bitsonly && frame != other.frame) {
      Log("frames don't match: %d, %d\n", frame, other.frame);
   }
   if (size != other.size) {
      Log("sizes don't match: %d, %d\n", size, other.size);
   }
   if (memcmp(bits, other.bits, size)) {
      Log("bits don't match\n");
   }
   ASSERT(size && other.size);
   return (bitsonly || frame == other.frame) &&
          size == other.size &&
          memcmp(bits, other.bits, size) == 0;
}

