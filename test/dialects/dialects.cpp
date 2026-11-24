/*
 * JSON schema validator for JSON for modern C++
 * Copyright (c) 2026 Volker Christian.
 * SPDX-License-Identifier: MIT
 */
#include <nlohmann/json-schema.hpp>

#include <iostream>

using nlohmann::json;
using namespace nlohmann::json_schema;

static bool accepts(const json &schema, const json &instance, schema_dialect dialect = schema_dialect::draft7)
{
	json_validator validator;
	validator.set_default_dialect(dialect);
	validator.set_root_schema(schema);
	basic_error_handler errors;
	validator.validate(instance, errors);
	return !errors;
}

int main()
{
	bool ok = true;
	const auto modern = schema_dialect::draft2020_12;
	const json tuple = {{"prefixItems", {{{"type", "integer"}}}}, {"items", false}};
	ok = ok && accepts(tuple, {1}, modern) && !accepts(tuple, {1, 2}, modern);
	ok = ok && !accepts(tuple, {1}); // The default remains draft-07: items:false rejects every item.
	json declared = tuple;
	declared["$schema"] = "https://json-schema.org/draft/2020-12/schema";
	ok = ok && accepts(declared, {1});
	const json count = {{"contains", {{"type", "integer"}}}, {"minContains", 2}, {"maxContains", 2}};
	ok = ok && accepts(count, {1, "x", 2}, modern) && !accepts(count, {1}, modern) && !accepts(count, {1, 2, 3}, modern);
	const json siblings = {{"$defs", {{"value", {{"type", "integer"}}}}}, {"$ref", "#/$defs/value"}, {"minimum", 2}};
	ok = ok && accepts(siblings, 1) && !accepts(siblings, 1, modern) && accepts(siblings, 2, modern);
	const json dependencies = {{"dependentRequired", {{"a", {"b"}}}}, {"dependentSchemas", {{"a", {{"required", {"c"}}}}}}};
	ok = ok && !accepts(dependencies, {{"a", 1}, {"b", 2}}, modern) && accepts(dependencies, {{"a", 1}, {"b", 2}, {"c", 3}}, modern);
	if (!ok) {
		std::cerr << "dialect/applicator regression failed\n";
	}
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
