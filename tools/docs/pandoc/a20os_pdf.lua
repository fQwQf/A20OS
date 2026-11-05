-- Render every Pandoc display formula through the template's bounded math
-- environment. Display math is represented as an Inline inside a Para, so
-- split any surrounding label text into separate paragraphs first.
function Para(paragraph)
  local blocks = {}
  local buffered = {}
  local has_display_math = false

  local function flush_text()
    if #buffered > 0 then
      table.insert(blocks, pandoc.Para(buffered))
      buffered = {}
    end
  end

  for _, inline in ipairs(paragraph.content) do
    if inline.t == "Math" and inline.mathtype == "DisplayMath" then
      has_display_math = true
      flush_text()
      table.insert(
        blocks,
        pandoc.RawBlock(
          "latex",
          "\\begin{aosdisplaymath}\n" .. inline.text .. "\n\\end{aosdisplaymath}"
        )
      )
    else
      table.insert(buffered, inline)
    end
  end

  if has_display_math then
    flush_text()
    return blocks
  end
end
