/*
 * JSON schema validator for JSON for modern C++
 * Copyright (c) 2026 Volker Christian.
 * SPDX-License-Identifier: MIT
 */
#include <nlohmann/json-schema.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

using nlohmann::json;
using namespace nlohmann::json_schema;

static bool compatible(const json &group, int dialect)
{
	std::istringstream constraints(group.value("compatibility", "0"));
	std::string constraint;
	bool result = true;
	bool done = false;
	while (!done && std::getline(constraints, constraint, ',')) {
		const auto start = constraint.find_first_of("0123456789");
		const int version = std::stoi(constraint.substr(start));
		const std::string op = constraint.substr(0, start);
		result = op == "=" ? dialect == version : op == "<=" ? dialect <= version
		                                                     : dialect >= version;
		done = dialect <= version;
	}
	return result;
}

int main(int argc, char **argv)
{
	std::size_t total = 0;
	std::size_t failed = 0;
	std::size_t excluded = 0;
	const int dialect = std::stoi(argv[1]);
	const std::string category = argv[2];
	for (int arg = 3; arg < argc; ++arg) {
		std::ifstream input(argv[arg]);
		json file;
		input >> file;
		for (const auto &group : file.at("tests")) {
			const auto tags = group.value("tags", json::array());
			const bool assertion = std::find(tags.begin(), tags.end(), "format-assertion") != tags.end();
			if (compatible(group, dialect) && (category == "format" ? assertion : !assertion)) {
				std::string compilation_error;
				const auto external = group.value("externalSchemas", json::object());
				auto loader = [&external](const nlohmann::json_uri &uri, json &schema) {
					if (uri.location() == "http://json-schema.org/draft-07/schema") {
						schema = draft7_schema_builtin;
					} else if (external.contains(uri.location())) {
						schema = external.at(uri.location());
					} else {
						throw std::invalid_argument("fixture not found: " + uri.location());
					}
				};
				format_checker format = assertion ? default_string_format_check : format_checker([](const std::string &, const std::string &) {});
				json_validator validator(loader, format);
				try {
					validator.set_root_schema(group.at("schema"));
				} catch (const std::exception &e) {
					compilation_error = e.what();
				}
				for (const auto &test : group.at("tests")) {
					bool valid = false;
					std::string error = compilation_error;
					if (compilation_error.empty()) {
						try {
							basic_error_handler errors;
							validator.validate(test.at("data"), errors);
							valid = !errors;
						} catch (const std::exception &e) {
							error = e.what();
						}
					}
					++total;
					if (!error.empty() || valid != test.at("valid").get<bool>()) {
						++failed;
						std::cout << argv[arg] << ": " << group.at("description") << " / " << test.at("description") << ": " << (error.empty() ? "incorrect validity" : error) << '\n';
					}
				}
			} else {
				excluded += group.at("tests").size();
			}
		}
	}
	std::cout << "RESULT dialect=" << dialect << " total=" << total << " passed=" << total - failed << " failed=" << failed << " inapplicable=" << excluded << '\n';
	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
