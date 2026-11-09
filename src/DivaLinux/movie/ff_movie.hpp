#pragma once

// Platform-independent movie decoding core: demux + decode with FFmpeg on a worker
// thread, converting video to BGRA into a small queue that the renderer picks frames
// from by presentation time. Audio (optional) is resampled to 16-bit stereo and
// handed to an AudioSink.

#include "ff_loader.hpp"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>

// ---- Minimal threading shim ----

#ifdef _WIN32
struct FFMutex
{
	SRWLOCK lock = SRWLOCK_INIT;
	void Lock() { AcquireSRWLockExclusive(&lock); }
	void Unlock() { ReleaseSRWLockExclusive(&lock); }
};
struct FFCond
{
	CONDITION_VARIABLE cond = CONDITION_VARIABLE_INIT;
	void Wait(FFMutex& m) { SleepConditionVariableSRW(&cond, &m.lock, INFINITE, 0); }
	void WaitFor(FFMutex& m, int ms) { SleepConditionVariableSRW(&cond, &m.lock, ms, 0); }
	void WakeAll() { WakeAllConditionVariable(&cond); }
};
struct FFThread
{
	HANDLE handle = NULL;
	void (*entry)(void*) = nullptr;
	void* argument = nullptr;
	bool Start(void (*function)(void*), void* object)
	{
		entry = function;
		argument = object;
		handle = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { FFThread* t = (FFThread*)p; t->entry(t->argument); return 0; }, this, 0, NULL);
		return handle != NULL;
	}
	void Join()
	{
		if (handle)
		{
			WaitForSingleObject(handle, INFINITE);
			CloseHandle(handle);
			handle = NULL;
		}
	}
};
inline double FFNow()
{
	static LARGE_INTEGER frequency = {};
	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)frequency.QuadPart;
}
#else
#include <pthread.h>
#include <time.h>
struct FFMutex
{
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	void Lock() { pthread_mutex_lock(&lock); }
	void Unlock() { pthread_mutex_unlock(&lock); }
};
struct FFCond
{
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	void Wait(FFMutex& m) { pthread_cond_wait(&cond, &m.lock); }
	void WaitFor(FFMutex& m, int ms)
	{
		timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)ms * 1000000;
		ts.tv_sec += ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
		pthread_cond_timedwait(&cond, &m.lock, &ts);
	}
	void WakeAll() { pthread_cond_broadcast(&cond); }
};
struct FFThread
{
	pthread_t thread;
	bool started = false;
	void (*entry)(void*) = nullptr;
	void* argument = nullptr;
	bool Start(void (*function)(void*), void* object)
	{
		entry = function;
		argument = object;
		started = pthread_create(&thread, NULL, [](void* p) -> void* { FFThread* t = (FFThread*)p; t->entry(t->argument); return nullptr; }, this) == 0;
		return started;
	}
	void Join()
	{
		if (started)
		{
			pthread_join(thread, NULL);
			started = false;
		}
	}
};
inline double FFNow()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

// Milliseconds on a monotonic clock, for the playback statistics.
static inline double FFNowMs()
{
#ifdef _WIN32
	static LARGE_INTEGER frequency = {};
	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart * 1000.0 / frequency.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

struct FFLockGuard
{
	FFMutex& m;
	FFLockGuard(FFMutex& m) : m(m) { m.Lock(); }
	~FFLockGuard() { m.Unlock(); }
};

// ---- Audio output interface ----

struct AudioSink
{
	static const int SAMPLE_RATE = 48000;
	static const int CHANNELS = 2;
	// Interleaved signed 16-bit stereo at SAMPLE_RATE. May block briefly while playing and full.
	virtual void Write(const int16_t* samples, int frames) = 0;
	// Audio accepted but not yet played, in seconds.
	virtual double BufferedSeconds() = 0;
	// No more audio will be written.
	virtual void EndOfStream() {}
	virtual ~AudioSink() {}
};

// ---- Decoder ----

// File access for the decoder, when the movie isn't a plain file FFmpeg can open itself
// (DivaLinux: a member of rom.zip or an mdata pack zip). FFmpeg reads through these instead.
struct FFFileIO
{
	void* (*open)(const char* utf8Path);             // nullptr when it can't be opened
	int (*read)(void* file, uint8_t* buffer, int size); // bytes read, 0 at the end, < 0 on error
	int64_t (*seek)(void* file, int64_t offset, int whence); // SEEK_SET/CUR/END; new position or < 0
	int64_t (*size)(void* file);                      // < 0 if unknown
	void (*close)(void* file);
};

class MovieDecoder
{
public:
	struct Frame
	{
		uint8_t* pixels = nullptr; // BGRA, stride = width * 4 (CPU conversion)
		AVFrame* yuv = nullptr;    // the decoded planes, when the GPU converts (isYuv)
		bool isYuv = false;
		bool bt709 = false;        // colour matrix and range of the YUV planes
		bool fullRange = false;
		int chromaWidth = 0, chromaHeight = 0;
		double pts = 0.0;          // seconds from the start of the movie
		double duration = 0.0;
	};

	// Video properties, valid after Open() succeeds
	int width = 0, height = 0;
	double fps = 0.0;
	double pixelAspect = 1.0;
	double duration = 0.0;      // seconds, 0 if unknown
	bool hasAudio = false;
	std::string error;

	// Decoder options, set before Open()
	bool fastDecode = false;    // no H.264/HEVC loop filter and FFmpeg's "fast" shortcuts, from the start
	volatile bool gpuConvert = false; // queue 8-bit planar YUV frames as they are, for the GPU to convert
	const FFFileIO* fileIO = nullptr; // read the movie through these instead of letting FFmpeg open it
	int decodeThreads = 0;      // 0 = FFmpeg picks (one per CPU)

	// Statistics
	volatile long framesShown = 0, framesSkipped = 0, keyframeJumps = 0;
	std::string videoInfo;      // codec, pixel format, threads (after Open)
	long framesDecoded = 0;     // video thread
	long framesConverted = 0;   // video thread
	long framesGpu = 0;         // video thread: frames queued as YUV for the GPU
	double decodeMs = 0.0;      // video thread time in send_packet/receive_frame
	double convertMs = 0.0;     // video thread time in sws_scale (YUV -> BGRA)
	double maxBehind = 0.0;     // worst lag of the newest decoded frame behind the clock, seconds
	double reducedSeconds = 0.0; // time spent with skip_frame/skip_loop_filter turned on by CatchUp
	double playStartMs = 0.0;   // when the consumer first asked for a frame

	// One line with everything above, for the log.
	std::string StatsLine();

	MovieDecoder(FFLib& ff, AudioSink* audio) : ff(ff), audioSink(audio) {}

	~MovieDecoder();

	// Opens the file and decodes the first video frame (synchronous).
	bool Open(const char* utf8Path);

	// Starts the demux/audio thread and the video decoding thread.
	void Start();

	void Stop();

	// Picks the frame to show at time t (seconds). Returns the frame and whether it
	// differs from the one returned last time. Only call from one consumer thread.
	const Frame* FrameFor(double t, bool* changed);

	// True once the last frame has been shown for its full duration.
	bool Finished(double t);

	double EndTime();

	// Reads the file and decodes audio. Audio is cheap, so it always stays ahead
	// regardless of how long video frames take to decode.
	void DemuxMain();

	// Decodes video packets and converts frames into the queue.
	void VideoMain();

private:
	static const int CAPACITY = 6;              // decoded video frames
	static const int FIFO_MAX = 2048;           // compressed video packets waiting for the decoder
	static const int MIN_VIDEO_PACKETS = 8;     // keep at least this many queued for the video thread
	static constexpr double AUDIO_AHEAD = 1.0;  // seconds of audio to keep buffered

	FFLib& ff;
	AudioSink* audioSink;

	// Custom I/O (fileIO): the open file and FFmpeg's context reading it
	void* ioFile = nullptr;
	AVIOContext* io = nullptr;

	static int IORead(void* opaque, uint8_t* buffer, int size);

	static int64_t IOSeek(void* opaque, int64_t offset, int whence);

	void CloseCustomIO();

	AVFormatContext* format = nullptr;
	AVCodecContext* videoCodec = nullptr;
	AVCodecContext* audioCodec = nullptr;
	AVFrame* frame = nullptr;       // video thread
	AVFrame* audioFrame = nullptr;  // demux thread
	AVPacket* packet = nullptr;     // demux thread
	SwsContext* sws = nullptr;
	SwrContext* swr = nullptr;
	int16_t* audioBuffer = nullptr;
	int audioBufferFrames = 0;

	int videoIndex = -1, audioIndex = -1;
	double timeBase = 0.0, startTime = -1.0;
	double lastPts = -1.0, lastDuration = 0.0;

	// Demuxed video packets not yet given to the video decoder (guarded by mutex)
	AVPacket* fifo[FIFO_MAX] = {};
	int fifoHead = 0, fifoCount = 0;
	bool demuxDone = false;
	bool flushSent = false;
	bool videoFlushed = false;

	FFMutex mutex;
	FFCond cond;
	FFThread demuxThread, videoThread;
	AVPacket* pendingVideo = nullptr; // packet the video decoder refused (video thread only)
	bool reducedQuality = false;      // video thread only
	double reducedSinceMs = 0.0;      // video thread only
	bool stopping = false;
	bool eof = false;

	Frame queue[CAPACITY];
	int head = 0, count = 0;
	const Frame* lastShown = nullptr;
	bool shownOnce = false;
	bool consumerStarted = false;
	double requestedTime = 0.0;

	bool Fail(const char* what, int code);

	bool OpenCodec(const AVCodec* decoder, int index, AVCodecContext** out);

	void PopPacket();

	bool QueueFull();

	// Read further while audio needs data or the video decoder is short of packets.
	bool CanDemux();

	// Demux thread: reads one packet. Returns false when it chose not to read.
	bool DemuxOnce();

	double PacketSeconds(const AVPacket* p);

	// When decoding cannot keep up with playback: first make the decoder cheaper,
	// and if that is not enough, jump to the next keyframe.
	void CatchUp();

	// Video thread: feeds packets to the decoder and collects frames. Returns false when idle.
	bool VideoOnce();

	// Returns true if any frame was received.
	bool ReceiveVideo();

	void QueueVideoFrame();

	void ReceiveAudio();

	void WriteAudioFrame();
};
