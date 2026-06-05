PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of the extension
EXT_NAME=firebird
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# We have no vcpkg dependencies; do not require a vcpkg toolchain.
USE_MERGED_VCPKG_MANIFEST=0

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
