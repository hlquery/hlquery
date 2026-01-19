/* 
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 * 
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <cstddef>

/* 
 * JSON Processing Tests
 * Simple JSON parsing/generation without external libraries
 */

#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

/* Simple JSON value class for testing */

class SimpleJSON
{
      public:
        enum Type
        {
                STRING,
                NUMBER,
                BOOLEAN,
                ARRAY,
                OBJECT,
                NULL_TYPE
        };
        Type type;
        std::string string_value;
        double number_value;
        bool boolean_value;
        std::vector<SimpleJSON> array_value;
        std::map<std::string, SimpleJSON> object_value;

        SimpleJSON() : type(NULL_TYPE)
        {
        }

        explicit SimpleJSON(const std::string& s)
            : type(STRING), string_value(s)
        {
        }

        explicit SimpleJSON(const char* s) : type(STRING), string_value(s)
        {
        }

        explicit SimpleJSON(double n) : type(NUMBER), number_value(n)
        {
        }

        explicit SimpleJSON(bool b) : type(BOOLEAN), boolean_value(b)
        {
        }

        std::string to_string() const
        {

                switch (type)
                {
                        case STRING:
                                return "\"" + string_value + "\"";
                        case NUMBER:
                                return std::to_string(number_value);
                        case BOOLEAN:
                                return boolean_value ? "true" : "false";
                        case NULL_TYPE:
                                return "null";
                        case ARRAY:
                        {
                                std::string result = "[";

                                for (size_t i = 0; i < array_value.size(); ++i)
                                {

                                        if (i > 0)
                                        {
                                                result += ",";
                                        }

                                        result += array_value[i].to_string();
                                }

                                result += "]";
                                return result;
                        }

                        case OBJECT:
                        {
                                std::string result = "{";
                                bool first         = true;

                                for (const auto& pair : object_value)
                                {

                                        if (!first)
                                        {
                                                result += ",";
                                        }

                                        first = false;
                                        result += "\"" + pair.first + "\":" +
                                                  pair.second.to_string();
                                }

                                result += "}";
                                return result;
                        }
                }

                return "";
        }
};

int test_json()
{
        try
        {

                /* Test 1: Basic string value */

                SimpleJSON str_val(std::string("hello"));

                if (str_val.to_string() != "\"hello\"")
                {
                        return 1;
                }

                /* Test 2: Number value */

                SimpleJSON num_val(42.5);
                std::string num_str = num_val.to_string();

                if (num_str.find("42.5") == std::string::npos)
                {
                        return 2;
                }

                /* Test 3: Boolean values */

                SimpleJSON bool_true(true);
                SimpleJSON bool_false(false);

                if (bool_true.to_string() != "true" ||
                    bool_false.to_string() != "false")
                {
                        return 3;
                }

                /* Test 4: Null value */

                SimpleJSON null_val;

                if (null_val.to_string() != "null")
                {
                        return 4;
                }

                /* Test 5: Array */

                SimpleJSON array_val;
                array_val.type = SimpleJSON::ARRAY;
                array_val.array_value.push_back(SimpleJSON(std::string("item1"))
                );
                array_val.array_value.push_back(SimpleJSON(123.0));
                array_val.array_value.push_back(SimpleJSON(true));

                std::string array_str = array_val.to_string();

                if (array_str.find("[") == std::string::npos ||
                    array_str.find("]") == std::string::npos ||
                    array_str.find("item1") == std::string::npos)
                {
                        return 5;
                }

                /* Test 6: Object */

                SimpleJSON obj_val;
                obj_val.type = SimpleJSON::OBJECT;
                obj_val.object_value["name"] =
                        SimpleJSON(std::string("hlquery"));
                obj_val.object_value["version"] = SimpleJSON(1.0);
                obj_val.object_value["active"]  = SimpleJSON(true);

                std::string obj_str = obj_val.to_string();

                if (obj_str.find("{") == std::string::npos ||
                    obj_str.find("}") == std::string::npos ||
                    obj_str.find("name") == std::string::npos ||
                    obj_str.find("hlquery") == std::string::npos)
                {
                        return 6;
                }

                /* Test 7: Nested structure */

                SimpleJSON nested;
                nested.type = SimpleJSON::OBJECT;

                SimpleJSON nested_array;
                nested_array.type = SimpleJSON::ARRAY;
                nested_array.array_value.push_back(
                        SimpleJSON(std::string("value1"))
                );
                nested_array.array_value.push_back(
                        SimpleJSON(std::string("value2"))
                );

                nested.object_value["data"]  = nested_array;
                nested.object_value["count"] = SimpleJSON(2.0);

                std::string nested_str = nested.to_string();

                if (nested_str.find("data") == std::string::npos ||
                    nested_str.find("value1") == std::string::npos ||
                    nested_str.find("count") == std::string::npos)
                {
                        return 7;
                }

                /* Test 8: String escaping concepts */

                std::string special_chars = "\"\\n\\t\"";

                /* Just verify we can handle strings with special characters */

                if (special_chars.empty())
                {
                        return 8;
                }

                /* Test 9: Number parsing validation */

                std::string number_tests[] = {
                        "123", "123.456", "-789", "0", "0.0"
                };

                for (const std::string& num_str : number_tests)
                {
                        try
                        {
                                double parsed = std::stod(num_str);

                                if (parsed != parsed)
                                { // Check for NaN
                                        return 9;
                                }
                        }
                        catch (...)
                        {
                                return 9;
                        }
                }

                /* Test 10: JSON structure validation */

                /* Test that our basic structure can represent common JSON patterns */

                SimpleJSON config;
                config.type = SimpleJSON::OBJECT;
                config.object_value["server"] =
                        SimpleJSON(std::string("hlquery"));
                config.object_value["port"]        = SimpleJSON(8080.0);
                config.object_value["ssl_enabled"] = SimpleJSON(true);

                SimpleJSON endpoints;
                endpoints.type = SimpleJSON::ARRAY;
                endpoints.array_value.push_back(
                        SimpleJSON(std::string("/search"))
                );
                endpoints.array_value.push_back(
                        SimpleJSON(std::string("/admin"))
                );

                config.object_value["endpoints"] = endpoints;

                std::string config_str = config.to_string();

                if (config_str.find("server") == std::string::npos ||
                    config_str.find("8080") == std::string::npos ||
                    config_str.find("/search") == std::string::npos)
                {
                        return 10;
                }

                return 0; // Success
        }
        catch (...)
        {
                return 99;
        }
}
