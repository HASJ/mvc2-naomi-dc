/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"

static FILE *logfile = NULL;

void LogFlush()
{
   if (logfile) {
      fflush(logfile);
   }
}

static char logbuf[4096];

void Log(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   Logv(fmt, args);
   va_end(args);
}

void Logv(const char *fmt, va_list args)
{
   if (!Platform::GetConfigBool("MVC2_GGPO_LOG") &&
       (!Platform::GetConfigBool("ggpo.log") || Platform::GetConfigBool("ggpo.log.ignore"))) {
      return;
   }
   if (!logfile) {
      snprintf(logbuf, sizeof(logbuf), "ggpo-log-%lu.log", (unsigned long)Platform::GetProcessID());
      logfile = fopen(logbuf, "w");
      if (!logfile) {
         return;
      }
   }
   Logv(logfile, fmt, args);
}

void Logv(FILE *fp, const char *fmt, va_list args)
{
   if (Platform::GetConfigBool("ggpo.log.timestamps")) {
      static int start = 0;
      int t = 0;
      if (!start) {
         start = Platform::GetCurrentTimeMS();
      } else {
         t = Platform::GetCurrentTimeMS() - start;
      }
      fprintf(fp, "%d.%03d : ", t / 1000, t % 1000);
   }

   vfprintf(fp, fmt, args);
   fflush(fp);

   vsnprintf(logbuf, sizeof(logbuf), fmt, args);
}

