#pragma once

// DivaLinux's FFmpeg movie player: a replacement for diva.exe's MoviePlayLib::Player
// (ff_player.cpp). movie.cpp hooks the game's CreatePlayer and hands these out.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace FFPlayer
{
	constexpr uintptr_t ADDR_CREATE_PLAYER = 0x1404255C0;   // HRESULT CreatePlayer(IPlayer** out)

	// [movie] settings, set before the first player
	extern bool movieAudio;     // movie_audio
	extern bool fastDecode;     // fast_decode
	extern bool gpuConvert;     // gpu_convert
	extern int decodeThreads;   // decode_threads

	// The game's movie player code is the one this replaces (diva.exe 7.10).
	bool IsSupportedExecutable();
	// Loads FFmpeg from plugins\ (once). False if it isn't there or the versions don't match.
	bool EnsureFFmpeg();
	// A new player (the game's IPlayer interface).
	void* NewPlayer();
	// True for players made by NewPlayer.
	bool IsPlayer(const void* object);
}

// movie.cpp: a line to DivaLinux.log (trailing newlines dropped), and the game folder.
void movie_log(const char* format, ...);
std::wstring movie_game_dir();
