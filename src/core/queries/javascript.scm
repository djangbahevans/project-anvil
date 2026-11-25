; Anvil's own patterns for JavaScript, which come before those of the grammar. They give the
; nodes the names that the scopes of VS Code's JavaScript grammar make themes tell apart: control
; flow from storage and from operators. The captures are the scope names without the language.

([(identifier) (shorthand_property_identifier) (shorthand_property_identifier_pattern)]
 @variable.other.constant
 (#match? @variable.other.constant "^[A-Z_][A-Z\\d_]+$"))

["break" "case" "catch" "continue" "debugger" "default" "do" "else" "finally" "for" "if"
 "return" "switch" "throw" "try" "while" "with" "yield" "await" "import" "export" "from" "as"]
@keyword.control

"new" @keyword.operator.new
["delete" "in" "instanceof" "typeof" "void" "of"] @keyword.operator.expression
["async" "static" "get" "set" "extends"] @storage.modifier
["class" "const" "function" "let" "var"] @storage.type
"=>" @storage.type.function.arrow

[(this) (super)] @variable.language
[(true) (false) (null) (undefined)] @constant.language

(template_string) @string.template
(escape_sequence) @constant.character.escape

(template_substitution
  "${" @punctuation.definition.template-expression
  "}" @punctuation.definition.template-expression) @embedded

(regex "/" @string.regexp)
