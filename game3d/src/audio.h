// Synthesised sound: engine, tyres, wind, effects (no audio files)
#ifndef CARPOLO_AUDIO_H
#define CARPOLO_AUDIO_H

#include <stdbool.h>

enum { SFX_CRASH, SFX_HORN, SFX_HONK_OTHER, SFX_BEEP, SFX_GO, SFX_FINISH, SFX_CLICK, SFX_WHOOSH, SFX_SCRAPE, SFX_COUNT };

void AudioInit(void);
void AudioShutdown(void);
void AudioEngine(float rpm, float throttle, float speedMs, bool running);
void AudioTyres(float skid, float offroad);
void AudioPlay(int sfx);
void AudioSetEnabled(bool on);
bool AudioEnabled(void);
void AudioPause(bool paused);

#endif
