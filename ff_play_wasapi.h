#pragma once

struct WasapiPlayback;

bool FfPlayWasapiOpen(WasapiPlayback** out_state, int* out_sample_rate, int* out_channels);
void FfPlayWasapiClose(WasapiPlayback* state);
void FfPlayWasapiReset(WasapiPlayback* state);
void FfPlayWasapiPause(WasapiPlayback* state);
void FfPlayWasapiResume(WasapiPlayback* state);
bool FfPlayWasapiWritePcm(WasapiPlayback* state, const uint8_t* pcm, int num_frames, int channels);
bool FfPlayWasapiSetVolume(WasapiPlayback* state, int volume_percent);
