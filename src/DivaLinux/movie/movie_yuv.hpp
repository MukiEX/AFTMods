#pragma once

// GPU colour conversion for movie frames (DivaLinux).
//
// FFmpeg decodes movies to planar YUV. DivaMovie converted every frame to BGRA on the CPU with
// swscale before uploading it, which on a slow CPU (a Quest or phone running the game through
// FEX, where swscale's x86 SIMD is emulated) costs as much as decoding. PV movies play while
// the GPU has next to nothing else to draw, so here the three planes are uploaded as they are
// (1.5 bytes per pixel instead of 4) and a small shader writes RGB straight into the game's
// movie texture.
//
// The conversion runs on the game's render thread through DivaVK's GL functions, between
// dvk_push_gl_state and dvk_pop_gl_state: every binding and switch it touches is put back
// exactly, so neither the game nor DivaGL's cached copy of the GL state notices.
//
// Platform-independent: DivaVK's Linux tests run it against SwiftShader.

#include <divavk/divavk.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

namespace MovieYuv
{
	typedef unsigned int GLenum_;
	typedef unsigned int GLuint_;
	typedef int GLint_;
	typedef int GLsizei_;
	typedef char GLchar_;

	enum : GLenum_
	{
		Y_TEXTURE_2D = 0x0DE1, Y_RED = 0x1903, Y_R8 = 0x8229, Y_UNSIGNED_BYTE = 0x1401,
		Y_TEXTURE_MIN_FILTER = 0x2801, Y_TEXTURE_MAG_FILTER = 0x2800, Y_LINEAR = 0x2601,
		Y_TEXTURE_WRAP_S = 0x2802, Y_TEXTURE_WRAP_T = 0x2803, Y_CLAMP_TO_EDGE = 0x812F,
		Y_TEXTURE_MAX_LEVEL = 0x813D, Y_TEXTURE0 = 0x84C0,
		Y_UNPACK_ALIGNMENT = 0x0CF5, Y_UNPACK_ROW_LENGTH = 0x0CF2, Y_PIXEL_UNPACK_BUFFER = 0x88EC,
		Y_FRAMEBUFFER = 0x8D40, Y_COLOR_ATTACHMENT0 = 0x8CE0, Y_FRAMEBUFFER_COMPLETE = 0x8CD5,
		Y_VERTEX_SHADER = 0x8B31, Y_FRAGMENT_SHADER = 0x8B30, Y_COMPILE_STATUS = 0x8B81, Y_LINK_STATUS = 0x8B82,
		Y_BLEND = 0x0BE2, Y_CULL_FACE = 0x0B44, Y_DEPTH_TEST = 0x0B71, Y_STENCIL_TEST = 0x0B90,
		Y_SCISSOR_TEST = 0x0C11, Y_TRIANGLES = 0x0004, Y_NO_ERROR = 0,
	};

	enum Matrix { BT601 = 0, BT709 = 1 };

	// One decoded frame: three 8-bit planes (4:2:0, 4:2:2 or 4:4:4).
	struct Planes
	{
		const uint8_t* data[3];
		int stride[3];          // bytes per row
		int width, height;      // luma size
		int chromaWidth, chromaHeight;
		Matrix matrix;
		bool fullRange;         // JPEG range (0-255) instead of video range (16-235)
	};

	class Converter
	{
	public:
		std::string error; // set when the converter gave up (the caller falls back to the CPU)

		bool Failed() const { return failed; }

		// Writes the frame into texture `target` (w x h). False if the GPU path isn't usable;
		// `error` says why, and later calls fail at once.
		bool Convert(GLuint_ target, int w, int h, const Planes& p);

	private:
		typedef void (APIENTRY* P_GenTextures)(GLsizei_, GLuint_*);
		typedef void (APIENTRY* P_BindTexture)(GLenum_, GLuint_);
		typedef void (APIENTRY* P_TexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void*);
		typedef void (APIENTRY* P_TexSubImage2D)(GLenum_, GLint_, GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, const void*);
		typedef void (APIENTRY* P_TexParameteri)(GLenum_, GLenum_, GLint_);
		typedef void (APIENTRY* P_ActiveTexture)(GLenum_);
		typedef void (APIENTRY* P_BindSampler)(GLuint_, GLuint_);
		typedef void (APIENTRY* P_PixelStorei)(GLenum_, GLint_);
		typedef void (APIENTRY* P_BindBuffer)(GLenum_, GLuint_);
		typedef void (APIENTRY* P_GenFramebuffers)(GLsizei_, GLuint_*);
		typedef void (APIENTRY* P_BindFramebuffer)(GLenum_, GLuint_);
		typedef void (APIENTRY* P_FramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, GLint_);
		typedef GLenum_ (APIENTRY* P_CheckFramebufferStatus)(GLenum_);
		typedef GLuint_ (APIENTRY* P_CreateShader)(GLenum_);
		typedef void (APIENTRY* P_ShaderSource)(GLuint_, GLsizei_, const GLchar_* const*, const GLint_*);
		typedef void (APIENTRY* P_CompileShader)(GLuint_);
		typedef void (APIENTRY* P_GetShaderiv)(GLuint_, GLenum_, GLint_*);
		typedef void (APIENTRY* P_GetShaderInfoLog)(GLuint_, GLsizei_, GLsizei_*, GLchar_*);
		typedef GLuint_ (APIENTRY* P_CreateProgram)(void);
		typedef void (APIENTRY* P_AttachShader)(GLuint_, GLuint_);
		typedef void (APIENTRY* P_LinkProgram)(GLuint_);
		typedef void (APIENTRY* P_GetProgramiv)(GLuint_, GLenum_, GLint_*);
		typedef void (APIENTRY* P_UseProgram)(GLuint_);
		typedef void (APIENTRY* P_BindVertexArray)(GLuint_);
		typedef void (APIENTRY* P_Viewport)(GLint_, GLint_, GLsizei_, GLsizei_);
		typedef void (APIENTRY* P_Disable)(GLenum_);
		typedef void (APIENTRY* P_ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
		typedef void (APIENTRY* P_DrawArrays)(GLenum_, GLint_, GLsizei_);
		typedef GLenum_ (APIENTRY* P_GetError)(void);

		P_GenTextures glGenTextures_ = nullptr;
		P_BindTexture glBindTexture_ = nullptr;
		P_TexImage2D glTexImage2D_ = nullptr;
		P_TexSubImage2D glTexSubImage2D_ = nullptr;
		P_TexParameteri glTexParameteri_ = nullptr;
		P_ActiveTexture glActiveTexture_ = nullptr;
		P_BindSampler glBindSampler_ = nullptr;
		P_PixelStorei glPixelStorei_ = nullptr;
		P_BindBuffer glBindBuffer_ = nullptr;
		P_GenFramebuffers glGenFramebuffers_ = nullptr;
		P_BindFramebuffer glBindFramebuffer_ = nullptr;
		P_FramebufferTexture2D glFramebufferTexture2D_ = nullptr;
		P_CheckFramebufferStatus glCheckFramebufferStatus_ = nullptr;
		P_CreateShader glCreateShader_ = nullptr;
		P_ShaderSource glShaderSource_ = nullptr;
		P_CompileShader glCompileShader_ = nullptr;
		P_GetShaderiv glGetShaderiv_ = nullptr;
		P_GetShaderInfoLog glGetShaderInfoLog_ = nullptr;
		P_CreateProgram glCreateProgram_ = nullptr;
		P_AttachShader glAttachShader_ = nullptr;
		P_LinkProgram glLinkProgram_ = nullptr;
		P_GetProgramiv glGetProgramiv_ = nullptr;
		P_UseProgram glUseProgram_ = nullptr;
		P_BindVertexArray glBindVertexArray_ = nullptr;
		P_Viewport glViewport_ = nullptr;
		P_Disable glDisable_ = nullptr;
		P_ColorMask glColorMask_ = nullptr;
		P_DrawArrays glDrawArrays_ = nullptr;
		P_GetError glGetError_ = nullptr;

		bool loaded = false, failed = false;
		GLuint_ programs[2][2] = {};   // [matrix][fullRange], compiled on first use
		GLuint_ planes[3] = {};
		int planeW[3] = {}, planeH[3] = {};
		GLuint_ fbo = 0;

		void Fail(const std::string& why);

		template <typename T> bool Get(T& fn, const char* name)
		{
			fn = (T)dvk_get_implemented_proc(name);
			if (!fn)
				Fail(std::string(name) + " is not available");
			return fn != nullptr;
		}

		bool Load();

		GLuint_ Shader(GLenum_ type, const std::string& source);

		GLuint_ Program(Matrix matrix, bool fullRange);

		void UploadPlane(int i, const uint8_t* data, int stride, int w, int h);

		bool Draw(GLuint_ target, int w, int h, const Planes& p);
	};
}
