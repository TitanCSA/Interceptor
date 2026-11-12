cmake_minimum_required(VERSION 3.24)
project(InterceptorSDK VERSION 1.4.22.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This SDK requires Windows x64. No driver is installed by this build.")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
add_library(TitanInterceptor STATIC
  "sdk/Interceptor/Interceptor.cpp"
)
add_library(InterceptorSDK::sdk ALIAS TitanInterceptor)
set_target_properties(TitanInterceptor PROPERTIES EXPORT_NAME sdk)
target_compile_features(TitanInterceptor PUBLIC cxx_std_17)
target_compile_definitions(TitanInterceptor PUBLIC )
target_compile_definitions(TitanInterceptor PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  target_compile_options(TitanInterceptor PRIVATE /utf-8 /EHsc /W4)
endif()
target_include_directories(TitanInterceptor PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
  "$<INSTALL_INTERFACE:include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/Interceptor>"
  "$<INSTALL_INTERFACE:include/Interceptor>"
)
target_link_libraries(TitanInterceptor PUBLIC advapi32 bcrypt cfgmgr32 comctl32 hid newdev ole32 setupapi user32)
install(TARGETS TitanInterceptor EXPORT InterceptorSDKTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY sdk/ DESTINATION include FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
install(FILES LICENSE DESTINATION share/interceptor)
install(EXPORT InterceptorSDKTargets NAMESPACE InterceptorSDK:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InterceptorSDK)
configure_file("InterceptorSDKConfig.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/InterceptorSDKConfig.cmake" @ONLY)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/InterceptorSDKConfigVersion.cmake" VERSION 1.4.22.0 COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/InterceptorSDKConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/InterceptorSDKConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InterceptorSDK)
