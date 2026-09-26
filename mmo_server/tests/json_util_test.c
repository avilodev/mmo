/**
 * @file
 * Exercise the shared JSON parser against the shapes the data files actually use.
 *
 * The race registry, progression tunables, and ability definitions all read nested
 * objects with repeated key names at different depths, which is exactly what the
 * substring scanning this parser replaced got wrong.
 */

#include "json_util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

/** Record one assertion result and print it. */
static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/** Compare two doubles within the tolerance a data file needs. */
static int near(double a, double b) {
    return fabs(a - b) < 1e-9;
}

/** Verify scalars parse to the right type and value. */
static void test_scalars(void) {
    printf("TEST 1: scalars carry their type and value\n");

    JsonValue* root = json_parse(
        "{\"n\": -12.5, \"s\": \"hi\", \"t\": true, \"f\": false, \"z\": null}");
    check(root != NULL, "an object of scalars parses");

    check(near(json_get_number(root, "n", 0), -12.5), "negative fractions survive");
    check(json_get_int(root, "n", 0) == -12, "int access truncates toward zero");
    check(strcmp(json_get_string(root, "s", ""), "hi") == 0, "strings survive");
    check(json_get_bool(root, "t", 0) == 1, "true reads as true");
    check(json_get_bool(root, "f", 1) == 0, "false reads as false");
    check(json_type(json_get(root, "z")) == JSON_NULL, "null has its own type");

    json_free(root);
}

/** Verify every accessor returns its fallback instead of failing. */
static void test_accessors_are_total(void) {
    printf("TEST 2: a missing key or wrong type yields the fallback, never a crash\n");

    JsonValue* root = json_parse("{\"a\": 1}");
    check(root != NULL, "fixture parses");

    check(json_get_int(root, "absent", 7) == 7, "absent key falls back");
    check(json_get_int(root, "a", 7) == 1, "present key wins over the fallback");
    check(strcmp(json_get_string(root, "a", "fb"), "fb") == 0, "a number read as a string falls back");
    check(json_get_int(NULL, "a", 7) == 7, "a NULL value falls back");
    check(json_get(root, NULL) == NULL, "a NULL key looks up nothing");
    check(json_count(root) == 0, "an object counted as an array is empty");
    check(json_at(root, 0) == NULL, "an object indexed as an array yields nothing");

    json_free(root);
    json_free(NULL);
}

/** Verify nested and repeated keys resolve by path, not by first textual match. */
static void test_nesting_does_not_collide(void) {
    printf("TEST 3: the same key at two depths does not collide\n");

    const char* doc =
        "{"
        "  \"races\": ["
        "    {\"id\": 1, \"key\": \"wolf\","
        "     \"stats\": {\"base\": {\"strength\": 12}, \"per_level\": {\"strength\": 30}}},"
        "    {\"id\": 2, \"key\": \"bear\","
        "     \"stats\": {\"base\": {\"strength\": 10}, \"per_level\": {\"strength\": 25}}}"
        "  ]"
        "}";

    JsonValue* root = json_parse(doc);
    check(root != NULL, "a two-race document parses");

    const JsonValue* races = json_get(root, "races");
    check(json_count(races) == 2, "the array holds both races");

    const JsonValue* bear = json_at(races, 1);
    check(strcmp(json_get_string(bear, "key", ""), "bear") == 0, "index 1 is the second race");

    const JsonValue* stats = json_get(bear, "stats");
    check(json_get_int(json_get(stats, "base"), "strength", 0) == 10,
          "bear's base strength is not wolf's");
    check(json_get_int(json_get(stats, "per_level"), "strength", 0) == 25,
          "per_level strength is not base strength");

    json_free(root);
}

/** Verify an object whose keys are data can be walked positionally. */
static void test_object_walk(void) {
    printf("TEST 4: objects keyed by data can be enumerated\n");

    JsonValue* root = json_parse("{\"strength\": 12, \"vitality\": 8, \"armor\": 3}");
    check(json_member_count(root) == 3, "all three members are present");
    check(strcmp(json_key_at(root, 0), "strength") == 0, "keys keep document order");
    check(json_as_int(json_member_at(root, 2), 0) == 3, "the third member is armor's value");
    check(json_key_at(root, 3) == NULL, "walking past the end yields nothing");

    json_free(root);
}

/** Verify escapes decode, including surrogate pairs. */
static void test_string_escapes(void) {
    printf("TEST 5: escapes decode to UTF-8\n");

    JsonValue* root = json_parse(
        "{\"a\": \"tab\\there\", \"b\": \"quote\\\"inside\", \"c\": \"\\u00e9\", \"d\": \"\\ud83d\\ude00\"}");
    check(root != NULL, "escaped strings parse");
    check(strcmp(json_get_string(root, "a", ""), "tab\there") == 0, "\\t decodes");
    check(strcmp(json_get_string(root, "b", ""), "quote\"inside") == 0, "an escaped quote decodes");
    check(strcmp(json_get_string(root, "c", ""), "\xc3\xa9") == 0, "\\u00e9 becomes two UTF-8 bytes");
    check(strcmp(json_get_string(root, "d", ""), "\xf0\x9f\x98\x80") == 0,
          "a surrogate pair becomes one four-byte code point");

    json_free(root);
}

/** Verify malformed input is rejected rather than half-accepted. */
static void test_rejects_malformed(void) {
    printf("TEST 6: malformed documents are refused\n");

    const char* bad[] = {
        "{",                    /* unterminated object          */
        "[1, 2",                /* unterminated array           */
        "{\"a\" 1}",            /* missing colon                */
        "{\"a\": 1,}",          /* trailing comma               */
        "{\"a\": \"unclosed}",  /* unterminated string          */
        "{\"a\": 1} extra",     /* trailing content             */
        "tru",                  /* truncated literal            */
        "",                     /* empty document               */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        JsonValue* v = json_parse(bad[i]);
        char label[128];
        snprintf(label, sizeof(label), "rejected: %s", bad[i][0] ? bad[i] : "(empty)");
        check(v == NULL, label);
        json_free(v);
    }

    check(json_parse(NULL) == NULL, "a NULL document is refused");
}

/** Verify nesting beyond the depth cap is refused instead of overflowing the stack. */
static void test_depth_cap(void) {
    printf("TEST 7: runaway nesting is refused, not crashed on\n");

    char deep[600];
    size_t n = 0;
    for (; n < 200; n++) deep[n] = '[';
    for (size_t i = 0; i < 200; i++) deep[n++] = ']';
    deep[n] = '\0';

    check(json_parse(deep) == NULL, "200 levels of nesting is refused");

    char shallow[80];
    n = 0;
    for (; n < 30; n++) shallow[n] = '[';
    for (size_t i = 0; i < 30; i++) shallow[n++] = ']';
    shallow[n] = '\0';

    JsonValue* ok = json_parse(shallow);
    check(ok != NULL, "30 levels of nesting is accepted");
    json_free(ok);
}

/** Verify file loading reports failure without leaking a partial tree. */
static void test_file_loading(void) {
    printf("TEST 8: file loading reports why it failed\n");

    const char* err = "unset";
    check(json_parse_file("/nonexistent/path/races.json", &err) == NULL, "a missing file fails");
    check(err != NULL && strcmp(err, "cannot open file") == 0, "the failure names the cause");

    const char* path = "/tmp/json_util_test_fixture.json";
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs("{\"races\": [{\"id\": 1}]}", f);
        fclose(f);

        err = "unset";
        JsonValue* root = json_parse_file(path, &err);
        check(root != NULL, "a well-formed file loads");
        check(err == NULL, "success clears the error");
        check(json_count(json_get(root, "races")) == 1, "the loaded tree is usable");
        json_free(root);
        remove(path);
    }
}

/**
 * Check the tail-counting helper the substring loaders use to report truncation.
 */
static void test_remaining_object_count(void) {
    printf("TEST 9: counting the elements a capped loader refused\n");

    check(json_count_remaining_objects(NULL) == 0, "NULL counts as nothing left");
    check(json_count_remaining_objects("]") == 0, "a closed array has nothing left");
    check(json_count_remaining_objects("  ]  ") == 0, "trailing space before ] is fine");

    check(json_count_remaining_objects(", {\"a\":1}, {\"b\":2} ]") == 2,
          "two remaining elements are counted");

    check(json_count_remaining_objects("{\"a\":{\"n\":1}},{\"b\":2}]") == 2,
          "nested objects count once, not once per brace");

    check(json_count_remaining_objects("{\"a\":\"}{\"},{\"b\":2}]") == 2,
          "braces inside strings are not elements");

    check(json_count_remaining_objects("{\"a\":\"\\\"}\"}]") == 1,
          "an escaped quote does not end the string early");

    check(json_count_remaining_objects("]{\"x\":1}") == 0,
          "counting stops at this array's closing bracket");

    check(json_count_remaining_objects("{\"a\":1}") == 1,
          "a missing closing bracket still counts what it saw");
}

int main(void) {
    printf("=== json_util test ===\n\n");

    test_scalars();
    test_accessors_are_total();
    test_nesting_does_not_collide();
    test_object_walk();
    test_string_escapes();
    test_rejects_malformed();
    test_depth_cap();
    test_file_loading();
    test_remaining_object_count();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
