#
# Output build summary
#

message(STATUS "---------------------------")
message(STATUS " Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS " Compiler C: ${CMAKE_C_COMPILER}")
message(STATUS " Compiler C++: ${CMAKE_CXX_COMPILER}")
message(STATUS " Compiler flags: ${CMAKE_CXX_FLAGS}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(STATUS " + debug flags:: ${CMAKE_CXX_FLAGS_DEBUG}")
  if(NOT DEFINED ${DEBUG_LOGGER})
    set(DEBUG_LOGGER ON)
  endif()
  if(NOT DEFINED ${ENABLE_CCACHE})
    set(ENABLE_CCACHE ON)
  endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS " + release flags:: ${CMAKE_CXX_FLAGS_RELEASE}")
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
  message(STATUS " + minsizerel flags:: ${CMAKE_CXX_FLAGS_MINSIZEREL}")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  message(STATUS " + relwithdebinfo flags:: ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

if(CXXLIB_CLANG)
  message(STATUS " Linking C++ library: libc++")
elseif(CXXLIB_GCC)
  message(STATUS " Linking C++ library: libstdc++")
else()
  message(STATUS " Linking C++ library: system default")
endif()

message(STATUS "---------------------------")
message(STATUS " Build testsuite        ${BUILD_TESTS}")
message(STATUS " Enable debug logger    ${DEBUG_LOGGER}")
message(STATUS " Enable extra tracing   ${VERBOSE_TRACELOG}")
message(STATUS " Enable ccache support  ${ENABLE_CCACHE}")
message(STATUS "---------------------------")
message(STATUS " Enable alsa support    ${ENABLE_ALSA}")
message(STATUS " Enable i3 support      ${ENABLE_I3}")
message(STATUS " Enable mpd support     ${ENABLE_MPD}")
message(STATUS " Enable network support ${ENABLE_NETWORK}")
message(STATUS "---------------------------")
