/*
 * A canned in-process macro plugin server for the smoke test, in plain C.
 *
 * Built as a dynamic library (for -in-process-plugin-server-path), it exports
 * swift_inproc_plugins_handle_message like the toolchain's
 * libSwiftInProcPluginServer. Compiled into smoke itself, the same function is
 * the handler for swifthost_set_plugin_message_handler.
 *
 * It implements one macro, M.AnswerMacro, a freestanding expression macro
 * that expands to 42 (messages as in swift-syntax's
 * SwiftCompilerPluginMessageHandling/PluginMessages.swift, protocol version 8).
 * SWIFTHOST_TEST_PLUGIN_REPLY selects a malformed or hostile reply to the
 * expansion request instead:
 *   truncated      JSON cut off in the middle
 *   garbage        not JSON at all
 *   wrong-kind     a well-formed reply of the wrong kind (getCapabilityResult)
 *   empty          a reply of length 0
 *   bad-positions  an error diagnostic whose position, highlight and Fix-It
 *                  ranges lie before the source buffer or end before they
 *                  start (fileName from SWIFTHOST_TEST_PLUGIN_FILE)
 *   error          the handler reports an error instead of a reply
 *   deep           200 Ki nested JSON arrays (enough to overflow a 16 MiB
 *                  stack in the compiler's JSON scanner)
 *   brackets       a valid reply whose expansion ("42" and a comment) holds
 *                  1000 '[', '{', an escaped quote and an escaped backslash:
 *                  brackets inside strings are not nesting
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

static int counts[4]; /* getCapability, loadPluginLibrary, expandFreestandingMacro, other */
static char lastLibraryPath[4096];

static const char *kinds[] = {"getCapability", "loadPluginLibrary", "expandFreestandingMacro"};

/* Number of messages of the given kind ("other" for anything else) seen so far. */
EXPORT int swifthost_test_plugin_count(const char *kind) {
  for (int i = 0; i < 3; ++i)
    if (!strcmp(kind, kinds[i]))
      return counts[i];
  return !strcmp(kind, "other") ? counts[3] : -1;
}

/* The libraryPath of the last loadPluginLibrary message. */
EXPORT const char *swifthost_test_plugin_last_library(void) { return lastLibraryPath; }

/* Every message is an object with one key, the message kind. */
static int kindOf(const char *msg, size_t len) {
  const char *p = msg, *end = msg + len;
  while (p < end && (*p == '{' || *p == ' ' || *p == '\n'))
    ++p;
  if (p >= end || *p != '"')
    return 3;
  ++p;
  for (int i = 0; i < 3; ++i) {
    size_t n = strlen(kinds[i]);
    if ((size_t)(end - p) > n && !memcmp(p, kinds[i], n) && p[n] == '"')
      return i;
  }
  return 3;
}

/* Copies the JSON string value of "key" (no escapes) into out. */
static void stringField(const char *msg, size_t len, const char *key, char *out, size_t outSize) {
  out[0] = 0;
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
  size_t n = strlen(pattern);
  for (const char *p = msg; p + n <= msg + len; ++p) {
    if (memcmp(p, pattern, n))
      continue;
    p += n;
    size_t i = 0;
    while (p < msg + len && *p != '"' && i + 1 < outSize)
      out[i++] = *p++;
    out[i] = 0;
    return;
  }
}

static bool reply(char **response, size_t *responseLength, const char *text, bool isError) {
  size_t n = strlen(text);
  char *buf = malloc(n ? n : 1);
  memcpy(buf, text, n);
  *response = buf;
  *responseLength = n;
  return isError;
}

static bool expansionReply(char **response, size_t *responseLength) {
  const char *mode = getenv("SWIFTHOST_TEST_PLUGIN_REPLY");
  if (!mode || !*mode || !strcmp(mode, "ok"))
    return reply(response, responseLength,
                 "{\"expandMacroResult\":{\"expandedSource\":\"42\",\"diagnostics\":[]}}", false);
  if (!strcmp(mode, "truncated"))
    return reply(response, responseLength, "{\"expandMacroResult\":{\"expandedSource\":\"4", false);
  if (!strcmp(mode, "garbage"))
    return reply(response, responseLength, "\x01\xff not json", false);
  if (!strcmp(mode, "wrong-kind"))
    return reply(response, responseLength,
                 "{\"getCapabilityResult\":{\"capability\":{\"protocolVersion\":8}}}", false);
  if (!strcmp(mode, "empty"))
    return reply(response, responseLength, "", false);
  if (!strcmp(mode, "error"))
    return reply(response, responseLength, "the test plugin failed on purpose", true);
  if (!strcmp(mode, "bad-positions")) {
    const char *file = getenv("SWIFTHOST_TEST_PLUGIN_FILE");
    char *text = NULL;
    if (asprintf(&text,
                 "{\"expandMacroResult\":{\"expandedSource\":null,\"diagnostics\":[{"
                 "\"message\":\"hostile diagnostic\",\"severity\":\"error\","
                 "\"position\":{\"fileName\":\"%1$s\",\"offset\":-100},"
                 "\"highlights\":[{\"fileName\":\"%1$s\",\"startOffset\":20,\"endOffset\":2}],"
                 "\"notes\":[{\"position\":{\"fileName\":\"%1$s\",\"offset\":-9223372036854775807},"
                 "\"message\":\"hostile note\"}],"
                 "\"fixIts\":[{\"message\":\"hostile fix\",\"changes\":[{\"range\":{"
                 "\"fileName\":\"%1$s\",\"startOffset\":-5,\"endOffset\":-1},\"newText\":\"x\"}]}]"
                 "}]}}",
                 file ? file : "") < 0)
      abort();
    *response = text;
    *responseLength = strlen(text);
    return false;
  }
  if (!strcmp(mode, "brackets")) {
    char brackets[1001];
    memset(brackets, '[', 1000);
    brackets[1000] = 0;
    char *text = NULL;
    if (asprintf(&text,
                 "{\"expandMacroResult\":{\"expandedSource\":\"42 /* %s {{ \\\" \\\\ */\","
                 "\"diagnostics\":[]}}",
                 brackets) < 0)
      abort();
    *response = text;
    *responseLength = strlen(text);
    return false;
  }
  if (!strcmp(mode, "deep")) {
    size_t n = 200u << 10;
    char *buf = malloc(n);
    memset(buf, '[', n);
    *response = buf;
    *responseLength = n;
    return false;
  }
  return reply(response, responseLength, "unknown SWIFTHOST_TEST_PLUGIN_REPLY", true);
}

EXPORT bool swift_inproc_plugins_handle_message(const char *message, size_t length,
                                                char **response, size_t *responseLength) {
  int kind = kindOf(message, length);
  ++counts[kind];
  switch (kind) {
  case 0:
    return reply(response, responseLength,
                 "{\"getCapabilityResult\":{\"capability\":{\"protocolVersion\":8,"
                 "\"features\":[\"load-plugin-library\"]}}}",
                 false);
  case 1:
    stringField(message, length, "libraryPath", lastLibraryPath, sizeof lastLibraryPath);
    return reply(response, responseLength,
                 "{\"loadPluginLibraryResult\":{\"loaded\":true,\"diagnostics\":[]}}", false);
  case 2:
    return expansionReply(response, responseLength);
  default:
    return reply(response, responseLength, "unexpected message", true);
  }
}
