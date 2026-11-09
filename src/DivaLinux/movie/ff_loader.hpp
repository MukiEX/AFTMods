#pragma once

// Runtime loading of the FFmpeg shared libraries (no import libraries needed).
// The struct layouts used elsewhere come from the headers this is compiled against,
// so the library major versions must match; Load() checks that.

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE FFLibHandle;
#else
#include <dlfcn.h>
typedef void* FFLibHandle;
#endif

#define FF_FUNCTIONS(X) \
	X(avutil, avutil_version) \
	X(avutil, av_frame_alloc) \
	X(avutil, av_frame_free) \
	X(avutil, av_frame_unref) \
	X(avutil, av_frame_move_ref) \
	X(avutil, av_malloc) \
	X(avutil, av_freep) \
	X(avutil, av_strerror) \
	X(avutil, av_log_set_level) \
	X(avutil, av_opt_set) \
	X(avutil, av_channel_layout_default) \
	X(avutil, av_channel_layout_uninit) \
	X(avutil, av_samples_get_buffer_size) \
	X(avutil, av_get_pix_fmt_name) \
	X(swresample, swresample_version) \
	X(swresample, swr_alloc_set_opts2) \
	X(swresample, swr_init) \
	X(swresample, swr_convert) \
	X(swresample, swr_free) \
	X(swresample, swr_get_out_samples) \
	X(swscale, swscale_version) \
	X(swscale, sws_getCachedContext) \
	X(swscale, sws_scale) \
	X(swscale, sws_freeContext) \
	X(swscale, sws_setColorspaceDetails) \
	X(swscale, sws_getCoefficients) \
	X(avcodec, avcodec_version) \
	X(avcodec, avcodec_alloc_context3) \
	X(avcodec, avcodec_parameters_to_context) \
	X(avcodec, avcodec_open2) \
	X(avcodec, avcodec_send_packet) \
	X(avcodec, avcodec_receive_frame) \
	X(avcodec, avcodec_free_context) \
	X(avcodec, av_packet_alloc) \
	X(avcodec, av_packet_free) \
	X(avcodec, av_packet_unref) \
	X(avcodec, av_packet_move_ref) \
	X(avcodec, avcodec_flush_buffers) \
	X(avformat, avformat_version) \
	X(avformat, avformat_open_input) \
	X(avformat, avformat_find_stream_info) \
	X(avformat, av_find_best_stream) \
	X(avformat, av_read_frame) \
	X(avformat, avformat_close_input) \
	X(avformat, avformat_alloc_context) \
	X(avformat, avio_alloc_context) \
	X(avformat, avio_context_free)

struct FFLib
{
#define FF_DECLARE(lib, name) decltype(&::name) name = nullptr;
	FF_FUNCTIONS(FF_DECLARE)
#undef FF_DECLARE

	bool loaded = false;
	std::string error;

	// Order matters: each library's dependencies are loaded first.
	enum { AVUTIL, SWRESAMPLE, SWSCALE, AVCODEC, AVFORMAT, COUNT };
	FFLibHandle handles[COUNT] = {};

	static const char* LibName(int i);

	static int ExpectedMajor(int i);

	FFLibHandle Handle(const char* lib);

	static void* Symbol(FFLibHandle h, const char* name);

	// directory: folder containing the libraries, with a trailing separator.
	bool Load(const std::wstring& directory);

	void Unload();

	std::string ErrorString(int code);
};
