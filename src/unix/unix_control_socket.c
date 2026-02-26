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
 *          Protocol (line-based text, newline-terminated):
 *
 *          Commands (client -> server):
 *            cdload <id> <path>         - mount CD-ROM image
 *            fddload <id> <path> <wp>   - mount floppy image (wp=0/1)
 *            moload <id> <path> <wp>    - mount MO image
 *            rdiskload <id> <path> <wp> - mount removable disk image
 *            cartload <id> <path> <wp>  - mount cartridge
 *            cdeject <id>               - eject CD-ROM
 *            fddeject <id>              - eject floppy
 *            moeject <id>               - eject MO
 *            rdiskeject <id>            - eject removable disk
 *            carteject <id>             - eject cartridge
 *            pause                      - toggle pause
 *            hardreset                  - hard reset
 *            status                     - query current LED/media state
 *            screenshot [monitor]       - raw BGRA visible area dump
 *            screencrc [mon [x y w h]]  - CRC-32 of visible screen region
 *            mousecapture               - capture mouse
 *            mouserelease               - release mouse
 *            exit                       - exit emulator
 *
 *          Responses (server -> client):
 *            OK [message]               - command succeeded
 *            ERR [message]              - command failed
 *
 *          Screenshot response (binary):
 *            OK <width> <height> <data_bytes>\n
 *            <raw BGRA pixel data>
 *
 *          Screencrc response:
 *            OK <crc32_hex> <width> <height>\n
 *
 *          Push events (server -> client, prefix '!'):
 *            !led <device> <id> <read|write|idle>
 *            !media <device> <id> <inserted|ejected>
 *
 * Authors: 86Box contributors.
 *
 *          Copyright 2026 86Box contributors.
 */
#ifdef __linux__
#    define _FILE_OFFSET_BITS   64
#    define _LARGEFILE64_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/config.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/hdd.h>
#include <86box/scsi_device.h>
#include <86box/cdrom.h>
#include <86box/mo.h>
#include <86box/rdisk.h>
#include <86box/cartridge.h>
#include <86box/cassette.h>
#include <86box/network.h>
#include <86box/machine_status.h>
#include <86box/video.h>
#include <86box/ui.h>
#include <86box/unix_control_socket.h>
#include <86box/version.h>

#define CTRL_MAX_CLIENTS 8
#define CTRL_BUF_SIZE    4096
#define CTRL_LED_POLL_MS 50

typedef struct ctrl_client_t {
    int  fd;
    char buf[CTRL_BUF_SIZE];
    int  buf_len;
} ctrl_client_t;

static int           ctrl_server_fd = -1;
static char          ctrl_socket_path[PATH_MAX];
static ctrl_client_t ctrl_clients[CTRL_MAX_CLIENTS];
static int           ctrl_num_clients       = 0;
static atomic_bool   ctrl_running           = false;
static thread_t     *ctrl_thread_handle     = NULL;
static thread_t     *ctrl_led_thread_handle = NULL;

/* Previous LED state for change detection. */
typedef struct led_state_t {
    bool active;
    bool write_active;
} led_state_t;

static led_state_t prev_fdd[FDD_NUM];
static led_state_t prev_cdrom[CDROM_NUM];
static led_state_t prev_hdd[HDD_BUS_USB];
static led_state_t prev_rdisk[RDISK_NUM];
static led_state_t prev_mo[MO_NUM];
static led_state_t prev_net[NET_CARD_MAX];

/* Previous media state for media change notifications. */
static bool prev_fdd_empty[FDD_NUM];
static bool prev_cdrom_empty[CDROM_NUM];
static bool prev_rdisk_empty[RDISK_NUM];
static bool prev_mo_empty[MO_NUM];

/* Forward declarations. */
static void ctrl_broadcast(const char *msg);
static void ctrl_handle_command(ctrl_client_t *client, char *line);
static void ctrl_remove_client(int idx);
static void ctrl_send_binary(ctrl_client_t *client, const void *data, size_t len);

/* Reuse the existing unix monitor command parser for
   3-argument media commands (id, filename, wp). */
static bool
ctrl_process_media_commands_3(uint8_t *id, char *fn, uint8_t *wp, char **xargv, int cmdargc)
{
    bool err = false;

    *id = atoi(xargv[1]);

    if (xargv[2][0] == '\'' || xargv[2][0] == '"') {
        for (int curarg = 2; curarg < cmdargc; curarg++) {
            if (strlen(fn) + strlen(xargv[curarg]) >= PATH_MAX) {
                err = true;
                fprintf(stderr, "Path name too long.\n");
            }
            strcat(fn, xargv[curarg] + (xargv[curarg][0] == '\'' || xargv[curarg][0] == '"'));
            if (fn[strlen(fn) - 1] == '\''
                || fn[strlen(fn) - 1] == '"') {
                if (curarg + 1 < cmdargc) {
                    *wp = atoi(xargv[curarg + 1]);
                }
                break;
            }
            strcat(fn, " ");
        }
    } else {
        if (strlen(xargv[2]) < PATH_MAX) {
            strcpy(fn, xargv[2]);
            *wp = atoi(xargv[3]);
        } else {
            fprintf(stderr, "Path name too long.\n");
            err = true;
        }
    }
    if (fn[strlen(fn) - 1] == '\''
        || fn[strlen(fn) - 1] == '"')
        fn[strlen(fn) - 1] = '\0';
    return err;
}

/* From musl. */
static char *
ctrl_strsep(char **str, const char *sep)
{
    char *s = *str;
    char *end;

    if (!s)
        return NULL;
    end = s + strcspn(s, sep);
    if (*end)
        *end++ = 0;
    else
        end = 0;
    *str = end;

    return s;
}

/* ------------------------------------------------------------------ */
/* Send a response/event to a single client.                          */
/* ------------------------------------------------------------------ */
static void
ctrl_send(ctrl_client_t *client, const char *msg)
{
    if (client->fd < 0)
        return;

    size_t  len     = strlen(msg);
    ssize_t written = 0;

    while ((size_t) written < len) {
        ssize_t n = write(client->fd, msg + written, len - written);
        if (n <= 0)
            break;
        written += n;
    }
}

/* ------------------------------------------------------------------ */
/* Send raw binary data to a single client.                           */
/* ------------------------------------------------------------------ */
static void
ctrl_send_binary(ctrl_client_t *client, const void *data, size_t len)
{
    if (client->fd < 0)
        return;

    const uint8_t *p         = (const uint8_t *) data;
    size_t         remaining = len;

    while (remaining > 0) {
        ssize_t n = write(client->fd, p, remaining);
        if (n <= 0)
            break;
        p += n;
        remaining -= n;
    }
}

/* ------------------------------------------------------------------ */
/* Broadcast a message to all connected clients.                      */
/* ------------------------------------------------------------------ */
static void
ctrl_broadcast(const char *msg)
{
    for (int i = 0; i < ctrl_num_clients; i++) {
        if (ctrl_clients[i].fd >= 0)
            ctrl_send(&ctrl_clients[i], msg);
    }
}

/* ------------------------------------------------------------------ */
/* Build a full status snapshot and send it to a client.               */
/* ------------------------------------------------------------------ */
static void
ctrl_send_status(ctrl_client_t *client)
{
    char line[256];

    for (int i = 0; i < FDD_NUM; i++) {
        const char *state = machine_status.fdd[i].write_active ? "write"
            : machine_status.fdd[i].active                     ? "read"
                                                               : "idle";
        snprintf(line, sizeof(line), "!led fdd %d %s\n", i, state);
        ctrl_send(client, line);

        snprintf(line, sizeof(line), "!media fdd %d %s\n", i,
                 machine_status.fdd[i].empty ? "ejected" : "inserted");
        ctrl_send(client, line);
    }

    for (int i = 0; i < CDROM_NUM; i++) {
        const char *state = machine_status.cdrom[i].write_active ? "write"
            : machine_status.cdrom[i].active                     ? "read"
                                                                 : "idle";
        snprintf(line, sizeof(line), "!led cdrom %d %s\n", i, state);
        ctrl_send(client, line);

        snprintf(line, sizeof(line), "!media cdrom %d %s\n", i,
                 machine_status.cdrom[i].empty ? "ejected" : "inserted");
        ctrl_send(client, line);
    }

    for (int i = 0; i < HDD_BUS_USB; i++) {
        const char *state = machine_status.hdd[i].write_active ? "write"
            : machine_status.hdd[i].active                     ? "read"
                                                               : "idle";
        snprintf(line, sizeof(line), "!led hdd %d %s\n", i, state);
        ctrl_send(client, line);
    }

    for (int i = 0; i < RDISK_NUM; i++) {
        const char *state = machine_status.rdisk[i].write_active ? "write"
            : machine_status.rdisk[i].active                     ? "read"
                                                                 : "idle";
        snprintf(line, sizeof(line), "!led rdisk %d %s\n", i, state);
        ctrl_send(client, line);

        snprintf(line, sizeof(line), "!media rdisk %d %s\n", i,
                 machine_status.rdisk[i].empty ? "ejected" : "inserted");
        ctrl_send(client, line);
    }

    for (int i = 0; i < MO_NUM; i++) {
        const char *state = machine_status.mo[i].write_active ? "write"
            : machine_status.mo[i].active                     ? "read"
                                                              : "idle";
        snprintf(line, sizeof(line), "!led mo %d %s\n", i, state);
        ctrl_send(client, line);

        snprintf(line, sizeof(line), "!media mo %d %s\n", i,
                 machine_status.mo[i].empty ? "ejected" : "inserted");
        ctrl_send(client, line);
    }

    for (int i = 0; i < NET_CARD_MAX; i++) {
        const char *state = machine_status.net[i].write_active ? "write"
            : machine_status.net[i].active                     ? "read"
                                                               : "idle";
        snprintf(line, sizeof(line), "!led net %d %s\n", i, state);
        ctrl_send(client, line);
    }

    snprintf(line, sizeof(line), "!paused %d\n", dopause ? 1 : 0);
    ctrl_send(client, line);
}

/* ------------------------------------------------------------------ */
/* Handle a single command line from a client.                         */
/* ------------------------------------------------------------------ */
static void
ctrl_handle_command(ctrl_client_t *client, char *line)
{
    char *xargv[512];
    int   cmdargc = 0;
    char *linecpy, *linespn;

    linecpy = linespn = strdup(line);
    if (!linecpy) {
        ctrl_send(client, "ERR out of memory\n");
        return;
    }

    linecpy[strcspn(linecpy, "\r\n")] = 0;

    /* Skip empty lines. */
    if (linecpy[0] == '\0') {
        free(linecpy);
        return;
    }

    memset(xargv, 0, sizeof(xargv));
    while (1) {
        xargv[cmdargc++] = ctrl_strsep(&linespn, " ");
        if (xargv[cmdargc - 1] == NULL || cmdargc >= 512)
            break;
    }
    cmdargc--;

    if (strcasecmp(xargv[0], "status") == 0) {
        ctrl_send_status(client);
        ctrl_send(client, "OK\n");
    } else if (strcasecmp(xargv[0], "pause") == 0) {
        plat_pause(dopause ^ 1);
        char msg[64];
        snprintf(msg, sizeof(msg), "OK %s\n", dopause ? "paused" : "unpaused");
        ctrl_send(client, msg);
        /* Broadcast pause state change. */
        snprintf(msg, sizeof(msg), "!paused %d\n", dopause ? 1 : 0);
        ctrl_broadcast(msg);
    } else if (strcasecmp(xargv[0], "hardreset") == 0) {
        pc_reset_hard();
        ctrl_send(client, "OK hard reset\n");
    } else if (strcasecmp(xargv[0], "exit") == 0) {
        ctrl_send(client, "OK exiting\n");
        plat_power_off();
    } else if (strcasecmp(xargv[0], "version") == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "OK %s %s\n", EMU_NAME, EMU_VERSION_FULL);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "cdload") == 0 && cmdargc >= 3) {
        uint8_t id;
        char    fn[PATH_MAX];

        if (!xargv[1] || !xargv[2]) {
            ctrl_send(client, "ERR missing arguments\n");
            free(linecpy);
            return;
        }
        id = atoi(xargv[1]);
        if (id >= CDROM_NUM) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        memset(fn, 0, sizeof(fn));

        /* Reassemble the path from remaining arguments (handles spaces). */
        for (int i = 2; i < cmdargc; i++) {
            if (strlen(fn) + strlen(xargv[i]) + 1 >= PATH_MAX) {
                ctrl_send(client, "ERR path too long\n");
                free(linecpy);
                return;
            }
            if (i > 2)
                strcat(fn, " ");
            strcat(fn, xargv[i]);
        }

        cdrom_mount(id, fn);
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "OK cdrom %d loaded\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "cdeject") == 0 && cmdargc >= 2) {
        uint8_t id = atoi(xargv[1]);
        if (id >= CDROM_NUM) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        cdrom_mount(id, "");
        char msg[64];
        snprintf(msg, sizeof(msg), "OK cdrom %d ejected\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "fddload") == 0 && cmdargc >= 4) {
        uint8_t id;
        uint8_t wp;
        char    fn[PATH_MAX];

        if (!xargv[1] || !xargv[2]) {
            ctrl_send(client, "ERR missing arguments\n");
            free(linecpy);
            return;
        }
        memset(fn, 0, sizeof(fn));
        bool err = ctrl_process_media_commands_3(&id, fn, &wp, xargv, cmdargc);
        if (err || id >= FDD_NUM) {
            ctrl_send(client, "ERR invalid arguments\n");
            free(linecpy);
            return;
        }
        if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
            fn[strlen(fn) - 1] = '\0';

        floppy_mount(id, fn, wp);
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "OK fdd %d loaded\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "fddeject") == 0 && cmdargc >= 2) {
        uint8_t id = atoi(xargv[1]);
        if (id >= FDD_NUM) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        floppy_eject(id);
        char msg[64];
        snprintf(msg, sizeof(msg), "OK fdd %d ejected\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "moload") == 0 && cmdargc >= 4) {
        uint8_t id;
        uint8_t wp;
        char    fn[PATH_MAX];

        if (!xargv[1] || !xargv[2]) {
            ctrl_send(client, "ERR missing arguments\n");
            free(linecpy);
            return;
        }
        memset(fn, 0, sizeof(fn));
        bool err = ctrl_process_media_commands_3(&id, fn, &wp, xargv, cmdargc);
        if (err || id >= MO_NUM) {
            ctrl_send(client, "ERR invalid arguments\n");
            free(linecpy);
            return;
        }
        if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
            fn[strlen(fn) - 1] = '\0';

        mo_mount(id, fn, wp);
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "OK mo %d loaded\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "moeject") == 0 && cmdargc >= 2) {
        uint8_t id = atoi(xargv[1]);
        if (id >= MO_NUM) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        mo_eject(id);
        char msg[64];
        snprintf(msg, sizeof(msg), "OK mo %d ejected\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "rdiskload") == 0 && cmdargc >= 4) {
        uint8_t id;
        uint8_t wp;
        char    fn[PATH_MAX];

        if (!xargv[1] || !xargv[2]) {
            ctrl_send(client, "ERR missing arguments\n");
            free(linecpy);
            return;
        }
        memset(fn, 0, sizeof(fn));
        bool err = ctrl_process_media_commands_3(&id, fn, &wp, xargv, cmdargc);
        if (err || id >= RDISK_NUM) {
            ctrl_send(client, "ERR invalid arguments\n");
            free(linecpy);
            return;
        }
        if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
            fn[strlen(fn) - 1] = '\0';

        rdisk_mount(id, fn, wp);
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "OK rdisk %d loaded\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "rdiskeject") == 0 && cmdargc >= 2) {
        uint8_t id = atoi(xargv[1]);
        if (id >= RDISK_NUM) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        rdisk_eject(id);
        char msg[64];
        snprintf(msg, sizeof(msg), "OK rdisk %d ejected\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "cartload") == 0 && cmdargc >= 4) {
        uint8_t id;
        uint8_t wp;
        char    fn[PATH_MAX];

        if (!xargv[1] || !xargv[2]) {
            ctrl_send(client, "ERR missing arguments\n");
            free(linecpy);
            return;
        }
        memset(fn, 0, sizeof(fn));
        bool err = ctrl_process_media_commands_3(&id, fn, &wp, xargv, cmdargc);
        if (err || id >= 2) {
            ctrl_send(client, "ERR invalid arguments\n");
            free(linecpy);
            return;
        }
        if (fn[strlen(fn) - 1] == '\'' || fn[strlen(fn) - 1] == '"')
            fn[strlen(fn) - 1] = '\0';

        cartridge_mount(id, fn, wp);
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "OK cartridge %d loaded\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "carteject") == 0 && cmdargc >= 2) {
        uint8_t id = atoi(xargv[1]);
        if (id >= 2) {
            ctrl_send(client, "ERR invalid drive id\n");
            free(linecpy);
            return;
        }
        cartridge_eject(id);
        char msg[64];
        snprintf(msg, sizeof(msg), "OK cartridge %d ejected\n", id);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "screenshot") == 0) {
        /* screenshot [monitor]  - return raw BGRA framebuffer.
           Response: OK <width> <height> <data_bytes>\n<raw BGRA data> */
        int mon_idx = 0;
        if (cmdargc >= 2)
            mon_idx = atoi(xargv[1]);
        if (mon_idx < 0 || mon_idx >= MONITORS_NUM) {
            ctrl_send(client, "ERR invalid monitor index\n");
            free(linecpy);
            return;
        }
        if (video_get_type_monitor(mon_idx) == VIDEO_FLAG_TYPE_NONE) {
            ctrl_send(client, "ERR monitor not active\n");
            free(linecpy);
            return;
        }

        const monitor_t *m = &monitors[mon_idx];
        int              bx, by, bw, bh;
        video_get_blit_rect(mon_idx, &bx, &by, &bw, &bh);

        if (bw <= 0 || bh <= 0 || !m->target_buffer) {
            ctrl_send(client, "ERR no framebuffer available\n");
            free(linecpy);
            return;
        }

        size_t data_len = (size_t) bw * (size_t) bh * 4;
        char   header[128];
        snprintf(header, sizeof(header), "OK %d %d %zu\n", bw, bh, data_len);
        ctrl_send(client, header);

        /* Send visible rows from the target_buffer line pointers. */
        for (int y = by; y < by + bh; y++) {
            ctrl_send_binary(client, &m->target_buffer->line[y][bx], (size_t) bw * 4);
        }
    } else if (strcasecmp(xargv[0], "screencrc") == 0) {
        /* screencrc [monitor [x y w h]]  - CRC-32 of framebuffer region.
           With no region args, CRCs the entire screen. */
        int mon_idx = 0;
        if (cmdargc >= 2)
            mon_idx = atoi(xargv[1]);
        if (mon_idx < 0 || mon_idx >= MONITORS_NUM) {
            ctrl_send(client, "ERR invalid monitor index\n");
            free(linecpy);
            return;
        }
        if (video_get_type_monitor(mon_idx) == VIDEO_FLAG_TYPE_NONE) {
            ctrl_send(client, "ERR monitor not active\n");
            free(linecpy);
            return;
        }

        const monitor_t *m = &monitors[mon_idx];
        int              bx, by, bw, bh;
        video_get_blit_rect(mon_idx, &bx, &by, &bw, &bh);

        if (bw <= 0 || bh <= 0 || !m->target_buffer) {
            ctrl_send(client, "ERR no framebuffer available\n");
            free(linecpy);
            return;
        }

        /* Optional region (relative to visible area): screencrc <mon> <x> <y> <w> <h> */
        int rx = 0, ry = 0, rw = bw, rh = bh;
        if (cmdargc >= 6) {
            rx = atoi(xargv[2]);
            ry = atoi(xargv[3]);
            rw = atoi(xargv[4]);
            rh = atoi(xargv[5]);
        }

        /* Clamp region to visible area bounds. */
        if (rx < 0)
            rx = 0;
        if (ry < 0)
            ry = 0;
        if (rx + rw > bw)
            rw = bw - rx;
        if (ry + rh > bh)
            rh = bh - ry;
        if (rw <= 0 || rh <= 0) {
            ctrl_send(client, "ERR region out of bounds\n");
            free(linecpy);
            return;
        }

        /* Compute CRC-32 over BGR channels only (skip alpha byte). */
        uint32_t crc = 0xFFFFFFFF;
        for (int y = by + ry; y < by + ry + rh; y++) {
            const uint8_t *row = (const uint8_t *) &m->target_buffer->line[y][bx + rx];
            for (int x = 0; x < rw; x++) {
                for (int c = 0; c < 3; c++) {
                    crc ^= row[x * 4 + c];
                    for (int j = 0; j < 8; j++)
                        crc = (crc >> 1) ^ ((-(crc & 1)) & 0xEDB88320);
                }
            }
        }
        crc ^= 0xFFFFFFFF;

        char msg[128];
        snprintf(msg, sizeof(msg), "OK %08X %d %d\n", crc, bw, bh);
        ctrl_send(client, msg);
    } else if (strcasecmp(xargv[0], "mousecapture") == 0) {
        plat_mouse_capture(1);
        ctrl_send(client, "OK mouse captured\n");
    } else if (strcasecmp(xargv[0], "mouserelease") == 0) {
        plat_mouse_capture(0);
        ctrl_send(client, "OK mouse released\n");
    } else if (strcasecmp(xargv[0], "help") == 0) {
        ctrl_send(client,
                  "Commands:\n"
                  "  cdload <id> <path>         - mount CD-ROM image\n"
                  "  fddload <id> <path> <wp>   - mount floppy (wp=0|1)\n"
                  "  moload <id> <path> <wp>    - mount MO image\n"
                  "  rdiskload <id> <path> <wp> - mount removable disk\n"
                  "  cartload <id> <path> <wp>  - mount cartridge\n"
                  "  cdeject <id>               - eject CD-ROM\n"
                  "  fddeject <id>              - eject floppy\n"
                  "  moeject <id>               - eject MO\n"
                  "  rdiskeject <id>            - eject removable disk\n"
                  "  carteject <id>             - eject cartridge\n"
                  "  pause                      - toggle pause\n"
                  "  hardreset                  - hard reset\n"
                  "  status                     - query all LED/media state\n"
                  "  screenshot [monitor]       - raw BGRA framebuffer dump\n"
                  "  screencrc [mon [x y w h]]  - CRC-32 of screen region\n"
                  "  mousecapture               - capture mouse\n"
                  "  mouserelease               - release mouse\n"
                  "  version                    - print version\n"
                  "  exit                       - exit emulator\n"
                  "OK\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERR unknown command: %s\n", xargv[0]);
        ctrl_send(client, msg);
    }

    free(linecpy);
}

/* ------------------------------------------------------------------ */
/* Remove a client from the active list.                               */
/* ------------------------------------------------------------------ */
static void
ctrl_remove_client(int idx)
{
    if (idx < 0 || idx >= ctrl_num_clients)
        return;

    if (ctrl_clients[idx].fd >= 0) {
        close(ctrl_clients[idx].fd);
        ctrl_clients[idx].fd = -1;
    }

    /* Shift remaining clients down. */
    for (int i = idx; i < ctrl_num_clients - 1; i++)
        ctrl_clients[i] = ctrl_clients[i + 1];

    ctrl_num_clients--;
}

/* ------------------------------------------------------------------ */
/* LED polling thread - detects changes and broadcasts events.         */
/* ------------------------------------------------------------------ */
static void
ctrl_led_poll_thread(UNUSED(void *param))
{
    char line[128];

    plat_set_thread_name(NULL, "ctrl_led_poll");

    /* Initialize previous state from current machine_status. */
    for (int i = 0; i < FDD_NUM; i++) {
        prev_fdd[i].active       = machine_status.fdd[i].active;
        prev_fdd[i].write_active = machine_status.fdd[i].write_active;
        prev_fdd_empty[i]        = machine_status.fdd[i].empty;
    }
    for (int i = 0; i < CDROM_NUM; i++) {
        prev_cdrom[i].active       = machine_status.cdrom[i].active;
        prev_cdrom[i].write_active = machine_status.cdrom[i].write_active;
        prev_cdrom_empty[i]        = machine_status.cdrom[i].empty;
    }
    for (int i = 0; i < HDD_BUS_USB; i++) {
        prev_hdd[i].active       = machine_status.hdd[i].active;
        prev_hdd[i].write_active = machine_status.hdd[i].write_active;
    }
    for (int i = 0; i < RDISK_NUM; i++) {
        prev_rdisk[i].active       = machine_status.rdisk[i].active;
        prev_rdisk[i].write_active = machine_status.rdisk[i].write_active;
        prev_rdisk_empty[i]        = machine_status.rdisk[i].empty;
    }
    for (int i = 0; i < MO_NUM; i++) {
        prev_mo[i].active       = machine_status.mo[i].active;
        prev_mo[i].write_active = machine_status.mo[i].write_active;
        prev_mo_empty[i]        = machine_status.mo[i].empty;
    }
    for (int i = 0; i < NET_CARD_MAX; i++) {
        prev_net[i].active       = machine_status.net[i].active;
        prev_net[i].write_active = machine_status.net[i].write_active;
    }

    while (ctrl_running) {
        plat_delay_ms(CTRL_LED_POLL_MS);

        if (ctrl_num_clients == 0)
            continue;

        /* Check floppy drives. */
        for (int i = 0; i < FDD_NUM; i++) {
            bool a = machine_status.fdd[i].active;
            bool w = machine_status.fdd[i].write_active;
            bool e = machine_status.fdd[i].empty;

            if (a != prev_fdd[i].active || w != prev_fdd[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led fdd %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_fdd[i].active       = a;
                prev_fdd[i].write_active = w;
            }
            if (e != prev_fdd_empty[i]) {
                snprintf(line, sizeof(line), "!media fdd %d %s\n", i,
                         e ? "ejected" : "inserted");
                ctrl_broadcast(line);
                prev_fdd_empty[i] = e;
            }
        }

        /* Check CD-ROM drives. */
        for (int i = 0; i < CDROM_NUM; i++) {
            bool a = machine_status.cdrom[i].active;
            bool w = machine_status.cdrom[i].write_active;
            bool e = machine_status.cdrom[i].empty;

            if (a != prev_cdrom[i].active || w != prev_cdrom[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led cdrom %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_cdrom[i].active       = a;
                prev_cdrom[i].write_active = w;
            }
            if (e != prev_cdrom_empty[i]) {
                snprintf(line, sizeof(line), "!media cdrom %d %s\n", i,
                         e ? "ejected" : "inserted");
                ctrl_broadcast(line);
                prev_cdrom_empty[i] = e;
            }
        }

        /* Check HDDs. */
        for (int i = 0; i < HDD_BUS_USB; i++) {
            bool a = machine_status.hdd[i].active;
            bool w = machine_status.hdd[i].write_active;

            if (a != prev_hdd[i].active || w != prev_hdd[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led hdd %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_hdd[i].active       = a;
                prev_hdd[i].write_active = w;
            }
        }

        /* Check removable disks. */
        for (int i = 0; i < RDISK_NUM; i++) {
            bool a = machine_status.rdisk[i].active;
            bool w = machine_status.rdisk[i].write_active;
            bool e = machine_status.rdisk[i].empty;

            if (a != prev_rdisk[i].active || w != prev_rdisk[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led rdisk %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_rdisk[i].active       = a;
                prev_rdisk[i].write_active = w;
            }
            if (e != prev_rdisk_empty[i]) {
                snprintf(line, sizeof(line), "!media rdisk %d %s\n", i,
                         e ? "ejected" : "inserted");
                ctrl_broadcast(line);
                prev_rdisk_empty[i] = e;
            }
        }

        /* Check MO drives. */
        for (int i = 0; i < MO_NUM; i++) {
            bool a = machine_status.mo[i].active;
            bool w = machine_status.mo[i].write_active;
            bool e = machine_status.mo[i].empty;

            if (a != prev_mo[i].active || w != prev_mo[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led mo %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_mo[i].active       = a;
                prev_mo[i].write_active = w;
            }
            if (e != prev_mo_empty[i]) {
                snprintf(line, sizeof(line), "!media mo %d %s\n", i,
                         e ? "ejected" : "inserted");
                ctrl_broadcast(line);
                prev_mo_empty[i] = e;
            }
        }

        /* Check network. */
        for (int i = 0; i < NET_CARD_MAX; i++) {
            bool a = machine_status.net[i].active;
            bool w = machine_status.net[i].write_active;

            if (a != prev_net[i].active || w != prev_net[i].write_active) {
                const char *state = w ? "write" : a ? "read"
                                                    : "idle";
                snprintf(line, sizeof(line), "!led net %d %s\n", i, state);
                ctrl_broadcast(line);
                prev_net[i].active       = a;
                prev_net[i].write_active = w;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main server thread - accepts connections and dispatches commands.    */
/* ------------------------------------------------------------------ */
static void
ctrl_server_thread(UNUSED(void *param))
{
    plat_set_thread_name(NULL, "ctrl_socket");

    while (ctrl_running) {
        fd_set         readfds;
        int            maxfd = ctrl_server_fd;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(ctrl_server_fd, &readfds);

        for (int i = 0; i < ctrl_num_clients; i++) {
            if (ctrl_clients[i].fd >= 0) {
                FD_SET(ctrl_clients[i].fd, &readfds);
                if (ctrl_clients[i].fd > maxfd)
                    maxfd = ctrl_clients[i].fd;
            }
        }

        tv.tv_sec  = 0;
        tv.tv_usec = 200000; /* 200ms timeout for shutdown checks */

        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        /* Check for new connections. */
        if (FD_ISSET(ctrl_server_fd, &readfds)) {
            int client_fd = accept(ctrl_server_fd, NULL, NULL);
            if (client_fd >= 0) {
                if (ctrl_num_clients >= CTRL_MAX_CLIENTS) {
                    const char *msg = "ERR too many clients\n";
                    ssize_t     ret = write(client_fd, msg, strlen(msg));
                    (void) ret;
                    close(client_fd);
                } else {
                    /* Set non-blocking. */
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    if (flags >= 0)
                        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    ctrl_clients[ctrl_num_clients].fd      = client_fd;
                    ctrl_clients[ctrl_num_clients].buf_len = 0;
                    ctrl_clients[ctrl_num_clients].buf[0]  = '\0';
                    ctrl_num_clients++;
                }
            }
        }

        /* Check existing clients for data. */
        for (int i = 0; i < ctrl_num_clients; i++) {
            if (ctrl_clients[i].fd < 0 || !FD_ISSET(ctrl_clients[i].fd, &readfds))
                continue;

            ctrl_client_t *c     = &ctrl_clients[i];
            int            space = CTRL_BUF_SIZE - c->buf_len - 1;
            if (space <= 0) {
                /* Buffer overflow - discard and disconnect. */
                ctrl_remove_client(i);
                i--;
                continue;
            }

            ssize_t n = read(c->fd, c->buf + c->buf_len, space);
            if (n <= 0) {
                /* Client disconnected or error. */
                ctrl_remove_client(i);
                i--;
                continue;
            }

            c->buf_len += n;
            c->buf[c->buf_len] = '\0';

            /* Process complete lines. */
            char *start = c->buf;
            char *newline;
            while ((newline = strchr(start, '\n')) != NULL) {
                *newline = '\0';
                ctrl_handle_command(c, start);
                start = newline + 1;

                /* Client may have been removed by exit command. */
                if (c->fd < 0)
                    break;
            }

            if (c->fd >= 0) {
                /* Move remaining partial data to beginning of buffer. */
                int remaining = c->buf_len - (int) (start - c->buf);
                if (remaining > 0)
                    memmove(c->buf, start, remaining);
                c->buf_len = remaining;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */
int
control_socket_init(const char *path)
{
    struct sockaddr_un addr;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Control socket: path too long: %s\n", path);
        return -1;
    }

    /* Remove stale socket file if it exists. */
    unlink(path);

    ctrl_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_server_fd < 0) {
        fprintf(stderr, "Control socket: failed to create socket: %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(ctrl_server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Control socket: failed to bind %s: %s\n",
                path, strerror(errno));
        close(ctrl_server_fd);
        ctrl_server_fd = -1;
        return -1;
    }

    if (listen(ctrl_server_fd, 4) < 0) {
        fprintf(stderr, "Control socket: failed to listen: %s\n",
                strerror(errno));
        close(ctrl_server_fd);
        ctrl_server_fd = -1;
        unlink(path);
        return -1;
    }

    strncpy(ctrl_socket_path, path, sizeof(ctrl_socket_path) - 1);
    ctrl_socket_path[sizeof(ctrl_socket_path) - 1] = '\0';

    ctrl_running     = true;
    ctrl_num_clients = 0;
    for (int i = 0; i < CTRL_MAX_CLIENTS; i++)
        ctrl_clients[i].fd = -1;

    /* Ignore SIGPIPE so writes to disconnected clients don't crash us. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "Control socket: listening on %s\n", path);

    ctrl_thread_handle     = thread_create(ctrl_server_thread, NULL);
    ctrl_led_thread_handle = thread_create(ctrl_led_poll_thread, NULL);

    return 0;
}

void
control_socket_close(void)
{
    if (!ctrl_running)
        return;

    ctrl_running = false;

    /* Close server socket to unblock select(). */
    if (ctrl_server_fd >= 0) {
        close(ctrl_server_fd);
        ctrl_server_fd = -1;
    }

    /* Wait for threads. */
    if (ctrl_thread_handle) {
        thread_wait(ctrl_thread_handle);
        ctrl_thread_handle = NULL;
    }
    if (ctrl_led_thread_handle) {
        thread_wait(ctrl_led_thread_handle);
        ctrl_led_thread_handle = NULL;
    }

    /* Close all client connections. */
    for (int i = 0; i < ctrl_num_clients; i++) {
        if (ctrl_clients[i].fd >= 0)
            close(ctrl_clients[i].fd);
    }
    ctrl_num_clients = 0;

    /* Remove socket file. */
    if (ctrl_socket_path[0] != '\0') {
        unlink(ctrl_socket_path);
        ctrl_socket_path[0] = '\0';
    }
}
