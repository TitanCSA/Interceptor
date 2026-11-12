vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/TitanCSA/Interceptor/releases/download/sdk-v1.4.22/interceptor-sdk-1.4.22.tar.gz"
    FILENAME "interceptor-sdk-1.4.22.tar.gz"
    SHA512 c24e0e62aae47f7d20ae046dba123ed29653bb725fcd9bde527c584493d22485e427d7bbd36d6e1f8bf1f4e30e0d3afc9d3ffa1671574d010d110151f2e7bb40
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME InterceptorSDK CONFIG_PATH lib/cmake/InterceptorSDK)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
