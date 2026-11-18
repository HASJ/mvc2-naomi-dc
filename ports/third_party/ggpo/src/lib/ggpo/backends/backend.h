/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _BACKEND_H
#define _BACKEND_H

#include "ggponet.h"
#include "types.h"

struct GGPOSession {
   GGPOConfirmedFrameCallback _confirmed_cb;
   void *_confirmed_user_data;

   GGPOSession() : _confirmed_cb(NULL), _confirmed_user_data(NULL) { }
   virtual ~GGPOSession() { }
   void SetConfirmedCallback(GGPOConfirmedFrameCallback cb, void *user_data) {
      _confirmed_cb = cb;
      _confirmed_user_data = user_data;
   }
   virtual GGPOErrorCode DoPoll(int timeout) { return GGPO_OK; }
   virtual GGPOErrorCode AddPlayer(GGPOPlayer *player, GGPOPlayerHandle *handle) = 0;
   virtual GGPOErrorCode AddLocalInput(GGPOPlayerHandle player, void *values, int size) = 0;
   virtual GGPOErrorCode SyncInput(void *values, int size, int *disconnect_flags) = 0;
   virtual GGPOErrorCode IncrementFrame(void) { return GGPO_OK; }
   virtual GGPOErrorCode Chat(char *text) { return GGPO_OK; }
   virtual GGPOErrorCode DisconnectPlayer(GGPOPlayerHandle handle) { return GGPO_OK; }
   virtual GGPOErrorCode GetNetworkStats(GGPONetworkStats *stats, GGPOPlayerHandle handle) { return GGPO_OK; }
   virtual GGPOErrorCode Logv(const char *fmt, va_list list) { ::Logv(fmt, list); return GGPO_OK; }

   virtual GGPOErrorCode SetFrameDelay(GGPOPlayerHandle player, int delay) { return GGPO_ERRORCODE_UNSUPPORTED; }
   virtual GGPOErrorCode SetDisconnectTimeout(int timeout) { return GGPO_ERRORCODE_UNSUPPORTED; }
   virtual GGPOErrorCode SetDisconnectNotifyStart(int timeout) { return GGPO_ERRORCODE_UNSUPPORTED; }
};

typedef struct GGPOSession Quark, IQuarkBackend; /* XXX: nuke this */

#endif

