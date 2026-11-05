// Compile-only check that src/plugin_abi.hpp matches PluginConfigApi.h (any x64 Windows compiler):
//   x86_64-w64-mingw32-clang++ -std=c++17 -fsyntax-only -Wno-invalid-offsetof tools/abi_check.cpp
#include "../third_party/PluginConfigApi.h"
#include "../src/plugin_abi.hpp"
#include <cstddef>
#define SAME(A,B,F) static_assert(offsetof(PluginConfig::A,F)==offsetof(pdl::B,F), #A "." #F);
#define SZ(A,B) static_assert(sizeof(PluginConfig::A)==sizeof(pdl::B), #A);
SZ(PluginConfigBooleanData,BooleanData) SZ(PluginConfigNumericData,NumericData) SZ(PluginConfigStringData,StringData)
SZ(PluginConfigDropdownIndexData,DropdownIndexData) SZ(PluginConfigDropdownTextData,DropdownTextData) SZ(PluginConfigDropdownNumberData,DropdownNumberData)
SZ(PluginConfigResolutionData,ResolutionData) SZ(PluginConfigGroupData,GroupData) SZ(PluginConfigButtonData,ButtonData)
SZ(PluginConfigOption,ConfigOption) SZ(PluginConfigArray,ConfigArray)
SAME(PluginConfigDropdownTextData,DropdownTextData,editable) SAME(PluginConfigDropdownTextData,DropdownTextData,useUtf8)
SAME(PluginConfigDropdownNumberData,DropdownNumberData,editable) SAME(PluginConfigResolutionData,ResolutionData,valueResolutions)
SAME(PluginConfigResolutionData,ResolutionData,editable) SAME(PluginConfigButtonData,ButtonData,func) SAME(PluginConfigStringData,StringData,useUtf8)
int main(){}
