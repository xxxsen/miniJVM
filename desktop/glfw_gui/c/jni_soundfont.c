#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

#include "jvm.h"
#include "jni_soundfont.h"

#define SOUNDFONT_SAMPLE_RATE 44100
#define SOUNDFONT_CHANNELS 2
#define SOUNDFONT_RENDER_BLOCK 512
#define SOUNDFONT_TAIL_MILLISECONDS 120
#define SOUNDFONT_MAX_MILLISECONDS 180000

static pthread_mutex_t soundfont_mutex = PTHREAD_MUTEX_INITIALIZER;
static tsf *soundfont_synth;

static void write_u16_le(unsigned char *target, unsigned int value) {
    target[0] = (unsigned char) value;
    target[1] = (unsigned char) (value >> 8);
}

static void write_u32_le(unsigned char *target, uint32_t value) {
    target[0] = (unsigned char) value;
    target[1] = (unsigned char) (value >> 8);
    target[2] = (unsigned char) (value >> 16);
    target[3] = (unsigned char) (value >> 24);
}

static int write_wave_header(FILE *output, uint32_t data_size) {
    unsigned char header[44];
    uint32_t byte_rate = SOUNDFONT_SAMPLE_RATE * SOUNDFONT_CHANNELS * 2;
    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4);
    write_u32_le(header + 4, data_size + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_u32_le(header + 16, 16);
    write_u16_le(header + 20, 1);
    write_u16_le(header + 22, SOUNDFONT_CHANNELS);
    write_u32_le(header + 24, SOUNDFONT_SAMPLE_RATE);
    write_u32_le(header + 28, byte_rate);
    write_u16_le(header + 32, SOUNDFONT_CHANNELS * 2);
    write_u16_le(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_u32_le(header + 40, data_size);
    return fwrite(header, 1, sizeof(header), output) == sizeof(header);
}

static int copy_path(Instance *array, char *target, int capacity) {
    int length;
    if (!array || !array->arr_body || array->arr_length <= 0 || capacity <= 1) return 0;
    length = array->arr_length;
    if (length >= capacity) length = capacity - 1;
    memcpy(target, array->arr_body, (size_t) length);
    target[length] = '\0';
    return length > 0;
}

static int render_frames(FILE *output, tsf *synth, uint64_t *current_frame, uint64_t target_frame) {
    short samples[SOUNDFONT_RENDER_BLOCK * SOUNDFONT_CHANNELS];
    while (*current_frame < target_frame) {
        uint64_t remaining = target_frame - *current_frame;
        int count = remaining > SOUNDFONT_RENDER_BLOCK ? SOUNDFONT_RENDER_BLOCK : (int) remaining;
        tsf_render_short(synth, samples, count, 0);
        if (fwrite(samples, sizeof(short) * SOUNDFONT_CHANNELS, (size_t) count, output) != (size_t) count) {
            return 0;
        }
        *current_frame += (uint64_t) count;
    }
    return 1;
}

static void apply_midi_message(tsf *synth, const tml_message *message) {
    int channel = (int) message->channel;
    switch (message->type) {
        case TML_PROGRAM_CHANGE:
            tsf_channel_set_presetnumber(synth, channel, (unsigned char) message->program, channel == 9);
            break;
        case TML_NOTE_ON:
            if ((unsigned char) message->velocity == 0) {
                tsf_channel_note_off(synth, channel, (unsigned char) message->key);
            } else {
                tsf_channel_note_on(synth, channel, (unsigned char) message->key,
                                    (unsigned char) message->velocity / 127.0f);
            }
            break;
        case TML_NOTE_OFF:
            tsf_channel_note_off(synth, channel, (unsigned char) message->key);
            break;
        case TML_PITCH_BEND:
            tsf_channel_set_pitchwheel(synth, channel, message->pitch_bend);
            break;
        case TML_CONTROL_CHANGE:
            tsf_channel_midi_control(synth, channel, (unsigned char) message->control,
                                     (unsigned char) message->control_value);
            break;
        default:
            break;
    }
}

static int soundfont_render_to_wave(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    Instance *midi_data = env->localvar_getRefer(runtime->localvar, 0);
    Instance *soundfont_path_array = env->localvar_getRefer(runtime->localvar, 1);
    Instance *wave_path_array = env->localvar_getRefer(runtime->localvar, 2);
    char soundfont_path[512];
    char wave_path[512];
    tml_message *messages = NULL;
    tml_message *message;
    FILE *output = NULL;
    uint64_t current_frame = 0;
    uint64_t target_frame;
    uint64_t end_frame;
    unsigned int last_milliseconds = 0;
    uint32_t data_size = 0;
    int result = -1;
    int channel;

    (void) clazz;
    if (!midi_data || !midi_data->arr_body || midi_data->arr_length <= 0 ||
        !copy_path(soundfont_path_array, soundfont_path, sizeof(soundfont_path)) ||
        !copy_path(wave_path_array, wave_path, sizeof(wave_path))) {
        env->push_int(runtime->stack, result);
        return 0;
    }

    messages = tml_load_memory(midi_data->arr_body, midi_data->arr_length);
    if (!messages) {
        env->push_int(runtime->stack, -2);
        return 0;
    }

    pthread_mutex_lock(&soundfont_mutex);
    if (!soundfont_synth) soundfont_synth = tsf_load_filename(soundfont_path);
    if (!soundfont_synth) {
        result = -3;
        goto cleanup;
    }

    output = fopen(wave_path, "wb+");
    if (!output || !write_wave_header(output, 0)) {
        result = -4;
        goto cleanup;
    }

    tsf_reset(soundfont_synth);
    tsf_set_output(soundfont_synth, TSF_STEREO_INTERLEAVED, SOUNDFONT_SAMPLE_RATE, -7.0f);
    for (channel = 0; channel < 16; channel++) {
        tsf_channel_set_presetnumber(soundfont_synth, channel, 0, channel == 9);
    }

    for (message = messages; message; message = message->next) {
        last_milliseconds = message->time;
        if (last_milliseconds > SOUNDFONT_MAX_MILLISECONDS) break;
        target_frame = (uint64_t) last_milliseconds * SOUNDFONT_SAMPLE_RATE / 1000;
        if (!render_frames(output, soundfont_synth, &current_frame, target_frame)) {
            result = -5;
            goto cleanup;
        }
        apply_midi_message(soundfont_synth, message);
    }

    if (last_milliseconds > SOUNDFONT_MAX_MILLISECONDS - SOUNDFONT_TAIL_MILLISECONDS) {
        last_milliseconds = SOUNDFONT_MAX_MILLISECONDS - SOUNDFONT_TAIL_MILLISECONDS;
    }
    end_frame = (uint64_t) (last_milliseconds + SOUNDFONT_TAIL_MILLISECONDS) * SOUNDFONT_SAMPLE_RATE / 1000;
    if (!render_frames(output, soundfont_synth, &current_frame, end_frame)) {
        result = -5;
        goto cleanup;
    }

    data_size = (uint32_t) (current_frame * SOUNDFONT_CHANNELS * sizeof(short));
    if (fseek(output, 0, SEEK_SET) != 0 || !write_wave_header(output, data_size)) {
        result = -6;
        goto cleanup;
    }
    result = (int) (current_frame * 1000 / SOUNDFONT_SAMPLE_RATE);
    printf("[audio] SoundFont MIDI rendered: %d ms, %u bytes\n", result, data_size);

cleanup:
    if (output) fclose(output);
    pthread_mutex_unlock(&soundfont_mutex);
    tml_free(messages);
    if (result < 0) remove(wave_path);
    env->push_int(runtime->stack, result);
    return 0;
}

static java_native_method soundfont_methods[] = {
    {"com/ebsee/emu/audio/SoundFontSynth", "renderToWave", "([B[B[B)I", soundfont_render_to_wave},
};

s32 count_SoundFontFuncTable(void) {
    return sizeof(soundfont_methods) / sizeof(java_native_method);
}

__refer ptr_SoundFontFuncTable(void) {
    return &soundfont_methods[0];
}
