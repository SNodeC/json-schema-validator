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
	const json anchor = {{"$defs", {{"value", {{"$anchor", "value"}, {"type", "integer"}}}}}, {"$ref", "#value"}};
	ok = ok && accepts(anchor, 1, modern) && !accepts(anchor, "x", modern);
	const json dynamic = json::parse(R"({
		"$id":"https://example.test/strict", "$dynamicAnchor":"node",
		"properties":{"value":{"type":"integer"}}, "$ref":"tree",
		"$defs":{"tree":{"$id":"tree", "$dynamicAnchor":"node",
			"properties":{"children":{"items":{"$dynamicRef":"#node"}}}}}
	})");
	ok = ok && accepts(dynamic, {{"children", {{{"value", 1}}}}}, modern);
	ok = ok && !accepts(dynamic, {{"children", {{{"value", "bad"}}}}}, modern);
	bool unresolved = false;
	try {
		accepts({{"$ref", "https://example.invalid/not-loaded"}}, nullptr, modern);
	} catch (const std::invalid_argument &) {
		unresolved = true;
	}
	ok = ok && unresolved;
	json_validator bounded;
	validation_limits limits;
	limits.max_depth = 16;
	bounded.set_validation_limits(limits);
	bounded.set_root_schema(json{{"$ref", "#"}});
	bool exhausted = false;
	try {
		bounded.validate(nullptr);
	} catch (const std::invalid_argument &) {
		exhausted = true;
	}
	ok = ok && exhausted;
	const json evaluated = json::parse(R"({
		"anyOf":[{"properties":{"a":true}},{"properties":{"b":true}}],
		"unevaluatedProperties":false
	})");
	ok = ok && accepts(evaluated, {{"a", 1}, {"b", 2}}, modern) && !accepts(evaluated, {{"c", 1}}, modern);
	const json discarded = json::parse(R"({
		"anyOf":[{"properties":{"a":true},"required":["missing"]},{"properties":{"b":true}}],
		"unevaluatedProperties":false
	})");
	ok = ok && !accepts(discarded, {{"a", 1}, {"b", 2}}, modern);
	const json evaluated_items = {{"contains", {{"type", "integer"}}}, {"unevaluatedItems", false}};
	ok = ok && accepts(evaluated_items, {1, 2}, modern) && !accepts(evaluated_items, {1, "x"}, modern);
	json_validator schema_checker;
	schema_checker.set_default_dialect(modern);
	schema_checker.validate_schema(evaluated);
	bool malformed = false;
	try {
		schema_checker.validate_schema({{"minLength", -1}});
	} catch (const std::invalid_argument &) {
		malformed = true;
	}
	ok = ok && malformed;
	ok = ok && accepts(json::parse(R"({"properties":{"loop":{"$ref":"#/properties/loop"}}})"), json::object());
	json deep = true;
	for (int i = 0; i < 20; ++i) {
		deep = {{"properties", {{"nested", deep}}}};
	}
	bool compile_exhausted = false;
	try {
		bounded.set_root_schema(deep);
	} catch (const std::invalid_argument &) {
		compile_exhausted = true;
	}
	ok = ok && compile_exhausted;
	json_validator defaults;
	defaults.set_default_dialect(modern);
	defaults.set_root_schema(json::parse(R"({
		"properties":{"mqtt":{"$ref":"#/$defs/mqtt"}},
		"$defs":{"mqtt":{"type":"object","default":{"client_id":""}}}
	})"));
	const json empty = json::object();
	ok = empty.patch(defaults.validate(empty)) == json{{"mqtt", {{"client_id", ""}}}} && ok;
	defaults.set_root_schema(json::parse(R"({"properties":{"loop":{"$ref":"#/properties/loop"}}})"));
	ok = defaults.validate(empty).empty() && ok;
	if (!ok) {
		std::cerr << "dialect/applicator regression failed\n";
	}
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
