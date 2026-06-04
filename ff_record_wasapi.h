#pragma once

struct FfSession;

bool FfRecordWasapiOpen(FfSession& session, int& out_sample_rate, int& out_channels);
void FfRecordWasapiClose(FfSession& session);
bool FfRecordWasapiGetEncodeFormat(const FfSession& session, int& out_sample_rate, int& out_channels);
void FfRecordWasapiFlushRemaining(FfSession& session);
bool FfRecordWasapiCaptureForVideoFrame(FfSession& session);
