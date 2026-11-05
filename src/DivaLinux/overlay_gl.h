#pragma once

// Draws an overlay image (the launcher's in-game menu) over the finished frame (DivaLinux).
//
// The launcher renders its UI on the CPU into a BGRA image with alpha; this uploads it and
// blends it onto the default framebuffer just before the frame is presented, once per
// rectangle given (two in side-by-side stereo, one per eye, so the menu sits on the screen
// plane in both).
//
// Like movie_yuv.h, it runs on the game's render thread through DivaVK's GL functions between
// dvk_push_gl_state and dvk_pop_gl_state, so the game and DivaGL's cached GL state never see
// it. Platform-independent: DivaVK's Linux tests run it against SwiftShader.

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

namespace OverlayGL
{
	typedef unsigned int GLenum_;
	typedef unsigned int GLuint_;
	typedef int GLint_;
	typedef int GLsizei_;
	typedef char GLchar_;

	enum : GLenum_
	{
		O_TEXTURE_2D = 0x0DE1, O_RGBA8 = 0x8058, O_BGRA = 0x80E1, O_UNSIGNED_BYTE = 0x1401,
		O_TEXTURE_MIN_FILTER = 0x2801, O_TEXTURE_MAG_FILTER = 0x2800, O_LINEAR = 0x2601,
		O_TEXTURE_WRAP_S = 0x2802, O_TEXTURE_WRAP_T = 0x2803, O_CLAMP_TO_EDGE = 0x812F,
		O_TEXTURE_MAX_LEVEL = 0x813D, O_TEXTURE0 = 0x84C0,
		O_UNPACK_ALIGNMENT = 0x0CF5, O_UNPACK_ROW_LENGTH = 0x0CF2, O_PIXEL_UNPACK_BUFFER = 0x88EC,
		O_DRAW_FRAMEBUFFER = 0x8CA9,
		O_VERTEX_SHADER = 0x8B31, O_FRAGMENT_SHADER = 0x8B30, O_COMPILE_STATUS = 0x8B81, O_LINK_STATUS = 0x8B82,
		O_BLEND = 0x0BE2, O_CULL_FACE = 0x0B44, O_DEPTH_TEST = 0x0B71, O_STENCIL_TEST = 0x0B90,
		O_SCISSOR_TEST = 0x0C11, O_TRIANGLES = 0x0004, O_NO_ERROR = 0,
		O_FUNC_ADD = 0x8006, O_SRC_ALPHA = 0x0302, O_ONE_MINUS_SRC_ALPHA = 0x0303, O_ONE = 1,
	};

	// One overlay image: 0xAARRGGBB words (B, G, R, A bytes in memory), top row first, not
	// premultiplied.
	struct Image
	{
		const uint32_t* pixels;
		int width, height;
		int stride;             // in pixels
		uint32_t serial;        // changes whenever the pixels do; same serial = reuse the upload
	};

	// Where to draw it, in default-framebuffer pixels with GL's origin (bottom left).
	struct Rect
	{
		int x, y, w, h;
	};

	class Compositor
	{
	public:
		std::string error; // set when the compositor gave up; later calls fail at once

		bool Failed() const { return failed; }

		bool Draw(const Image& img, const Rect* rects, int rectCount)
		{
			if (failed || (!loaded && !Load()))
				return false;
			if (!img.pixels || img.width <= 0 || img.height <= 0 || rectCount <= 0)
				return true;

			dvk_push_gl_state();
			bool ok = DrawImpl(img, rects, rectCount);
			GLenum_ e = glGetError_();
			if (ok && e != O_NO_ERROR)
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "GL error 0x%04X while drawing the overlay", e);
				Fail(buf);
				ok = false;
			}
			dvk_pop_gl_state(); // also restores the game's own error flag
			return ok;
		}

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
		typedef void (APIENTRY* P_BindFramebuffer)(GLenum_, GLuint_);
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
		typedef void (APIENTRY* P_Enable)(GLenum_);
		typedef void (APIENTRY* P_Disable)(GLenum_);
		typedef void (APIENTRY* P_BlendFuncSeparate)(GLenum_, GLenum_, GLenum_, GLenum_);
		typedef void (APIENTRY* P_BlendEquation)(GLenum_);
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
		P_BindFramebuffer glBindFramebuffer_ = nullptr;
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
		P_Enable glEnable_ = nullptr;
		P_Disable glDisable_ = nullptr;
		P_BlendFuncSeparate glBlendFuncSeparate_ = nullptr;
		P_BlendEquation glBlendEquation_ = nullptr;
		P_ColorMask glColorMask_ = nullptr;
		P_DrawArrays glDrawArrays_ = nullptr;
		P_GetError glGetError_ = nullptr;

		bool loaded = false, failed = false;
		GLuint_ program = 0, texture = 0;
		int texW = 0, texH = 0;
		uint32_t uploadedSerial = 0;
		bool uploaded = false;

		void Fail(const std::string& why)
		{
			failed = true;
			if (error.empty())
				error = why;
		}

		template <typename T> bool Get(T& fn, const char* name)
		{
			fn = (T)dvk_get_implemented_proc(name);
			if (!fn)
				Fail(std::string(name) + " is not available");
			return fn != nullptr;
		}

		bool Load()
		{
			loaded = true;
			return Get(glGenTextures_, "glGenTextures") & Get(glBindTexture_, "glBindTexture")
				& Get(glTexImage2D_, "glTexImage2D") & Get(glTexSubImage2D_, "glTexSubImage2D")
				& Get(glTexParameteri_, "glTexParameteri") & Get(glActiveTexture_, "glActiveTexture")
				& Get(glBindSampler_, "glBindSampler") & Get(glPixelStorei_, "glPixelStorei")
				& Get(glBindBuffer_, "glBindBuffer") & Get(glBindFramebuffer_, "glBindFramebuffer")
				& Get(glCreateShader_, "glCreateShader") & Get(glShaderSource_, "glShaderSource")
				& Get(glCompileShader_, "glCompileShader") & Get(glGetShaderiv_, "glGetShaderiv")
				& Get(glGetShaderInfoLog_, "glGetShaderInfoLog") & Get(glCreateProgram_, "glCreateProgram")
				& Get(glAttachShader_, "glAttachShader") & Get(glLinkProgram_, "glLinkProgram")
				& Get(glGetProgramiv_, "glGetProgramiv") & Get(glUseProgram_, "glUseProgram")
				& Get(glBindVertexArray_, "glBindVertexArray") & Get(glViewport_, "glViewport")
				& Get(glEnable_, "glEnable") & Get(glDisable_, "glDisable")
				& Get(glBlendFuncSeparate_, "glBlendFuncSeparate") & Get(glBlendEquation_, "glBlendEquation")
				& Get(glColorMask_, "glColorMask") & Get(glDrawArrays_, "glDrawArrays")
				& Get(glGetError_, "glGetError");
		}

		GLuint_ Shader(GLenum_ type, const char* source)
		{
			GLuint_ s = glCreateShader_(type);
			glShaderSource_(s, 1, &source, nullptr);
			glCompileShader_(s);
			GLint_ status = 0;
			glGetShaderiv_(s, O_COMPILE_STATUS, &status);
			if (!status)
			{
				char log[1024] = {};
				glGetShaderInfoLog_(s, sizeof(log) - 1, nullptr, log);
				Fail(std::string("overlay shader compile failed: ") + log);
				return 0;
			}
			return s;
		}

		bool Program()
		{
			if (program)
				return true;
			// A full-viewport triangle; the image's top row goes to the top of the viewport
			// (GL's rows run bottom-up, the image's top-down).
			static const char* VS =
				"#version 430 core\n"
				"layout(location = 0) out vec2 v_uv;\n"
				"void main() {\n"
				"    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
				"    v_uv = vec2(p.x, 1.0 - p.y);\n"
				"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
				"}\n";
			static const char* FS =
				"#version 430 core\n"
				"layout(binding = 0) uniform sampler2D g_overlay;\n"
				"layout(location = 0) in vec2 v_uv;\n"
				"layout(location = 0) out vec4 result;\n"
				"void main() {\n"
				"    result = texture(g_overlay, v_uv);\n"
				"}\n";
			GLuint_ vs = Shader(O_VERTEX_SHADER, VS);
			GLuint_ fs = vs ? Shader(O_FRAGMENT_SHADER, FS) : 0;
			if (!fs)
				return false;
			GLuint_ prog = glCreateProgram_();
			glAttachShader_(prog, vs);
			glAttachShader_(prog, fs);
			glLinkProgram_(prog);
			GLint_ status = 0;
			glGetProgramiv_(prog, O_LINK_STATUS, &status);
			if (!status)
			{
				Fail("overlay program link failed");
				return false;
			}
			program = prog;
			return true;
		}

		bool DrawImpl(const Image& img, const Rect* rects, int rectCount)
		{
			if (!Program())
				return false;

			// Everything below is undone by dvk_pop_gl_state.
			glActiveTexture_(O_TEXTURE0);
			glBindSampler_(0, 0);
			if (!texture)
				glGenTextures_(1, &texture);
			glBindTexture_(O_TEXTURE_2D, texture);
			if (texW != img.width || texH != img.height)
			{
				glTexImage2D_(O_TEXTURE_2D, 0, (GLint_)O_RGBA8, img.width, img.height, 0, O_BGRA, O_UNSIGNED_BYTE, nullptr);
				glTexParameteri_(O_TEXTURE_2D, O_TEXTURE_MIN_FILTER, O_LINEAR);
				glTexParameteri_(O_TEXTURE_2D, O_TEXTURE_MAG_FILTER, O_LINEAR);
				glTexParameteri_(O_TEXTURE_2D, O_TEXTURE_WRAP_S, O_CLAMP_TO_EDGE);
				glTexParameteri_(O_TEXTURE_2D, O_TEXTURE_WRAP_T, O_CLAMP_TO_EDGE);
				glTexParameteri_(O_TEXTURE_2D, O_TEXTURE_MAX_LEVEL, 0);
				texW = img.width;
				texH = img.height;
				uploaded = false;
			}
			if (!uploaded || img.serial != uploadedSerial)
			{
				glBindBuffer_(O_PIXEL_UNPACK_BUFFER, 0);
				glPixelStorei_(O_UNPACK_ALIGNMENT, 4);
				glPixelStorei_(O_UNPACK_ROW_LENGTH, img.stride != img.width ? img.stride : 0);
				glTexSubImage2D_(O_TEXTURE_2D, 0, 0, 0, img.width, img.height, O_BGRA, O_UNSIGNED_BYTE, img.pixels);
				uploaded = true;
				uploadedSerial = img.serial;
			}

			glBindFramebuffer_(O_DRAW_FRAMEBUFFER, 0);
			glEnable_(O_BLEND);
			glBlendEquation_(O_FUNC_ADD);
			glBlendFuncSeparate_(O_SRC_ALPHA, O_ONE_MINUS_SRC_ALPHA, O_ONE, O_ONE_MINUS_SRC_ALPHA);
			glDisable_(O_CULL_FACE);
			glDisable_(O_DEPTH_TEST);
			glDisable_(O_STENCIL_TEST);
			glDisable_(O_SCISSOR_TEST);
			glColorMask_(1, 1, 1, 1);
			glUseProgram_(program);
			glBindVertexArray_(0);
			for (int i = 0; i < rectCount; i++)
			{
				if (rects[i].w <= 0 || rects[i].h <= 0)
					continue;
				glViewport_(rects[i].x, rects[i].y, rects[i].w, rects[i].h);
				glDrawArrays_(O_TRIANGLES, 0, 3);
			}
			return true;
		}
	};
}
