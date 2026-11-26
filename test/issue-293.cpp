#include "nlohmann/json-schema.hpp"

#include <cmath>
#include <limits>

using nlohmann::json_schema::json_validator;

template <typename T>
int should_throw(const nlohmann::json &schema, T value)
{
	try {
		json_validator(schema).validate(value);
	} catch (const std::exception &ex) {
		return 0;
	}
	return 1;
}

int main(void)
{

	json_validator({{"type", "number"}, {"multipleOf", 0.001}}).validate(0.3 - 0.2);
	json_validator({{"type", "number"}, {"multipleOf", 3.3}}).validate(8.0 - 1.4);
	json_validator({{"type", "number"}, {"multipleOf", 1000.01}}).validate((1000.03 - 0.02) * 15.0);
	json_validator({{"type", "number"}, {"multipleOf", 0.001}}).validate(0.030999999999999993);
	json_validator({{"type", "number"}, {"multipleOf", 0.100000}}).validate(1.9);
	json_validator({{"type", "number"}, {"multipleOf", 100000.1}}).validate(9000009);
	json_validator({{"type", "number"}, {"multipleOf", 0.3}}).validate(0.0);
	json_validator({{"type", "number"}, {"multipleOf", 0.3}}).validate(-0.0);
	const nlohmann::json integer_schema = {{"type", "integer"}};
	json_validator(integer_schema).validate(1.0);
	json_validator(integer_schema).validate(-1.0);
	json_validator(integer_schema).validate(-0.0);
	json_validator(integer_schema).validate(9007199254740992.0);
	const double smallest = std::numeric_limits<double>::denorm_min();
	json_validator({{"type", "number"}, {"multipleOf", smallest}}).validate(1.0);

	int exc_count = 0;
	exc_count += should_throw({{"type", "number"}, {"multipleOf", 0.001}}, 0.3 - 0.2005);
	exc_count += should_throw({{"type", "number"}, {"multipleOf", 1000.02}}, (1000.03 - 0.02) * 15.0);
	exc_count += should_throw({{"type", "number"}, {"multipleOf", 100000.11}}, 9000009);
	exc_count += should_throw(integer_schema, std::nextafter(1.0, 0.0));
	exc_count += should_throw(integer_schema, std::nextafter(1.0, 2.0));
	exc_count += should_throw(integer_schema, std::nextafter(-1.0, 0.0));
	exc_count += should_throw(integer_schema, std::nextafter(-1.0, -2.0));
	exc_count += should_throw(integer_schema, smallest);
	exc_count += should_throw({{"type", "number"}, {"multipleOf", 3.0 * smallest}}, 1.0);

	return exc_count;
}
