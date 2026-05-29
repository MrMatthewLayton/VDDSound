/* audio_out.h - DirectSound streaming output + FM render thread. */
#ifndef VDDSOUND_AUDIO_OUT_H
#define VDDSOUND_AUDIO_OUT_H

#define AUDIO_SAMPLE_RATE 48000u

/* Starts DirectSound and the render thread. Returns 0 on success; on failure
 * the rest of the VDD still runs (MIDI works without FM output). */
int  audio_init(void);
void audio_shutdown(void);

#endif /* VDDSOUND_AUDIO_OUT_H */
