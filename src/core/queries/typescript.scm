; Anvil's own patterns for TypeScript and TSX, which come before those of JavaScript and of the
; grammar.

(predefined_type) @support.type.primitive
(type_identifier) @entity.name.type

["abstract" "declare" "implements" "override" "private" "protected" "public" "readonly"]
@storage.modifier

["enum" "interface" "namespace" "type"] @storage.type
["keyof" "satisfies"] @keyword.operator.expression

(enum_body (property_identifier) @variable.other.enummember)
(enum_assignment name: (property_identifier) @variable.other.enummember)
