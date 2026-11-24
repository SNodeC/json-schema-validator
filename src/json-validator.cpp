/*
 * JSON schema validator for JSON for modern C++
 *
 * Copyright (c) 2016-2019 Patrick Boettcher <p@yai.se>.
 *
 * SPDX-License-Identifier: MIT
 *
 */
#include <nlohmann/json-schema.hpp>

#include "json-patch.hpp"

#include <deque>
#include <memory>
#include <set>
#include <sstream>
#include <string>

using nlohmann::json;
using nlohmann::json_patch;
using nlohmann::json_uri;
using nlohmann::json_schema::root_schema;
using namespace nlohmann::json_schema;

#include "json-schema-regex.hpp"

#define REGEX_NAMESPACE json_schema_regex

namespace
{

struct evaluated_locations {
	std::set<std::string> properties;
	std::set<std::size_t> items;
};

struct validation_context {
	std::size_t depth = 0;
	std::size_t evaluations = 0;
	std::vector<std::string> dynamic_scope;
	std::vector<std::map<std::string, evaluated_locations>> frames;
};

class schema
{
protected:
	root_schema *root_;
	json default_value_ = nullptr;
	std::string resource_;
	bool resource_set_ = false;

protected:
	virtual std::shared_ptr<schema> make_for_default_(
	    std::shared_ptr<::schema> & /* sch */,
	    root_schema * /* root */,
	    std::vector<nlohmann::json_uri> & /* uris */,
	    nlohmann::json & /* default_value */) const
	{
		return nullptr;
	};

public:
	virtual ~schema() = default;

	schema(root_schema *root)
	    : root_(root) {}

	virtual void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const = 0;

	void validate(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const;
	void set_resource(const std::string &resource)
	{
		resource_ = resource;
		resource_set_ = true;
	}

	virtual const json &default_value(const json::json_pointer &, const json &, error_handler &) const
	{
		return default_value_;
	}

	void set_default_value(const json &v) { default_value_ = v; }

	static std::shared_ptr<schema> make(json &schema,
	                                    root_schema *root,
	                                    const std::vector<std::string> &key,
	                                    std::vector<nlohmann::json_uri> uris);
};

class schema_ref : public schema
{
	const std::string id_;
	std::weak_ptr<schema> target_;
	std::shared_ptr<schema> target_strong_; // for references to references keep also the shared_ptr because
	                                        // no one else might use it after resolving

	std::string dynamic_name_;
	std::string dynamic_resource_;
	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &) const final;

	const json &default_value(const json::json_pointer &ptr, const json &instance, error_handler &e) const override final
	{
		const json *result = &default_value_;
		const schema_ref *reference = this;
		std::set<const schema_ref *> visited;
		while (reference && result->is_null() && visited.insert(reference).second) {
			result = &reference->default_value_;
			if (result->is_null()) {
				auto target = reference->target_.lock();
				if (target) {
					reference = dynamic_cast<const schema_ref *>(target.get());
					if (!reference) {
						result = &target->default_value(ptr, instance, e);
					}
				} else {
					e.error(ptr, instance, "unresolved or freed schema-reference " + reference->id_);
					reference = nullptr;
				}
			}
		}
		return *result;
	}

protected:
	virtual std::shared_ptr<schema> make_for_default_(
	    std::shared_ptr<::schema> &sch,
	    root_schema *root,
	    std::vector<nlohmann::json_uri> &uris,
	    nlohmann::json &default_value) const override
	{
		// create a new reference schema using the original reference (which will be resolved later)
		// to store this overloaded default value #209
		auto result = std::make_shared<schema_ref>(uris[0].to_string(), root);
		result->set_target(sch, true);
		result->set_default_value(default_value);
		return result;
	};

public:
	schema_ref(const std::string &id, root_schema *root)
	    : schema(root), id_(id) {}

	const std::string &id() const { return id_; }
	void set_dynamic(const json_uri &uri)
	{
		dynamic_name_ = uri.identifier();
		dynamic_resource_ = uri.location();
	}

	void set_target(const std::shared_ptr<schema> &target, bool strong = false)
	{
		target_ = target;
		if (strong)
			target_strong_ = target;
	}
};

} // namespace

namespace nlohmann
{
namespace json_schema
{

bool builtin_schema(const json_uri &, json &);

class root_schema
{
	bool format_assertion_ = false;
	std::set<std::string> vocabularies_{"core", "applicator", "validation", "unevaluated", "meta-data", "format-annotation", "content"};
	validation_limits limits_;
	std::size_t schema_count_ = 0;
	std::size_t compile_depth_ = 0;
	std::map<std::string, std::map<std::string, std::weak_ptr<schema>>> dynamic_anchors_;
	schema_dialect default_dialect_ = schema_dialect::draft7;
	schema_dialect dialect_ = schema_dialect::draft7;
	schema_loader loader_;
	format_checker format_check_;
	content_checker content_check_;

	std::shared_ptr<schema> root_;

	struct schema_file {
		std::map<std::string, std::shared_ptr<schema>> schemas;
		std::vector<std::shared_ptr<schema_ref>> references;
		std::map<std::string, std::shared_ptr<schema_ref>> unresolved; // contains all unresolved references from any other file seen during parsing
		json unknown_keywords;
	};

	// location as key
	std::map<std::string, schema_file> files_;

	schema_file &get_or_create_file(const std::string &loc)
	{
		auto file = files_.lower_bound(loc);
		if (file != files_.end() && !(files_.key_comp()(loc, file->first)))
			return file->second;
		else
			return files_.insert(file, {loc, {}})->second;
	}

public:
	root_schema(schema_loader &&loader,
	            format_checker &&format,
	            content_checker &&content)

	    : loader_(std::move(loader)),
	      format_check_(std::move(format)),
	      content_check_(std::move(content))
	{
	}

	void set_default_dialect(schema_dialect dialect) { default_dialect_ = dialect; }
	void set_validation_limits(const validation_limits &limits) { limits_ = limits; }
	void set_format_assertion(bool enabled) { format_assertion_ = enabled; }
	bool format_assertion() const { return dialect_ == schema_dialect::draft7 || format_assertion_ || vocabularies_.count("format-assertion") != 0; }
	const std::set<std::string> &vocabularies() const { return vocabularies_; }
	void set_vocabularies(std::set<std::string> value) { vocabularies_ = std::move(value); }
	bool enabled(const std::string &vocabulary) const { return dialect_ == schema_dialect::draft7 || vocabularies_.count(vocabulary) != 0; }

	void load_schema(const json_uri &uri, json &value) const
	{
		if (!builtin_schema(uri, value)) {
			if (loader_) {
				loader_(uri, value);
			} else {
				throw std::invalid_argument("external schema reference '" + uri.location() + "' needs loading, but no loader callback given");
			}
		}
	}

	void select_dialect(const std::string &declared, std::size_t depth = 0)
	{
		if (depth >= limits_.max_depth) {
			throw std::invalid_argument("metaschema dialect recursion limit exceeded");
		}
		if (declared == "http://json-schema.org/draft-07/schema" || declared == "https://json-schema.org/draft-07/schema" || declared == "http://json-schema.org/draft-04/schema") {
			dialect_ = schema_dialect::draft7;
		} else if (declared == "https://json-schema.org/draft/2020-12/schema") {
			dialect_ = schema_dialect::draft2020_12;
			vocabularies_ = {"core", "applicator", "validation", "unevaluated", "meta-data", "format-annotation", "content"};
		} else {
			json meta;
			load_schema(declared, meta);
			select_dialect(json_uri(meta.at("$schema").get<std::string>()).location(), depth + 1);
			if (meta.contains("$vocabulary")) {
				vocabularies_.clear();
				const std::string prefix = "https://json-schema.org/draft/2020-12/vocab/";
				const std::set<std::string> supported{"core", "applicator", "validation", "unevaluated", "meta-data", "format-annotation", "format-assertion", "content"};
				for (const auto &vocabulary : meta.at("$vocabulary").items()) {
					const auto name = vocabulary.key().substr(0, prefix.size()) == prefix ? vocabulary.key().substr(prefix.size()) : "";
					if (supported.count(name) != 0) {
						vocabularies_.insert(name);
					} else if (vocabulary.value().get<bool>()) {
						throw std::invalid_argument("unsupported required vocabulary: " + vocabulary.key());
					}
				}
			}
		}
	}

	void validate_schema(const json &value) const
	{
		std::string uri = default_dialect_ == schema_dialect::draft7 ? "http://json-schema.org/draft-07/schema" : "https://json-schema.org/draft/2020-12/schema";
		if (value.is_object() && value.contains("$schema")) {
			uri = value.at("$schema").get<std::string>();
		}
		json_validator validator(loader_, [](const std::string &, const std::string &) {});
		validator.set_default_dialect(default_dialect_);
		validator.set_validation_limits(limits_);
		validator.set_root_schema(json{{"$ref", uri}});
		validator.validate(value);
	}
	const validation_limits &limits() const { return limits_; }
	void count_schema()
	{
		if (compile_depth_ >= limits_.max_depth || schema_count_ >= limits_.max_schemas) {
			throw std::invalid_argument("schema compilation limit exceeded");
		}
		++schema_count_;
		++compile_depth_;
	}
	void leave_schema() { --compile_depth_; }
	void insert_dynamic(const std::string &resource, const std::string &name, const std::shared_ptr<schema> &value)
	{
		dynamic_anchors_[resource][name] = value;
	}
	std::shared_ptr<schema> dynamic_target(const std::string &resource, const std::string &name) const
	{
		std::shared_ptr<schema> result;
		const auto file = dynamic_anchors_.find(resource);
		if (file != dynamic_anchors_.end()) {
			const auto anchor = file->second.find(name);
			if (anchor != file->second.end()) {
				result = anchor->second.lock();
			}
		}
		return result;
	}
	schema_dialect dialect() const { return dialect_; }
	void set_dialect(schema_dialect dialect) { dialect_ = dialect; }

	format_checker &format_check() { return format_check_; }
	content_checker &content_check() { return content_check_; }

	void insert(const json_uri &uri, const std::shared_ptr<schema> &s)
	{
		auto &file = get_or_create_file(uri.location());
		auto sch = file.schemas.lower_bound(uri.fragment());
		if (sch != file.schemas.end() && !(file.schemas.key_comp()(uri.fragment(), sch->first))) {
			throw std::invalid_argument("schema with " + uri.to_string() + " already inserted");
			return;
		}

		file.schemas.insert({uri.fragment(), s});

		// was someone referencing this newly inserted schema?
		auto unresolved = file.unresolved.find(uri.fragment());
		if (unresolved != file.unresolved.end()) {
			unresolved->second->set_target(s);
			file.unresolved.erase(unresolved);
		}
	}

	void insert_unknown_keyword(const json_uri &uri, const std::string &key, json &value, std::size_t depth = 0)
	{
		if (depth >= limits_.max_depth) {
			throw std::invalid_argument("schema annotation depth limit exceeded");
		}
		auto &file = get_or_create_file(uri.location());
		auto new_uri = uri.append(key);
		auto fragment = new_uri.pointer();

		// is there a reference looking for this unknown-keyword, which is thus no longer a unknown keyword but a schema
		auto unresolved = file.unresolved.find(fragment.to_string());
		if (unresolved != file.unresolved.end())
			schema::make(value, this, {}, {{new_uri}});
		else { // no, nothing ref'd it, keep for later

			// need to create an object for each reference-token in the
			// JSON-Pointer When not existing, a stringified integer reference
			// token (e.g. "123") in the middle of the pointer will be
			// interpreted a an array-index and an array will be created.

			// json_pointer's reference_tokens is private - get them
			std::deque<std::string> ref_tokens;
			auto uri_pointer = uri.pointer();
			while (!uri_pointer.empty()) {
				ref_tokens.push_front(uri_pointer.back());
				uri_pointer.pop_back();
			}

			// for each token create an object, if not already existing
			auto unk_kw = &file.unknown_keywords;
			for (auto &rt : ref_tokens) {
				// create a json_pointer from rt as rt can be an stringified integer doing find on an array won't work
				// rt is an unescaped reference token (it may contain '/' or '~'), escape it
				json::json_pointer rt_ptr{"/" + json_uri::escape(rt)};
				if (unk_kw->contains(rt_ptr) == false)
					(*unk_kw)[rt] = json::object();
				unk_kw = &(*unk_kw)[rt_ptr];
			}
			(*unk_kw)[key] = value;
		}

		// recursively add possible subschemas of unknown keywords
		if (value.type() == json::value_t::object)
			for (auto &subsch : value.items())
				insert_unknown_keyword(new_uri, subsch.key(), subsch.value(), depth + 1);
	}

	std::shared_ptr<schema> get_or_create_ref(const json_uri &uri)
	{
		auto &file = get_or_create_file(uri.location());

		// existing schema
		auto sch = file.schemas.find(uri.fragment());
		if (sch != file.schemas.end())
			return sch->second;

		// get or create a schema_ref
		auto r = file.unresolved.lower_bound(uri.fragment());
		if (r != file.unresolved.end() && !(file.unresolved.key_comp()(uri.fragment(), r->first)))
			return r->second; // unresolved, already seen previously - use existing reference

		auto ref = file.unresolved.insert(r,
		                                  {uri.fragment(), std::make_shared<schema_ref>(uri.to_string(), this)})
		               ->second; // unresolved, create reference
		file.references.push_back(ref);

		// referencing an unknown keyword, turn it into schema
		//
		// an unknown keyword can only be referenced by a json-pointer,
		// not by a plain name fragment
		//
		// the reference has been created before: a recursive reference to this schema
		// found while creating it uses it, it is resolved when the schema is inserted
		if (!uri.pointer().to_string().empty() && file.unknown_keywords.contains(uri.pointer()))
			return schema::make(file.unknown_keywords.at(uri.pointer()), this, {}, {{uri}});

		return ref;
	}

	void set_root_schema(json sch)
	{
		files_.clear();
		dynamic_anchors_.clear();
		schema_count_ = 0;
		compile_depth_ = 0;
		dialect_ = default_dialect_;
		root_ = schema::make(sch, this, {}, {{"#"}});

		// load all files which have not yet been loaded
		do {
			bool new_schema_loaded = false;

			// files_ is modified during parsing, iterators are invalidated
			std::vector<std::string> locations;
			for (auto &file : files_)
				locations.push_back(file.first);

			for (auto &loc : locations) {
				if (files_[loc].schemas.size() == 0) { // nothing has been loaded for this file
					json loaded_schema;
					load_schema(loc, loaded_schema);
					schema::make(loaded_schema, this, {}, {{loc}});
					new_schema_loaded = true;
				}
			}

			if (!new_schema_loaded) // if no new schema loaded, no need to try again
				break;
		} while (1);

		for (const auto &file : files_) {
			if (file.second.unresolved.size() != 0) {
				// Build a representation of the undefined
				// references as a list of comma-separated strings.
				auto n_urefs = file.second.unresolved.size();
				std::string urefs = "[";

				decltype(n_urefs) counter = 0;
				for (const auto &p : file.second.unresolved) {
					urefs += p.first;

					if (counter != n_urefs - 1u) {
						urefs += ", ";
					}

					++counter;
				}

				urefs += "]";

				throw std::invalid_argument("after all files have been parsed, '" +
				                            (file.first == "" ? "<root>" : file.first) +
				                            "' has still the following undefined references: " + urefs);
			}
		}
	}

	void validate(const json::json_pointer &ptr,
	              const json &instance,
	              json_patch &patch,
	              error_handler &e,
	              const json_uri &initial, validation_context &context) const
	{
		if (!root_) {
			e.error(ptr, "", "no root schema has yet been set for validating an instance");
			return;
		}

		auto file_entry = files_.find(initial.location());
		if (file_entry == files_.end()) {
			e.error(ptr, "", "no file found serving requested root-URI. " + initial.location());
			return;
		}

		auto &file = file_entry->second;
		auto sch = file.schemas.find(initial.fragment());
		if (sch == file.schemas.end()) {
			e.error(ptr, "", "no schema find for request initial URI: " + initial.to_string());
			return;
		}

		sch->second->validate(ptr, instance, patch, e, context);
	}
};

} // namespace json_schema
} // namespace nlohmann

namespace
{

void schema::validate(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const
{
	if (context.depth >= root_->limits().max_depth || ++context.evaluations > root_->limits().max_evaluations) {
		throw std::invalid_argument("schema evaluation limit exceeded");
	}
	const bool enter_resource = resource_set_ && (context.dynamic_scope.empty() || context.dynamic_scope.back() != resource_);
	if (enter_resource) {
		context.dynamic_scope.push_back(resource_);
	}
	++context.depth;
	context.frames.emplace_back();
	struct counting_error_handler : error_handler {
		error_handler &destination;
		bool failed = false;
		explicit counting_error_handler(error_handler &handler) : destination(handler) {}
		void error(const json::json_pointer &where, const json &value, const std::string &message) override
		{
			failed = true;
			destination.error(where, value, message);
		}
	} errors(e);
	struct scope_exit {
		validation_context &context;
		bool resource;
		~scope_exit()
		{
			--context.depth;
			context.frames.pop_back();
			if (resource) {
				context.dynamic_scope.pop_back();
			}
		}
	} scope{context, enter_resource};
	validate_(ptr, instance, patch, errors, context);
	if (!errors.failed && context.frames.size() > 1) {
		auto &parent = context.frames[context.frames.size() - 2];
		for (const auto &entry : context.frames.back()) {
			auto &annotations = parent[entry.first];
			annotations.properties.insert(entry.second.properties.begin(), entry.second.properties.end());
			annotations.items.insert(entry.second.items.begin(), entry.second.items.end());
		}
	}
}

void schema_ref::validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const
{
	auto target = target_.lock();
	if (!dynamic_name_.empty() && root_->dynamic_target(dynamic_resource_, dynamic_name_)) {
		bool found = false;
		for (std::size_t i = 0; i < context.dynamic_scope.size() && !found; ++i) {
			auto dynamic = root_->dynamic_target(context.dynamic_scope[i], dynamic_name_);
			if (dynamic) {
				target = std::move(dynamic);
				found = true;
			}
		}
	}
	if (target) {
		target->validate(ptr, instance, patch, e, context);
	} else {
		e.error(ptr, instance, "unresolved or freed schema-reference " + id_);
	}
}

class first_error_handler : public error_handler
{
public:
	bool error_{false};
	json::json_pointer ptr_;
	json instance_;
	std::string message_;

	void error(const json::json_pointer &ptr, const json &instance, const std::string &message) override
	{
		if (*this)
			return;
		error_ = true;
		ptr_ = ptr;
		instance_ = instance;
		message_ = message;
	}

	operator bool() const { return error_; }
};

class logical_not : public schema
{
	std::shared_ptr<schema> subschema_;

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const final
	{
		first_error_handler esub;
		subschema_->validate(ptr, instance, patch, esub, context);

		if (!esub)
			e.error(ptr, instance, "the subschema has succeeded, but it is required to not validate");
	}

public:
	logical_not(json &sch,
	            root_schema *root,
	            const std::vector<nlohmann::json_uri> &uris)
	    : schema(root)
	{
		subschema_ = schema::make(sch, root, {"not"}, uris);
	}
};

enum logical_combination_types {
	allOf,
	anyOf,
	oneOf
};

class logical_combination_error_handler : public error_handler
{
public:
	struct error_entry {
		json::json_pointer ptr_;
		json instance_;
		std::string message_;
	};

	std::vector<error_entry> error_entry_list_;

	void error(const json::json_pointer &ptr, const json &instance, const std::string &message) override
	{
		error_entry_list_.push_back(error_entry{ptr, instance, message});
	}

	void propagate(error_handler &e, const std::string &prefix) const
	{
		for (const error_entry &entry : error_entry_list_)
			e.error(entry.ptr_, entry.instance_, prefix + entry.message_);
	}

	operator bool() const { return !error_entry_list_.empty(); }
};

template <enum logical_combination_types combine_logic>
class logical_combination : public schema
{
	std::vector<std::shared_ptr<schema>> subschemata_;

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const final
	{
		size_t count = 0;
		bool reported = false;
		logical_combination_error_handler error_summary;

		for (std::size_t index = 0; index < subschemata_.size(); ++index) {
			const std::shared_ptr<schema> &s = subschemata_[index];
			logical_combination_error_handler esub;
			auto oldPatchSize = patch.get_json().size();
			s->validate(ptr, instance, patch, esub, context);
			if (!esub)
				count++;
			else {
				patch.get_json().get_ref<nlohmann::json::array_t &>().resize(oldPatchSize);
				esub.propagate(error_summary, "case#" + std::to_string(index) + "] ");
			}

			if (!reported) {
				reported = is_validate_complete(instance, ptr, e, esub, count, index);
			}
		}

		if (count == 0 && !reported) {
			e.error(ptr, instance, "no subschema has succeeded, but one of them is required to validate. Type: " + key + ", number of failed subschemas: " + std::to_string(subschemata_.size()));
			error_summary.propagate(e, "[combination: " + key + " / ");
		}
	}

	// specialized for each of the logical_combination_types
	static const std::string key;
	static bool is_validate_complete(const json &, const json::json_pointer &, error_handler &, const logical_combination_error_handler &, size_t, size_t);

public:
	logical_combination(json &sch,
	                    root_schema *root,
	                    const std::vector<nlohmann::json_uri> &uris)
	    : schema(root)
	{
		size_t c = 0;
		for (auto &subschema : sch)
			subschemata_.push_back(schema::make(subschema, root, {key, std::to_string(c++)}, uris));

		// value of allOf, anyOf, and oneOf "MUST be a non-empty array"
		// TODO error/throw? when subschemata_.empty()
	}
};

template <>
const std::string logical_combination<allOf>::key = "allOf";
template <>
const std::string logical_combination<anyOf>::key = "anyOf";
template <>
const std::string logical_combination<oneOf>::key = "oneOf";

template <>
bool logical_combination<allOf>::is_validate_complete(const json &, const json::json_pointer &, error_handler &e, const logical_combination_error_handler &esub, size_t, size_t current_schema_index)
{
	if (esub) {
		e.error(esub.error_entry_list_.front().ptr_, esub.error_entry_list_.front().instance_, "at least one subschema has failed, but all of them are required to validate - " + esub.error_entry_list_.front().message_);
		esub.propagate(e, "[combination: allOf / case#" + std::to_string(current_schema_index) + "] ");
	}
	return esub;
}

template <>
bool logical_combination<anyOf>::is_validate_complete(const json &, const json::json_pointer &, error_handler &, const logical_combination_error_handler &, size_t count, size_t)
{
	return count == 1;
}

template <>
bool logical_combination<oneOf>::is_validate_complete(const json &instance, const json::json_pointer &ptr, error_handler &e, const logical_combination_error_handler &, size_t count, size_t)
{
	if (count > 1)
		e.error(ptr, instance, "more than one subschema has succeeded, but exactly one of them is required to validate");
	return count > 1;
}

class type_schema : public schema
{
	std::vector<std::shared_ptr<schema>> type_;
	std::pair<bool, json> enum_, const_;
	std::vector<std::shared_ptr<schema>> logic_;
	std::shared_ptr<schema> reference_;
	std::shared_ptr<schema> unevaluated_properties_;
	std::shared_ptr<schema> unevaluated_items_;
	std::shared_ptr<schema> integer_float_;

	static std::shared_ptr<schema> make(json &schema,
	                                    json::value_t type,
	                                    root_schema *,
	                                    const std::vector<nlohmann::json_uri> &,
	                                    std::set<std::string> &);

	std::shared_ptr<schema> if_, then_, else_;

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const override final
	{
		if (reference_) {
			reference_->validate(ptr, instance, patch, e, context);
		}

		// depending on the type of instance run the type specific validator - if present
		auto type = type_[static_cast<uint8_t>(instance.type())];
		if (!type && instance.is_number_float()) {
			const double value = instance.get<double>();
			if (std::isfinite(value) && std::floor(value) == value) {
				type = integer_float_;
			}
		}

		if (type)
			type->validate(ptr, instance, patch, e, context);
		else
			e.error(ptr, instance, "unexpected instance type");

		if (enum_.first) {
			bool seen_in_enum = false;
			for (auto &v : enum_.second)
				if (instance == v) {
					seen_in_enum = true;
					break;
				}

			if (!seen_in_enum)
				e.error(ptr, instance, "instance not found in required enum");
		}

		if (const_.first &&
		    const_.second != instance)
			e.error(ptr, instance, "instance not const");

		for (auto l : logic_)
			l->validate(ptr, instance, patch, e, context);

		if (if_) {
			first_error_handler err;

			if_->validate(ptr, instance, patch, err, context);
			if (!err) {
				if (then_)
					then_->validate(ptr, instance, patch, e, context);
			} else {
				if (else_)
					else_->validate(ptr, instance, patch, e, context);
			}
		}
		if (unevaluated_properties_ && instance.is_object()) {
			for (const auto &property : instance.items()) {
				if (context.frames.back()[ptr.to_string()].properties.count(property.key()) == 0) {
					unevaluated_properties_->validate(ptr / property.key(), property.value(), patch, e, context);
					context.frames.back()[ptr.to_string()].properties.insert(property.key());
				}
			}
		}
		if (unevaluated_items_ && instance.is_array()) {
			for (std::size_t index = 0; index < instance.size(); ++index) {
				if (context.frames.back()[ptr.to_string()].items.count(index) == 0) {
					unevaluated_items_->validate(ptr / index, instance[index], patch, e, context);
					context.frames.back()[ptr.to_string()].items.insert(index);
				}
			}
		}

		// an empty (null) document is replaced by the root's default value (issue-243),
		// null values inside a document are values and are kept
		if (ptr.empty() && instance.is_null() && !default_value_.is_null())
			patch.add(ptr, default_value_);
	}

protected:
	virtual std::shared_ptr<schema> make_for_default_(
	    std::shared_ptr<::schema> & /* sch */,
	    root_schema * /* root */,
	    std::vector<nlohmann::json_uri> & /* uris */,
	    nlohmann::json &default_value) const override
	{
		auto result = std::make_shared<type_schema>(*this);
		result->set_default_value(default_value);
		return result;
	};

public:
	void set_reference(std::shared_ptr<schema> reference) { reference_ = std::move(reference); }

	type_schema(json &sch,
	            root_schema *root,
	            const std::vector<nlohmann::json_uri> &uris)
	    : schema(root), type_(static_cast<uint8_t>(json::value_t::discarded) + 1)
	{
		if (root->dialect() == schema_dialect::draft2020_12) {
			if (sch.contains("unevaluatedProperties")) {
				unevaluated_properties_ = schema::make(sch.at("unevaluatedProperties"), root, {"unevaluatedProperties"}, uris);
				sch.erase("unevaluatedProperties");
			}
			if (sch.contains("unevaluatedItems")) {
				unevaluated_items_ = schema::make(sch.at("unevaluatedItems"), root, {"unevaluatedItems"}, uris);
				sch.erase("unevaluatedItems");
			}
		}
		// association between JSON-schema-type and NLohmann-types
		static const struct {
			const char *name;
			json::value_t type;
		} schema_types[] = {
		    {"null", json::value_t::null},
		    {"object", json::value_t::object},
		    {"array", json::value_t::array},
		    {"string", json::value_t::string},
		    {"boolean", json::value_t::boolean},
		    {"integer", json::value_t::number_integer},
		    {"number", json::value_t::number_float},
		};

		std::set<std::string> known_keywords;

		auto attr = sch.find("type");
		if (attr == sch.end()) // no type field means all sub-types possible
			for (auto &t : schema_types)
				type_[static_cast<uint8_t>(t.type)] = type_schema::make(sch, t.type, root, uris, known_keywords);
		else {
			switch (attr.value().type()) { // "type": "type"

			case json::value_t::string: {
				auto schema_type = attr.value().get<std::string>();
				for (auto &t : schema_types)
					if (schema_type == t.name)
						type_[static_cast<uint8_t>(t.type)] = type_schema::make(sch, t.type, root, uris, known_keywords);
			} break;

			case json::value_t::array: // "type": ["type1", "type2"]
				for (auto &array_value : attr.value()) {
					auto schema_type = array_value.get<std::string>();
					for (auto &t : schema_types)
						if (schema_type == t.name)
							type_[static_cast<uint8_t>(t.type)] = type_schema::make(sch, t.type, root, uris, known_keywords);
				}
				break;

			default:
				break;
			}

			sch.erase(attr);
		}

		attr = sch.find("default");
		if (attr != sch.end()) {
			set_default_value(attr.value());
			sch.erase(attr);
		}

		if (type_[static_cast<uint8_t>(json::value_t::number_integer)] && !type_[static_cast<uint8_t>(json::value_t::number_float)]) {
			integer_float_ = type_schema::make(sch, json::value_t::number_float, root, uris, known_keywords);
		}
		for (auto &key : known_keywords)
			sch.erase(key);

		// with nlohmann::json float instance (but number in schema-definition) can be seen as unsigned or integer -
		// reuse the number-validator for integer values as well, if they have not been specified explicitly
		if (type_[static_cast<uint8_t>(json::value_t::number_float)] && !type_[static_cast<uint8_t>(json::value_t::number_integer)])
			type_[static_cast<uint8_t>(json::value_t::number_integer)] = type_[static_cast<uint8_t>(json::value_t::number_float)];

		// #54: JSON-schema does not differentiate between unsigned and signed integer - nlohmann::json does
		// we stick with JSON-schema: use the integer-validator if instance-value is unsigned
		type_[static_cast<uint8_t>(json::value_t::number_unsigned)] = type_[static_cast<uint8_t>(json::value_t::number_integer)];

		// special for binary types
		if (type_[static_cast<uint8_t>(json::value_t::string)]) {
			type_[static_cast<uint8_t>(json::value_t::binary)] = type_[static_cast<uint8_t>(json::value_t::string)];
		}

		attr = sch.find("enum");
		if (attr != sch.end()) {
			enum_ = {true, attr.value()};
			sch.erase(attr);
		}

		attr = sch.find("const");
		if (attr != sch.end()) {
			const_ = {true, attr.value()};
			sch.erase(attr);
		}

		attr = sch.find("not");
		if (attr != sch.end()) {
			logic_.push_back(std::make_shared<logical_not>(attr.value(), root, uris));
			sch.erase(attr);
		}

		attr = sch.find("allOf");
		if (attr != sch.end()) {
			logic_.push_back(std::make_shared<logical_combination<allOf>>(attr.value(), root, uris));
			sch.erase(attr);
		}

		attr = sch.find("anyOf");
		if (attr != sch.end()) {
			logic_.push_back(std::make_shared<logical_combination<anyOf>>(attr.value(), root, uris));
			sch.erase(attr);
		}

		attr = sch.find("oneOf");
		if (attr != sch.end()) {
			logic_.push_back(std::make_shared<logical_combination<oneOf>>(attr.value(), root, uris));
			sch.erase(attr);
		}

		// if, then and else are compiled even when they have no effect alone, they can be referenced
		attr = sch.find("if");
		if (attr != sch.end()) {
			if_ = schema::make(attr.value(), root, {"if"}, uris);
			sch.erase(attr);
		}

		attr = sch.find("then");
		if (attr != sch.end()) {
			then_ = schema::make(attr.value(), root, {"then"}, uris);
			sch.erase(attr);
		}

		attr = sch.find("else");
		if (attr != sch.end()) {
			else_ = schema::make(attr.value(), root, {"else"}, uris);
			sch.erase(attr);
		}
	}
};

class string : public schema
{
	std::pair<bool, size_t> maxLength_{false, 0};
	std::pair<bool, size_t> minLength_{false, 0};

#ifndef NO_STD_REGEX
	std::pair<bool, REGEX_NAMESPACE::regex> pattern_{false, REGEX_NAMESPACE::regex()};
	std::string patternString_;
#endif

	std::pair<bool, std::string> format_;
	std::tuple<bool, std::string, std::string> content_{false, "", ""};

	std::size_t utf8_length(const std::string &s) const
	{
		size_t len = 0;
		for (auto c : s)
			if ((c & 0xc0) != 0x80)
				len++;
		return len;
	}

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &, error_handler &e, validation_context &) const override
	{
		if (minLength_.first) {
			if (utf8_length(instance.get<std::string>()) < minLength_.second) {
				std::ostringstream s;
				s << "instance is too short as per minLength:" << minLength_.second;
				e.error(ptr, instance, s.str());
			}
		}

		if (maxLength_.first) {
			if (utf8_length(instance.get<std::string>()) > maxLength_.second) {
				std::ostringstream s;
				s << "instance is too long as per maxLength: " << maxLength_.second;
				e.error(ptr, instance, s.str());
			}
		}

		// the checkers are present, the constructor checks it
		if (std::get<0>(content_)) {
			try {
				root_->content_check()(std::get<1>(content_), std::get<2>(content_), instance);
			} catch (const std::exception &ex) {
				e.error(ptr, instance, std::string("content-checking failed: ") + ex.what());
			}
		} else if (instance.type() == json::value_t::binary) {
			e.error(ptr, instance, "expected string, but get binary data");
		}

		if (instance.type() != json::value_t::string) {
			return; // next checks only for strings
		}

#ifndef NO_STD_REGEX
		if (pattern_.first &&
		    !REGEX_NAMESPACE::regex_search(instance.get<std::string>(), pattern_.second))
			e.error(ptr, instance, "instance does not match regex pattern: " + patternString_);
#endif

		if (format_.first) {
			try {
				root_->format_check()(format_.second, instance.get<std::string>());
			} catch (const std::exception &ex) {
				e.error(ptr, instance, std::string("format-checking failed: ") + ex.what());
			}
		}
	}

public:
	string(json &sch, root_schema *root)
	    : schema(root)
	{
		auto attr = sch.find("maxLength");
		if (attr != sch.end()) {
			maxLength_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("minLength");
		if (attr != sch.end()) {
			minLength_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("contentEncoding");
		if (attr != sch.end()) {
			std::get<0>(content_) = true;
			std::get<1>(content_) = attr.value().get<std::string>();

			// special case for nlohmann::json-binary-types
			//
			// https://github.com/pboettch/json-schema-validator/pull/114
			//
			// We cannot use explicitly in a schema: {"type": "binary"} or
			// "type": ["binary", "number"] we have to be implicit. For a
			// schema where "contentEncoding" is set to "binary", an instance
			// of type json::value_t::binary is accepted. If a
			// contentEncoding-callback has to be provided and is called
			// accordingly. For encoding=binary, no other type validations are done

			sch.erase(attr);
		}

		attr = sch.find("contentMediaType");
		if (attr != sch.end()) {
			std::get<0>(content_) = true;
			std::get<2>(content_) = attr.value().get<std::string>();

			sch.erase(attr);
		}

		if (root->dialect() == schema_dialect::draft2020_12 && root->content_check() == nullptr) {
			std::get<0>(content_) = false;
		}
		if (std::get<0>(content_) == true && root_->content_check() == nullptr) {
			throw std::invalid_argument{"schema contains contentEncoding/contentMediaType but content checker was not set"};
		}

#ifndef NO_STD_REGEX
		attr = sch.find("pattern");
		if (attr != sch.end()) {
			patternString_ = attr.value().get<std::string>();
			pattern_ = {true, REGEX_NAMESPACE::regex(attr.value().get<std::string>(),
			                                         REGEX_NAMESPACE::regex::ECMAScript)};
			sch.erase(attr);
		}
#endif

		attr = sch.find("format");
		if (attr != sch.end() && root->format_assertion()) {
			if (root_->format_check() == nullptr)
				throw std::invalid_argument{"a format checker was not provided but a format keyword for this string is present: " + attr.value().get<std::string>()};

			format_ = {true, attr.value().get<std::string>()};
			sch.erase(attr);
		}
	}
};

template <typename T>
class numeric : public schema
{
	std::pair<bool, T> maximum_{false, 0};
	std::pair<bool, T> minimum_{false, 0};

	bool exclusiveMaximum_ = false;
	bool exclusiveMinimum_ = false;

	std::pair<bool, json::number_float_t> multipleOf_{false, 0};

	// multipleOf - if the remainder of the division is 0 -> OK
	bool violates_multiple_of(T x) const
	{
		double res = std::remainder(x, multipleOf_.second);
		double multiple = std::fabs(static_cast<double>(x) / multipleOf_.second);
		if (std::isfinite(multiple) && multiple > 1) {
			res = res / multiple;
		}
		double eps = std::nextafter(x, 0) - static_cast<double>(x);

		return std::isfinite(multiple) ? std::fabs(res) > std::fabs(eps) : res != 0;
	}

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &, error_handler &e, validation_context &) const override
	{
		T value = instance.get<T>(); // conversion of json to value_type

		std::ostringstream oss;

		if (multipleOf_.first && value != 0) // zero is multiple of everything
			if (violates_multiple_of(value))
				oss << "instance is not a multiple of " << json(multipleOf_.second);

		if (maximum_.first) {
			if (exclusiveMaximum_ && value >= maximum_.second)
				oss << "instance exceeds or equals maximum of " << json(maximum_.second);
			else if (value > maximum_.second)
				oss << "instance exceeds maximum of " << json(maximum_.second);
		}

		if (minimum_.first) {
			if (exclusiveMinimum_ && value <= minimum_.second)
				oss << "instance is below or equals minimum of " << json(minimum_.second);
			else if (value < minimum_.second)
				oss << "instance is below minimum of " << json(minimum_.second);
		}

		oss.seekp(0, std::ios::end);
		auto size = oss.tellp();
		if (size != 0) {
			oss.seekp(0, std::ios::beg);
			e.error(ptr, instance, oss.str());
		}
	}

public:
	numeric(const json &sch, root_schema *root, std::set<std::string> &kw)
	    : schema(root)
	{
		auto attr = sch.find("maximum");
		if (attr != sch.end()) {
			maximum_ = {true, attr.value().get<T>()};
			kw.insert("maximum");
		}

		attr = sch.find("minimum");
		if (attr != sch.end()) {
			minimum_ = {true, attr.value().get<T>()};
			kw.insert("minimum");
		}

		attr = sch.find("exclusiveMaximum");
		if (attr != sch.end()) {
			exclusiveMaximum_ = true;
			maximum_ = {true, attr.value().get<T>()};
			kw.insert("exclusiveMaximum");
		}

		attr = sch.find("exclusiveMinimum");
		if (attr != sch.end()) {
			exclusiveMinimum_ = true;
			minimum_ = {true, attr.value().get<T>()};
			kw.insert("exclusiveMinimum");
		}

		attr = sch.find("multipleOf");
		if (attr != sch.end()) {
			multipleOf_ = {true, attr.value().get<json::number_float_t>()};
			kw.insert("multipleOf");
		}
	}
};

class null : public schema
{
	// only used for null instances (type_schema), nothing to check
	void validate_(const json::json_pointer &, const json &, json_patch &, error_handler &, validation_context &) const override {}

public:
	null(json &, root_schema *root)
	    : schema(root) {}
};

class boolean_type : public schema
{
	void validate_(const json::json_pointer &, const json &, json_patch &, error_handler &, validation_context &) const override {}

public:
	boolean_type(json &, root_schema *root)
	    : schema(root) {}
};

class boolean : public schema
{
	bool true_;
	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &, error_handler &e, validation_context &) const override
	{
		if (!true_) { // false schema
			// empty array
			// switch (instance.type()) {
			// case json::value_t::array:
			//	if (instance.size() != 0) // valid false-schema
			//		e.error(ptr, instance, "false-schema required empty array");
			//	return;
			//}

			e.error(ptr, instance, "instance invalid as per false-schema");
		}
	}

protected:
	virtual std::shared_ptr<schema> make_for_default_(
	    std::shared_ptr<::schema> & /* sch */,
	    root_schema * /* root */,
	    std::vector<nlohmann::json_uri> & /* uris */,
	    nlohmann::json &default_value) const override
	{
		auto result = std::make_shared<boolean>(*this);
		result->set_default_value(default_value);
		return result;
	}

public:
	boolean(json &sch, root_schema *root)
	    : schema(root), true_(sch) {}
};

class required : public schema
{
	const std::vector<std::string> required_;

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &, error_handler &e, validation_context &) const override final
	{
		for (auto &r : required_)
			if (instance.find(r) == instance.end())
				e.error(ptr, instance, "required property '" + r + "' not found in object as a dependency");
	}

public:
	required(const std::vector<std::string> &r, root_schema *root)
	    : schema(root), required_(r) {}
};

class object : public schema
{
	std::pair<bool, size_t> maxProperties_{false, 0};
	std::pair<bool, size_t> minProperties_{false, 0};
	std::vector<std::string> required_;

	std::map<std::string, std::shared_ptr<schema>> properties_;
#ifndef NO_STD_REGEX
	std::vector<std::pair<REGEX_NAMESPACE::regex, std::shared_ptr<schema>>> patternProperties_;
#endif
	std::shared_ptr<schema> additionalProperties_;

	std::multimap<std::string, std::shared_ptr<schema>> dependencies_;

	std::shared_ptr<schema> propertyNames_;

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const override
	{
		if (maxProperties_.first && instance.size() > maxProperties_.second)
			e.error(ptr, instance, "too many properties");

		if (minProperties_.first && instance.size() < minProperties_.second)
			e.error(ptr, instance, "too few properties");

		for (auto &r : required_)
			if (instance.find(r) == instance.end())
				e.error(ptr, instance, "required property '" + r + "' not found in object");

		// for each property in instance
		for (auto &p : instance.items()) {
			if (propertyNames_)
				propertyNames_->validate(ptr, p.key(), patch, e, context);

			bool a_prop_or_pattern_matched = false;
			auto schema_p = properties_.find(p.key());
			// check if it is in "properties"
			if (schema_p != properties_.end()) {
				a_prop_or_pattern_matched = true;
				schema_p->second->validate(ptr / p.key(), p.value(), patch, e, context);
				context.frames.back()[ptr.to_string()].properties.insert(p.key());
			}

#ifndef NO_STD_REGEX
			// check all matching patternProperties
			for (auto &schema_pp : patternProperties_)
				if (REGEX_NAMESPACE::regex_search(p.key(), schema_pp.first)) {
					a_prop_or_pattern_matched = true;
					schema_pp.second->validate(ptr / p.key(), p.value(), patch, e, context);
					context.frames.back()[ptr.to_string()].properties.insert(p.key());
				}
#endif

			// check additionalProperties as a last resort
			if (!a_prop_or_pattern_matched && additionalProperties_) {
				first_error_handler additional_prop_err;
				additionalProperties_->validate(ptr / p.key(), p.value(), patch, additional_prop_err, context);
				context.frames.back()[ptr.to_string()].properties.insert(p.key());
				if (additional_prop_err)
					e.error(ptr, instance, "validation failed for additional property '" + p.key() + "': " + additional_prop_err.message_);
			}
		}

		// reverse search
		for (auto const &prop : properties_) {
			const auto finding = instance.find(prop.first);
			if (instance.end() == finding) { // if the prop is not in the instance
				const auto &default_value = prop.second->default_value(ptr, instance, e);
				if (!default_value.is_null()) { // if default value is available
					patch.add((ptr / prop.first), default_value);
				}
			}
		}

		for (auto &dep : dependencies_) {
			auto prop = instance.find(dep.first);
			if (prop != instance.end())                                 // if dependency-property is present in instance
				dep.second->validate(ptr, instance, patch, e, context); // validate
		}
	}

public:
	object(json &sch,
	       root_schema *root,
	       const std::vector<nlohmann::json_uri> &uris)
	    : schema(root)
	{
		auto attr = sch.find("maxProperties");
		if (attr != sch.end()) {
			maxProperties_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("minProperties");
		if (attr != sch.end()) {
			minProperties_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("required");
		if (attr != sch.end()) {
			required_ = attr.value().get<std::vector<std::string>>();
			sch.erase(attr);
		}

		attr = sch.find("properties");
		if (attr != sch.end()) {
			for (auto prop : attr.value().items())
				properties_.insert(
				    std::make_pair(
				        prop.key(),
				        schema::make(prop.value(), root, {"properties", prop.key()}, uris)));
			sch.erase(attr);
		}

#ifndef NO_STD_REGEX
		attr = sch.find("patternProperties");
		if (attr != sch.end()) {
			for (auto prop : attr.value().items())
				patternProperties_.push_back(
				    std::make_pair(
				        REGEX_NAMESPACE::regex(prop.key(), REGEX_NAMESPACE::regex::ECMAScript),
				        schema::make(prop.value(), root, {prop.key()}, uris)));
			sch.erase(attr);
		}
#endif

		attr = sch.find("additionalProperties");
		if (attr != sch.end()) {
			additionalProperties_ = schema::make(attr.value(), root, {"additionalProperties"}, uris);
			sch.erase(attr);
		}

		const std::string dependencies = root->dialect() == schema_dialect::draft7 ? "dependencies" : "dependentSchemas";
		attr = sch.find(dependencies);
		if (attr != sch.end()) {
			for (auto &dep : attr.value().items())
				switch (dep.value().type()) {
				case json::value_t::array:
					dependencies_.emplace(dep.key(),
					                      std::make_shared<required>(
					                          dep.value().get<std::vector<std::string>>(), root));
					break;

				default:
					dependencies_.emplace(dep.key(),
					                      schema::make(dep.value(), root, {dependencies, dep.key()}, uris));
					break;
				}
			sch.erase(attr);
		}

		if (root->dialect() == schema_dialect::draft2020_12 && sch.contains("dependentRequired")) {
			for (auto &dep : sch.at("dependentRequired").items()) {
				dependencies_.emplace(dep.key(), std::make_shared<required>(dep.value().get<std::vector<std::string>>(), root));
			}
			sch.erase("dependentRequired");
		}

		attr = sch.find("propertyNames");
		if (attr != sch.end()) {
			propertyNames_ = schema::make(attr.value(), root, {"propertyNames"}, uris);
			sch.erase(attr);
		}

		attr = sch.find("default");
		if (attr != sch.end()) {
			set_default_value(*attr);
		}
	}
};

class array : public schema
{
	std::pair<bool, size_t> maxItems_{false, 0};
	std::pair<bool, size_t> minItems_{false, 0};
	bool uniqueItems_ = false;

	std::shared_ptr<schema> items_schema_;

	std::vector<std::shared_ptr<schema>> items_;
	std::shared_ptr<schema> additionalItems_;

	std::shared_ptr<schema> contains_;
	std::size_t minContains_ = 1;
	std::pair<bool, std::size_t> maxContains_{false, 0};

	void validate_(const json::json_pointer &ptr, const json &instance, json_patch &patch, error_handler &e, validation_context &context) const override
	{
		if (maxItems_.first && instance.size() > maxItems_.second)
			e.error(ptr, instance, "array has too many items");

		if (minItems_.first && instance.size() < minItems_.second)
			e.error(ptr, instance, "array has too few items");

		if (uniqueItems_) {
			for (auto it = instance.cbegin(); it != instance.cend(); ++it) {
				auto v = std::find(it + 1, instance.end(), *it);
				if (v != instance.end())
					e.error(ptr, instance, "items have to be unique for this array");
			}
		}

		size_t index = 0;
		if (items_schema_)
			for (auto &i : instance) {
				items_schema_->validate(ptr / index, i, patch, e, context);
				context.frames.back()[ptr.to_string()].items.insert(index++);
			}
		else {
			auto item = items_.cbegin();
			for (auto &i : instance) {
				std::shared_ptr<schema> item_validator;
				if (item == items_.cend())
					item_validator = additionalItems_;
				else {
					item_validator = *item;
					item++;
				}

				if (!item_validator)
					break;

				item_validator->validate(ptr / index, i, patch, e, context);
				context.frames.back()[ptr.to_string()].items.insert(index++);
			}
		}

		if (contains_) {
			std::size_t count = 0;
			std::size_t contains_index = 0;
			for (auto &item : instance) {
				first_error_handler local_e;
				contains_->validate(ptr / contains_index, item, patch, local_e, context);
				if (!local_e) {
					++count;
					context.frames.back()[ptr.to_string()].items.insert(contains_index);
				}
				++contains_index;
			}
			if (count < minContains_) {
				e.error(ptr, instance, "array does not contain required element as per 'contains'");
			}
			if (maxContains_.first && count > maxContains_.second) {
				e.error(ptr, instance, "array has too many matching elements as per 'maxContains'");
			}
		}
	}

public:
	array(json &sch, root_schema *root, const std::vector<nlohmann::json_uri> &uris)
	    : schema(root)
	{
		auto attr = sch.find("maxItems");
		if (attr != sch.end()) {
			maxItems_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("minItems");
		if (attr != sch.end()) {
			minItems_ = {true, attr.value().get<size_t>()};
			sch.erase(attr);
		}

		attr = sch.find("uniqueItems");
		if (attr != sch.end()) {
			uniqueItems_ = attr.value().get<bool>();
			sch.erase(attr);
		}

		if (root->dialect() == schema_dialect::draft2020_12) {
			if (sch.contains("prefixItems")) {
				std::size_t index = 0;
				for (auto &subsch : sch.at("prefixItems")) {
					items_.push_back(schema::make(subsch, root, {"prefixItems", std::to_string(index++)}, uris));
				}
				sch.erase("prefixItems");
			}
			if (sch.contains("items")) {
				additionalItems_ = schema::make(sch.at("items"), root, {"items"}, uris);
				sch.erase("items");
			}
			minContains_ = sch.value("minContains", std::size_t(1));
			if (sch.contains("maxContains")) {
				maxContains_ = {true, sch.at("maxContains").get<std::size_t>()};
			}
			sch.erase("minContains");
			sch.erase("maxContains");
		} else {
			attr = sch.find("items");
			if (attr != sch.end()) {

				if (attr.value().type() == json::value_t::array) {
					size_t c = 0;
					for (auto &subsch : attr.value())
						items_.push_back(schema::make(subsch, root, {"items", std::to_string(c++)}, uris));

					auto attr_add = sch.find("additionalItems");
					if (attr_add != sch.end()) {
						additionalItems_ = schema::make(attr_add.value(), root, {"additionalItems"}, uris);
						sch.erase(attr_add);
					}

				} else if (attr.value().type() == json::value_t::object ||
				           attr.value().type() == json::value_t::boolean)
					items_schema_ = schema::make(attr.value(), root, {"items"}, uris);

				sch.erase(attr);
			}
		}

		attr = sch.find("contains");
		if (attr != sch.end()) {
			contains_ = schema::make(attr.value(), root, {"contains"}, uris);
			sch.erase(attr);
		}
	}
};

std::shared_ptr<schema> type_schema::make(json &schema,
                                          json::value_t type,
                                          root_schema *root,
                                          const std::vector<nlohmann::json_uri> &uris,
                                          std::set<std::string> &kw)
{
	switch (type) {
	case json::value_t::null:
		return std::make_shared<null>(schema, root);

	case json::value_t::number_unsigned:
	case json::value_t::number_integer:
		return std::make_shared<numeric<json::number_integer_t>>(schema, root, kw);
	case json::value_t::number_float:
		return std::make_shared<numeric<json::number_float_t>>(schema, root, kw);
	case json::value_t::string:
		return std::make_shared<string>(schema, root);
	case json::value_t::boolean:
		return std::make_shared<boolean_type>(schema, root);
	case json::value_t::object:
		return std::make_shared<object>(schema, root, uris);
	case json::value_t::array:
		return std::make_shared<array>(schema, root, uris);

	case json::value_t::discarded: // not a real type - silence please
		break;

	case json::value_t::binary:
		break;
	}
	return nullptr;
}
} // namespace

namespace
{

std::shared_ptr<schema> schema::make(json &schema,
                                     root_schema *root,
                                     const std::vector<std::string> &keys,
                                     std::vector<nlohmann::json_uri> uris)
{
	root->count_schema();
	struct dialect_scope {
		root_schema *root;
		schema_dialect previous;
		std::set<std::string> vocabularies;
		~dialect_scope()
		{
			root->leave_schema();
			root->set_dialect(previous);
			root->set_vocabularies(std::move(vocabularies));
		}
	} scope{root, root->dialect(), root->vocabularies()};

	if (schema.is_object() && schema.contains("$schema")) {
		root->select_dialect(json_uri(schema.at("$schema").get<std::string>()).location());
	}
	json annotations = json::object();
	if (schema.is_object()) {
		const std::map<std::string, std::vector<std::string>> keywords = {
		    {"validation", {"type", "enum", "const", "multipleOf", "maximum", "minimum", "exclusiveMaximum", "exclusiveMinimum", "maxLength", "minLength", "pattern", "maxItems", "minItems", "uniqueItems", "minContains", "maxContains", "maxProperties", "minProperties", "required", "dependentRequired"}},
		    {"applicator", {"allOf", "anyOf", "oneOf", "not", "if", "then", "else", "dependentSchemas", "prefixItems", "items", "contains", "properties", "patternProperties", "additionalProperties", "propertyNames"}},
		    {"unevaluated", {"unevaluatedProperties", "unevaluatedItems"}}};
		for (const auto &vocabulary : keywords) {
			if (!root->enabled(vocabulary.first)) {
				for (const auto &keyword : vocabulary.second) {
					if (schema.contains(keyword)) {
						annotations[keyword] = schema.at(keyword);
						schema.erase(keyword);
					}
				}
			}
		}
	}

	// remove URIs which contain plain name identifiers, as sub-schemas cannot be referenced
	for (auto uri = uris.begin(); uri != uris.end();)
		if (uri->identifier() != "")
			uri = uris.erase(uri);
		else
			uri++;

	// append to all URIs the keys for this sub-schema
	for (auto &key : keys)
		for (auto &uri : uris)
			uri = uri.append(key);

	std::shared_ptr<::schema> sch;
	std::string anchor;
	std::string dynamic_anchor;
	if (schema.is_object() && root->dialect() == schema_dialect::draft2020_12) {
		anchor = schema.value("$anchor", "");
		dynamic_anchor = schema.value("$dynamicAnchor", "");
	}

	// boolean schema
	if (schema.type() == json::value_t::boolean)
		sch = std::make_shared<boolean>(schema, root);
	else if (schema.type() == json::value_t::object) {

		auto attr = schema.find("$id"); // if $id is present, this schema can be referenced by this ID
		                                // as an additional URI
		if (attr != schema.end() && !(root->dialect() == schema_dialect::draft7 && schema.contains("$ref"))) {
			// resolved against the current base URI, a relative $id can be the URI of the loaded file itself
			auto id = uris.back().derive(attr.value().get<std::string>());
			auto existing = std::find(uris.begin(), uris.end(), id);
			if (existing != uris.end()) // already known, but the base URI for this schema now
				uris.erase(existing);
			uris.push_back(id);
			schema.erase(attr);
		}

		auto findDefinitions = [&](const std::string &defs) -> bool {
			attr = schema.find(defs);
			if (attr != schema.end()) {
				for (auto &def : attr.value().items())
					schema::make(def.value(), root, {defs, def.key()}, uris);
				schema.erase(attr);
				return true;
			}
			return false;
		};
		findDefinitions("$defs");
		findDefinitions("definitions");

		const std::string reference_key = root->dialect() == schema_dialect::draft2020_12 && schema.contains("$dynamicRef") ? "$dynamicRef" : "$ref";
		attr = schema.find(reference_key);
		if (attr != schema.end()) { // this schema is a reference
			// the last one on the uri-stack is the last id seen before coming here,
			// so this is the origial URI for this reference, the $ref-value has thus be resolved from it
			auto id = uris.back().derive(attr.value().get<std::string>());
			auto reference = std::make_shared<schema_ref>(id.to_string(), root);
			reference->set_target(root->get_or_create_ref(id));
			if (reference_key == "$dynamicRef") {
				reference->set_dynamic(id);
			}
			sch = reference;
			if (root->dialect() == schema_dialect::draft2020_12) {
				schema.erase(attr);
				auto sibling = std::make_shared<type_schema>(schema, root, uris);
				sibling->set_reference(sch);
				sch = sibling;
				attr = schema.end();
			}

			if (attr != schema.end()) {
				schema.erase(attr);
			}

			// special case where we break draft-7 and allow overriding of properties when a $ref is used
			attr = schema.find("default");
			if (attr != schema.end()) {
				// copy the referenced schema depending on the underlying type and modify the default value
				if (auto new_sch = sch->make_for_default_(sch, root, uris, attr.value())) {
					sch = new_sch;
				}
				schema.erase(attr);
			}
		} else {
			sch = std::make_shared<type_schema>(schema, root, uris);
		}

		schema.erase("$schema");
		schema.erase("title");
		schema.erase("description");
	} else {
		throw std::invalid_argument("invalid JSON-type for a schema for " + uris[0].to_string() + ", expected: boolean or object");
	}

	if (schema.is_object()) {
		schema.update(annotations);
	}
	sch->set_resource(uris.back().location());
	if (!anchor.empty()) {
		root->insert(uris.back().derive("#" + anchor), sch);
	}
	if (!dynamic_anchor.empty()) {
		root->insert(uris.back().derive("#" + dynamic_anchor), sch);
		root->insert_dynamic(uris.back().location(), dynamic_anchor, sch);
	}

	for (auto &uri : uris) { // for all URIs this schema is referenced by
		root->insert(uri, sch);

		if (schema.type() == json::value_t::object)
			for (auto &u : schema.items())
				root->insert_unknown_keyword(uri, u.key(), u.value()); // insert unknown keywords for later reference
	}
	return sch;
}

class throwing_error_handler : public error_handler
{
	void error(const json::json_pointer &ptr, const json &instance, const std::string &message) override
	{
		throw std::invalid_argument(std::string("At ") + ptr.to_string() + " of " + instance.dump() + " - " + message + "\n");
	}
};

} // namespace

namespace nlohmann
{
namespace json_schema
{

json_validator::json_validator(schema_loader loader,
                               format_checker format,
                               content_checker content)
    : root_(std::unique_ptr<root_schema>(new root_schema(std::move(loader),
                                                         std::move(format),
                                                         std::move(content))))
{
}

json_validator::json_validator(const json &schema,
                               schema_loader loader,
                               format_checker format,
                               content_checker content)
    : json_validator(std::move(loader),
                     std::move(format),
                     std::move(content))
{
	set_root_schema(schema);
}

json_validator::json_validator(json &&schema,
                               schema_loader loader,
                               format_checker format,
                               content_checker content)

    : json_validator(std::move(loader),
                     std::move(format),
                     std::move(content))
{
	set_root_schema(std::move(schema));
}

// move constructor, destructor and move assignment operator can be defaulted here
// where root_schema is a complete type
json_validator::json_validator(json_validator &&) = default;
json_validator::~json_validator() = default;
json_validator &json_validator::operator=(json_validator &&) = default;

void json_validator::set_format_assertion(bool enabled)
{
	root_->set_format_assertion(enabled);
}

void json_validator::validate_schema(const json &schema) const
{
	root_->validate_schema(schema);
}

void json_validator::set_validation_limits(const validation_limits &limits)
{
	root_->set_validation_limits(limits);
}

void json_validator::set_default_dialect(schema_dialect dialect)
{
	root_->set_default_dialect(dialect);
}

void json_validator::set_root_schema(const json &schema)
{
	root_->set_root_schema(schema);
}

void json_validator::set_root_schema(json &&schema)
{
	root_->set_root_schema(std::move(schema));
}

json json_validator::validate(const json &instance) const
{
	throwing_error_handler err;
	return validate(instance, err);
}

json json_validator::validate(const json &instance, error_handler &err, const json_uri &initial_uri) const
{
	json::json_pointer ptr;
	json_patch patch;
	validation_context context;
	root_->validate(ptr, instance, patch, err, initial_uri, context);
	return patch;
}

} // namespace json_schema
} // namespace nlohmann
