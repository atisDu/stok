#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace stok::text {

// Appends the UTF-8 encoding of a code point.
void append_utf8(std::string& out, uint32_t cp);

// Decodes XML/HTML character references (&amp; &lt; &#8217; &#x2019; &nbsp; ...)
// and appends the result. Unknown entities are copied through verbatim.
void append_decoded_entities(std::string_view in, std::string& out);

// Converts an HTML fragment to plain text: drops tags, script and style
// contents and comments, decodes entities, turns block-level tags into
// separators, and collapses whitespace. Appends at most `max_out` bytes.
// `block_sep` is emitted for block-level tags ('\n' keeps paragraph breaks).
void html_to_text(std::string_view html, std::string& out, std::size_t max_out = SIZE_MAX, char block_sep = ' ');

// Collapses runs of whitespace to single spaces and trims (in place).
void collapse_whitespace(std::string& s);

// Hash of a headline normalized to lowercase alphanumeric words, so the same
// story from two wires (different punctuation, entities or case) gets the
// same key. Used for cross-source dedupe and "who published first" stats.
uint64_t headline_key(std::string_view title);

// Escapes a string for inclusion in a JSON string literal (appends).
void append_json_escaped(std::string& out, std::string_view s);

}  // namespace stok::text
