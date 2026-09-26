#ifndef JSON_UTIL_H
#define JSON_UTIL_H

/** @file Parse JSON data files into a small read-only document tree.
 *
 * The world server's data files are the tuning surface for the whole game, and
 * `make run` reloads them without a rebuild. That only stays true if reading a
 * nested structure is cheap to write, so this parser returns a real tree instead
 * of the substring scanning that predates it — sibling and nested keys with the
 * same name no longer collide.
 *
 * Every accessor is total: a NULL value, a wrong type, or a missing key returns
 * the supplied fallback rather than failing. Callers validate meaning, not shape.
 */

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/** Parse a NUL-terminated JSON document.
 *
 * @return An owned tree the caller frees with json_free(), or NULL on a syntax error.
 */
JsonValue* json_parse(const char* text);

/** Read and parse a JSON file.
 *
 * @param path     Path to the document.
 * @param err      Receives a static description of the failure; may be NULL.
 * @return         An owned tree the caller frees with json_free(), or NULL on failure.
 */
JsonValue* json_parse_file(const char* path, const char** err);

/** Release a tree returned by json_parse() or json_parse_file(). Tolerates NULL. */
void json_free(JsonValue* value);

/** Report a value's type, or JSON_NULL when the value is NULL. */
JsonType json_type(const JsonValue* value);

/** Look up a member of an object.
 *
 * @return The member, or NULL when absent or when `value` is not an object.
 */
const JsonValue* json_get(const JsonValue* value, const char* key);

/** Count an array's elements, or zero when `value` is not an array. */
int json_count(const JsonValue* value);

/** Count the `{...}` elements left unconsumed in the tail of a JSON array.
 *
 * For the hand-rolled substring loaders that stop at a compiled cap. Pass the
 * position where the loop gave up and learn how many elements it refused, so the
 * refusal can be logged instead of being indistinguishable from content that was
 * never written. Braces inside strings and escapes are not counted.
 *
 * @param array_tail  Position inside a JSON array, at or before the next element.
 * @return            Elements remaining before the array's closing bracket, or 0
 *                    when `array_tail` is NULL or the array is exhausted.
 */
int json_count_remaining_objects(const char* array_tail);

/** Index an array.
 *
 * @return The element, or NULL when out of range or when `value` is not an array.
 */
const JsonValue* json_at(const JsonValue* value, int index);

/** Read a string value, or `fallback` when absent or of another type.
 *
 * The returned pointer is owned by the tree and dies with json_free().
 */
const char* json_as_string(const JsonValue* value, const char* fallback);

/** Read a numeric value, or `fallback` when absent or of another type. */
double json_as_number(const JsonValue* value, double fallback);

/** Read a numeric value truncated to int, or `fallback` when absent or of another type. */
int json_as_int(const JsonValue* value, int fallback);

/** Read a boolean value, or `fallback` when absent or of another type. */
int json_as_bool(const JsonValue* value, int fallback);

/** Read `key` from an object as a string, or `fallback` when absent. */
const char* json_get_string(const JsonValue* value, const char* key, const char* fallback);

/** Read `key` from an object as a number, or `fallback` when absent. */
double json_get_number(const JsonValue* value, const char* key, double fallback);

/** Read `key` from an object as an int, or `fallback` when absent. */
int json_get_int(const JsonValue* value, const char* key, int fallback);

/** Read `key` from an object as a boolean, or `fallback` when absent. */
int json_get_bool(const JsonValue* value, const char* key, int fallback);

/** Return the key of the object member at `index`, or NULL when out of range.
 *
 * Pairs with json_member_at() to walk an object whose keys are data, such as a
 * stat block whose keys are stat names.
 */
const char* json_key_at(const JsonValue* value, int index);

/** Return the value of the object member at `index`, or NULL when out of range. */
const JsonValue* json_member_at(const JsonValue* value, int index);

/** Count an object's members, or zero when `value` is not an object. */
int json_member_count(const JsonValue* value);

#endif // JSON_UTIL_H
