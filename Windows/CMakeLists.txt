cmake_minimum_required(VERSION 3.20)

project(DropCode LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "DropCode's WinUI 3 settings surface requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
endif()

set(DROPCODE_WASDK_VERSION "2.3.1")
set(DROPCODE_CPPWINRT_VERSION "2.0.250303.1")
set(DROPCODE_WASDK_ROOT "${CMAKE_BINARY_DIR}/wasdk")
set(DROPCODE_WASDK_PACKAGES "${DROPCODE_WASDK_ROOT}/packages")
set(DROPCODE_WASDK_NUGET "${DROPCODE_WASDK_ROOT}/nuget.exe")
set(DROPCODE_WASDK_RESTORE_MARKER
    "${DROPCODE_WASDK_ROOT}/.restored-${DROPCODE_WASDK_VERSION}-${DROPCODE_CPPWINRT_VERSION}")

find_program(DROPCODE_POWERSHELL powershell.exe)
if(NOT DROPCODE_POWERSHELL)
  message(FATAL_ERROR "PowerShell is required to restore Windows App SDK packages.")
endif()

if(NOT EXISTS "${DROPCODE_WASDK_RESTORE_MARKER}")
  execute_process(
    COMMAND "${DROPCODE_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/restore-wasdk.ps1"
            -PackageDirectory "${DROPCODE_WASDK_PACKAGES}"
            -NuGetExe "${DROPCODE_WASDK_NUGET}"
            -WindowsAppSdkVersion "${DROPCODE_WASDK_VERSION}"
            -CppWinRtVersion "${DROPCODE_CPPWINRT_VERSION}"
    RESULT_VARIABLE DROPCODE_WASDK_RESTORE_RESULT)
  if(NOT DROPCODE_WASDK_RESTORE_RESULT EQUAL 0)
    message(FATAL_ERROR "Windows App SDK package restore failed with ${DROPCODE_WASDK_RESTORE_RESULT}.")
  endif()
  file(TOUCH "${DROPCODE_WASDK_RESTORE_MARKER}")
endif()

function(dropcode_find_wasdk_package output prefix)
  file(GLOB package_candidates LIST_DIRECTORIES true
       "${DROPCODE_WASDK_PACKAGES}/${prefix}.*")
  if(NOT package_candidates)
    message(FATAL_ERROR "Could not find restored NuGet package ${prefix}.")
  endif()
  list(LENGTH package_candidates package_count)
  if(NOT package_count EQUAL 1)
    message(FATAL_ERROR
            "Expected one restored ${prefix} package; remove the build directory and configure again.")
  endif()
  list(GET package_candidates 0 selected_package)
  set(${output} "${selected_package}" PARENT_SCOPE)
endfunction()

dropcode_find_wasdk_package(DROPCODE_WASDK_FOUNDATION
                            "Microsoft.WindowsAppSDK.Foundation")
dropcode_find_wasdk_package(DROPCODE_WASDK_INTERACTIVE
                            "Microsoft.WindowsAppSDK.InteractiveExperiences")
dropcode_find_wasdk_package(DROPCODE_WASDK_WINUI "Microsoft.WindowsAppSDK.WinUI")
dropcode_find_wasdk_package(DROPCODE_WASDK_RUNTIME "Microsoft.WindowsAppSDK.Runtime")
dropcode_find_wasdk_package(DROPCODE_WEBVIEW2 "Microsoft.Web.WebView2")
dropcode_find_wasdk_package(DROPCODE_CPPWINRT "Microsoft.Windows.CppWinRT")

if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(DROPCODE_WASDK_NATIVE_ARCH "win-arm64")
  set(DROPCODE_WASDK_LIB_ARCH "arm64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(DROPCODE_WASDK_NATIVE_ARCH "win-x64")
  set(DROPCODE_WASDK_LIB_ARCH "x64")
else()
  set(DROPCODE_WASDK_NATIVE_ARCH "win-x86")
  set(DROPCODE_WASDK_LIB_ARCH "x86")
endif()

set(DROPCODE_CPPWINRT_EXE "${DROPCODE_CPPWINRT}/bin/cppwinrt.exe")
set(DROPCODE_CPPWINRT_OUTPUT "${DROPCODE_WASDK_ROOT}/cppwinrt")
set(DROPCODE_CPPWINRT_MARKER "${DROPCODE_CPPWINRT_OUTPUT}/.generated")
set(DROPCODE_WINUI_METADATA "${DROPCODE_WASDK_WINUI}/metadata")
set(DROPCODE_INTERACTIVE_METADATA
    "${DROPCODE_WASDK_INTERACTIVE}/metadata/10.0.18362.0")
set(DROPCODE_FOUNDATION_METADATA "${DROPCODE_WASDK_FOUNDATION}/metadata")
set(DROPCODE_WEBVIEW2_WINMD
    "${DROPCODE_WEBVIEW2}/lib/Microsoft.Web.WebView2.Core.winmd")

if(NOT EXISTS "${DROPCODE_CPPWINRT_MARKER}")
  file(MAKE_DIRECTORY "${DROPCODE_CPPWINRT_OUTPUT}")
  execute_process(
    COMMAND "${DROPCODE_CPPWINRT_EXE}"
            -input sdk
            -input "${DROPCODE_WINUI_METADATA}"
            -input "${DROPCODE_INTERACTIVE_METADATA}"
            -input "${DROPCODE_FOUNDATION_METADATA}"
            -input "${DROPCODE_WEBVIEW2_WINMD}"
            -output "${DROPCODE_CPPWINRT_OUTPUT}"
    RESULT_VARIABLE DROPCODE_CPPWINRT_RESULT)
  if(NOT DROPCODE_CPPWINRT_RESULT EQUAL 0)
    message(FATAL_ERROR "C++/WinRT projection generation failed with ${DROPCODE_CPPWINRT_RESULT}.")
  endif()
  file(TOUCH "${DROPCODE_CPPWINRT_MARKER}")
endif()

if(NOT EXISTS "${DROPCODE_CPPWINRT_OUTPUT}/winrt/Microsoft.UI.Xaml.h")
  message(FATAL_ERROR "C++/WinRT did not generate the WinUI projection headers.")
endif()

unset(DROPCODE_WASDK_BOOTSTRAP_LIB CACHE)
unset(DROPCODE_WASDK_RUNTIME_LIB CACHE)
unset(DROPCODE_WASDK_DISPATCHING_LIB CACHE)
unset(DROPCODE_WASDK_BOOTSTRAP_DLL CACHE)

find_library(DROPCODE_WASDK_BOOTSTRAP_LIB
             NAMES Microsoft.WindowsAppRuntime.Bootstrap
             PATHS "${DROPCODE_WASDK_FOUNDATION}/lib/native/${DROPCODE_WASDK_LIB_ARCH}"
             NO_DEFAULT_PATH)
find_library(DROPCODE_WASDK_RUNTIME_LIB
             NAMES Microsoft.WindowsAppRuntime
             PATHS "${DROPCODE_WASDK_FOUNDATION}/lib/native/${DROPCODE_WASDK_LIB_ARCH}"
             NO_DEFAULT_PATH)
find_library(DROPCODE_WASDK_DISPATCHING_LIB
             NAMES Microsoft.UI.Dispatching
             PATHS "${DROPCODE_WASDK_INTERACTIVE}/lib/native/${DROPCODE_WASDK_LIB_ARCH}"
             NO_DEFAULT_PATH)
find_file(DROPCODE_WASDK_BOOTSTRAP_DLL
          NAMES Microsoft.WindowsAppRuntime.Bootstrap.dll
          PATHS "${DROPCODE_WASDK_FOUNDATION}/runtimes/${DROPCODE_WASDK_NATIVE_ARCH}/native"
          NO_DEFAULT_PATH)
if(NOT DROPCODE_WASDK_BOOTSTRAP_LIB OR NOT DROPCODE_WASDK_RUNTIME_LIB OR
   NOT DROPCODE_WASDK_DISPATCHING_LIB OR NOT DROPCODE_WASDK_BOOTSTRAP_DLL)
  message(FATAL_ERROR "Could not find the Windows App SDK libraries for ${DROPCODE_WASDK_NATIVE_ARCH}.")
endif()

add_library(libvterm STATIC
  third_party/libvterm/src/encoding.c
  third_party/libvterm/src/keyboard.c
  third_party/libvterm/src/mouse.c
  third_party/libvterm/src/parser.c
  third_party/libvterm/src/pen.c
  third_party/libvterm/src/screen.c
  third_party/libvterm/src/state.c
  third_party/libvterm/src/unicode.c
  third_party/libvterm/src/vterm.c
)
target_include_directories(libvterm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/include
)

add_executable(DropCode WIN32
  src/main.cpp
  src/app.cpp
  src/hotkey.cpp
  src/launcher.cpp
  src/monitor.cpp
  src/panel.cpp
  src/registry.cpp
  src/settings.cpp
  src/winui_runtime.cpp
  src/terminal.cpp
  src/tray.cpp
  src/vtrenderer.cpp
  src/resources.rc
)

set_property(SOURCE src/resources.rc APPEND PROPERTY OBJECT_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/settings.xaml")

target_include_directories(DropCode PRIVATE
  src
  "${DROPCODE_CPPWINRT_OUTPUT}"
  "${DROPCODE_WASDK_FOUNDATION}/include"
  "${DROPCODE_WASDK_INTERACTIVE}/include"
  "${DROPCODE_WASDK_WINUI}/include"
  "${DROPCODE_WASDK_RUNTIME}/include"
)

target_compile_definitions(DropCode PRIVATE
  UNICODE
  _UNICODE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NOMINMAX
)

target_link_libraries(DropCode PRIVATE
  libvterm
  d2d1
  dwrite
  winmm
  dxgi
  user32
  gdi32
  dwmapi
  shell32
  ole32
  runtimeobject
  comctl32
  advapi32
  shcore
  "${DROPCODE_WASDK_BOOTSTRAP_LIB}"
  "${DROPCODE_WASDK_RUNTIME_LIB}"
  "${DROPCODE_WASDK_DISPATCHING_LIB}"
  delayimp
)

if(MSVC)
  target_link_options(DropCode PRIVATE
    "/DELAYLOAD:Microsoft.UI.Windowing.Core.dll"
  )
endif()

add_custom_command(TARGET DropCode POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
           "${DROPCODE_WASDK_BOOTSTRAP_DLL}"
           "$<TARGET_FILE_DIR:DropCode>"
  VERBATIM)

install(TARGETS DropCode RUNTIME DESTINATION .)
install(FILES "${DROPCODE_WASDK_BOOTSTRAP_DLL}"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)
