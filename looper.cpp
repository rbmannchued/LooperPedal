/*
 * Guitar Looper LV2 Plugin
 *
 * MIDI control (default notes, configurable via control ports):
 *   C4  (60) - Record / Stop+Play / Overdub toggle
 *   D4  (62) - Clear (erase loop, return to idle)
 *   E4  (64) - Pause / Resume
 *
 * States:
 *   IDLE        -> press REC  -> RECORDING
 *   RECORDING   -> press REC  -> PLAYING  (loop starts)
 *   PLAYING     -> press REC  -> OVERDUBBING
 *   OVERDUBBING -> press REC  -> PLAYING
 *   PLAYING     -> press PAU  -> PAUSED
 *   PAUSED      -> press PAU  -> PLAYING
 *   any         -> press CLR  -> IDLE  (buffer cleared)
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PLUGIN_URI   "urn:rafa:Looper"
#define MAX_LOOP_SEC 120   /* maximum loop length in seconds */

/* -------------------------------------------------------------------------
 * Port indices
 * ---------------------------------------------------------------------- */
typedef enum {
    PORT_AUDIO_IN    = 0,
    PORT_AUDIO_OUT   = 1,
    PORT_MIDI_IN     = 2,
    PORT_REC_NOTE    = 3,   /* MIDI note: record / overdub toggle  */
    PORT_CLR_NOTE    = 4,   /* MIDI note: clear loop               */
    PORT_PAUSE_NOTE  = 5,   /* MIDI note: pause / resume           */
    PORT_LEVEL       = 6,   /* playback level  0.0 – 2.0           */
    PORT_STATE_OUT   = 7,   /* current state (0-4, read-only)      */
} PortIndex;

/* -------------------------------------------------------------------------
 * Looper state machine
 * ---------------------------------------------------------------------- */
typedef enum {
    STATE_IDLE        = 0,
    STATE_RECORDING   = 1,
    STATE_PLAYING     = 2,
    STATE_OVERDUBBING = 3,
    STATE_PAUSED      = 4,
} LooperState;

/* -------------------------------------------------------------------------
 * Plugin instance
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Ports */
    const float*             p_audio_in;
    float*                   p_audio_out;
    const LV2_Atom_Sequence* p_midi_in;
    const float*             p_rec_note;
    const float*             p_clr_note;
    const float*             p_pause_note;
    const float*             p_level;
    float*                   p_state_out;

    /* LV2 URID */
    LV2_URID_Map* map;
    LV2_URID      midi_MidiEvent;

    /* Loop buffer */
    float*   buf;
    uint32_t buf_size;    /* allocated capacity in samples        */
    uint32_t loop_len;    /* actual recorded loop length          */
    uint32_t loop_pos;    /* current read/write position          */

    /* State */
    LooperState state;
    LooperState paused_from;  /* which state to return to after pause */

    double sample_rate;
} Looper;

/* -------------------------------------------------------------------------
 * Audio processing kernel (called in chunks between MIDI events)
 * ---------------------------------------------------------------------- */
static void process_audio(Looper* self, const float* in, float* out,
                           uint32_t n)
{
    const float level = *self->p_level;

    for (uint32_t i = 0; i < n; i++) {
        const float x = in[i];

        switch (self->state) {

        case STATE_IDLE:
            out[i] = x;
            break;

        case STATE_RECORDING:
            if (self->loop_pos < self->buf_size) {
                self->buf[self->loop_pos++] = x;
                out[i] = x;
            } else {
                /* Buffer full: auto-stop recording, start playing */
                self->loop_len = self->buf_size;
                self->loop_pos = 0;
                self->state    = STATE_PLAYING;
                const float lo = self->buf[self->loop_pos] * level;
                self->loop_pos = (self->loop_pos + 1) % self->loop_len;
                out[i] = x + lo;
            }
            break;

        case STATE_PLAYING:
            if (self->loop_len > 0) {
                const float lo = self->buf[self->loop_pos] * level;
                self->loop_pos = (self->loop_pos + 1) % self->loop_len;
                out[i] = x + lo;
            } else {
                out[i] = x;
            }
            break;

        case STATE_OVERDUBBING:
            if (self->loop_len > 0) {
                float mixed = self->buf[self->loop_pos] + x;
                /* Hard-clip to prevent unbounded buildup */
                if (mixed >  1.5f) mixed =  1.5f;
                if (mixed < -1.5f) mixed = -1.5f;
                self->buf[self->loop_pos] = mixed;
                self->loop_pos = (self->loop_pos + 1) % self->loop_len;
                out[i] = mixed * level;
            } else {
                out[i] = x;
            }
            break;

        case STATE_PAUSED:
            /* Pass-through only; position does not advance */
            out[i] = x;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * MIDI event handler
 * ---------------------------------------------------------------------- */
static void handle_midi(Looper* self, const uint8_t* msg)
{
    /* Accept note-on with velocity > 0 only */
    if ((msg[0] & 0xF0) != 0x90 || msg[2] == 0) return;

    const int note      = (int)msg[1];
    const int rec_note  = (int)(*self->p_rec_note   + 0.5f);
    const int clr_note  = (int)(*self->p_clr_note   + 0.5f);
    const int pau_note  = (int)(*self->p_pause_note + 0.5f);

    if (note == rec_note) {
        switch (self->state) {
        case STATE_IDLE:
            self->loop_pos = 0;
            self->state    = STATE_RECORDING;
            break;
        case STATE_RECORDING:
            self->loop_len = self->loop_pos;
            self->loop_pos = 0;
            self->state    = (self->loop_len > 0) ? STATE_PLAYING : STATE_IDLE;
            break;
        case STATE_PLAYING:
            self->state = STATE_OVERDUBBING;
            break;
        case STATE_OVERDUBBING:
            self->state = STATE_PLAYING;
            break;
        case STATE_PAUSED:
            self->state = self->paused_from;
            break;
        }
    } else if (note == clr_note) {
        self->state    = STATE_IDLE;
        self->loop_len = 0;
        self->loop_pos = 0;
        /* Old data stays in buf but will be overwritten on next recording */
    } else if (note == pau_note) {
        if (self->state == STATE_PLAYING || self->state == STATE_OVERDUBBING) {
            self->paused_from = self->state;
            self->state       = STATE_PAUSED;
        } else if (self->state == STATE_PAUSED) {
            self->state = self->paused_from;
        }
    }
}

/* -------------------------------------------------------------------------
 * LV2 interface
 * ---------------------------------------------------------------------- */
static LV2_Handle instantiate(const LV2_Descriptor*     descriptor,
                               double                    rate,
                               const char*               bundle_path,
                               const LV2_Feature* const* features)
{
    (void)descriptor; (void)bundle_path;

    Looper* self = (Looper*)calloc(1, sizeof(Looper));
    if (!self) return NULL;

    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map))
            self->map = (LV2_URID_Map*)features[i]->data;
    }
    if (!self->map) { free(self); return NULL; }

    self->midi_MidiEvent =
        self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    self->sample_rate = rate;

    self->buf_size = (uint32_t)(rate * MAX_LOOP_SEC);
    self->buf = (float*)calloc(self->buf_size, sizeof(float));
    if (!self->buf) { free(self); return NULL; }

    self->state = STATE_IDLE;
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    Looper* self = (Looper*)instance;
    switch ((PortIndex)port) {
    case PORT_AUDIO_IN:   self->p_audio_in   = (const float*)data;            break;
    case PORT_AUDIO_OUT:  self->p_audio_out  = (float*)data;                  break;
    case PORT_MIDI_IN:    self->p_midi_in    = (const LV2_Atom_Sequence*)data; break;
    case PORT_REC_NOTE:   self->p_rec_note   = (const float*)data;            break;
    case PORT_CLR_NOTE:   self->p_clr_note   = (const float*)data;            break;
    case PORT_PAUSE_NOTE: self->p_pause_note = (const float*)data;            break;
    case PORT_LEVEL:      self->p_level      = (const float*)data;            break;
    case PORT_STATE_OUT:  self->p_state_out  = (float*)data;                  break;
    }
}

static void activate(LV2_Handle instance)
{
    Looper* self = (Looper*)instance;
    self->state    = STATE_IDLE;
    self->loop_len = 0;
    self->loop_pos = 0;
    memset(self->buf, 0, self->buf_size * sizeof(float));
}

static void run(LV2_Handle instance, uint32_t n_samples)
{
    Looper* self = (Looper*)instance;

    uint32_t offset = 0;

    /* Process MIDI events with sample-accurate timing */
    LV2_ATOM_SEQUENCE_FOREACH(self->p_midi_in, ev) {
        uint32_t t = (uint32_t)ev->time.frames;
        if (t > n_samples) t = n_samples;

        if (t > offset)
            process_audio(self,
                          self->p_audio_in  + offset,
                          self->p_audio_out + offset,
                          t - offset);
        offset = t;

        if (ev->body.type == self->midi_MidiEvent)
            handle_midi(self, (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body));
    }

    /* Remaining samples after last event */
    if (offset < n_samples)
        process_audio(self,
                      self->p_audio_in  + offset,
                      self->p_audio_out + offset,
                      n_samples - offset);

    /* Expose current state for host visualisation */
    if (self->p_state_out)
        *self->p_state_out = (float)self->state;
}

static void deactivate(LV2_Handle instance) { (void)instance; }

static void cleanup(LV2_Handle instance)
{
    Looper* self = (Looper*)instance;
    free(self->buf);
    free(self);
}

static const void* extension_data(const char* uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : NULL;
}
