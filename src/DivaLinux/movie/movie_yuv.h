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
		bool Convert(GLuint_ target, int w, int h, const Planes& p)
		{
			if (failed || (!loaded && !Load()))
				return false;

			dvk_push_gl_state();
			bool ok = Draw(target, w, h, p);
			GLenum_ e = glGetError_();
			if (ok && e != Y_NO_ERROR)
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "GL error 0x%04X during the conversion", e);
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
			bool ok = Get(glGenTextures_, "glGenTextures") & Get(glBindTexture_, "glBindTexture")
				& Get(glTexImage2D_, "glTexImage2D") & Get(glTexSubImage2D_, "glTexSubImage2D")
				& Get(glTexParameteri_, "glTexParameteri") & Get(glActiveTexture_, "glActiveTexture")
				& Get(glBindSampler_, "glBindSampler") & Get(glPixelStorei_, "glPixelStorei")
				& Get(glBindBuffer_, "glBindBuffer") & Get(glGenFramebuffers_, "glGenFramebuffers")
				& Get(glBindFramebuffer_, "glBindFramebuffer") & Get(glFramebufferTexture2D_, "glFramebufferTexture2D")
				& Get(glCheckFramebufferStatus_, "glCheckFramebufferStatus") & Get(glCreateShader_, "glCreateShader")
				& Get(glShaderSource_, "glShaderSource") & Get(glCompileShader_, "glCompileShader")
				& Get(glGetShaderiv_, "glGetShaderiv") & Get(glGetShaderInfoLog_, "glGetShaderInfoLog")
				& Get(glCreateProgram_, "glCreateProgram") & Get(glAttachShader_, "glAttachShader")
				& Get(glLinkProgram_, "glLinkProgram") & Get(glGetProgramiv_, "glGetProgramiv")
				& Get(glUseProgram_, "glUseProgram") & Get(glBindVertexArray_, "glBindVertexArray")
				& Get(glViewport_, "glViewport") & Get(glDisable_, "glDisable") & Get(glColorMask_, "glColorMask")
				& Get(glDrawArrays_, "glDrawArrays") & Get(glGetError_, "glGetError");
			return ok;
		}

		GLuint_ Shader(GLenum_ type, const std::string& source)
		{
			GLuint_ s = glCreateShader_(type);
			const GLchar_* src = source.c_str();
			glShaderSource_(s, 1, &src, nullptr);
			glCompileShader_(s);
			GLint_ status = 0;
			glGetShaderiv_(s, Y_COMPILE_STATUS, &status);
			if (!status)
			{
				char log[1024] = {};
				glGetShaderInfoLog_(s, sizeof(log) - 1, nullptr, log);
				Fail(std::string("shader compile failed: ") + log);
				return 0;
			}
			return s;
		}

		GLuint_ Program(Matrix matrix, bool fullRange)
		{
			GLuint_& p = programs[matrix == BT709][fullRange ? 1 : 0];
			if (p)
				return p;

			static const char* VS =
				"#version 430 core\n"
				"void main() {\n"
				"    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
				"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
				"}\n";
			std::string fs = "#version 430 core\n";
			fs += matrix == BT709 ? "#define BT709 1\n" : "#define BT709 0\n";
			fs += fullRange ? "#define FULL_RANGE 1\n" : "#define FULL_RANGE 0\n";
			fs +=
				"layout(binding = 0) uniform sampler2D g_y;\n"
				"layout(binding = 1) uniform sampler2D g_u;\n"
				"layout(binding = 2) uniform sampler2D g_v;\n"
				"layout(location = 0) out vec4 result;\n"
				"void main() {\n"
				// Same rows in every image: luma texel for this pixel, chroma by position
				"    float y = texelFetch(g_y, ivec2(gl_FragCoord.xy), 0).r;\n"
				"    vec2 uv = gl_FragCoord.xy / vec2(textureSize(g_y, 0));\n"
				"    float u = texture(g_u, uv).r - 128.0 / 255.0;\n"
				"    float v = texture(g_v, uv).r - 128.0 / 255.0;\n"
				"#if !FULL_RANGE\n"
				"    y = (y - 16.0 / 255.0) * (255.0 / 219.0);\n"
				"    u *= 255.0 / 224.0;\n"
				"    v *= 255.0 / 224.0;\n"
				"#endif\n"
				"#if BT709\n"
				"    vec3 rgb = vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u);\n"
				"#else\n"
				"    vec3 rgb = vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
				"#endif\n"
				"    result = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
				"}\n";

			GLuint_ vs = Shader(Y_VERTEX_SHADER, VS);
			GLuint_ f = vs ? Shader(Y_FRAGMENT_SHADER, fs) : 0;
			if (!f)
				return 0;
			GLuint_ prog = glCreateProgram_();
			glAttachShader_(prog, vs);
			glAttachShader_(prog, f);
			glLinkProgram_(prog);
			GLint_ status = 0;
			glGetProgramiv_(prog, Y_LINK_STATUS, &status);
			if (!status)
			{
				Fail("conversion program didn't link");
				return 0;
			}
			p = prog;
			return p;
		}

		void UploadPlane(int i, const uint8_t* data, int stride, int w, int h)
		{
			glActiveTexture_(Y_TEXTURE0 + i);
			glBindSampler_(i, 0); // the game may have a sampler object on this unit
			if (!planes[i])
				glGenTextures_(1, &planes[i]);
			glBindTexture_(Y_TEXTURE_2D, planes[i]);
			if (planeW[i] != w || planeH[i] != h)
			{
				glTexImage2D_(Y_TEXTURE_2D, 0, Y_R8, w, h, 0, Y_RED, Y_UNSIGNED_BYTE, nullptr);
				glTexParameteri_(Y_TEXTURE_2D, Y_TEXTURE_MAX_LEVEL, 0);
				glTexParameteri_(Y_TEXTURE_2D, Y_TEXTURE_MIN_FILTER, Y_LINEAR);
				glTexParameteri_(Y_TEXTURE_2D, Y_TEXTURE_MAG_FILTER, Y_LINEAR);
				glTexParameteri_(Y_TEXTURE_2D, Y_TEXTURE_WRAP_S, Y_CLAMP_TO_EDGE);
				glTexParameteri_(Y_TEXTURE_2D, Y_TEXTURE_WRAP_T, Y_CLAMP_TO_EDGE);
				planeW[i] = w;
				planeH[i] = h;
			}
			glPixelStorei_(Y_UNPACK_ROW_LENGTH, stride);
			glTexSubImage2D_(Y_TEXTURE_2D, 0, 0, 0, w, h, Y_RED, Y_UNSIGNED_BYTE, data);
		}

		bool Draw(GLuint_ target, int w, int h, const Planes& p)
		{
			GLuint_ prog = Program(p.matrix, p.fullRange);
			if (!prog)
				return false;

			// Everything below is undone by dvk_pop_gl_state.
			glBindBuffer_(Y_PIXEL_UNPACK_BUFFER, 0);
			glPixelStorei_(Y_UNPACK_ALIGNMENT, 1);
			UploadPlane(0, p.data[0], p.stride[0], p.width, p.height);
			UploadPlane(1, p.data[1], p.stride[1], p.chromaWidth, p.chromaHeight);
			UploadPlane(2, p.data[2], p.stride[2], p.chromaWidth, p.chromaHeight);

			if (!fbo)
				glGenFramebuffers_(1, &fbo);
			glBindFramebuffer_(Y_FRAMEBUFFER, fbo);
			// Attached every time: a new movie can get a texture with a freed name.
			glFramebufferTexture2D_(Y_FRAMEBUFFER, Y_COLOR_ATTACHMENT0, Y_TEXTURE_2D, target, 0);
			if (glCheckFramebufferStatus_(Y_FRAMEBUFFER) != Y_FRAMEBUFFER_COMPLETE)
			{
				Fail("the movie texture can't be rendered to");
				return false;
			}

			glDisable_(Y_BLEND);
			glDisable_(Y_CULL_FACE);
			glDisable_(Y_DEPTH_TEST);
			glDisable_(Y_STENCIL_TEST);
			glDisable_(Y_SCISSOR_TEST);
			glColorMask_(1, 1, 1, 1);
			glViewport_(0, 0, w, h);
			glUseProgram_(prog);
			glBindVertexArray_(0);
			glDrawArrays_(Y_TRIANGLES, 0, 3);
			return true;
		}
	};
}
