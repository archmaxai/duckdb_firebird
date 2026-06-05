include_directories(include)

add_library(
  firebird_ext_library OBJECT
  firebird_extension.cpp
  firebird_storage.cpp
  firebird_scanner.cpp
  firebird_connection.cpp
  firebird_settings.cpp
  storage/firebird_catalog.cpp
  storage/firebird_schema_entry.cpp
  storage/firebird_table_entry.cpp
  storage/firebird_transaction.cpp
  storage/firebird_transaction_manager.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:firebird_ext_library>
    PARENT_SCOPE)
