// Read-only mirror of PD Loader's PluginConfigApi.h.
//
// The real header puts std::vector inside the option structs, so reading them ties the
// launcher to whichever C++ library the plugin was built with. Release builds of MSVC's
// std::vector (what every PD Loader plugin uses), libc++ and libstdc++ all store three
// pointers: first, last, end of capacity. Mirroring that layout lets this launcher read
// plugin options whatever compiler built it. We never modify or free these.
#pragma once

#include <cstddef>

namespace pdl {
    typedef const wchar_t* LPCWSTR_;

    template <typename T>
    struct Vec {
        T* first;
        T* last;
        T* cap;
        size_t size() const { return first && last >= first ? (size_t)(last - first) : 0; }
        const T* begin() const { return first; }
        const T* end() const { return first + size(); }
    };

    struct Resolution {
        unsigned int width;
        unsigned int height;
    };

#pragma pack(push, 1)
    enum ConfigType {
        CONFIG_BOOLEAN,
        CONFIG_NUMERIC,
        CONFIG_STRING,
        CONFIG_DROPDOWN_INDEX,
        CONFIG_DROPDOWN_TEXT,
        CONFIG_DROPDOWN_NUMBER,
        CONFIG_RESOLUTION,
        CONFIG_GROUP_START,
        CONFIG_GROUP_END,
        CONFIG_BUTTON,
        CONFIG_SPACER
    };

    struct BooleanData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        bool defaultVal;
        bool saveAsString;
    };

    struct NumericData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        int defaultVal, minVal, maxVal;
    };

    struct StringData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        LPCWSTR_ defaultVal;
        bool useUtf8;
    };

    struct DropdownIndexData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        int defaultVal;
        Vec<LPCWSTR_> valueStrings;
    };

    struct DropdownTextData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        LPCWSTR_ defaultVal;
        Vec<LPCWSTR_> valueStrings;
        bool editable;
        bool useUtf8;
    };

    struct DropdownNumberData {
        LPCWSTR_ iniVarName, iniSectionName, iniFilePath, friendlyName, description;
        int defaultVal;
        Vec<int> valueInts;
        bool editable;
    };

    struct ResolutionData {
        LPCWSTR_ iniVarName, iniVarName2, iniSectionName, iniFilePath, friendlyName, description;
        Resolution defaultVal;
        Vec<Resolution> valueResolutions;
        bool editable;
    };

    struct GroupData {
        LPCWSTR_ name;
        int height;
    };

    struct ButtonData {
        LPCWSTR_ friendlyName, description;
        void (*func)();
    };

    struct ConfigOption {
        ConfigType cfgType;
        void* data;
    };

    struct ConfigArray {
        int len;
        ConfigOption* options;
    };
#pragma pack(pop)

    // Layout checks against the original header (x64, pack(1))
    static_assert(sizeof(Vec<int>) == 24, "std::vector is three pointers");
    static_assert(sizeof(BooleanData) == 42, "BooleanData layout");
    static_assert(sizeof(DropdownIndexData) == 40 + 4 + 24, "DropdownIndexData layout");
    static_assert(sizeof(ResolutionData) == 48 + 8 + 24 + 1, "ResolutionData layout");
    static_assert(sizeof(ConfigOption) == 12, "ConfigOption layout");
    static_assert(sizeof(ConfigArray) == 12, "ConfigArray layout");
}
