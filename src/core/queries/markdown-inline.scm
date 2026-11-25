; Anvil's own patterns for the inline content of Markdown, which come before those of the grammar.
; The delimiters of emphasis and of a code span have no scope of their own, so that they keep the
; style of the span they are in, as a TextMate scope stack gives them.

(code_span) @markup.inline.raw.string
(emphasis) @markup.italic
(strong_emphasis) @markup.bold
(strikethrough) @markup.strikethrough

(inline_link (link_text) @string.other.link.description)
(image (image_description) @string.other.link.description)
(link_title) @string.other.link.title
(link_destination) @markup.underline.link
(link_label) @constant.other.reference.link
