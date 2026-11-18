/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "platform_windows.h"

uint32
Platform::GetCurrentTimeMS()
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER now;
   if (freq.QuadPart == 0) {
      QueryPerformanceFrequency(&freq);
   }
   QueryPerformanceCounter(&now);
   return (uint32)((now.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
}

int
Platform::GetConfigInt(const char* name)
{
   char buf[1024];
   if (GetEnvironmentVariable(name, buf, ARRAY_SIZE(buf)) == 0) {
      return 0;
   }
   return atoi(buf);
}

bool Platform::GetConfigBool(const char* name)
{
   char buf[1024];
   if (GetEnvironmentVariable(name, buf, ARRAY_SIZE(buf)) == 0) {
      return false;
   }
   return atoi(buf) != 0 || _stricmp(buf, "true") == 0;
}
