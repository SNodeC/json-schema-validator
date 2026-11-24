/*
 * JSON schema validator for JSON for modern C++
 * Copyright (c) 2026 Volker Christian.
 * SPDX-License-Identifier: MIT
 */
#ifndef JSON_SCHEMA_REGEX_HPP
#define JSON_SCHEMA_REGEX_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef JSON_SCHEMA_PCRE2
#	define PCRE2_CODE_UNIT_WIDTH 8
#	include <pcre2.h>
#else
#	include <regex>
#endif

namespace json_schema_regex
{
#ifdef JSON_SCHEMA_PCRE2
// Translate ECMA-262 spellings which PCRE2 does not recognize, and reject
// Perl-only groups/escapes rather than accepting them as schema syntax.
inline std::string ecma_pattern(const std::string &pattern)
{
	static const std::map<std::string, std::string> categories = {
	    {"Letter", "L"}, {"Cased_Letter", "LC"}, {"Uppercase_Letter", "Lu"}, {"Lowercase_Letter", "Ll"}, {"Titlecase_Letter", "Lt"}, {"Modifier_Letter", "Lm"}, {"Other_Letter", "Lo"}, {"Mark", "M"}, {"Nonspacing_Mark", "Mn"}, {"Spacing_Mark", "Mc"}, {"Enclosing_Mark", "Me"}, {"Number", "N"}, {"Decimal_Number", "Nd"}, {"Letter_Number", "Nl"}, {"Other_Number", "No"}, {"Punctuation", "P"}, {"Connector_Punctuation", "Pc"}, {"Dash_Punctuation", "Pd"}, {"Open_Punctuation", "Ps"}, {"Close_Punctuation", "Pe"}, {"Initial_Punctuation", "Pi"}, {"Final_Punctuation", "Pf"}, {"Other_Punctuation", "Po"}, {"Symbol", "S"}, {"Math_Symbol", "Sm"}, {"Currency_Symbol", "Sc"}, {"Modifier_Symbol", "Sk"}, {"Other_Symbol", "So"}, {"Separator", "Z"}, {"Space_Separator", "Zs"}, {"Line_Separator", "Zl"}, {"Paragraph_Separator", "Zp"}, {"Other", "C"}, {"Control", "Cc"}, {"Format", "Cf"}, {"Surrogate", "Cs"}, {"Private_Use", "Co"}, {"Unassigned", "Cn"}};
	std::string result;
	bool character_class = false;
	for (std::size_t i = 0; i < pattern.size(); ++i) {
		const char c = pattern[i];
		if (c == '\\' && i + 1 < pattern.size()) {
			const char escaped = pattern[++i];
			if (std::string("aAeEgGhHKNRVXCQ").find(escaped) != std::string::npos) {
				throw std::invalid_argument("non-ECMA-262 regex escape");
			}
			if ((escaped == 'p' || escaped == 'P') && i + 1 < pattern.size() && pattern[i + 1] == '{') {
				const auto end = pattern.find('}', i + 2);
				if (end == std::string::npos) {
					throw std::invalid_argument("unterminated Unicode property escape");
				}
				auto property = pattern.substr(i + 2, end - i - 2);
				for (const std::string prefix : {"General_Category=", "gc="}) {
					if (property.compare(0, prefix.size(), prefix) == 0) {
						property.erase(0, prefix.size());
					}
				}
				const auto alias = categories.find(property);
				result += std::string("\\") + escaped + '{' + (alias == categories.end() ? property : alias->second) + '}';
				i = end;
			} else {
				result += std::string("\\") + escaped;
			}
		} else if (!character_class && pattern.compare(i, 2, "[]") == 0) {
			result += "(?!)";
			++i;
		} else if (!character_class && pattern.compare(i, 3, "[^]") == 0) {
			result += "[\\s\\S]";
			i += 2;
		} else {
			if (!character_class && c == '(' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
				throw std::invalid_argument("non-ECMA-262 regex verb");
			}
			if (!character_class && pattern.compare(i, 2, "(?") == 0 && i + 2 < pattern.size() && std::string(":!=<").find(pattern[i + 2]) == std::string::npos) {
				throw std::invalid_argument("non-ECMA-262 regex group");
			}
			if (c == '[') {
				character_class = true;
			} else if (c == ']') {
				character_class = false;
			}
			result += c == '.' && !character_class ? "[^\\r\\n\\u2028\\u2029]" : std::string(1, c);
		}
	}
	return result;
}

class regex
{
	std::shared_ptr<pcre2_code> code_;

public:
	static constexpr int ECMAScript = 0;

	explicit regex(const std::string &source = "", int = ECMAScript)
	{
		const auto pattern = ecma_pattern(source);
		std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> context(pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
		if (!context) {
			throw std::bad_alloc();
		}
		uint32_t extra = 0;
#	ifdef PCRE2_EXTRA_ALT_BSUX
		extra |= PCRE2_EXTRA_ALT_BSUX;
#	endif
#	ifdef PCRE2_EXTRA_ASCII_BSD
		extra |= PCRE2_EXTRA_ASCII_BSD | PCRE2_EXTRA_ASCII_BSW;
#	endif
		pcre2_set_compile_extra_options(context.get(), extra);
		int error = 0;
		PCRE2_SIZE offset = 0;
		code_.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), PCRE2_UTF | PCRE2_UCP | PCRE2_ALT_BSUX | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DOLLAR_ENDONLY, &error, &offset, context.get()), pcre2_code_free);
		if (!code_) {
			PCRE2_UCHAR message[256];
			pcre2_get_error_message(error, message, sizeof(message));
			throw std::invalid_argument("invalid regex at " + std::to_string(offset) + ": " + reinterpret_cast<const char *>(message));
		}
	}

	bool search(const std::string &value) const
	{
		std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(pcre2_match_data_create_from_pattern(code_.get(), nullptr), pcre2_match_data_free);
		std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(pcre2_match_context_create(nullptr), pcre2_match_context_free);
		if (!data || !context) {
			throw std::bad_alloc();
		}
		pcre2_set_match_limit(context.get(), 100000);
		pcre2_set_depth_limit(context.get(), 256);
		const int result = pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(value.data()), value.size(), 0, 0, data.get(), context.get());
		if (result < 0 && result != PCRE2_ERROR_NOMATCH) {
			throw std::invalid_argument("regex matching failed: " + std::to_string(result));
		}
		return result >= 0;
	}
};

inline bool regex_search(const std::string &value, const regex &pattern)
{
	return pattern.search(value);
}
#else
using std::regex;
using std::regex_search;
#endif
} // namespace json_schema_regex

#endif
