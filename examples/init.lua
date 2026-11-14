-- Example cedit Lua configuration (build with `make LUA=1`).
-- Copy to ~/.config/cedit/init.lua and `chmod 600` it.
-- Scripts run with your full privileges: they are NOT sandboxed.

-- Insert today's date.
cedit.command("insert-date", function()
  cedit.insert(os.date("%Y-%m-%d"))
end)
cedit.bind("C-c d", "insert-date")

-- Duplicate the current line below the cursor.
cedit.command("duplicate-line", function()
  local line, col = cedit.cursor()
  local text = cedit.get_line(line)
  cedit.set_cursor(line, #text + 1)   -- end of line
  cedit.insert("\n" .. text)
  cedit.set_cursor(line + 1, col)
end)
cedit.bind("C-c C-d", "duplicate-line")

-- Options set from Lua apply editor-wide, like config lines.
cedit.set("line-numbers relative")

cedit.on("save", function(name)
  cedit.message("Saved " .. name)
end)
