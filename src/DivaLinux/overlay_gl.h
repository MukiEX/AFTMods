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
#include <stddef.h>
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
		O_UNIFORM_BUFFER = 0x8A11, O_DYNAMIC_DRAW = 0x88E8,
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

	enum PointerKind { POINTER_CROSSHAIR = 0, POINTER_LEEK = 1 };

	// The mouse pointer (a leek or a crosshair), drawn procedurally (no texture). Positions and sizes are in the
	// picture's own pixels ("layout" pixels, top-down), the same space the menu is laid out in;
	// each rect maps that space onto the framebuffer.
	struct Crosshair
	{
		float layWidth, layHeight;  // the picture's size
		float x, y;                 // the hotspot: crosshair centre, leek's leaf tip
		int kind;                   // PointerKind
		float radius;               // crosshair: ring radius; leek: length from tip to bulb
		float alpha;                // overall opacity (fades out when the mouse is idle)
		float glow;                 // strength of the soft halo (pulses)
		float ripple;               // 0..1 progress of a click ripple, < 0 for none
		float r, g, b;              // colour
	};

	// The crosshair block the shader reads (std140; binding 7). DivaGL's own blocks use lower
	// bindings, and dvk_push/pop_gl_state put every binding back anyway.
	static const GLuint_ CROSSHAIR_UBO_BINDING = 7;

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

		bool DrawCrosshair(const Crosshair& c, const Rect* rects, int rectCount)
		{
			if (failed || (!loaded && !Load()))
				return false;
			if (c.alpha <= 0.003f || rectCount <= 0 || c.layWidth <= 0 || c.layHeight <= 0)
				return true;

			dvk_push_gl_state();
			bool ok = CrosshairImpl(c, rects, rectCount);
			GLenum_ e = glGetError_();
			if (ok && e != O_NO_ERROR)
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "GL error 0x%04X while drawing the crosshair", e);
				Fail(buf);
				ok = false;
			}
			dvk_pop_gl_state();
			return ok;
		}

	private:
		typedef void (APIENTRY* P_GenBuffers)(GLsizei_, GLuint_*);
		typedef void (APIENTRY* P_BufferData)(GLenum_, ptrdiff_t, const void*, GLenum_);
		typedef void (APIENTRY* P_BindBufferBase)(GLenum_, GLuint_, GLuint_);
		typedef void (APIENTRY* P_Scissor)(GLint_, GLint_, GLsizei_, GLsizei_);
		P_GenBuffers glGenBuffers_ = nullptr;
		P_BufferData glBufferData_ = nullptr;
		P_BindBufferBase glBindBufferBase_ = nullptr;
		P_Scissor glScissor_ = nullptr;
		GLuint_ crossProgram = 0, crossUbo = 0;

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
				& Get(glGetError_, "glGetError") & Get(glGenBuffers_, "glGenBuffers")
				& Get(glBufferData_, "glBufferData") & Get(glBindBufferBase_, "glBindBufferBase")
				& Get(glScissor_, "glScissor");
		}

		GLuint_ Link(const char* vsSrc, const char* fsSrc, const char* what)
		{
			GLuint_ vs = Shader(O_VERTEX_SHADER, vsSrc);
			GLuint_ fs = vs ? Shader(O_FRAGMENT_SHADER, fsSrc) : 0;
			if (!fs)
				return 0;
			GLuint_ prog = glCreateProgram_();
			glAttachShader_(prog, vs);
			glAttachShader_(prog, fs);
			glLinkProgram_(prog);
			GLint_ status = 0;
			glGetProgramiv_(prog, O_LINK_STATUS, &status);
			if (!status)
			{
				Fail(std::string(what) + " program link failed");
				return 0;
			}
			return prog;
		}

		bool CrosshairProgram()
		{
			if (crossProgram)
				return true;
			static const char* VS =
				"#version 430 core\n"
				"void main() {\n"
				"    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
				"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
				"}\n";
			// Signed distances in picture pixels, antialiased over one framebuffer pixel, with a
			// dark edge so the pointer reads on bright and dark scenes alike, a soft halo that
			// pulses, and a ring that expands from the hotspot after a click.
			//   Crosshair: a ring, four ticks pointing at the centre and a dot.
			//   Leek: the hotspot is the tip of the left leaf; the stalk runs down to the right
			//   like an arrow pointer's tail, white at the bulb, pale green up to the leaves.
			static const char* FS =
				"#version 430 core\n"
				"layout(std140, binding = 7) uniform CrosshairBlock {\n"
				"    vec4 rect0; vec4 rect1; vec4 lay; vec4 style; vec4 color; vec4 extra;\n"
				"} u;\n"
				"layout(location = 0) out vec4 result;\n"
				"float box(vec2 p, vec2 b) { vec2 d = abs(p) - b; return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0); }\n"
				"vec4 over(vec4 s, vec4 d) {\n"
				"    float a = s.a + d.a * (1.0 - s.a);\n"
				"    return vec4((s.rgb * s.a + d.rgb * d.a * (1.0 - s.a)) / max(a, 1e-5), a);\n"
				"}\n"
				// Tapered capsule from a (radius r1) to b (radius r2); h = position along it
				"float taper(vec2 p, vec2 a, vec2 b, float r1, float r2, out float h) {\n"
				"    vec2 pa = p - a, ba = b - a;\n"
				"    h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);\n"
				"    return length(pa - ba * h) - mix(r1, r2, h);\n"
				"}\n"
				"vec2 rot(vec2 v, float deg) { float a = radians(deg); return vec2(v.x * cos(a) - v.y * sin(a), v.x * sin(a) + v.y * cos(a)); }\n"
				"void main() {\n"
				"    vec4 r = (u.color.w > 1.5 && gl_FragCoord.x >= u.rect1.x) ? u.rect1 : u.rect0;\n"
				"    vec2 scale = u.lay.xy / r.zw;\n"
				"    vec2 lp = (gl_FragCoord.xy - r.xy) * scale;\n"
				"    lp.y = u.lay.y - lp.y;\n"
				"    vec2 d = lp - u.lay.zw;\n"
				"    float aa = max(max(scale.x, scale.y), 0.75);\n"
				"    float dist = length(d);\n"
				"    float S = u.style.x;\n"
				"    vec4 acc;\n"
				"    float shape;\n"
				"    if (u.extra.x > 0.5) {\n"
				"        vec2 p = d / S; float a1 = aa / S;\n"
				"        vec2 ax = vec2(0.6, 0.8);\n"
				"        float hs, h1, h2;\n"
				"        float st = taper(p, ax * 0.42, ax, 0.068, 0.088, hs);\n"
				"        vec2 b0 = ax * 0.46;\n"
				"        float l1 = taper(p, b0, vec2(0.0), 0.090, 0.022, h1);\n"
				"        float l2 = taper(p, b0, b0 + rot(-ax * 0.36, 36.0), 0.085, 0.022, h2);\n"
				"        float leaf = min(l1, l2);\n"
				"        float hl = l1 < l2 ? h1 : h2;\n"
				"        float sh = min(st, leaf);\n"
				"        shape = sh * S;\n"
				"        vec3 stalk = mix(vec3(0.72, 0.90, 0.55), vec3(0.97, 0.98, 0.93), smoothstep(0.2, 0.75, hs));\n"
				"        vec3 leafc = mix(vec3(0.10, 0.55, 0.16), vec3(0.30, 0.82, 0.30), hl);\n"
				"        float edge = clamp(0.5 - (sh - 0.035) / a1, 0.0, 1.0) * 0.75;\n"
				"        acc = vec4(u.color.rgb, u.style.z * exp(-max(sh, 0.0) / 0.14));\n"
				"        acc = over(vec4(0.05, 0.16, 0.07, edge), acc);\n"
				"        acc = over(vec4(stalk, clamp(0.5 - st / a1, 0.0, 1.0)), acc);\n"
				"        acc = over(vec4(leafc, clamp(0.5 - leaf / a1, 0.0, 1.0)), acc);\n"
				"        if (u.style.w >= 0.0) {\n"
				"            float rr = S * (0.18 + 0.5 * u.style.w);\n"
				"            float rip = clamp(0.5 - (abs(dist - rr) - S * 0.02) / aa, 0.0, 1.0) * (1.0 - u.style.w) * 0.85;\n"
				"            acc = over(vec4(u.color.rgb, rip), acc);\n"
				"        }\n"
				"    } else {\n"
				"        float R = S;\n"
				"        float t = max(R * 0.15, 1.5);\n"
				"        float a0 = R * 0.4, a1 = R * 0.8;\n"
				"        float ring = abs(dist - R) - t * 0.5;\n"
				"        float hx = box(vec2(abs(d.x) - (a0 + a1) * 0.5, d.y), vec2((a1 - a0) * 0.5, t * 0.5));\n"
				"        float vy = box(vec2(d.x, abs(d.y) - (a0 + a1) * 0.5), vec2(t * 0.5, (a1 - a0) * 0.5));\n"
				"        shape = min(min(ring, dist - t * 0.7), min(hx, vy));\n"
				"        float core = clamp(0.5 - shape / aa, 0.0, 1.0);\n"
				"        float edge = clamp(0.5 - (shape - t * 0.65) / aa, 0.0, 1.0) * 0.55;\n"
				"        float rip = 0.0;\n"
				"        if (u.style.w >= 0.0) {\n"
				"            float rr = R * (1.0 + 1.3 * u.style.w);\n"
				"            rip = clamp(0.5 - (abs(dist - rr) - t * 0.35) / aa, 0.0, 1.0) * (1.0 - u.style.w) * 0.8;\n"
				"        }\n"
				"        acc = vec4(u.color.rgb, u.style.z * exp(-max(shape, 0.0) / (R * 0.5)));\n"
				"        acc = over(vec4(0.0, 0.0, 0.0, edge), acc);\n"
				"        acc = over(vec4(u.color.rgb, max(core, rip)), acc);\n"
				"    }\n"
				"    result = vec4(acc.rgb, acc.a * u.style.y);\n"
				"}\n";
			crossProgram = Link(VS, FS, "crosshair");
			return crossProgram != 0;
		}

		bool CrosshairImpl(const Crosshair& c, const Rect* rects, int rectCount)
		{
			if (!CrosshairProgram())
				return false;
			const int n = rectCount > 2 ? 2 : rectCount;
			float block[24] = {
				(float)rects[0].x, (float)rects[0].y, (float)rects[0].w, (float)rects[0].h,
				(float)rects[n - 1].x, (float)rects[n - 1].y, (float)rects[n - 1].w, (float)rects[n - 1].h,
				c.layWidth, c.layHeight, c.x, c.y,
				c.radius, c.alpha, c.glow, c.ripple,
				c.r, c.g, c.b, (float)n,
				(float)c.kind, 0.0f, 0.0f, 0.0f,
			};
			// Everything below is undone by dvk_pop_gl_state.
			if (!crossUbo)
				glGenBuffers_(1, &crossUbo);
			glBindBuffer_(O_UNIFORM_BUFFER, crossUbo);
			glBufferData_(O_UNIFORM_BUFFER, sizeof(block), block, O_DYNAMIC_DRAW);
			glBindBufferBase_(O_UNIFORM_BUFFER, CROSSHAIR_UBO_BINDING, crossUbo);

			glBindFramebuffer_(O_DRAW_FRAMEBUFFER, 0);
			glEnable_(O_BLEND);
			glBlendEquation_(O_FUNC_ADD);
			glBlendFuncSeparate_(O_SRC_ALPHA, O_ONE_MINUS_SRC_ALPHA, O_ONE, O_ONE_MINUS_SRC_ALPHA);
			glDisable_(O_CULL_FACE);
			glDisable_(O_DEPTH_TEST);
			glDisable_(O_STENCIL_TEST);
			glEnable_(O_SCISSOR_TEST);
			glColorMask_(1, 1, 1, 1);
			glUseProgram_(crossProgram);
			glBindVertexArray_(0);
			// Only the pixels around the crosshair, in each eye's rect
			// The leek reaches about its length down-right of the tip; the crosshair, its halo.
			const float reach = (c.kind == POINTER_LEEK ? c.radius * 1.4f : c.radius * 3.5f) + 4.0f;
			for (int i = 0; i < n; i++)
			{
				const Rect& r = rects[i];
				if (r.w <= 0 || r.h <= 0)
					continue;
				const float sx = r.w / c.layWidth, sy = r.h / c.layHeight;
				int x0 = r.x + (int)((c.x - reach) * sx), x1 = r.x + (int)((c.x + reach) * sx) + 1;
				int y1 = r.y + r.h - (int)((c.y - reach) * sy) + 1, y0 = r.y + r.h - (int)((c.y + reach) * sy);
				x0 = x0 < r.x ? r.x : x0; x1 = x1 > r.x + r.w ? r.x + r.w : x1;
				y0 = y0 < r.y ? r.y : y0; y1 = y1 > r.y + r.h ? r.y + r.h : y1;
				if (x1 <= x0 || y1 <= y0)
					continue;
				glViewport_(r.x, r.y, r.w, r.h);
				glScissor_(x0, y0, x1 - x0, y1 - y0);
				glDrawArrays_(O_TRIANGLES, 0, 3);
			}
			return true;
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
