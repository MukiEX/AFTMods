#pragma once

// Draws an overlay image (the launcher's in-game menu) over the finished frame (DivaLinux).
//
// The launcher renders its UI on the CPU into a BGRA image with alpha; this uploads it and
// blends it onto the default framebuffer just before the frame is presented, once per
// rectangle given (two in side-by-side stereo, one per eye, so the menu sits on the screen
// plane in both).
//
// DrawMesh does the same for UI that comes as triangles (DivaOverlay's Dear ImGui draw lists):
// coloured, textured triangles with clip rectangles, drawn on the GPU, so the CPU only builds
// the lists.
//
// Like movie/movie_yuv.cpp, it runs on the game's render thread through DivaVK's GL functions between
// dvk_push_gl_state and dvk_pop_gl_state, so the game and DivaGL's cached GL state never see
// it. Platform-independent: DivaVK's Linux tests run it against SwiftShader.

#include <divavk/divavk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

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
		O_UNIFORM_BUFFER = 0x8A11, O_DYNAMIC_DRAW = 0x88E8, O_STREAM_DRAW = 0x88E0,
		O_ARRAY_BUFFER = 0x8892, O_ELEMENT_ARRAY_BUFFER = 0x8893, O_FLOAT = 0x1406, O_UNSIGNED_SHORT = 0x1403,
		O_RGBA = 0x1908,
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

	// Triangles in layout pixels (top-down, like the image), coloured per vertex and multiplied
	// by a texture: the layout of Dear ImGui's ImDrawVert.
	struct Vertex
	{
		float x, y, u, v;
		uint32_t rgba;          // R in the lowest byte, straight (not premultiplied) alpha
	};
	struct MeshCmd
	{
		uint32_t count;         // indices
		uint32_t firstIndex;
		int32_t baseVertex;
		float clip[4];          // x0, y0, x1, y1 in layout pixels
		uint32_t texture;       // an id given to SetTexture
	};
	struct Mesh
	{
		float layWidth, layHeight;
		const Vertex* vertices; int vertexCount;
		const uint16_t* indices; int indexCount;
		const MeshCmd* cmds; int cmdCount;
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

		bool Draw(const Image& img, const Rect* rects, int rectCount);

		bool DrawCrosshair(const Crosshair& c, const Rect* rects, int rectCount);

		// A texture for meshes: RGBA8, rows top-first. Uploading again under the same id replaces it.
		bool SetTexture(uint32_t id, int w, int h, const void* rgba);

		bool DrawMesh(const Mesh& m, const Rect* rects, int rectCount);

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
		typedef void (APIENTRY* P_GenVertexArrays)(GLsizei_, GLuint_*);
		typedef void (APIENTRY* P_EnableVertexAttribArray)(GLuint_);
		typedef void (APIENTRY* P_VertexAttribPointer)(GLuint_, GLint_, GLenum_, unsigned char, GLsizei_, const void*);
		typedef void (APIENTRY* P_DrawElementsBaseVertex)(GLenum_, GLsizei_, GLenum_, const void*, GLint_);
		P_GenVertexArrays glGenVertexArrays_ = nullptr;
		P_EnableVertexAttribArray glEnableVertexAttribArray_ = nullptr;
		P_VertexAttribPointer glVertexAttribPointer_ = nullptr;
		P_DrawElementsBaseVertex glDrawElementsBaseVertex_ = nullptr;
		struct MeshTexture { uint32_t id; GLuint_ gl; };
		std::vector<MeshTexture> meshTextures;
		std::vector<Vertex> ndc;   // the mesh's vertices in clip space
		GLuint_ meshProgram = 0, meshVao = 0, meshVbo = 0, meshIbo = 0;

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

		void Fail(const std::string& why);

		template <typename T> bool Get(T& fn, const char* name)
		{
			fn = (T)dvk_get_implemented_proc(name);
			if (!fn)
				Fail(std::string(name) + " is not available");
			return fn != nullptr;
		}

		bool Load();

		bool CheckError(const char* what);

		bool MeshProgram();

		bool MeshImpl(const Mesh& m, const Rect* rects, int rectCount);

		GLuint_ Link(const char* vsSrc, const char* fsSrc, const char* what);

		bool CrosshairProgram();

		bool CrosshairImpl(const Crosshair& c, const Rect* rects, int rectCount);

		GLuint_ Shader(GLenum_ type, const char* source);

		bool Program();

		bool DrawImpl(const Image& img, const Rect* rects, int rectCount);
	};
}
