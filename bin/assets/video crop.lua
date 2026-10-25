-- Loaded into mpv by ArrowVortex when it shows the recording of a replayed
-- play. Press c and drag over the part of the video to show (or click two
-- of its corners), e.g. the camera or the pad pressure values, and again to
-- zoom in further. Shift+C shows the whole video. ArrowVortex keeps the crop
-- for the next videos.

local mp = require 'mp'

-- The part of the video shown, in video pixels, nil for all of it.
local crop = nil
local selecting = false
local corner = nil  -- Where the drag started, in OSD coordinates.
local overlay = mp.create_osd_overlay('ass-events')

local function format_crop(c)
    return string.format('%d:%d:%d:%d', c.w, c.h, c.x, c.y)
end

local function parse_crop(text)
    local w, h, x, y = (text or ''):match('^(%d+):(%d+):(%d+):(%d+)$')
    if not w then return nil end
    return {w = tonumber(w), h = tonumber(h), x = tonumber(x), y = tonumber(y)}
end

local function apply(c)
    if crop then mp.commandv('vf', 'remove', '@arrowvortex-crop') end
    if c then
        mp.commandv('vf', 'add', '@arrowvortex-crop:crop=' .. format_crop(c))
    end
    crop = c
end

-- Tells ArrowVortex, which saves it.
local function report()
    mp.commandv('script-message', 'arrowvortex-crop', crop and format_crop(crop) or '')
end

-- The video pixel under an OSD point, within the part that is shown.
local function to_video(px, py)
    local d = mp.get_property_native('osd-dimensions')
    local width, height = mp.get_property_number('width'), mp.get_property_number('height')
    if not d or not width or not height then return nil end
    local shown = crop or {w = width, h = height, x = 0, y = 0}
    local w, h = d.w - d.ml - d.mr, d.h - d.mt - d.mb
    if w <= 0 or h <= 0 then return nil end
    local x = shown.x + (px - d.ml) / w * shown.w
    local y = shown.y + (py - d.mt) / h * shown.h
    x = math.max(shown.x, math.min(shown.x + shown.w, x))
    y = math.max(shown.y, math.min(shown.y + shown.h, y))
    return x, y
end

local function draw()
    local d = mp.get_property_native('osd-dimensions')
    local mouse = mp.get_property_native('mouse-pos')
    if not corner or not d or not mouse then
        overlay:remove()
        return
    end
    overlay.res_x, overlay.res_y = d.w, d.h
    overlay.data = string.format(
        '{\\an7\\pos(0,0)\\bord2\\shad0\\1a&HE0&\\3c&H00D7FF&\\p1}m %d %d l %d %d l %d %d l %d %d{\\p0}',
        corner.x, corner.y, mouse.x, corner.y, mouse.x, mouse.y, corner.x, mouse.y)
    overlay:update()
end

-- Window dragging, which mpv starts when the mouse moves with the left
-- button down, also for script key bindings, and cancels the button then.
local window_dragging = nil

local function stop()
    selecting, corner = false, nil
    overlay:remove()
    mp.unobserve_property(draw)
    mp.remove_key_binding('arrowvortex-drag')
    mp.remove_key_binding('arrowvortex-double')
    mp.remove_key_binding('arrowvortex-cancel')
    if window_dragging ~= nil then
        mp.set_property_bool('window-dragging', window_dragging)
        window_dragging = nil
    end
end

local function finish(mouse)
    local x0, y0 = to_video(corner.x, corner.y)
    local x1, y1 = to_video(mouse.x, mouse.y)
    stop()
    if not x0 or not x1 then return end
    -- Even sizes, which every pixel format can be cropped to.
    local c = {x = math.floor(math.min(x0, x1) / 2) * 2, y = math.floor(math.min(y0, y1) / 2) * 2}
    c.w = math.floor((math.max(x0, x1) - c.x) / 2) * 2
    c.h = math.floor((math.max(y0, y1) - c.y) / 2) * 2
    if c.w < 16 or c.h < 16 then
        mp.osd_message('The part is too small, press c to try again.')
        return
    end
    apply(c)
    report()
end

-- A drag, or two clicks on opposite corners.
local function drag(event)
    local mouse = mp.get_property_native('mouse-pos')
    if not mouse or event.canceled then return end
    if event.event == 'down' or event.event == 'press' then
        if corner then return finish(mouse) end
        corner = {x = mouse.x, y = mouse.y}
        draw()
    elseif event.event == 'up' and corner and
        (math.abs(mouse.x - corner.x) > 8 or math.abs(mouse.y - corner.y) > 8) then
        finish(mouse)
    end
end

local function select()
    if selecting then return stop() end
    selecting = true
    window_dragging = mp.get_property_bool('window-dragging')
    mp.set_property_bool('window-dragging', false)
    mp.add_forced_key_binding('MBTN_LEFT', 'arrowvortex-drag', drag, {complex = true})
    -- A second click soon after the first is a double click, which replaces
    -- it (and would toggle fullscreen).
    mp.add_forced_key_binding('MBTN_LEFT_DBL', 'arrowvortex-double', drag, {complex = true})
    mp.add_forced_key_binding('ESC', 'arrowvortex-cancel', stop)
    mp.observe_property('mouse-pos', 'native', draw)
    mp.osd_message('Drag over the part of the video to show, or click two of its corners (Esc cancels).', 4)
end

local function reset()
    if selecting then stop() end
    apply(nil)
    report()
    mp.osd_message('Showing the whole video.')
end

mp.add_key_binding('c', 'arrowvortex-select', select)
mp.add_key_binding('C', 'arrowvortex-reset', reset)

-- The crop ArrowVortex kept, applied when it fits this video.
mp.register_event('file-loaded', function()
    local saved = parse_crop(mp.get_opt('arrowvortex-crop'))
    local width, height = mp.get_property_number('width'), mp.get_property_number('height')
    if saved and width and height and saved.x + saved.w <= width and saved.y + saved.h <= height then
        apply(saved)
    else
        mp.osd_message('Press c to show only a part of the video.', 4)
    end
end)
