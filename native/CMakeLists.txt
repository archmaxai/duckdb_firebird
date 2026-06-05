cmake_minimum_required(VERSION 3.5...3.29)

set(TARGET_NAME firebird)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME} CXX)

add_subdirectory(src)
set(EXTENSION_OBJECT_FILES ${ALL_OBJECT_FILES})

add_library(${EXTENSION_NAME} STATIC ${EXTENSION_OBJECT_FILES})

set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_OBJECT_FILES})

#===--------------------------------------------------------------------------===
# Build and link the pure-Rust Firebird client (firebird_client.a) via cargo.
#===--------------------------------------------------------------------------===
set(FIREBIRD_RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rust)
if(WIN32)
  set(FIREBIRD_RUST_LIB ${FIREBIRD_RUST_DIR}/target/release/firebird_client.lib)
else()
  set(FIREBIRD_RUST_LIB ${FIREBIRD_RUST_DIR}/target/release/libfirebird_client.a)
endif()

file(GLOB_RECURSE FIREBIRD_RUST_SOURCES ${FIREBIRD_RUST_DIR}/src/*.rs)
add_custom_command(
  OUTPUT ${FIREBIRD_RUST_LIB}
  COMMAND ${CMAKE_COMMAND} -E env cargo build --release
  WORKING_DIRECTORY ${FIREBIRD_RUST_DIR}
  DEPENDS ${FIREBIRD_RUST_SOURCES} ${FIREBIRD_RUST_DIR}/Cargo.toml
  COMMENT "Building Rust Firebird client (cargo build --release)"
  VERBATIM)
add_custom_target(firebird_rust_client DEPENDS ${FIREBIRD_RUST_LIB})

# Extra system libraries the Rust static library depends on.
set(FIREBIRD_LINK_LIBS ${FIREBIRD_RUST_LIB})
if(APPLE)
  list(APPEND FIREBIRD_LINK_LIBS "-framework CoreFoundation" "-framework Security")
elseif(WIN32)
  list(APPEND FIREBIRD_LINK_LIBS ws2_32 userenv ntdll bcrypt advapi32)
elseif(UNIX)
  list(APPEND FIREBIRD_LINK_LIBS pthread ${CMAKE_DL_LIBS} m)
endif()

add_dependencies(${EXTENSION_NAME} firebird_rust_client)
target_link_libraries(${EXTENSION_NAME} ${FIREBIRD_LINK_LIBS})

add_dependencies(${LOADABLE_EXTENSION_NAME} firebird_rust_client)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${FIREBIRD_LINK_LIBS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
