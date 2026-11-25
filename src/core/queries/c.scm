; Anvil's own patterns for C, which come before those of the grammar. They give the nodes below
; the names that the scopes of VS Code's C grammar make themes tell apart: control flow from
; storage and from the other keywords. The captures are the scope names without the language.

["break" "case" "continue" "default" "do" "else" "for" "goto" "if" "return" "switch" "while"] @keyword.control

["sizeof" "alignof" "_Alignof" "offsetof"] @keyword.operator.sizeof

["const" "extern" "inline" "__inline" "register" "restrict" "static" "volatile" "_Atomic"] @storage.modifier

["enum" "struct" "union" "long" "short" "signed" "unsigned"] @storage.type

(primitive_type) @storage.type
(sized_type_specifier) @storage.type

"typedef" @keyword.other.typedef

["#define" "#elif" "#else" "#endif" "#if" "#ifdef" "#ifndef" "#include"] @keyword.control.directive
(preproc_directive) @keyword.control.directive

(preproc_def name: (identifier) @entity.name.function.preprocessor)
(preproc_function_def name: (identifier) @entity.name.function.preprocessor)

(system_lib_string) @string.quoted.other.lt-gt.include
(string_literal) @string.quoted.double
(char_literal) @string.quoted.single
(escape_sequence) @constant.character.escape

[(true) (false) (null)] @constant.language
