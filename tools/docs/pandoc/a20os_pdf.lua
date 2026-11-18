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

-- Paths and ABI identifiers often exceed table cells or text width. Add
-- invisible break opportunities without changing the printed/copied text.
local function breakable_text(text)
  local escapes = {
    ['\\'] = '\\textbackslash{}', ['{'] = '\\{', ['}'] = '\\}',
    ['#'] = '\\#', ['$'] = '\\$', ['%'] = '\\%', ['&'] = '\\&',
    ['_'] = '\\_', ['^'] = '\\textasciicircum{}', ['~'] = '\\textasciitilde{}'
  }
  local printed, plain, run = {}, {}, 0
  for _, point in utf8.codes(text) do
    local char = utf8.char(point)
    local escaped = escapes[char] or char
    table.insert(printed, escaped)
    table.insert(plain, escaped)
    run = run + 1
    if char:match('[%s%p]') or run >= 6 then
      table.insert(printed, '\\allowbreak{}')
      run = 0
    end
  end
  return table.concat(printed), table.concat(plain)
end

function Code(code)
  if not FORMAT:match('latex') then return nil end
  local printed, plain = breakable_text(code.text)
  return pandoc.RawInline('latex', '\\texorpdfstring{\\texttt{' .. printed .. '}}{' .. plain .. '}')
end

-- Some reference tables use bare identifiers instead of Markdown Code nodes.
function Str(str)
  if not FORMAT:match('latex') or not str.text:find('[_/=]') then return nil end
  local printed, plain = breakable_text(str.text)
  return pandoc.RawInline('latex', '\\texorpdfstring{' .. printed .. '}{' .. plain .. '}')
end

function Math(math)
  if FORMAT:match('latex') and math.mathtype == 'InlineMath' and math.text:find(',') then
    return pandoc.RawInline('latex', '\\(' .. math.text:gsub(',', ',\\allowbreak{}') .. '\\)')
  end
end
