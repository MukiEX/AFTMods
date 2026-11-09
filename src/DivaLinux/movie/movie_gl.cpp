#include "movie_gl.hpp"

namespace GLDXEmu
{
	PFN_glBindTexture pglBindTexture;
	PFN_glTexSubImage2D pglTexSubImage2D;
	PFN_glGetIntegerv pglGetIntegerv;
	PFN_glPixelStorei pglPixelStorei;
	PFN_glGetError pglGetError;
	PFN_glBindBuffer pglBindBuffer;

	bool LoadGL()
	{
		pglBindTexture = (PFN_glBindTexture)dvk_get_implemented_proc("glBindTexture");
		pglTexSubImage2D = (PFN_glTexSubImage2D)dvk_get_implemented_proc("glTexSubImage2D");
		pglGetIntegerv = (PFN_glGetIntegerv)dvk_get_implemented_proc("glGetIntegerv");
		pglPixelStorei = (PFN_glPixelStorei)dvk_get_implemented_proc("glPixelStorei");
		pglGetError = (PFN_glGetError)dvk_get_implemented_proc("glGetError");
		pglBindBuffer = (PFN_glBindBuffer)dvk_get_implemented_proc("glBindBuffer");
		return pglBindTexture && pglTexSubImage2D && pglGetIntegerv && pglPixelStorei && pglGetError;
	}

	void GLUploadState::Save()
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

	void GLUploadState::Restore()
	{
		pglBindTexture(GL_TEXTURE_2D, (GLuint)texture);
		pglPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		pglPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		if (pglBindBuffer && unpackBuffer)
			pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
	}
}
