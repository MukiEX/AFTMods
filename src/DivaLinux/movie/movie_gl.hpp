#pragma once

// The few GL functions the FFmpeg player uses to upload frames, taken straight from the
// Vulkan GL implementation in this plugin (DivaVK core).
//
// In the standalone DivaMovie these came from opengl32.dll. Inside DivaLinux there is no real
// OpenGL: the game's GL calls are routed to DivaVK, and the movie upload has to land in the
// same place or the frame would go to a context that doesn't exist. This keeps the names of
// DivaMovie's GLDXEmu helpers so ff_player.cpp stays close to the DivaMovie original.

#include <GL/gl.h>
#include <divavk/divavk.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif

namespace GLDXEmu
{
	typedef void (APIENTRY* PFN_glBindTexture)(GLenum, GLuint);
	typedef void (APIENTRY* PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
	typedef void (APIENTRY* PFN_glGetIntegerv)(GLenum, GLint*);
	typedef void (APIENTRY* PFN_glPixelStorei)(GLenum, GLint);
	typedef GLenum (APIENTRY* PFN_glGetError)(void);
	typedef void (APIENTRY* PFN_glBindBuffer)(GLenum, GLuint);

	extern PFN_glBindTexture pglBindTexture;
	extern PFN_glTexSubImage2D pglTexSubImage2D;
	extern PFN_glGetIntegerv pglGetIntegerv;
	extern PFN_glPixelStorei pglPixelStorei;
	extern PFN_glGetError pglGetError;
	extern PFN_glBindBuffer pglBindBuffer;

	bool LoadGL();

	// Saves and restores the GL state touched by an upload, so the game's renderer
	// doesn't see a different texture binding or unpack state afterwards.
	struct GLUploadState
	{
		// DivaVK has no unpack skip state (always 0), so only these are saved.
		GLint texture, rowLength, alignment, unpackBuffer;

		void Save();

		void Restore();
	};
}
