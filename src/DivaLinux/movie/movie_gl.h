#pragma once

// The few GL functions the FFmpeg player uses to upload frames, taken straight from the
// Vulkan GL implementation in this plugin (DivaVK core).
//
// In the standalone DivaMovie these came from opengl32.dll. Inside DivaLinux there is no real
// OpenGL: the game's GL calls are routed to DivaVK, and the movie upload has to land in the
// same place or the frame would go to a context that doesn't exist. This keeps the names of
// DivaMovie's GLDXEmu helpers so FFPlayer.h stays close to the DivaMovie original.

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

	static PFN_glBindTexture pglBindTexture;
	static PFN_glTexSubImage2D pglTexSubImage2D;
	static PFN_glGetIntegerv pglGetIntegerv;
	static PFN_glPixelStorei pglPixelStorei;
	static PFN_glGetError pglGetError;
	static PFN_glBindBuffer pglBindBuffer;

	inline bool LoadGL()
	{
		pglBindTexture = (PFN_glBindTexture)dvk_get_implemented_proc("glBindTexture");
		pglTexSubImage2D = (PFN_glTexSubImage2D)dvk_get_implemented_proc("glTexSubImage2D");
		pglGetIntegerv = (PFN_glGetIntegerv)dvk_get_implemented_proc("glGetIntegerv");
		pglPixelStorei = (PFN_glPixelStorei)dvk_get_implemented_proc("glPixelStorei");
		pglGetError = (PFN_glGetError)dvk_get_implemented_proc("glGetError");
		pglBindBuffer = (PFN_glBindBuffer)dvk_get_implemented_proc("glBindBuffer");
		return pglBindTexture && pglTexSubImage2D && pglGetIntegerv && pglPixelStorei && pglGetError;
	}

	// Saves and restores the GL state touched by an upload, so the game's renderer
	// doesn't see a different texture binding or unpack state afterwards.
	struct GLUploadState
	{
		// DivaVK has no unpack skip state (always 0), so only these are saved.
		GLint texture, rowLength, alignment, unpackBuffer;

		void Save()
		{
			pglGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
			pglGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
			pglGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
			unpackBuffer = 0;
			if (pglBindBuffer)
			{
				pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
				if (unpackBuffer)
					pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			}
		}

		void Restore()
		{
			pglBindTexture(GL_TEXTURE_2D, (GLuint)texture);
			pglPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
			pglPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
			if (pglBindBuffer && unpackBuffer)
				pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
		}
	};
}
