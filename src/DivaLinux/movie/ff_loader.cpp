#include "ff_loader.hpp"
#include <cstdio>

const char* FFLib::LibName(int i)
{
	static const char* names[COUNT] = { "avutil", "swresample", "swscale", "avcodec", "avformat" };
	return names[i];
}

int FFLib::ExpectedMajor(int i)
{
	static const int majors[COUNT] = { LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR, LIBSWSCALE_VERSION_MAJOR,
		LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR };
	return majors[i];
}

FFLibHandle FFLib::Handle(const char* lib)
{
	for (int i = 0; i < COUNT; i++)
		if (strcmp(lib, LibName(i)) == 0)
			return handles[i];
	return nullptr;
}

void* FFLib::Symbol(FFLibHandle h, const char* name)
{
#ifdef _WIN32
	return (void*)GetProcAddress(h, name);
#else
	return dlsym(h, name);
#endif
}

bool FFLib::Load(const std::wstring& directory)
{
	if (loaded)
		return true;

	for (int i = 0; i < COUNT; i++)
	{
		char file[64];
#ifdef _WIN32
		snprintf(file, sizeof(file), "%s-%d.dll", LibName(i), ExpectedMajor(i));
		std::wstring path = directory;
		for (const char* c = file; *c; c++)
			path += (wchar_t)*c;
		handles[i] = LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
		snprintf(file, sizeof(file), "lib%s.so.%d", LibName(i), ExpectedMajor(i));
		std::string path(directory.begin(), directory.end());
		handles[i] = dlopen((path + file).c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handles[i])
			handles[i] = dlopen(file, RTLD_NOW | RTLD_LOCAL);
#endif
		if (!handles[i])
		{
			error = std::string("could not load ") + file;
			Unload();
			return false;
		}
	}

#define FF_RESOLVE(lib, name) \
	name = (decltype(name))Symbol(Handle(#lib), #name); \
	if (!name) { error = "missing function " #name; Unload(); return false; }
	FF_FUNCTIONS(FF_RESOLVE)
#undef FF_RESOLVE

	const unsigned versions[COUNT] = { avutil_version(), swresample_version(), swscale_version(),
		avcodec_version(), avformat_version() };
	for (int i = 0; i < COUNT; i++)
	{
		if ((int)(versions[i] >> 16) != ExpectedMajor(i))
		{
			char message[128];
			snprintf(message, sizeof(message), "%s major version %u, expected %d", LibName(i), versions[i] >> 16, ExpectedMajor(i));
			error = message;
			Unload();
			return false;
		}
	}

	av_log_set_level(AV_LOG_ERROR);
	loaded = true;
	return true;
}

void FFLib::Unload()
{
	for (int i = COUNT - 1; i >= 0; i--)
	{
		if (handles[i])
		{
#ifdef _WIN32
			FreeLibrary(handles[i]);
#else
			dlclose(handles[i]);
#endif
			handles[i] = nullptr;
		}
	}
	loaded = false;
}

std::string FFLib::ErrorString(int code)
{
	char buffer[128] = "?";
	if (av_strerror)
		av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}
