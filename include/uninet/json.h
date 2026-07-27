// UniNet — JSON in, CBOR on the wire, JSON out.
//
// CBOR is what UniNet sends: compact, typed, and it carries float arrays as
// contiguous blocks so a 4096-vertex mesh costs no per-element overhead. But
// CBOR is awkward to type by hand and awkward to look at while debugging, and
// every language already has a JSON idiom.
//
// So JSON is the face and CBOR is the wire. `from_json` parses text into the
// same Cbor value a native builder would produce, and `to_json` renders any
// Cbor back. A Python dict, a C# JSON string and a C++ Cbor::map that describe
// the same thing produce the same bytes on the wire and arrive as the same
// value everywhere else. That is the whole point: one data model, three
// languages, no per-peer schema mirroring.
//
// Round-trip caveats, because JSON is a smaller type system than CBOR:
//   - byte strings render as base64 text (JSON has no byte type)
//   - float arrays render as ordinary JSON arrays; parsing one back gives a
//     generic array unless it is all numbers, in which case the CBOR encoder
//     picks the fast float path again
//   - integers beyond 2^53 survive CBOR but lose precision in JSON consumers
#pragma once

#include "uninet/cbor.h"

#include <string>

namespace uninet {

// Parse JSON text. Returns Cbor::null() and sets *ok=false on malformed input,
// on nesting deeper than the decoder's limit, or on trailing garbage. Never
// throws: this parses payloads that arrived over the network.
Cbor from_json(const std::string& text, bool* ok = nullptr);

// Render a Cbor value as JSON text. `indent` 0 gives one compact line (what you
// want in a log); 2 or 4 gives pretty output (what you want in a terminal).
std::string to_json(const Cbor& value, int indent = 0);

}  // namespace uninet
