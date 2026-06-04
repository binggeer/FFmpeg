#pragma once

#include <cstdint>

struct FfSession;

struct AVFrame;

void FfOrientReset(FfSession& session);
void FfOrientUpdateFromAvFrame(FfSession& session, AVFrame* frame);
void FfOrientUpdateFromBgra(FfSession& session, const uint8_t* bgra, int width, int height, int pitch, int capacity);
int FfOrientIsLandscape(const FfSession& session);
