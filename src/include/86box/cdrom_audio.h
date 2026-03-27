/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the CD-ROM drive audio emulation.
 *
 * Authors: Toni Riikonen, <riikonen.toni@gmail.com>
 *
 *          Copyright 2026 Toni Riikonen.
 */
#ifndef EMU_CDROM_AUDIO_H
#define EMU_CDROM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDROM_AUDIO_PROFILE_MAX 64

/* Spindle motor states */
typedef enum {
    CDROM_SPINDLE_STOPPED = 0,
    CDROM_SPINDLE_STARTING,
    CDROM_SPINDLE_RUNNING,
    CDROM_SPINDLE_STOPPING
} cdrom_spindle_state_t;

/* Seek sound playback phases (three-segment approach) */
typedef enum {
    CDROM_SEEK_PHASE_HEAD = 0, /* Attack / initial click */
    CDROM_SEEK_PHASE_LOOP,     /* Loopable travel noise */
    CDROM_SEEK_PHASE_TAIL      /* Settle / deceleration */
} cdrom_seek_phase_t;

/* Audio sample configuration structure */
typedef struct {
    char  filename[512];
    float volume;
} cdrom_audio_sample_config_t;

/* CD-ROM audio profile configuration */
typedef struct {
    int                         id;
    char                        name[128];
    char                        internal_name[64];
    cdrom_audio_sample_config_t spindlemotor_start;
    cdrom_audio_sample_config_t spindlemotor_loop;
    cdrom_audio_sample_config_t spindlemotor_stop;
    cdrom_audio_sample_config_t seek_track;
    cdrom_audio_sample_config_t tray_open;
    cdrom_audio_sample_config_t tray_close;
    int                         idle_timeout_ms;   /* Spindown after idle (ms), 0=never */
    int                         read_delay_ms;     /* Spinup time before data available (ms), 0=use WAV duration */
    int                         seek_loop_start;   /* Start of loopable middle section (sample index), 0=disabled */
    int                         seek_loop_end;     /* End of loopable middle section (sample index), 0=disabled */
} cdrom_audio_profile_config_t;

/* Functions for profile management */
extern void                                cdrom_audio_load_profiles(void);
extern int                                 cdrom_audio_get_profile_count(void);
extern const cdrom_audio_profile_config_t *cdrom_audio_get_profile(int id);
extern const char                         *cdrom_audio_get_profile_name(int id);
extern const char                         *cdrom_audio_get_profile_internal_name(int id);
extern int                                 cdrom_audio_get_profile_by_internal_name(const char *internal_name);

/* CD-ROM audio initialization and cleanup */
extern void cdrom_audio_init(void);
extern void cdrom_audio_reset(void);
extern void cdrom_audio_close(void);
extern void cdrom_activity_audio_callback(int16_t *buffer, int length);
extern void cdrom_audio_seek(uint8_t cdrom_id, uint32_t new_pos, double seek_time_us);

/* Per-drive spindle control */
extern void                 cdrom_audio_spinup_drive(uint8_t cdrom_id);
extern void                 cdrom_audio_spindown_drive(uint8_t cdrom_id);
extern cdrom_spindle_state_t cdrom_audio_get_drive_spindle_state(uint8_t cdrom_id);
extern double                cdrom_audio_get_spin_delay_us(uint8_t cdrom_id);

/* Tray open/close sounds */
extern void cdrom_audio_tray_open(uint8_t cdrom_id);
extern void cdrom_audio_tray_close(uint8_t cdrom_id);

#ifdef __cplusplus
}
#endif

#endif /* EMU_CDROM_AUDIO_H */
