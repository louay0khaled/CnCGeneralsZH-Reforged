cmake_minimum_required(VERSION 3.22)
project(GeneralsZHAndroid LANGUAGES C CXX)

# Use the same CMake graph as desktop; Android-specific platform selection and GLES sources
# are guarded by ZH_PLATFORM_ANDROID inside Code/CMakeLists.txt.
add_subdirectory(../../../Code ${CMAKE_CURRENT_BINARY_DIR}/reforged-code)
