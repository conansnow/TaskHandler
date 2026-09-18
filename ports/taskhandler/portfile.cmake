# Overlay port for the current tree: CURRENT_PORT_DIR is ports/taskhandler,
# so two levels up is the repository root. An official microsoft/vcpkg port
# would replace this with vcpkg_from_github() against a tagged release.
set(SOURCE_PATH "${CURRENT_PORT_DIR}/../..")
get_filename_component(SOURCE_PATH "${SOURCE_PATH}" ABSOLUTE)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DTASKHANDLER_BUILD_TESTS=OFF
        -DTASKHANDLER_BUILD_EXAMPLES=OFF
        -DTASKHANDLER_BUILD_BENCHMARKS=OFF
        -DTASKHANDLER_INSTALL=ON
        # The source tree has a development vcpkg.json (gtest behind tests).
        # A port build must not treat that file as a consumer manifest.
        -DVCPKG_MANIFEST_INSTALL=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME TaskHandler CONFIG_PATH lib/cmake/TaskHandler)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
