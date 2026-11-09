// GPU colour conversion for movie frames: see movie_yuv.hpp.

#include "movie_yuv.hpp"

namespace MovieYuv
{
	bool Converter::Convert(GLuint_ target, int w, int h, const Planes& p)
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

	void Converter::Fail(const std::string& why)
	{
		failed = true;
		if (error.empty())
			error = why;
	}

	bool Converter::Load()
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

	GLuint_ Converter::Shader(GLenum_ type, const std::string& source)
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

	GLuint_ Converter::Program(Matrix matrix, bool fullRange)
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

	void Converter::UploadPlane(int i, const uint8_t* data, int stride, int w, int h)
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

	bool Converter::Draw(GLuint_ target, int w, int h, const Planes& p)
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
}
