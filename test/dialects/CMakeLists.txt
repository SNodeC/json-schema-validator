add_executable(dialects dialects.cpp)
target_link_libraries(dialects PRIVATE nlohmann_json_schema_validator)
add_test(NAME dialects COMMAND dialects)
