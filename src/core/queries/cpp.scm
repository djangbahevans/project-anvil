; Anvil's own patterns for C++, which come before those of C and of the grammar.

(default_method_clause "default" @keyword.other)
(delete_method_clause "delete" @keyword.other)

["catch" "throw" "try" "co_await" "co_return" "co_yield"] @keyword.control

"new" @keyword.operator.new
"delete" @keyword.operator.delete
"using" @keyword.other.using
"namespace" @keyword.other.namespace
"noexcept" @keyword.operator.noexcept
["concept" "requires"] @keyword.other

["class" "template" "typename"] @storage.type

["public" "private" "protected"] @storage.type.modifier.access

["constexpr" "constinit" "consteval" "explicit" "final" "friend" "mutable" "override" "virtual"] @storage.modifier

(this) @variable.language.this

(auto) @storage.type
(raw_string_literal) @string.quoted.double.raw
(namespace_identifier) @entity.name.namespace
