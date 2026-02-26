/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Unix domain socket control interface for external IPC.
 *
 *          Allows external programs to mount/eject media, query and
 *          receive push notifications for disk activity LED state,
 *          and control the emulator (pause, reset, etc.) over a
 *          Unix domain socket specified via --control-socket.
 *
 * Authors: 86Box contributors.
 *
 *          Copyright 2026 86Box contributors.
 */
#ifndef EMU_UNIX_CONTROL_SOCKET_H
#define EMU_UNIX_CONTROL_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the control socket server on the given path.
   Returns 0 on success, -1 on failure. */
extern int  control_socket_init(const char *path);

/* Shut down the control socket server and clean up. */
extern void control_socket_close(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_UNIX_CONTROL_SOCKET_H */
