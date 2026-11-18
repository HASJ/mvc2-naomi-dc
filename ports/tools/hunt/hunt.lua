-- Flycast's half of tools/hunt: the script port/sdl/hunt.c runs, and the same dump each game frame
local m, inp = flycast.memory, flycast.input
local function env(k) return os.getenv(k) or "" end
local syms = {}
for k, v in string.gmatch(env("MVC2_HUNT_SYMS"), "([%w_]+)=(%w+)") do syms[k] = tonumber(v) end
local function addr(s)
  local name, off = string.match(s, "^([%a_][%w_]*)%+?(%w*)$")
  if not name then return tonumber(s) end
  return syms[name] + (off ~= "" and tonumber(off) or 0)
end
local dumps = {}
for k, n in string.gmatch(env("MVC2_HUNT_DUMP"), "([%w_+]+):(%w+)") do dumps[#dumps + 1] = { addr(k), tonumber(n) } end
local bits = { start = 0x0008, up = 0x0010, down = 0x0020, left = 0x0040, right = 0x0080,
  lp = 0x0004, hp = 0x0002, a1 = 0x0001, lk = 0x0400, hk = 0x0200, a2 = 0x0100 }
local function keys(s)
  local b = 0
  for t in string.gmatch(s or "", "[^+]+") do b = b | (bits[t] or (t:sub(1, 1) == "x" and tonumber(t:sub(2), 16)) or 0) end
  return b
end
local ops = {}
for line in io.lines(env("MVC2_HUNT")) do
  local w = {}
  for t in string.gmatch(line, "%S+") do w[#w + 1] = t end
  if #w > 0 and string.sub(w[1], 1, 1) ~= "#" then
    local o = { op = w[1] }
    if o.op == "wait" or o.op == "waitne" or o.op == "poke" then
      o.addr, o.size, o.val = addr(w[2]), tonumber(w[3]), tonumber(w[4])
    elseif o.op == "until" then o.addr, o.size, o.val, o.k1, o.k2 = addr(w[2]), tonumber(w[3]), tonumber(w[4]), keys(w[5]), keys(w[6])
    elseif o.op == "frames" then o.n = tonumber(w[2])
    elseif o.op == "hold" then o.n, o.k1, o.k2 = tonumber(w[2]), keys(w[3]), keys(w[4])
    elseif o.op == "trace" then o.on = w[2] == "on" end
    ops[#ops + 1] = o
  end
end
do
  local f = io.open("hunt_lua_log.txt", "w")
  f:write(string.format("ops %d syms %s dumps %d", #ops, env("MVC2_HUNT_SYMS"), #dumps))
  f:close()
end
local function peek(a, size)
  if size == 1 then return m.read8(a) elseif size == 2 then return m.read16(a) end
  return m.read32(a)
end
local trace = env("MVC2_HUNT_TRACE") ~= "" and io.open(env("MVC2_HUNT_TRACE"), "wb") or nil
local timer_addr = syms.sys_work + 0x1C
local pc, left, tracing, last, vbl, coin_left = 1, 0, false, -1, 0, 0
local held = { 0, 0 }
local logged = -1
local function set(p, b)
  inp.releaseButtons(p, 0xFFFF)
  if b ~= 0 then inp.pressButtons(p, b) end
end
local function frame()
  local timer = m.read32(timer_addr)
  if coin_left > 0 then
    coin_left = coin_left - 1
    if coin_left == 0 then set(1, held[1]) end
  end
  if timer == last then return end
  last = timer
  if tracing and trace then
    local parts = { "HUNT", string.pack("<I4", vbl) }
    for _, d in ipairs(dumps) do
      local t = m.readTable32(d[1], d[2] // 4)
      local s = {}
      for i = 0, d[2] // 4 - 1 do s[#s + 1] = string.pack("<I4", t[d[1] + i * 4]) end
      parts[#parts + 1] = table.concat(s)
    end
    trace:write(table.concat(parts))
  end
  held = { 0, 0 }
  local coin = false
  while pc <= #ops do
    local o = ops[pc]
    if o.op == "wait" or o.op == "waitne" then
      if (peek(o.addr, o.size) == o.val) ~= (o.op == "wait") then break end
      pc = pc + 1
    elseif o.op == "until" then
      if peek(o.addr, o.size) == o.val then
        left = 0
        pc = pc + 1
      else
        if left % 16 < 2 then held = { o.k1, o.k2 } end
        left = left + 1
        break
      end
    elseif o.op == "poke" then
      if o.size == 1 then m.write8(o.addr, o.val) elseif o.size == 2 then m.write16(o.addr, o.val)
      else m.write32(o.addr, o.val) end
      pc = pc + 1
    elseif o.op == "trace" then tracing = o.on pc = pc + 1
    elseif o.op == "quit" then pc = #ops + 1
    elseif o.op == "coin" then coin = true pc = pc + 1 break
    else
      if left == 0 then left = o.n end
      if o.op == "hold" then held = { o.k1, o.k2 } end
      left = left - 1
      if left == 0 then pc = pc + 1 end
      break
    end
  end
  if pc ~= logged then
    logged = pc
    local f = io.open("hunt_lua_log.txt", "a")
    f:write(string.format("vbl %d timer %d op %d %s", vbl, timer, pc, ops[pc] and ops[pc].op or "end") .. string.char(10))
    f:close()
  end
  set(1, held[1] | (coin and 0x0800 or 0))
  set(2, held[2])
  if coin then coin_left = 4 end
  if pc > #ops then
    if trace then trace:close() end
    os.exit(0)
  end
end
flycast_callbacks = {
  vblank = function()
    vbl = vbl + 1
    local ok, err = pcall(frame)
    if not ok then
      local f = io.open("hunt_lua_err.txt", "w") f:write(tostring(err)) f:close()
      os.exit(1)
    end
    if vbl > tonumber(env("MVC2_HUNT_MAXVBL") ~= "" and env("MVC2_HUNT_MAXVBL") or "100000") then os.exit(3) end
  end,
}
