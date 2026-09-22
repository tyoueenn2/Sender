cmake_minimum_required(VERSION 3.24)
project(frame_sender VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FrameSender requires Windows 10 or Windows 11 and Direct3D 11")
endif()

add_executable(frame_sender src/main.cpp)
target_compile_features(frame_sender PRIVATE cxx_std_20)
target_compile_definitions(frame_sender PRIVATE FRAME_SENDER_VERSION=\"${PROJECT_VERSION}\")
target_compile_options(frame_sender PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->)
target_link_libraries(frame_sender PRIVATE d3d11 dxgi ws2_32 bcrypt)

install(TARGETS frame_sender RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(FILES docs/PROTOCOL.md DESTINATION docs)

set(CPACK_PACKAGE_NAME "FrameSender")
set(CPACK_PACKAGE_VENDOR "tyoueenn2")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR ZIP)
include(CPack)
