/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM drive audio emulation.
 *
 * Authors: Toni Riikonen, <riikonen.toni@gmail.com>
 *
 *          Copyright 2026 Toni Riikonen.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/cdrom.h>
#include <86box/cdrom_audio.h>
#include <86box/sound.h>
#include <86box/sound_util.h>
#include <86box/thread.h>
#include <86box/plat.h>
#include <86box/path.h>
#include <86box/ini.h>
#include <86box/mem.h>
#include <86box/rom.h>

/* Seek sound state (single voice - a physical drive has one head) */
typedef struct {
    int                active;
    int                position;          /* Current position within the WAV segment */
    int                elapsed;           /* Total samples played so far */
    int                duration_samples;  /* Total duration to play (converted from seek time) */
    float              volume;
    int                profile_id;
    cdrom_seek_phase_t phase;             /* Current playback phase (HEAD/LOOP/TAIL) */
    int                segmented;         /* 1 if using three-segment playback */
} cdrom_seek_state_t;

/* Tray sound state */
typedef struct {
    int   active;
    int   position;
    float volume;
    int   is_open; /* 1 = playing open sound, 0 = playing close sound */
} cdrom_tray_state_t;

/* Pending actions queued after a transition finishes */
#define CDROM_PENDING_NONE       0
#define CDROM_PENDING_TRAY_OPEN  1  /* Play tray open after spindown */
#define CDROM_PENDING_TRAY_CLOSE 2  /* Play tray close after spindown/stop */
#define CDROM_PENDING_SPINUP     4  /* Spin up after tray close finishes */

/* Per-CD-ROM audio state */
typedef struct {
    int                   cdrom_index;
    int                   profile_id;
    cdrom_spindle_state_t spindle_state;
    int                   spindle_pos;
    int                   spindle_transition_pos;
    cdrom_seek_state_t    seek_state;
    cdrom_tray_state_t    tray_state;
    int                   pending_actions; /* Bitmask of CDROM_PENDING_* */
    int                   idle_samples;         /* Samples since last activity */
    int                   idle_timeout_samples; /* Threshold for spindown (0=disabled) */
    int                   read_delay_samples;   /* Spinup time before data available (0=use WAV duration) */
} cdrom_audio_drive_state_t;

/* Audio samples structure for a profile */
typedef struct {
    int16_t *spindle_start_buffer;
    int      spindle_start_samples;
    float    spindle_start_volume;
    int16_t *spindle_loop_buffer;
    int      spindle_loop_samples;
    float    spindle_loop_volume;
    int16_t *spindle_stop_buffer;
    int      spindle_stop_samples;
    float    spindle_stop_volume;
    int16_t *seek_buffer;
    int      seek_samples;
    float    seek_volume;
    int      seek_head_end;    /* End of head segment (sample index, exclusive) */
    int      seek_loop_start;  /* Start of loopable middle (sample index) */
    int      seek_loop_end;    /* End of loopable middle (sample index, exclusive) */
    int      seek_tail_start;  /* Start of tail segment (sample index) */
    int      seek_segmented;   /* 1 if segment markers are valid */
    int16_t *tray_open_buffer;
    int      tray_open_samples;
    float    tray_open_volume;
    int16_t *tray_close_buffer;
    int      tray_close_samples;
    float    tray_close_volume;
    int      loaded;
} cdrom_audio_samples_t;

/* Global audio profile configurations */
static cdrom_audio_profile_config_t audio_profiles[CDROM_AUDIO_PROFILE_MAX];
static int                          audio_profile_count = 0;

/* Per-profile loaded samples */
static cdrom_audio_samples_t profile_samples[CDROM_AUDIO_PROFILE_MAX];

/* Per-CD-ROM audio states */
static cdrom_audio_drive_state_t drive_states[CDROM_NUM];
static int                       active_drive_count = 0;

static mutex_t *cdrom_audio_mutex = NULL;

#ifdef ENABLE_CDROM_AUDIO_LOG
int cdrom_audio_do_log = ENABLE_CDROM_AUDIO_LOG;

static void
cdrom_audio_log(const char *fmt, ...)
{
    va_list ap;

    if (cdrom_audio_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define cdrom_audio_log(fmt, ...)
#endif

/* Load audio profiles from configuration file */
void
cdrom_audio_load_profiles(void)
{
    ini_t profiles_ini;
    char  cfg_fn[1024] = { 0 };

    int ret = asset_getfile("assets/sounds/cdrom/cdrom_audio_profiles.cfg", cfg_fn, 1024);
    if (!ret) {
        cdrom_audio_log("CDROM Audio: Could not find cdrom_audio_profiles.cfg\n");
        return;
    }

    /* Validate that the path does not contain path traversal sequences */
    if (strstr(cfg_fn, "..") != NULL) {
        cdrom_audio_log("CDROM Audio: Invalid path detected\n");
        return;
    }

    /* Validate the path ends with our expected filename */
    const char *expected_suffix = "cdrom_audio_profiles.cfg";
    size_t      cfg_len         = strlen(cfg_fn);
    size_t      suffix_len      = strlen(expected_suffix);
    if (cfg_len < suffix_len || strcmp(cfg_fn + cfg_len - suffix_len, expected_suffix) != 0) {
        pclog("CDROM Audio: Unexpected config path\n");
        return;
    }

    profiles_ini = ini_read_ex(cfg_fn, 1);  /* lgtm[cpp/path-injection] */
    if (profiles_ini == NULL) {
        cdrom_audio_log("CDROM Audio: Failed to load cdrom_audio_profiles.cfg\n");
        return;
    }

    audio_profile_count = 0;

    for (int i = 0; i < CDROM_AUDIO_PROFILE_MAX && audio_profile_count < CDROM_AUDIO_PROFILE_MAX; i++) {
        char section_name[64];
        sprintf(section_name, "Profile \"%d\"", i);

        ini_section_t cat = ini_find_section(profiles_ini, section_name);
        if (cat == NULL)
            continue;

        cdrom_audio_profile_config_t *config = &audio_profiles[audio_profile_count];
        memset(config, 0, sizeof(cdrom_audio_profile_config_t));

        config->id = ini_section_get_int(cat, "id", i);

        const char *name = ini_section_get_string(cat, "name", "Unknown");
        strncpy(config->name, name, sizeof(config->name) - 1);

        const char *internal_name = ini_section_get_string(cat, "internal_name", "unknown");
        strncpy(config->internal_name, internal_name, sizeof(config->internal_name) - 1);

        /* Load spindle motor sample files */
        const char *file = ini_section_get_string(cat, "spindlemotor_start_file", "");
        strncpy(config->spindlemotor_start.filename, file, sizeof(config->spindlemotor_start.filename) - 1);
        config->spindlemotor_start.volume = (float) ini_section_get_double(cat, "spindlemotor_start_volume", 1.0);

        file = ini_section_get_string(cat, "spindlemotor_loop_file", "");
        strncpy(config->spindlemotor_loop.filename, file, sizeof(config->spindlemotor_loop.filename) - 1);
        config->spindlemotor_loop.volume = (float) ini_section_get_double(cat, "spindlemotor_loop_volume", 1.0);

        file = ini_section_get_string(cat, "spindlemotor_stop_file", "");
        strncpy(config->spindlemotor_stop.filename, file, sizeof(config->spindlemotor_stop.filename) - 1);
        config->spindlemotor_stop.volume = (float) ini_section_get_double(cat, "spindlemotor_stop_volume", 1.0);

        /* Load seek sample file */
        file = ini_section_get_string(cat, "seek_track_file", "");
        strncpy(config->seek_track.filename, file, sizeof(config->seek_track.filename) - 1);
        config->seek_track.volume = (float) ini_section_get_double(cat, "seek_track_volume", 1.0);

        /* Load tray open/close sample files */
        file = ini_section_get_string(cat, "tray_open_file", "");
        strncpy(config->tray_open.filename, file, sizeof(config->tray_open.filename) - 1);
        config->tray_open.volume = (float) ini_section_get_double(cat, "tray_open_volume", 1.0);

        file = ini_section_get_string(cat, "tray_close_file", "");
        strncpy(config->tray_close.filename, file, sizeof(config->tray_close.filename) - 1);
        config->tray_close.volume = (float) ini_section_get_double(cat, "tray_close_volume", 1.0);

        /* Idle spindown timeout (milliseconds, 0 = never spin down) */
        config->idle_timeout_ms = ini_section_get_int(cat, "idle_timeout_ms", 300000);

        /* Read delay: time from spinup start until data is available (0 = use spinup WAV duration) */
        config->read_delay_ms = ini_section_get_int(cat, "read_delay_ms", 0);

        /* Seek segment markers for three-part WAV (sample positions, 0 = disabled) */
        config->seek_loop_start = ini_section_get_int(cat, "seek_loop_start", 0);
        config->seek_loop_end   = ini_section_get_int(cat, "seek_loop_end", 0);

        cdrom_audio_log("CDROM Audio: Loaded profile %d: %s (%s)\n",
                        audio_profile_count, config->name, config->internal_name);

        audio_profile_count++;
    }

    ini_close(profiles_ini);

    cdrom_audio_log("CDROM Audio: Loaded %d audio profiles\n", audio_profile_count);
}

/* Public API functions */
int
cdrom_audio_get_profile_count(void)
{
    return audio_profile_count;
}

const cdrom_audio_profile_config_t *
cdrom_audio_get_profile(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return &audio_profiles[id];
}

const char *
cdrom_audio_get_profile_name(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return audio_profiles[id].name;
}

const char *
cdrom_audio_get_profile_internal_name(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return audio_profiles[id].internal_name;
}

int
cdrom_audio_get_profile_by_internal_name(const char *internal_name)
{
    if (!internal_name)
        return 0;

    for (int i = 0; i < audio_profile_count; i++) {
        if (strcmp(audio_profiles[i].internal_name, internal_name) == 0)
            return i;
    }
    return 0;
}

static void
cdrom_audio_free_profile_samples(int i)
{
    if (profile_samples[i].spindle_start_buffer) {
        free(profile_samples[i].spindle_start_buffer);
        profile_samples[i].spindle_start_buffer = NULL;
    }
    if (profile_samples[i].spindle_loop_buffer) {
        free(profile_samples[i].spindle_loop_buffer);
        profile_samples[i].spindle_loop_buffer = NULL;
    }
    if (profile_samples[i].spindle_stop_buffer) {
        free(profile_samples[i].spindle_stop_buffer);
        profile_samples[i].spindle_stop_buffer = NULL;
    }
    if (profile_samples[i].seek_buffer) {
        free(profile_samples[i].seek_buffer);
        profile_samples[i].seek_buffer = NULL;
    }
    if (profile_samples[i].tray_open_buffer) {
        free(profile_samples[i].tray_open_buffer);
        profile_samples[i].tray_open_buffer = NULL;
    }
    if (profile_samples[i].tray_close_buffer) {
        free(profile_samples[i].tray_close_buffer);
        profile_samples[i].tray_close_buffer = NULL;
    }
    profile_samples[i].loaded = 0;
}

void
cdrom_audio_close(void)
{
    for (int i = 0; i < CDROM_AUDIO_PROFILE_MAX; i++)
        cdrom_audio_free_profile_samples(i);

    if (cdrom_audio_mutex) {
        thread_close_mutex(cdrom_audio_mutex);
        cdrom_audio_mutex = NULL;
    }
}

/* Load samples for a specific profile */
static void
cdrom_audio_load_profile_samples(int profile_id)
{
    if (profile_id < 0 || profile_id >= audio_profile_count)
        return;

    cdrom_audio_profile_config_t *config  = &audio_profiles[profile_id];
    cdrom_audio_samples_t        *samples = &profile_samples[profile_id];

    if (samples->loaded)
        return;

    /* Profile 0 is "None" - no audio */
    if (profile_id == 0 || strcmp(config->internal_name, "none") == 0) {
        samples->loaded = 1;
        return;
    }

    cdrom_audio_log("CDROM Audio: Loading samples for profile %d (%s)\n", profile_id, config->name);

    /* Load spindle loop */
    if (config->spindlemotor_loop.filename[0]) {
        samples->spindle_loop_buffer = sound_load_wav(
            config->spindlemotor_loop.filename,
            &samples->spindle_loop_samples);
        if (samples->spindle_loop_buffer) {
            samples->spindle_loop_volume = config->spindlemotor_loop.volume;
            cdrom_audio_log("CDROM Audio: Loaded spindle loop, %d frames\n", samples->spindle_loop_samples);
        } else {
            cdrom_audio_log("CDROM Audio: Failed to load spindle loop: %s\n", config->spindlemotor_loop.filename);
        }
    }

    /* Load spindle start */
    if (config->spindlemotor_start.filename[0]) {
        samples->spindle_start_buffer = sound_load_wav(
            config->spindlemotor_start.filename,
            &samples->spindle_start_samples);
        if (samples->spindle_start_buffer) {
            samples->spindle_start_volume = config->spindlemotor_start.volume;
            cdrom_audio_log("CDROM Audio: Loaded spindle start, %d frames\n", samples->spindle_start_samples);
        }
    }

    /* Load spindle stop */
    if (config->spindlemotor_stop.filename[0]) {
        samples->spindle_stop_buffer = sound_load_wav(
            config->spindlemotor_stop.filename,
            &samples->spindle_stop_samples);
        if (samples->spindle_stop_buffer) {
            samples->spindle_stop_volume = config->spindlemotor_stop.volume;
            cdrom_audio_log("CDROM Audio: Loaded spindle stop, %d frames\n", samples->spindle_stop_samples);
        }
    }

    /* Load seek sound */
    if (config->seek_track.filename[0]) {
        samples->seek_buffer = sound_load_wav(
            config->seek_track.filename,
            &samples->seek_samples);
        if (samples->seek_buffer) {
            samples->seek_volume = config->seek_track.volume;
            cdrom_audio_log("CDROM Audio: Loaded seek sound, %d frames (%.1f ms)\n",
                            samples->seek_samples, (float) samples->seek_samples / 48.0f);

            /* Set three-segment boundaries if markers are configured */
            if (config->seek_loop_start > 0 && config->seek_loop_end > 0 &&
                config->seek_loop_end > config->seek_loop_start) {
                samples->seek_head_end   = config->seek_loop_start;
                samples->seek_loop_start = config->seek_loop_start;
                samples->seek_loop_end   = config->seek_loop_end;
                samples->seek_tail_start = config->seek_loop_end;

                /* Clamp to WAV length */
                if (samples->seek_head_end > samples->seek_samples)
                    samples->seek_head_end = samples->seek_samples;
                if (samples->seek_loop_start > samples->seek_samples)
                    samples->seek_loop_start = samples->seek_samples;
                if (samples->seek_loop_end > samples->seek_samples)
                    samples->seek_loop_end = samples->seek_samples;
                if (samples->seek_tail_start > samples->seek_samples)
                    samples->seek_tail_start = samples->seek_samples;

                /* Only mark as segmented if loop section has positive length */
                if (samples->seek_loop_end > samples->seek_loop_start) {
                    samples->seek_segmented = 1;
                    cdrom_audio_log("CDROM Audio: Seek segments: head 0-%d, loop %d-%d, tail %d-%d\n",
                                    samples->seek_head_end, samples->seek_loop_start,
                                    samples->seek_loop_end, samples->seek_tail_start,
                                    samples->seek_samples);
                }
            }
        } else {
            cdrom_audio_log("CDROM Audio: Failed to load seek sound: %s\n", config->seek_track.filename);
        }
    }

    /* Load tray open sound */
    if (config->tray_open.filename[0]) {
        samples->tray_open_buffer = sound_load_wav(
            config->tray_open.filename,
            &samples->tray_open_samples);
        if (samples->tray_open_buffer) {
            samples->tray_open_volume = config->tray_open.volume;
            cdrom_audio_log("CDROM Audio: Loaded tray open sound, %d frames\n", samples->tray_open_samples);
        }
    }

    /* Load tray close sound */
    if (config->tray_close.filename[0]) {
        samples->tray_close_buffer = sound_load_wav(
            config->tray_close.filename,
            &samples->tray_close_samples);
        if (samples->tray_close_buffer) {
            samples->tray_close_volume = config->tray_close.volume;
            cdrom_audio_log("CDROM Audio: Loaded tray close sound, %d frames\n", samples->tray_close_samples);
        }
    }

    samples->loaded = 1;
}

/* Find drive state for a given CD-ROM index, or NULL if not tracked */
static cdrom_audio_drive_state_t *
cdrom_audio_find_drive_state(uint8_t cdrom_id)
{
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].cdrom_index == cdrom_id)
            return &drive_states[i];
    }
    return NULL;
}

static void
cdrom_audio_init_drive_states(void)
{
    active_drive_count = 0;

    for (int i = 0; i < CDROM_NUM && active_drive_count < CDROM_NUM; i++) {
        if (cdrom[i].bus_type != CDROM_BUS_DISABLED && cdrom[i].audio_profile > 0) {
            cdrom_audio_log("CDROM Audio Init: CDROM %d bus_type=%d audio_profile=%d\n",
                            i, cdrom[i].bus_type, cdrom[i].audio_profile);

            cdrom_audio_drive_state_t *state = &drive_states[active_drive_count];
            state->cdrom_index             = i;
            state->profile_id              = cdrom[i].audio_profile;
            state->spindle_state           = CDROM_SPINDLE_STOPPED;
            state->spindle_pos             = 0;
            state->spindle_transition_pos  = 0;
            state->tray_state.active       = 0;
            state->pending_actions         = CDROM_PENDING_NONE;
            state->idle_samples            = 0;
            state->idle_timeout_samples    = (audio_profiles[state->profile_id].idle_timeout_ms > 0)
                                           ? (48000 * audio_profiles[state->profile_id].idle_timeout_ms / 1000)
                                           : 0;
            state->read_delay_samples      = (audio_profiles[state->profile_id].read_delay_ms > 0)
                                           ? (48000 * audio_profiles[state->profile_id].read_delay_ms / 1000)
                                           : 0;

            state->seek_state.active           = 0;
            state->seek_state.position         = 0;
            state->seek_state.elapsed          = 0;
            state->seek_state.duration_samples = 0;
            state->seek_state.volume           = 1.0f;
            state->seek_state.profile_id       = state->profile_id;

            cdrom_audio_load_profile_samples(state->profile_id);

            cdrom_audio_log("CDROM Audio: Initialized drive %d with profile %d (%s)\n",
                            i, state->profile_id,
                            cdrom_audio_get_profile_name(state->profile_id));

            active_drive_count++;
        }
    }

    cdrom_audio_log("CDROM Audio Init: %d active drives with audio\n", active_drive_count);
}

void
cdrom_audio_init(void)
{
    memset(profile_samples, 0, sizeof(profile_samples));
    memset(drive_states, 0, sizeof(drive_states));
    active_drive_count = 0;

    cdrom_audio_log("CDROM Audio Init: audio_profile_count=%d\n", audio_profile_count);

    if (!cdrom_audio_mutex)
        cdrom_audio_mutex = thread_create_mutex();

    cdrom_audio_init_drive_states();

    /* Spin up drives that have media loaded */
    for (int i = 0; i < active_drive_count; i++) {
        if (cdrom[drive_states[i].cdrom_index].cd_status != CD_STATUS_EMPTY)
            cdrom_audio_spinup_drive(drive_states[i].cdrom_index);
    }

    sound_cdrom_activity_thread_init();
}

void
cdrom_audio_reset(void)
{
    cdrom_audio_log("CDROM Audio: Reset\n");

    if (cdrom_audio_mutex)
        thread_wait_mutex(cdrom_audio_mutex);

    /* Reset all drive states */
    for (int i = 0; i < active_drive_count; i++) {
        drive_states[i].spindle_state          = CDROM_SPINDLE_STOPPED;
        drive_states[i].spindle_pos            = 0;
        drive_states[i].spindle_transition_pos = 0;
        drive_states[i].tray_state.active      = 0;
        drive_states[i].pending_actions        = CDROM_PENDING_NONE;
        drive_states[i].seek_state.active           = 0;
        drive_states[i].seek_state.position         = 0;
        drive_states[i].seek_state.elapsed          = 0;
        drive_states[i].seek_state.duration_samples = 0;
        drive_states[i].seek_state.volume           = 1.0f;
    }
    active_drive_count = 0;

    /* Free previously loaded samples (but keep profiles) */
    for (int i = 0; i < CDROM_AUDIO_PROFILE_MAX; i++)
        cdrom_audio_free_profile_samples(i);

    if (cdrom_audio_mutex)
        thread_release_mutex(cdrom_audio_mutex);

    cdrom_audio_init_drive_states();

    /* Spin up drives that have media loaded */
    for (int i = 0; i < active_drive_count; i++) {
        if (cdrom[drive_states[i].cdrom_index].cd_status != CD_STATUS_EMPTY)
            cdrom_audio_spinup_drive(drive_states[i].cdrom_index);
    }
}

void
cdrom_audio_seek(uint8_t cdrom_id, uint32_t new_pos, double seek_time_us)
{
    cdrom_audio_drive_state_t *drive_state = cdrom_audio_find_drive_state(cdrom_id);
    if (!drive_state)
        return;

    /* Reset idle timer on any drive activity */
    drive_state->idle_samples = 0;

    /* Auto-spinup if the drive is stopped/stopping */
    if (drive_state->spindle_state == CDROM_SPINDLE_STOPPED ||
        drive_state->spindle_state == CDROM_SPINDLE_STOPPING) {
        cdrom_audio_spinup_drive(cdrom_id);
    }

    int profile_id = drive_state->profile_id;
    if (profile_id == 0 || profile_id >= audio_profile_count)
        return;

    if (!profile_samples[profile_id].loaded)
        cdrom_audio_load_profile_samples(profile_id);

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->seek_buffer || samples->seek_samples == 0)
        return;

    /* Convert seek time from microseconds to audio samples at 48 kHz. */
    int duration = (int) (seek_time_us * 48000.0 / 1000000.0);
    if (duration < 1)
        duration = 1;

    if (!cdrom_audio_mutex)
        return;

    thread_wait_mutex(cdrom_audio_mutex);

    /* Restart the single seek voice — cuts any in-progress seek sound */
    drive_state->seek_state.active           = 1;
    drive_state->seek_state.position         = 0;
    drive_state->seek_state.elapsed          = 0;
    drive_state->seek_state.duration_samples = duration;
    drive_state->seek_state.volume           = samples->seek_volume;
    drive_state->seek_state.profile_id       = profile_id;
    drive_state->seek_state.segmented        = samples->seek_segmented;
    drive_state->seek_state.phase            = CDROM_SEEK_PHASE_HEAD;

    thread_release_mutex(cdrom_audio_mutex);
}

void
cdrom_audio_spinup_drive(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return;

    if (state->spindle_state == CDROM_SPINDLE_RUNNING || state->spindle_state == CDROM_SPINDLE_STARTING)
        return;

    cdrom_audio_log("CDROM Audio: Spinup requested for drive %d (current state: %d)\n",
                    cdrom_id, state->spindle_state);

    if (cdrom_audio_mutex)
        thread_wait_mutex(cdrom_audio_mutex);

    if (state->spindle_state == CDROM_SPINDLE_STOPPING) {
        /* Reverse spindown immediately: the motor hasn't fully stopped yet.
           Calculate how far through spindown we are, then skip proportionally
           into the spinup sound (disc is still partially spinning). */
        int                    profile_id = state->profile_id;
        cdrom_audio_samples_t *samples    = &profile_samples[profile_id];

        int spinup_skip = 0;
        if (samples->spindle_stop_samples > 0 && samples->spindle_start_samples > 0) {
            double spindown_progress = (double) state->spindle_transition_pos
                                     / (double) samples->spindle_stop_samples;
            /* Skip the portion of spinup corresponding to remaining motor speed.
               e.g. 30% through spindown → motor at ~70% → skip 70% of spinup */
            spinup_skip = (int) ((1.0 - spindown_progress) * (double) samples->spindle_start_samples);
            if (spinup_skip >= samples->spindle_start_samples)
                spinup_skip = samples->spindle_start_samples - 1;
            if (spinup_skip < 0)
                spinup_skip = 0;
        }

        state->spindle_state          = CDROM_SPINDLE_STARTING;
        state->spindle_transition_pos = spinup_skip;
        cdrom_audio_log("CDROM Audio: Drive %d reversed spindown at %.0f%%, spinup skip=%d/%d\n",
                        cdrom_id,
                        samples->spindle_stop_samples > 0
                            ? 100.0 * state->spindle_transition_pos / samples->spindle_stop_samples
                            : 0.0,
                        spinup_skip, samples->spindle_start_samples);
    } else if (state->tray_state.active ||
               (state->pending_actions & (CDROM_PENDING_TRAY_CLOSE | CDROM_PENDING_TRAY_OPEN))) {
        state->pending_actions |= CDROM_PENDING_SPINUP;
        cdrom_audio_log("CDROM Audio: Drive %d spinup deferred (tray=%d, pending=0x%x)\n",
                        cdrom_id, state->tray_state.active, state->pending_actions);
    } else {
        state->spindle_state          = CDROM_SPINDLE_STARTING;
        state->spindle_transition_pos = 0;
    }

    if (cdrom_audio_mutex)
        thread_release_mutex(cdrom_audio_mutex);
}

void
cdrom_audio_spindown_drive(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return;

    if (state->spindle_state == CDROM_SPINDLE_STOPPED || state->spindle_state == CDROM_SPINDLE_STOPPING)
        return;

    cdrom_audio_log("CDROM Audio: Spindown requested for drive %d (current state: %d)\n",
                    cdrom_id, state->spindle_state);

    if (cdrom_audio_mutex)
        thread_wait_mutex(cdrom_audio_mutex);
    state->spindle_state          = CDROM_SPINDLE_STOPPING;
    state->spindle_transition_pos = 0;
    if (cdrom_audio_mutex)
        thread_release_mutex(cdrom_audio_mutex);
}

cdrom_spindle_state_t
cdrom_audio_get_drive_spindle_state(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return CDROM_SPINDLE_STOPPED;
    return state->spindle_state;
}

double
cdrom_audio_get_spin_delay_us(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return 0.0;

    int profile_id = state->profile_id;
    if (profile_id <= 0 || profile_id >= audio_profile_count)
        return 0.0;

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->loaded)
        return 0.0;

    /* Effective spinup duration for timing purposes:
       if read_delay_samples is set, use it; otherwise use the WAV duration */
    double full_spinup = (state->read_delay_samples > 0)
                       ? (double) state->read_delay_samples
                       : (double) samples->spindle_start_samples;

    double delay_samples = 0.0;

    switch (state->spindle_state) {
        case CDROM_SPINDLE_RUNNING:
            return 0.0;
        case CDROM_SPINDLE_STARTING:
            /* Scale the remaining delay proportionally to how much spinup WAV remains.
               This handles both normal spinup and reversed-spindown cases correctly:
               after a reversal, transition_pos is already partway through the WAV. */
            if (samples->spindle_start_samples > 0) {
                double remaining_fraction = (double) (samples->spindle_start_samples - state->spindle_transition_pos)
                                          / (double) samples->spindle_start_samples;
                delay_samples = remaining_fraction * full_spinup;
            }
            break;
        case CDROM_SPINDLE_STOPPING:
            /* This case should be rare (reversal now happens immediately),
               but handle defensively: remaining spindown + full spinup */
            delay_samples = (double) (samples->spindle_stop_samples - state->spindle_transition_pos)
                          + full_spinup;
            break;
        case CDROM_SPINDLE_STOPPED:
            delay_samples = full_spinup;
            break;
    }

    if (delay_samples <= 0.0)
        return 0.0;

    /* Convert audio samples at 48 kHz to microseconds */
    return (delay_samples / 48000.0) * 1000000.0;
}

void
cdrom_audio_tray_open(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return;

    int profile_id = state->profile_id;
    if (profile_id <= 0 || profile_id >= audio_profile_count)
        return;

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->tray_open_buffer || samples->tray_open_samples == 0)
        return;

    if (cdrom_audio_mutex)
        thread_wait_mutex(cdrom_audio_mutex);

    /* If spindle is still stopping, defer the tray open sound */
    if (state->spindle_state == CDROM_SPINDLE_STOPPING ||
        state->spindle_state == CDROM_SPINDLE_RUNNING ||
        state->spindle_state == CDROM_SPINDLE_STARTING) {
        state->pending_actions |= CDROM_PENDING_TRAY_OPEN;
        cdrom_audio_log("CDROM Audio: Drive %d tray open deferred until spindown\n", cdrom_id);
    } else {
        state->tray_state.active   = 1;
        state->tray_state.position = 0;
        state->tray_state.volume   = samples->tray_open_volume;
        state->tray_state.is_open  = 1;
    }

    if (cdrom_audio_mutex)
        thread_release_mutex(cdrom_audio_mutex);
}

void
cdrom_audio_tray_close(uint8_t cdrom_id)
{
    cdrom_audio_drive_state_t *state = cdrom_audio_find_drive_state(cdrom_id);
    if (!state)
        return;

    int profile_id = state->profile_id;
    if (profile_id <= 0 || profile_id >= audio_profile_count)
        return;

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];

    if (cdrom_audio_mutex)
        thread_wait_mutex(cdrom_audio_mutex);

    /* If a tray open sound is still playing or pending, wait for it */
    if (state->tray_state.active || (state->pending_actions & CDROM_PENDING_TRAY_OPEN)) {
        state->pending_actions |= CDROM_PENDING_TRAY_CLOSE;
        cdrom_audio_log("CDROM Audio: Drive %d tray close deferred\n", cdrom_id);
    } else if (samples->tray_close_buffer && samples->tray_close_samples > 0) {
        state->tray_state.active   = 1;
        state->tray_state.position = 0;
        state->tray_state.volume   = samples->tray_close_volume;
        state->tray_state.is_open  = 0;
    }

    if (cdrom_audio_mutex)
        thread_release_mutex(cdrom_audio_mutex);
}

/* ---- Float mixing helpers ---- */

static void
cdrom_audio_mix_spindle_start_float(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                    float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_start_buffer || samples->spindle_start_samples <= 0) {
        state->spindle_state = CDROM_SPINDLE_RUNNING;
        state->spindle_pos   = 0;
        return;
    }

    float vol = samples->spindle_start_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_start_samples; i++) {
        float_buffer[i * 2]     += (float) samples->spindle_start_buffer[state->spindle_transition_pos * 2] / 131072.0f * vol;
        float_buffer[i * 2 + 1] += (float) samples->spindle_start_buffer[state->spindle_transition_pos * 2 + 1] / 131072.0f * vol;
        state->spindle_transition_pos++;
    }

    if (state->spindle_transition_pos >= samples->spindle_start_samples) {
        state->spindle_state = CDROM_SPINDLE_RUNNING;
        state->spindle_pos   = 0;
        cdrom_audio_log("CDROM Audio: Drive %d spinup complete, now running\n", state->cdrom_index);
    }
}

static void
cdrom_audio_mix_spindle_loop_float(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                   float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_loop_buffer || samples->spindle_loop_samples <= 0)
        return;

    float vol = samples->spindle_loop_volume;
    for (int i = 0; i < frames_in_buffer; i++) {
        float_buffer[i * 2]     += (float) samples->spindle_loop_buffer[state->spindle_pos * 2] / 131072.0f * vol;
        float_buffer[i * 2 + 1] += (float) samples->spindle_loop_buffer[state->spindle_pos * 2 + 1] / 131072.0f * vol;

        state->spindle_pos++;
        if (state->spindle_pos >= samples->spindle_loop_samples)
            state->spindle_pos = 0;
    }
}

static void
cdrom_audio_mix_spindle_stop_float(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                   float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_stop_buffer || samples->spindle_stop_samples <= 0) {
        state->spindle_state = CDROM_SPINDLE_STOPPED;
        return;
    }

    float vol = samples->spindle_stop_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_stop_samples; i++) {
        float_buffer[i * 2]     += (float) samples->spindle_stop_buffer[state->spindle_transition_pos * 2] / 131072.0f * vol;
        float_buffer[i * 2 + 1] += (float) samples->spindle_stop_buffer[state->spindle_transition_pos * 2 + 1] / 131072.0f * vol;
        state->spindle_transition_pos++;
    }

    if (state->spindle_transition_pos >= samples->spindle_stop_samples) {
        state->spindle_state = CDROM_SPINDLE_STOPPED;
        cdrom_audio_log("CDROM Audio: Drive %d spindown complete, now stopped\n", state->cdrom_index);
    }
}

static void
cdrom_audio_mix_seek_float(cdrom_audio_drive_state_t *state, float *float_buffer, int frames_in_buffer)
{
    if (!state->seek_state.active)
        return;

    int                    seek_profile_id = state->seek_state.profile_id;
    cdrom_audio_samples_t *seek_samples    = &profile_samples[seek_profile_id];
    if (!seek_samples->seek_buffer || seek_samples->seek_samples == 0)
        return;

    float vol      = state->seek_state.volume;
    int   pos      = state->seek_state.position;
    int   elapsed  = state->seek_state.elapsed;
    int   duration = state->seek_state.duration_samples;

    if (!state->seek_state.segmented) {
        /* Non-segmented: loop entire WAV for duration with fade-out */
        int wav_len    = seek_samples->seek_samples;
        int fade_start = duration - 480;
        if (fade_start < 0)
            fade_start = 0;

        for (int i = 0; i < frames_in_buffer && elapsed < duration; i++, elapsed++) {
            float env = 1.0f;
            if (elapsed >= fade_start && duration > fade_start)
                env = (float) (duration - elapsed) / (float) (duration - fade_start);

            float_buffer[i * 2]     += (float) seek_samples->seek_buffer[pos * 2] / 131072.0f * vol * env;
            float_buffer[i * 2 + 1] += (float) seek_samples->seek_buffer[pos * 2 + 1] / 131072.0f * vol * env;

            pos++;
            if (pos >= wav_len)
                pos = 0;
        }
    } else {
        /* Three-segment playback: HEAD -> LOOP (repeated) -> TAIL */
        int head_end   = seek_samples->seek_head_end;
        int loop_start = seek_samples->seek_loop_start;
        int loop_end   = seek_samples->seek_loop_end;
        int tail_start = seek_samples->seek_tail_start;
        int tail_len   = seek_samples->seek_samples - tail_start;

        /* Reserve time for the tail so we always play it */
        int tail_budget = tail_len;
        /* Fade out over last 480 samples of the tail */
        int fade_start  = duration - 480;
        if (fade_start < 0)
            fade_start = 0;

        for (int i = 0; i < frames_in_buffer && elapsed < duration; i++, elapsed++) {
            float env = 1.0f;
            if (elapsed >= fade_start && duration > fade_start)
                env = (float) (duration - elapsed) / (float) (duration - fade_start);

            /* Determine which segment to play */
            if (state->seek_state.phase == CDROM_SEEK_PHASE_HEAD) {
                float_buffer[i * 2]     += (float) seek_samples->seek_buffer[pos * 2] / 131072.0f * vol * env;
                float_buffer[i * 2 + 1] += (float) seek_samples->seek_buffer[pos * 2 + 1] / 131072.0f * vol * env;
                pos++;
                if (pos >= head_end) {
                    /* Head done; if remaining time can't fit loop, skip to tail */
                    int remaining = duration - elapsed - 1;
                    if (remaining <= tail_budget || loop_end <= loop_start) {
                        state->seek_state.phase = CDROM_SEEK_PHASE_TAIL;
                        pos = tail_start;
                    } else {
                        state->seek_state.phase = CDROM_SEEK_PHASE_LOOP;
                        pos = loop_start;
                    }
                }
            } else if (state->seek_state.phase == CDROM_SEEK_PHASE_LOOP) {
                float_buffer[i * 2]     += (float) seek_samples->seek_buffer[pos * 2] / 131072.0f * vol * env;
                float_buffer[i * 2 + 1] += (float) seek_samples->seek_buffer[pos * 2 + 1] / 131072.0f * vol * env;
                pos++;
                if (pos >= loop_end)
                    pos = loop_start; /* Repeat loop section */

                /* Transition to tail when remaining time <= tail length */
                int remaining = duration - elapsed - 1;
                if (remaining <= tail_budget) {
                    state->seek_state.phase = CDROM_SEEK_PHASE_TAIL;
                    pos = tail_start;
                }
            } else {
                /* TAIL phase */
                if (pos < seek_samples->seek_samples) {
                    float_buffer[i * 2]     += (float) seek_samples->seek_buffer[pos * 2] / 131072.0f * vol * env;
                    float_buffer[i * 2 + 1] += (float) seek_samples->seek_buffer[pos * 2 + 1] / 131072.0f * vol * env;
                    pos++;
                }
                /* If tail WAV runs out before duration, we just output silence until duration ends */
            }
        }
    }

    if (elapsed >= duration) {
        state->seek_state.active   = 0;
        state->seek_state.position = 0;
        state->seek_state.elapsed  = 0;
    } else {
        state->seek_state.position = pos;
        state->seek_state.elapsed  = elapsed;
    }
}

static void
cdrom_audio_mix_tray_float(cdrom_audio_drive_state_t *state, float *float_buffer, int frames_in_buffer)
{
    if (!state->tray_state.active)
        return;

    int                    profile_id = state->profile_id;
    cdrom_audio_samples_t *samples    = &profile_samples[profile_id];
    int16_t               *buf;
    int                    total_samples;

    if (state->tray_state.is_open) {
        buf           = samples->tray_open_buffer;
        total_samples = samples->tray_open_samples;
    } else {
        buf           = samples->tray_close_buffer;
        total_samples = samples->tray_close_samples;
    }

    if (!buf || total_samples == 0) {
        state->tray_state.active = 0;
        return;
    }

    float vol = state->tray_state.volume;
    int   pos = state->tray_state.position;

    for (int i = 0; i < frames_in_buffer && pos < total_samples; i++, pos++) {
        float_buffer[i * 2]     += (float) buf[pos * 2] / 131072.0f * vol;
        float_buffer[i * 2 + 1] += (float) buf[pos * 2 + 1] / 131072.0f * vol;
    }

    if (pos >= total_samples) {
        state->tray_state.active = 0;
        state->tray_state.position = 0;
    } else {
        state->tray_state.position = pos;
    }
}

/* ---- Int16 mixing helpers ---- */

static void
cdrom_audio_mix_spindle_start_int16(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                    int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_start_buffer || samples->spindle_start_samples <= 0) {
        state->spindle_state = CDROM_SPINDLE_RUNNING;
        state->spindle_pos   = 0;
        return;
    }

    float vol = samples->spindle_start_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_start_samples; i++) {
        int32_t left  = buffer[i * 2] + (int32_t) (samples->spindle_start_buffer[state->spindle_transition_pos * 2] * vol);
        int32_t right = buffer[i * 2 + 1] + (int32_t) (samples->spindle_start_buffer[state->spindle_transition_pos * 2 + 1] * vol);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;
        state->spindle_transition_pos++;
    }

    if (state->spindle_transition_pos >= samples->spindle_start_samples) {
        state->spindle_state = CDROM_SPINDLE_RUNNING;
        state->spindle_pos   = 0;
        cdrom_audio_log("CDROM Audio: Drive %d spinup complete, now running\n", state->cdrom_index);
    }
}

static void
cdrom_audio_mix_spindle_loop_int16(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                   int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_loop_buffer || samples->spindle_loop_samples <= 0)
        return;

    float vol = samples->spindle_loop_volume;
    for (int i = 0; i < frames_in_buffer; i++) {
        int32_t left  = buffer[i * 2] + (int32_t) (samples->spindle_loop_buffer[state->spindle_pos * 2] * vol);
        int32_t right = buffer[i * 2 + 1] + (int32_t) (samples->spindle_loop_buffer[state->spindle_pos * 2 + 1] * vol);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;

        state->spindle_pos++;
        if (state->spindle_pos >= samples->spindle_loop_samples)
            state->spindle_pos = 0;
    }
}

static void
cdrom_audio_mix_spindle_stop_int16(cdrom_audio_drive_state_t *state, cdrom_audio_samples_t *samples,
                                   int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_stop_buffer || samples->spindle_stop_samples <= 0) {
        state->spindle_state = CDROM_SPINDLE_STOPPED;
        return;
    }

    float vol = samples->spindle_stop_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_stop_samples; i++) {
        int32_t left  = buffer[i * 2] + (int32_t) (samples->spindle_stop_buffer[state->spindle_transition_pos * 2] * vol);
        int32_t right = buffer[i * 2 + 1] + (int32_t) (samples->spindle_stop_buffer[state->spindle_transition_pos * 2 + 1] * vol);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;
        state->spindle_transition_pos++;
    }

    if (state->spindle_transition_pos >= samples->spindle_stop_samples) {
        state->spindle_state = CDROM_SPINDLE_STOPPED;
        cdrom_audio_log("CDROM Audio: Drive %d spindown complete, now stopped\n", state->cdrom_index);
    }
}

static void
cdrom_audio_mix_seek_int16(cdrom_audio_drive_state_t *state, int16_t *buffer, int frames_in_buffer)
{
    if (!state->seek_state.active)
        return;

    int                    seek_profile_id = state->seek_state.profile_id;
    cdrom_audio_samples_t *seek_samples    = &profile_samples[seek_profile_id];
    if (!seek_samples->seek_buffer || seek_samples->seek_samples == 0)
        return;

    float vol      = state->seek_state.volume;
    int   pos      = state->seek_state.position;
    int   elapsed  = state->seek_state.elapsed;
    int   duration = state->seek_state.duration_samples;

    if (!state->seek_state.segmented) {
        /* Non-segmented: loop entire WAV for duration with fade-out */
        int wav_len    = seek_samples->seek_samples;
        int fade_start = duration - 480;
        if (fade_start < 0)
            fade_start = 0;

        for (int i = 0; i < frames_in_buffer && elapsed < duration; i++, elapsed++) {
            float env = 1.0f;
            if (elapsed >= fade_start && duration > fade_start)
                env = (float) (duration - elapsed) / (float) (duration - fade_start);

            int32_t left  = buffer[i * 2] + (int32_t) (seek_samples->seek_buffer[pos * 2] * vol * env);
            int32_t right = buffer[i * 2 + 1] + (int32_t) (seek_samples->seek_buffer[pos * 2 + 1] * vol * env);
            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;
            buffer[i * 2]     = (int16_t) left;
            buffer[i * 2 + 1] = (int16_t) right;

            pos++;
            if (pos >= wav_len)
                pos = 0;
        }
    } else {
        /* Three-segment playback: HEAD -> LOOP (repeated) -> TAIL */
        int head_end   = seek_samples->seek_head_end;
        int loop_start = seek_samples->seek_loop_start;
        int loop_end   = seek_samples->seek_loop_end;
        int tail_start = seek_samples->seek_tail_start;
        int tail_len   = seek_samples->seek_samples - tail_start;

        int tail_budget = tail_len;
        int fade_start  = duration - 480;
        if (fade_start < 0)
            fade_start = 0;

        for (int i = 0; i < frames_in_buffer && elapsed < duration; i++, elapsed++) {
            float env = 1.0f;
            if (elapsed >= fade_start && duration > fade_start)
                env = (float) (duration - elapsed) / (float) (duration - fade_start);

            int32_t left, right;

            if (state->seek_state.phase == CDROM_SEEK_PHASE_HEAD) {
                left  = buffer[i * 2] + (int32_t) (seek_samples->seek_buffer[pos * 2] * vol * env);
                right = buffer[i * 2 + 1] + (int32_t) (seek_samples->seek_buffer[pos * 2 + 1] * vol * env);
                pos++;
                if (pos >= head_end) {
                    int remaining = duration - elapsed - 1;
                    if (remaining <= tail_budget || loop_end <= loop_start) {
                        state->seek_state.phase = CDROM_SEEK_PHASE_TAIL;
                        pos = tail_start;
                    } else {
                        state->seek_state.phase = CDROM_SEEK_PHASE_LOOP;
                        pos = loop_start;
                    }
                }
            } else if (state->seek_state.phase == CDROM_SEEK_PHASE_LOOP) {
                left  = buffer[i * 2] + (int32_t) (seek_samples->seek_buffer[pos * 2] * vol * env);
                right = buffer[i * 2 + 1] + (int32_t) (seek_samples->seek_buffer[pos * 2 + 1] * vol * env);
                pos++;
                if (pos >= loop_end)
                    pos = loop_start;

                int remaining = duration - elapsed - 1;
                if (remaining <= tail_budget) {
                    state->seek_state.phase = CDROM_SEEK_PHASE_TAIL;
                    pos = tail_start;
                }
            } else {
                if (pos < seek_samples->seek_samples) {
                    left  = buffer[i * 2] + (int32_t) (seek_samples->seek_buffer[pos * 2] * vol * env);
                    right = buffer[i * 2 + 1] + (int32_t) (seek_samples->seek_buffer[pos * 2 + 1] * vol * env);
                    pos++;
                } else {
                    left  = buffer[i * 2];
                    right = buffer[i * 2 + 1];
                }
            }

            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;
            buffer[i * 2]     = (int16_t) left;
            buffer[i * 2 + 1] = (int16_t) right;
        }
    }

    if (elapsed >= duration) {
        state->seek_state.active   = 0;
        state->seek_state.position = 0;
        state->seek_state.elapsed  = 0;
    } else {
        state->seek_state.position = pos;
        state->seek_state.elapsed  = elapsed;
    }
}

static void
cdrom_audio_mix_tray_int16(cdrom_audio_drive_state_t *state, int16_t *buffer, int frames_in_buffer)
{
    if (!state->tray_state.active)
        return;

    int                    profile_id = state->profile_id;
    cdrom_audio_samples_t *samples    = &profile_samples[profile_id];
    int16_t               *buf;
    int                    total_samples;

    if (state->tray_state.is_open) {
        buf           = samples->tray_open_buffer;
        total_samples = samples->tray_open_samples;
    } else {
        buf           = samples->tray_close_buffer;
        total_samples = samples->tray_close_samples;
    }

    if (!buf || total_samples == 0) {
        state->tray_state.active = 0;
        return;
    }

    float vol = state->tray_state.volume;
    int   pos = state->tray_state.position;

    for (int i = 0; i < frames_in_buffer && pos < total_samples; i++, pos++) {
        int32_t left  = buffer[i * 2] + (int32_t) (buf[pos * 2] * vol);
        int32_t right = buffer[i * 2 + 1] + (int32_t) (buf[pos * 2 + 1] * vol);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;
    }

    if (pos >= total_samples) {
        state->tray_state.active   = 0;
        state->tray_state.position = 0;
    } else {
        state->tray_state.position = pos;
    }
}

/* ---- Process pending actions after transitions complete ---- */

static void
cdrom_audio_process_pending(cdrom_audio_drive_state_t *state)
{
    if (state->pending_actions == CDROM_PENDING_NONE)
        return;

    /* After spindown completes, play tray open if pending */
    if ((state->pending_actions & CDROM_PENDING_TRAY_OPEN) &&
        state->spindle_state == CDROM_SPINDLE_STOPPED) {
        state->pending_actions &= ~CDROM_PENDING_TRAY_OPEN;

        int                    profile_id = state->profile_id;
        cdrom_audio_samples_t *samples    = &profile_samples[profile_id];
        if (samples->tray_open_buffer && samples->tray_open_samples > 0) {
            state->tray_state.active   = 1;
            state->tray_state.position = 0;
            state->tray_state.volume   = samples->tray_open_volume;
            state->tray_state.is_open  = 1;
            cdrom_audio_log("CDROM Audio: Drive %d playing deferred tray open\n", state->cdrom_index);
        }
    }

    /* After spindown or stop, play tray close if pending */
    if ((state->pending_actions & CDROM_PENDING_TRAY_CLOSE) &&
        state->spindle_state == CDROM_SPINDLE_STOPPED &&
        !state->tray_state.active) {
        state->pending_actions &= ~CDROM_PENDING_TRAY_CLOSE;

        int                    profile_id = state->profile_id;
        cdrom_audio_samples_t *samples    = &profile_samples[profile_id];
        if (samples->tray_close_buffer && samples->tray_close_samples > 0) {
            state->tray_state.active   = 1;
            state->tray_state.position = 0;
            state->tray_state.volume   = samples->tray_close_volume;
            state->tray_state.is_open  = 0;
            cdrom_audio_log("CDROM Audio: Drive %d playing deferred tray close\n", state->cdrom_index);
        } else {
            /* No tray close sound, process spinup immediately */
            if (state->pending_actions & CDROM_PENDING_SPINUP) {
                state->pending_actions &= ~CDROM_PENDING_SPINUP;
                state->spindle_state          = CDROM_SPINDLE_STARTING;
                state->spindle_transition_pos = 0;
                cdrom_audio_log("CDROM Audio: Drive %d spinup (no tray close sound)\n", state->cdrom_index);
            }
        }
    }

    /* After tray close finishes, spin up if pending */
    if ((state->pending_actions & CDROM_PENDING_SPINUP) &&
        !state->tray_state.active &&
        state->spindle_state == CDROM_SPINDLE_STOPPED) {
        state->pending_actions &= ~CDROM_PENDING_SPINUP;
        state->spindle_state          = CDROM_SPINDLE_STARTING;
        state->spindle_transition_pos = 0;
        cdrom_audio_log("CDROM Audio: Drive %d spinning up after tray close\n", state->cdrom_index);
    }
}

/* ---- Per-drive processing ---- */

static void
cdrom_audio_process_drive_float(cdrom_audio_drive_state_t *state, float *float_buffer, int frames_in_buffer)
{
    int profile_id = state->profile_id;
    if (profile_id <= 0 || profile_id >= CDROM_AUDIO_PROFILE_MAX)
        return;

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->loaded)
        return;

    switch (state->spindle_state) {
        case CDROM_SPINDLE_STARTING:
            cdrom_audio_mix_spindle_start_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_RUNNING:
            cdrom_audio_mix_spindle_loop_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_STOPPING:
            cdrom_audio_mix_spindle_stop_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_STOPPED:
        default:
            break;
    }

    /* Seek sounds - only play when spindle is running */
    if (samples->seek_buffer && samples->seek_samples > 0 &&
        cdrom_audio_mutex && state->spindle_state == CDROM_SPINDLE_RUNNING) {
        thread_wait_mutex(cdrom_audio_mutex);
        cdrom_audio_mix_seek_float(state, float_buffer, frames_in_buffer);
        thread_release_mutex(cdrom_audio_mutex);
    }

    /* Tray sounds - play regardless of spindle state */
    if (cdrom_audio_mutex) {
        thread_wait_mutex(cdrom_audio_mutex);
        cdrom_audio_mix_tray_float(state, float_buffer, frames_in_buffer);
        thread_release_mutex(cdrom_audio_mutex);
    }

    /* Idle spindown: accumulate idle time when running, trigger spindown on timeout */
    if (state->spindle_state == CDROM_SPINDLE_RUNNING && state->idle_timeout_samples > 0) {
        state->idle_samples += frames_in_buffer;
        if (state->idle_samples >= state->idle_timeout_samples) {
            cdrom_audio_log("CDROM Audio: Drive %d idle timeout, spinning down\n", state->cdrom_index);
            state->spindle_state          = CDROM_SPINDLE_STOPPING;
            state->spindle_transition_pos = 0;
            state->idle_samples           = 0;
        }
    }

    /* Process pending sequenced actions */
    cdrom_audio_process_pending(state);
}

static void
cdrom_audio_process_drive_int16(cdrom_audio_drive_state_t *state, int16_t *buffer, int frames_in_buffer)
{
    int profile_id = state->profile_id;
    if (profile_id <= 0 || profile_id >= CDROM_AUDIO_PROFILE_MAX)
        return;

    cdrom_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->loaded)
        return;

    switch (state->spindle_state) {
        case CDROM_SPINDLE_STARTING:
            cdrom_audio_mix_spindle_start_int16(state, samples, buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_RUNNING:
            cdrom_audio_mix_spindle_loop_int16(state, samples, buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_STOPPING:
            cdrom_audio_mix_spindle_stop_int16(state, samples, buffer, frames_in_buffer);
            break;
        case CDROM_SPINDLE_STOPPED:
        default:
            break;
    }

    /* Seek sounds - only play when spindle is running */
    if (samples->seek_buffer && samples->seek_samples > 0 &&
        cdrom_audio_mutex && state->spindle_state == CDROM_SPINDLE_RUNNING) {
        thread_wait_mutex(cdrom_audio_mutex);
        cdrom_audio_mix_seek_int16(state, buffer, frames_in_buffer);
        thread_release_mutex(cdrom_audio_mutex);
    }

    /* Tray sounds - play regardless of spindle state */
    if (cdrom_audio_mutex) {
        thread_wait_mutex(cdrom_audio_mutex);
        cdrom_audio_mix_tray_int16(state, buffer, frames_in_buffer);
        thread_release_mutex(cdrom_audio_mutex);
    }

    /* Idle spindown: accumulate idle time when running, trigger spindown on timeout */
    if (state->spindle_state == CDROM_SPINDLE_RUNNING && state->idle_timeout_samples > 0) {
        state->idle_samples += frames_in_buffer;
        if (state->idle_samples >= state->idle_timeout_samples) {
            cdrom_audio_log("CDROM Audio: Drive %d idle timeout, spinning down\n", state->cdrom_index);
            state->spindle_state          = CDROM_SPINDLE_STOPPING;
            state->spindle_transition_pos = 0;
            state->idle_samples           = 0;
        }
    }

    /* Process pending sequenced actions */
    cdrom_audio_process_pending(state);
}

void
cdrom_activity_audio_callback(int16_t *buffer, int length)
{
    int frames_in_buffer = length / 2;

    if (sound_is_float) {
        float *float_buffer = (float *) buffer;

        for (int i = 0; i < length; i++)
            float_buffer[i] = 0.0f;

        for (int d = 0; d < active_drive_count; d++)
            cdrom_audio_process_drive_float(&drive_states[d], float_buffer, frames_in_buffer);
    } else {
        for (int i = 0; i < length; i++)
            buffer[i] = 0;

        for (int d = 0; d < active_drive_count; d++)
            cdrom_audio_process_drive_int16(&drive_states[d], buffer, frames_in_buffer);
    }
}
