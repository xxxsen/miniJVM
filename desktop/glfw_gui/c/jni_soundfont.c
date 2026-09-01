#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

#include "jvm.h"
#include "jni_soundfont.h"

#define SOUNDFONT_SAMPLE_RATE 22050
#define SOUNDFONT_CHANNELS 1
/*
 * Emscripten's virtual filesystem makes each fwrite comparatively expensive.
 * A larger block keeps identical PCM output while avoiding hundreds of tiny
 * writes during the synchronous TinySoundFont render.
 */
#define SOUNDFONT_RENDER_BLOCK 8192
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

static int render_messages_to_wave(tml_message *messages, const char *soundfont_path, const char *wave_path) {
    tml_message *message;
    FILE *output = NULL;
    uint64_t current_frame = 0;
    uint64_t target_frame;
    uint64_t end_frame;
    unsigned int last_milliseconds = 0;
    uint32_t data_size = 0;
    int result = -1;
    int channel;

    if (!messages) return -2;

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
    /* MIDP devices are predominantly mono; this cuts browser render work and
       memory by roughly four times without replacing the SoundFont synth. */
    tsf_set_output(soundfont_synth, TSF_MONO, SOUNDFONT_SAMPLE_RATE, -7.0f);
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
    return result;
}

static int soundfont_render_to_wave(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    Instance *midi_data = env->localvar_getRefer(runtime->localvar, 0);
    Instance *soundfont_path_array = env->localvar_getRefer(runtime->localvar, 1);
    Instance *wave_path_array = env->localvar_getRefer(runtime->localvar, 2);
    char soundfont_path[512];
    char wave_path[512];
    tml_message *messages;
    int result = -1;

    (void) clazz;
    if (midi_data && midi_data->arr_body && midi_data->arr_length > 0 &&
        copy_path(soundfont_path_array, soundfont_path, sizeof(soundfont_path)) &&
        copy_path(wave_path_array, wave_path, sizeof(wave_path))) {
        messages = tml_load_memory(midi_data->arr_body, midi_data->arr_length);
        result = render_messages_to_wave(messages, soundfont_path, wave_path);
    }
    env->push_int(runtime->stack, result);
    return 0;
}

static int soundfont_render_file_to_wave(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    Instance *midi_path_array = env->localvar_getRefer(runtime->localvar, 0);
    Instance *soundfont_path_array = env->localvar_getRefer(runtime->localvar, 1);
    Instance *wave_path_array = env->localvar_getRefer(runtime->localvar, 2);
    char midi_path[512];
    char soundfont_path[512];
    char wave_path[512];
    tml_message *messages;
    int result = -1;

    (void) clazz;
    if (copy_path(midi_path_array, midi_path, sizeof(midi_path)) &&
        copy_path(soundfont_path_array, soundfont_path, sizeof(soundfont_path)) &&
        copy_path(wave_path_array, wave_path, sizeof(wave_path))) {
        messages = tml_load_filename(midi_path);
        result = render_messages_to_wave(messages, soundfont_path, wave_path);
    }
    env->push_int(runtime->stack, result);
    return 0;
}

static int browser_audio_create(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    Instance *media = env->localvar_getRefer(runtime->localvar, 0);
    s64 duration_micros = env->localvar_getLong_2slot(runtime->localvar, 1);
    int duration_millis = duration_micros < 0 ? -1 : (int) (duration_micros / 1000);
    int handle = 0;
    (void) clazz;
#ifdef __EMSCRIPTEN__
    if (media && media->arr_body && media->arr_length > 0) {
        handle = MAIN_THREAD_EM_ASM_INT({
            const root = window.__j2meWebAudio || (window.__j2meWebAudio = {});
            if (!root.context) {
                const AudioContextClass = window.AudioContext || window.webkitAudioContext;
                root.context = new AudioContextClass();
                root.items = new Map();
                root.nextId = 1;
                root.stats = new Object();
                root.stats.begins = 0;
                root.stats.closes = 0;
                root.stats.creates = 0;
                root.stats.starts = 0;
                root.stats.stops = 0;
                root.analyser = root.context.createAnalyser();
                root.analyser.fftSize = 2048;
                root.analyser.connect(root.context.destination);
                root.begin = (item) => {
                    if (!item.buffer || !item.playRequested || item.closed) return;
                    root.stats.begins += 1;
                    if (item.source) {
                        item.source.onended = null;
                        try { item.source.stop(); } catch (_) {}
                    }
                    const source = root.context.createBufferSource();
                    const gain = root.context.createGain();
                    source.buffer = item.buffer;
                    source.loop = item.loopCount < 0;
                    gain.gain.value = item.volume;
                    source.connect(gain);
                    gain.connect(root.analyser);
                    item.source = source;
                    item.gain = gain;
                    item.running = true;
                    item.startedAt = root.context.currentTime;
                    source.onended = () => {
                        if (item.source !== source) return;
                        item.source = null;
                        item.running = false;
                        if (item.playRequested && item.remaining > 1) {
                            item.remaining -= 1;
                            item.position = 0;
                            root.begin(item);
                        } else if (!source.loop) {
                            item.playRequested = false;
                            item.position = 0;
                        }
                    };
                    source.start(0, Math.min(item.position, Math.max(0, item.buffer.duration - 0.001)));
                };
                root.stop = (item) => {
                    if (item.source) {
                        item.position += Math.max(0, root.context.currentTime - item.startedAt);
                        item.source.onended = null;
                        try { item.source.stop(); } catch (_) {}
                        item.source = null;
                    }
                    item.running = false;
                };
            }
            const id = root.nextId++;
            const item = new Object();
            item.buffer = null;
            item.closed = false;
            item.duration = $2 / 1000;
            item.gain = null;
            item.loopCount = 1;
            item.playRequested = false;
            item.position = 0;
            item.remaining = 1;
            item.running = false;
            item.source = null;
            item.volume = 1;
            root.items.set(id, item);
            root.stats.creates += 1;
            const encoded = HEAPU8.slice($0, $0 + $1).buffer;
            root.context.decodeAudioData(encoded).then((buffer) => {
                if (item.closed) return;
                item.buffer = buffer;
                item.duration = buffer.duration;
                if (item.playRequested) root.begin(item);
            }).catch((error) => {
                item.playRequested = false;
                console.error("[audio] browser decode failed", error);
            });
            return id;
        }, media->arr_body, media->arr_length, duration_millis);
    }
#endif
    env->push_int(runtime->stack, handle);
    return 0;
}

static int browser_audio_start(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    (void) clazz;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (item && !item.closed) {
            root.stats.starts += 1;
            item.playRequested = true;
            item.remaining = item.loopCount < 0 ? -1 : Math.max(1, item.loopCount || 1);
            root.context.resume();
            if (!item.running) root.begin(item);
        }
    }, handle);
#endif
    return 0;
}

static int browser_audio_stop(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    (void) clazz;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (item) {
            root.stats.stops += 1;
            item.playRequested = false;
            root.stop(item);
        }
    }, handle);
#endif
    return 0;
}

static int browser_audio_close(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    (void) clazz;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (item) {
            root.stats.closes += 1;
            item.playRequested = false;
            item.closed = true;
            root.stop(item);
            root.items.delete($0);
        }
    }, handle);
#endif
    return 0;
}

static int browser_audio_set_loop_count(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    int count = env->localvar_getInt(runtime->localvar, 1);
    (void) clazz;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (item) {
            item.loopCount = $1;
            item.remaining = $1 < 0 ? -1 : Math.max(1, $1 || 1);
            if (item.source) item.source.loop = $1 < 0;
        }
    }, handle, count);
#endif
    return 0;
}

static int browser_audio_set_media_time(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    s64 micros = env->localvar_getLong_2slot(runtime->localvar, 1);
    s64 result = micros;
    (void) clazz;
#ifdef __EMSCRIPTEN__
    int milliseconds = (int) (micros / 1000);
    int applied = MAIN_THREAD_EM_ASM_INT({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (!item) return $1;
        const restart = item.playRequested;
        root.stop(item);
        item.position = Math.max(0, Math.min($1 / 1000, item.duration || $1 / 1000));
        item.playRequested = restart;
        if (restart) root.begin(item);
        return Math.round(item.position * 1000);
    }, handle, milliseconds);
    result = (s64) applied * 1000;
#endif
    env->push_long(runtime->stack, result);
    return 0;
}

static int browser_audio_get_media_time(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    int milliseconds = 0;
    (void) clazz;
#ifdef __EMSCRIPTEN__
    milliseconds = MAIN_THREAD_EM_ASM_INT({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (!item) return 0;
        const elapsed = item.running ? Math.max(0, root.context.currentTime - item.startedAt) : 0;
        return Math.round((item.position + elapsed) * 1000);
    }, handle);
#endif
    env->push_long(runtime->stack, (s64) milliseconds * 1000);
    return 0;
}

static int browser_audio_get_duration(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    int milliseconds = -1;
    (void) clazz;
#ifdef __EMSCRIPTEN__
    milliseconds = MAIN_THREAD_EM_ASM_INT({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        return item && item.duration >= 0 ? Math.round(item.duration * 1000) : -1;
    }, handle);
#endif
    env->push_long(runtime->stack, milliseconds < 0 ? -1 : (s64) milliseconds * 1000);
    return 0;
}

static int browser_audio_is_running(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    int running = 0;
    (void) clazz;
#ifdef __EMSCRIPTEN__
    running = MAIN_THREAD_EM_ASM_INT({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        return item && item.playRequested ? 1 : 0;
    }, handle);
#endif
    env->push_int(runtime->stack, running);
    return 0;
}

static int browser_audio_set_volume(Runtime *runtime, JClass *clazz) {
    JniEnv *env = runtime->jnienv;
    int handle = env->localvar_getInt(runtime->localvar, 0);
    Int2Float volume;
    volume.i = env->localvar_getInt(runtime->localvar, 1);
    (void) clazz;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_EM_ASM({
        const root = window.__j2meWebAudio;
        const item = root && root.items.get($0);
        if (item) {
            item.volume = Math.max(0, Math.min(1, $1));
            if (item.gain) item.gain.gain.value = item.volume;
        }
    }, handle, volume.f);
#endif
    return 0;
}

static java_native_method soundfont_methods[] = {
    {"com/ebsee/emu/audio/SoundFontSynth", "renderToWave", "([B[B[B)I", soundfont_render_to_wave},
    {"com/ebsee/emu/audio/SoundFontSynth", "renderFileToWave", "([B[B[B)I", soundfont_render_file_to_wave},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "create", "([BJ)I", browser_audio_create},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "start", "(I)V", browser_audio_start},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "stop", "(I)V", browser_audio_stop},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "close", "(I)V", browser_audio_close},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "setLoopCount", "(II)V", browser_audio_set_loop_count},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "setMediaTime", "(IJ)J", browser_audio_set_media_time},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "getMediaTime", "(I)J", browser_audio_get_media_time},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "getDuration", "(I)J", browser_audio_get_duration},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "isRunning", "(I)Z", browser_audio_is_running},
    {"com/ebsee/emu/audio/BrowserAudioHandle", "setVolume", "(IF)V", browser_audio_set_volume},
};

s32 count_SoundFontFuncTable(void) {
    return sizeof(soundfont_methods) / sizeof(java_native_method);
}

__refer ptr_SoundFontFuncTable(void) {
    return &soundfont_methods[0];
}
