target_sources(nlohmann_json_schema_validator PRIVATE
        smtp-address-validator.cpp
        json-schema-draft7.json.cpp
        json-uri.cpp
        json-validator.cpp
        json-patch.cpp
        string-format-check.cpp
        )
target_include_directories(nlohmann_json_schema_validator PUBLIC
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        )

set_target_properties(nlohmann_json_schema_validator PROPERTIES
        PUBLIC_HEADER nlohmann/json-schema.hpp)

# Windows: export the API when building the DLL, import it when using it
if (JSON_VALIDATOR_SHARED_LIBS)
    target_compile_definitions(nlohmann_json_schema_validator
            PRIVATE JSON_SCHEMA_VALIDATOR_EXPORTS
            INTERFACE JSON_SCHEMA_VALIDATOR_IMPORTS)
endif ()

# TODO: Consider setting minimum cxx standard instead
target_compile_features(nlohmann_json_schema_validator PUBLIC
        cxx_range_for) # for C++11 - flags

# TODO: This should be handled by the CI/presets, not the cmake
# Not for clang-cl (MSVC-like command line), which takes -Wall as /Wall (all warnings)
if (("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR
        "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU") AND
        NOT "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC")
    target_compile_options(nlohmann_json_schema_validator
            PRIVATE
            -Wall -Wextra -Wshadow)
endif ()

if (PCRE2_FOUND AND JSON_VALIDATOR_USE_PCRE2)
    target_compile_definitions(nlohmann_json_schema_validator PRIVATE JSON_SCHEMA_PCRE2)
    target_link_libraries(nlohmann_json_schema_validator PRIVATE PkgConfig::PCRE2)
    message(STATUS "JsonValidator: PCRE2 UTF/UCP regex backend")
else ()
    message(STATUS "JsonValidator: std::regex ECMAScript backend")
endif ()

target_link_libraries(nlohmann_json_schema_validator PUBLIC
        nlohmann_json::nlohmann_json)

if (JSON_VALIDATOR_INSTALL)
    # Normal installation target to system. When using scikit-build check python subdirectory
    install(TARGETS nlohmann_json_schema_validator
            EXPORT nlohmann_json_schema_validatorTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT nlohmann_json_schema_validator_Runtime
            NAMELINK_COMPONENT nlohmann_json_schema_validator_Development
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT nlohmann_json_schema_validator_Development
            PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nlohmann COMPONENT nlohmann_json_schema_validator_Development
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT nlohmann_json_schema_validator_Runtime)
endif ()

# Embed the authoritative JSON documents at configure time for offline use.
file(GLOB_RECURSE metaschemas CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/metaschemas/draft2020-12/*.json")
set(metaschema_entries "")
foreach(metaschema IN LISTS metaschemas)
    file(RELATIVE_PATH name "${CMAKE_CURRENT_SOURCE_DIR}/metaschemas/draft2020-12" "${metaschema}")
    string(REGEX REPLACE "[.]json$" "" name "${name}")
    file(READ "${metaschema}" content)
    string(APPEND metaschema_entries "{\"https://json-schema.org/draft/2020-12/${name}\", json::parse(R\"META(${content})META\")},\n")
endforeach()
configure_file(metaschemas/builtin.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/json-schema-2020-12.cpp" @ONLY)
target_sources(nlohmann_json_schema_validator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/json-schema-2020-12.cpp")
