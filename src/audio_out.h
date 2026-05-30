/* audio_out.h - DirectSound streaming output + FM render thread. */
#ifndef VDDSOUND_AUDIO_OUT_H
#define VDDSOUND_AUDIO_OUT_H

#define AUDIO_SAMPLE_RATE 48000u

/* Starts DirectSound and the render thread. Returns 0 on success; on failure
 * the rest of the VDD still runs (MIDI works without FM output). */
int  audio_init(void);
void audio_shutdown(void);

/* Streaming PCM via a big (~1.5 s) looping DirectSound buffer at the SB's native
 * format/rate (DirectSound does the SRC + mixing). VDMSound-style: the consumer
 * meters guest bytes off the wall clock and pushes them with audio_pcm_play,
 * which keeps the ring ~125-250 ms full and returns a load factor (<1 underfull,
 * >1 overfull, 1 on target) for closed-loop rate control. open: create+start
 * (silence). play: write n bytes, return load. close: stop+release. */
int      audio_pcm_open(unsigned rate, int bits, int channels);
unsigned audio_pcm_play(const void *data, unsigned n);   /* returns load (8.8 fp, 256=on target) */
void     audio_pcm_close(void);

#endif /* VDDSOUND_AUDIO_OUT_H */
