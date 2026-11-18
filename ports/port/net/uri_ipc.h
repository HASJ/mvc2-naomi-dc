/*
 * uri_ipc.h  umvsc2:// URL protocol registration (HKCU) & single-instance loopback URI forwarding
 */

#ifndef URI_IPC_H
#define URI_IPC_H

#include <stddef.h>

/* Early check before SDL/NAOMI init: if `--join <uri>` is passed and a live instance is running
   in the same config dir, forward the URI and return 1 so this helper process exits 0. */
int  uri_ipc_try_forward_cli(int argc, char** argv);

/* Register HKCU\Software\Classes\umvsc2 pointing to the running executable (no UAC required). */
void uri_ipc_register_protocol(void);

/* Bind loopback UDP listener for the running instance and record port in <config_dir>/mvc2_ipc.port. */
int  uri_ipc_listen_start(void);
void uri_ipc_listen_stop(void);

/* Queue initial URI (when launching fresh with `--join`) or poll incoming URIs from helper processes. */
void uri_ipc_push_pending(const char* uri);
int  uri_ipc_poll(char* out_uri, size_t max_len);

#endif
