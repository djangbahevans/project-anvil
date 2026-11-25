; Anvil's own patterns for the JSX of JavaScript and TSX: the brackets of a tag have the scope of
; VS Code's grammar, which themes give a color of their own.

(jsx_opening_element (["<" ">"]) @punctuation.definition.tag)
(jsx_closing_element (["</" ">"]) @punctuation.definition.tag)
(jsx_self_closing_element (["<" "/>"]) @punctuation.definition.tag)
