/* audio_out.h - DirectSound streaming output + FM render thread. */
#ifndef VDDSOUND_AUDIO_OUT_H
#define VDDSOUND_AUDIO_OUT_H

#define AUDIO_SAMPLE_RATE 48000u

/* Starts DirectSound and the render thread. Returns 0 on success; on failure
 * the rest of the VDD still runs (MIDI works without FM output). */
int  audio_init(void);
void audio_shutdown(void);

/* One-shot PCM via a dedicated DirectSound buffer at the SB's native format/rate
 * (DirectSound does the sample-rate conversion + mixing, not us). audio_pcm_play
 * creates+plays the buffer (call from the VDM thread); audio_pcm_done returns 1
 * once (per play) when it has finished, to time the SB completion IRQ. */
int  audio_pcm_play(const void *data, unsigned bytes, unsigned rate,
                    int bits, int channels);
int  audio_pcm_done(void);

#endif /* VDDSOUND_AUDIO_OUT_H */
