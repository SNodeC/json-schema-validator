# Offline metaschemas

The JSON documents in `draft2020-12` are the official documents from
<https://json-schema.org/draft/2020-12/schema> and its `meta/*` references.
They are bundled without modifications under the accompanying upstream license.
CMake embeds these authoritative JSON files into a private translation unit;
installed libraries do not need a data directory or network access.

`json_validator::validate_schema` checks a schema against its declared metaschema,
or the configured default dialect's metaschema. Compilation itself stays separate
for compatibility with existing callers. Unknown extension keywords remain
annotations. A custom dialect requires an explicit loader; unknown required
vocabularies are errors and unknown optional vocabularies are ignored.

The legacy acceptance of draft-04 declarations is retained for existing users;
it selects the historical draft-07 behavior and is not a draft-04 conformance
claim. New code should declare draft-07 or 2020-12 explicitly.
