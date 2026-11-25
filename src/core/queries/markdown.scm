; Anvil's own patterns for the blocks of Markdown, which come before those of the grammar. They
; give the nodes the scopes of VS Code's Markdown grammar that themes style: the grammar's query
; captures the marks of headings, lists and quotes alike as `punctuation.special`.

(atx_heading (inline) @markup.heading)
(setext_heading (paragraph) @markup.heading)
[
  (atx_h1_marker)
  (atx_h2_marker)
  (atx_h3_marker)
  (atx_h4_marker)
  (atx_h5_marker)
  (atx_h6_marker)
  (setext_h1_underline)
  (setext_h2_underline)
] @markup.heading

[
  (list_marker_plus)
  (list_marker_minus)
  (list_marker_star)
  (list_marker_dot)
  (list_marker_parenthesis)
] @punctuation.definition.list.begin

(block_quote_marker) @punctuation.definition.quote.begin
((block_continuation) @punctuation.definition.quote.begin
 (#match? @punctuation.definition.quote.begin ">"))

(thematic_break) @meta.separator

(fenced_code_block) @markup.fenced_code.block
(indented_code_block) @markup.raw.block
(info_string (language) @fenced_code.block.language)

(link_title) @string.other.link.title
(link_destination) @markup.underline.link
(link_label) @constant.other.reference.link
(backslash_escape) @constant.character.escape
