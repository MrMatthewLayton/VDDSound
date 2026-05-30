/* audio_out.h - DirectSound streaming output + FM render thread. */
#ifndef VDDSOUND_AUDIO_OUT_H
#define VDDSOUND_AUDIO_OUT_H

#define AUDIO_SAMPLE_RATE 48000u

/* Starts DirectSound and the render thread. Returns 0 on success; on failure
 * the rest of the VDD still runs (MIDI works without FM output). */
int  audio_init(void);
void audio_shutdown(void);

/* Streaming PCM via a looping DirectSound buffer at the SB's native format/rate
 * (DirectSound does the SRC + mixing). We read the guest buffer LIVE just behind
 * the play cursor (like the hardware DMA), feeding small amounts to hold a short
 * lead. open: create+start (silence). lead: unplayed bytes ahead of the play
 * cursor. feed: append n bytes at our write cursor. close: stop+release. */
int      audio_pcm_open(unsigned rate, int bits, int channels);
unsigned audio_pcm_lead(void);
unsigned audio_pcm_feed(const void *data, unsigned n);
void     audio_pcm_close(void);
unsigned audio_pcm_underruns(void);   /* play-cursor-overtook-write count (diag) */

#endif /* VDDSOUND_AUDIO_OUT_H */
