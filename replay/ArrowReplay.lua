-- ArrowReplay recorder (Simply Love module for ITGmania)
--
-- Copy this file to "Themes/Simply Love/Modules/". While you play, it records
-- every panel press and release plus every judgment the engine hands out, and
-- when the song ends it writes a replay to "Save/ArrowReplays/". Open that file
-- in the ArrowVortex replay viewer (Replay > Open replay...). Recording can be
-- turned off per player with "Record Replay" in the Gameplay Extras row of the
-- second page of the player options.
--
-- Times are written in the engine's music seconds (the clock judgments use),
-- without the global offset. The viewer converts them to chart time with the
-- GlobalOffset, SyncOffset and Rate fields from the header.
--
-- Event lines (tab separated):
--   P  time  col  down             panel pressed (1) or released (0)
--   T  time  col  tns  offset  earlyTns  earlyOffset  heldMiss  noteType
--                                  final judgment of one tap/hold head/lift
--   E  time  col  tns  offset      early hit that may still get rescored
--   H  time  col  hns              hold/roll result
--   M  time  col  tns              mine hit
-- "time" is when the engine reported the event, "offset" is the engine's
-- TapNoteOffset in real seconds (negative = early), columns start at 1.
-- The last line is "END  time  failed  next", where next is the screen that
-- followed (ScreenEvaluationStage when the song was finished).

local FORMAT_VERSION = 1
local OUT_DIR = "Save/ArrowReplays/"
local GAMEPLAY_SCREEN = "ScreenGameplay"

local recordings = {}  -- [PlayerNumber] = recording
local recording = false
local in_gameplay = false

-- "TapNoteScore_W1" -> "W1". Unlike ToEnumShortString this never asserts, an
-- error here would cost the rest of the recording.
local function short(value)
	if value == nil then return "None" end
	return (tostring(value):gsub("^[^_]*_", ""))
end

local function clean(s)
	return (tostring(s or ""):gsub("[\t\r\n]", " "))
end

local function music_seconds(player)
	return GAMESTATE:GetPlayerState(player):GetSongPosition():GetMusicSeconds()
end

local function pref(name, default)
	local ok, value = pcall(function() return PREFSMAN:GetPreference(name) end)
	if ok and type(value) == "number" then return value end
	return default
end

-- "Record Replay" toggle in the Gameplay Extras row on the second page of the
-- player options, saved per profile in ArrowReplay.ini. Simply Love has no way
-- for modules to add an OptionRow, so this adds a choice to one of its rows by
-- wrapping its global CustomOptionRow(). Without that function (other themes)
-- every play is recorded.
local OPTION_ROW = "GameplayExtrasB"
local OPTION_CHOICE = "Record Replay"
local SETTINGS_FILE = "ArrowReplay.ini"

local record_enabled = {}  -- [PlayerNumber] = false when turned off

local function settings_path(player)
	local slot = { PlayerNumber_P1 = "ProfileSlot_Player1", PlayerNumber_P2 = "ProfileSlot_Player2" }
	local ok, dir = pcall(function() return PROFILEMAN:GetProfileDir(slot[player]) end)
	-- Guests have no profile, their choice lasts until the game is closed.
	if not ok or not dir or dir == "" then return nil end
	return dir .. SETTINGS_FILE
end

local function load_enabled(player)
	local path = settings_path(player)
	local enabled = true
	if not path then
		enabled = record_enabled[player] ~= false
	elseif FILEMAN:DoesFileExist(path) then
		local f = RageFileUtil.CreateRageFile()
		if f:Open(path, 1) then
			enabled = not f:Read():find("Record=0")
			f:Close()
		end
		f:destroy()
	end
	record_enabled[player] = enabled
	return enabled
end

local function save_enabled(player, enabled)
	if record_enabled[player] == enabled then return end
	record_enabled[player] = enabled
	local path = settings_path(player)
	if not path then return end
	local f = RageFileUtil.CreateRageFile()
	if f:Open(path, 2) then
		f:Write(enabled and "Record=1\n" or "Record=0\n")
		f:Close()
	end
	f:destroy()
end

if type(CustomOptionRow) == "function" then
	local custom_option_row = CustomOptionRow
	CustomOptionRow = function(name)
		local row = custom_option_row(name)
		if name ~= OPTION_ROW or type(row) ~= "table" or type(row.Choices) ~= "table"
			or row.SelectType ~= "SelectMultiple" then
			return row
		end
		local index = #row.Choices + 1
		row.Choices[index] = OPTION_CHOICE
		if row.Values then row.Values[index] = "ArrowReplayRecord" end
		local load, save = row.LoadSelections, row.SaveSelections
		row.LoadSelections = function(self, list, player)
			list = load(self, list, player)
			list[index] = load_enabled(player)
			return list
		end
		row.SaveSelections = function(self, list, player)
			save(self, list, player)
			save_enabled(player, list[index] == true)
		end
		return row
	end
end

-- Maps "<side><GameButton>" (e.g. "1Left", "2Up" for doubles) to a column.
local function build_column_map(player)
	local style = GAMESTATE:GetCurrentStyle(player)
	local two_sides = style:GetStyleType() == "StyleType_OnePlayerTwoSides"
	local map, seen = {}, {}
	for col = 1, style:ColumnsPerPlayer() do
		local name = style:GetColumnInfo(player, col).Name
		seen[name] = (seen[name] or 0) + 1
		map[seen[name] .. name] = col
	end
	return map, two_sides, style:ColumnsPerPlayer()
end

local function counts_notes_separately()
	local ok, value = pcall(function() return GAMESTATE:GetCurrentGame():CountNotesSeparately() end)
	return ok and value or false
end

local function timing_windows()
	local scale = pref("TimingWindowScale", 1)
	local add = pref("TimingWindowAdd", 0)
	local parts = {}
	for i = 1, 5 do
		local w = pref("TimingWindowSecondsW" .. i, 0)
		parts[#parts+1] = string.format("W%d=%.6f", i, w * scale + add)
	end
	-- FA+ (blue fantastic) window used by Simply Love on top of the ITG windows.
	local fa = 0.0135
	if SL and SL.Preferences and SL.Preferences["FA+"] then
		fa = SL.Preferences["FA+"].TimingWindowSecondsW1 or fa
	end
	parts[#parts+1] = string.format("W0=%.6f", fa * scale + add)
	parts[#parts+1] = string.format("Hold=%.6f", pref("TimingWindowSecondsHold", 0) * scale + add)
	parts[#parts+1] = string.format("Mine=%.6f", pref("TimingWindowSecondsMine", 0) * scale + add)
	parts[#parts+1] = string.format("Roll=%.6f", pref("TimingWindowSecondsRoll", 0) * scale + add)
	return table.concat(parts, " ")
end

-- The sync correction ITGmania applies to the song's pack (Pack.ini
-- SyncOffset, or the DefaultSyncOffset preference): -0.009 for ITG synced
-- packs, 0 for null synced ones. Empty if this ITGmania has no pack groups.
local function sync_offset(song)
	local ok, value = pcall(function()
		local group = SONGMAN:GetGroup(song)
		return group and group:GetSyncOffset()
	end)
	if ok and type(value) == "number" then return string.format("%.6f", value) end
	return ""
end

local function date_string(sep_date, sep_mid, sep_time)
	return string.format("%04d%s%02d%s%02d%s%02d%s%02d%s%02d",
		Year(), sep_date, MonthOfYear() + 1, sep_date, DayOfMonth(), sep_mid,
		Hour(), sep_time, Minute(), sep_time, Second())
end

local function build_header(player, song, steps, rate, columns)
	local pn = ToEnumShortString(player)
	local profile = PROFILEMAN:GetProfile(player)
	local hash = ""
	if SL and SL[pn] and SL[pn].Streams and SL[pn].Streams.Hash then
		hash = SL[pn].Streams.Hash
	end
	local lines = {
		{ "ArrowReplay", FORMAT_VERSION },
		{ "Player", pn },
		{ "Profile", profile and profile:GetDisplayName() or "" },
		{ "SongDir", song:GetSongDir() },
		{ "SimfilePath", song:GetSongFilePath() },
		{ "Title", song:GetDisplayMainTitle() },
		{ "Subtitle", song:GetDisplaySubTitle() },
		{ "Artist", song:GetDisplayArtist() },
		{ "StepsType", steps:GetStepsType() },
		{ "Difficulty", steps:GetDifficulty() },
		{ "Meter", steps:GetMeter() },
		{ "Description", steps:GetDescription() },
		{ "ChartName", steps:GetChartName() },
		{ "ChartHash", hash },
		{ "Columns", columns },
		{ "Rate", string.format("%.6f", rate) },
		{ "GlobalOffset", string.format("%.6f", pref("GlobalOffsetSeconds", 0)) },
		{ "SyncOffset", sync_offset(song) },
		{ "GameMode", (SL and SL.Global and SL.Global.GameMode) or "" },
		{ "Windows", timing_windows() },
		{ "Date", date_string("-", " ", ":") },
		{ "Autoplay", GAMESTATE:GetPlayerState(player):GetPlayerController() == "PlayerController_Autoplay" and 1 or 0 },
	}
	local out = {}
	for _, kv in ipairs(lines) do
		out[#out+1] = clean(kv[1]) .. "\t" .. clean(kv[2])
	end
	return table.concat(out, "\n")
end

local function start_recording()
	recordings = {}
	local song = GAMESTATE:GetCurrentSong()
	if not song then return end
	local rate = GAMESTATE:GetSongOptionsObject("ModsLevel_Song"):MusicRate()
	for player in ivalues(GAMESTATE:GetHumanPlayers()) do
		local steps = GAMESTATE:GetCurrentSteps(player)
		if steps and record_enabled[player] ~= false then
			local colmap, two_sides, columns = build_column_map(player)
			recordings[player] = {
				player = player,
				song = song,
				colmap = colmap,
				two_sides = two_sides,
				rate = rate,
				separate = counts_notes_separately(),
				header = build_header(player, song, steps, rate, columns),
				events = {},
				last_time = 0,
			}
		end
	end
	recording = true
end

local function write_recording(rec, next_screen)
	if #rec.events == 0 then return end
	local stats = STATSMAN:GetCurStageStats():GetPlayerStageStats(rec.player)
	local failed = stats and stats:GetFailed() or false
	local text = rec.header .. "\n---\n" .. table.concat(rec.events, "\n")
		.. string.format("\nEND\t%.6f\t%d\t%s\n", rec.last_time, failed and 1 or 0, clean(next_screen))

	local title = rec.song:GetDisplayMainTitle():gsub("[^%w%-_ ]", ""):sub(1, 40)
	local path = string.format("%s%s_%s_%s.avreplay", OUT_DIR,
		date_string("", "-", ""), ToEnumShortString(rec.player), title)

	local f = RageFileUtil.CreateRageFile()
	if f:Open(path, 2) then
		f:Write(text)
		f:Close()
		Trace("ArrowReplay: wrote " .. path)
	else
		lua.ReportScriptError("ArrowReplay: could not write " .. path)
	end
	f:destroy()
end

local function stop_recording(next_screen)
	if not recording then return end
	recording = false
	for _, rec in pairs(recordings) do
		write_recording(rec, next_screen)
	end
	recordings = {}
end

local function add_event(rec, line, t)
	rec.events[#rec.events+1] = line
	if t > rec.last_time then rec.last_time = t end
end

local function on_input(event)
	if not recording then return false end
	if event.type ~= "InputEventType_FirstPress" and event.type ~= "InputEventType_Release" then
		return false
	end
	local rec = recordings[event.PlayerNumber]
	if not rec then return false end

	local side = 1
	if rec.two_sides and event.controller == "GameController_2" then side = 2 end
	local col = rec.colmap[side .. tostring(event.button)]
	if not col then return false end

	-- The engine judges a step at "now - ago" (DeviceInput timestamp), do the same.
	local ago = event.DeviceInput and event.DeviceInput.ago or 0
	local t = music_seconds(rec.player) - ago * rec.rate
	local down = event.type == "InputEventType_FirstPress" and 1 or 0
	add_event(rec, string.format("P\t%.6f\t%d\t%d", t, col, down), t)
	return false
end

local function on_judgment(params)
	local rec = recordings[params.Player]
	if not rec then return end
	local t = music_seconds(rec.player)

	if params.HoldNoteScore then
		add_event(rec, string.format("H\t%.6f\t%d\t%s", t, (params.FirstTrack or 0) + 1,
			short(params.HoldNoteScore)), t)
		return
	end

	local tns = short(params.TapNoteScore)
	if tns == "HitMine" then
		add_event(rec, string.format("M\t%.6f\t%d\t%s", t, (params.FirstTrack or 0) + 1, tns), t)
		return
	end
	if tns == "AvoidMine" or not params.Notes then return end

	local only_col = rec.separate and (params.FirstTrack or 0) + 1 or nil
	for col, tapnote in pairs(params.Notes) do
		local tnt = short(tapnote:GetTapNoteType())
		if (tnt == "Tap" or tnt == "HoldHead" or tnt == "Lift") and (not only_col or col == only_col) then
			local result = tapnote:GetTapNoteResult()
			add_event(rec, string.format("T\t%.6f\t%d\t%s\t%.6f\t%s\t%.6f\t%d\t%s",
				t, col, short(result:GetTapNoteScore()), result:GetTapNoteOffset(),
				short(params.EarlyTapNoteScore), params.EarlyTapNoteOffset or 0,
				result:GetHeld() and 1 or 0, tnt), t)
		end
	end
end

local recorder = Def.Actor{
	ModuleCommand=function(self)
		in_gameplay = true
		start_recording()
		local screen = SCREENMAN:GetTopScreen()
		if screen then screen:AddInputCallback(on_input) end
	end,
	ScreenChangedMessageCommand=function(self)
		-- Fires before ModuleCommand, also when gameplay restarts into itself.
		local screen = SCREENMAN:GetTopScreen()
		stop_recording(screen and screen:GetName() or "")
		in_gameplay = false
	end,
	BeforeLoadingNextCourseSongMessageCommand=function(self)
		stop_recording("NextCourseSong")
	end,
	DoneLoadingNextSongMessageCommand=function(self)
		-- Course mode: every song gets its own replay file.
		if in_gameplay and not recording then start_recording() end
	end,
	JudgmentMessageCommand=function(self, params)
		if recording then on_judgment(params) end
	end,
	EarlyHitMessageCommand=function(self, params)
		if not recording then return end
		local rec = recordings[params.Player]
		if not rec then return end
		local t = music_seconds(rec.player)
		add_event(rec, string.format("E\t%.6f\t%d\t%s\t%.6f", t, (params.Column or 0) + 1,
			short(params.TapNoteScore), params.TapNoteOffset or 0), t)
	end,
}

return { [GAMEPLAY_SCREEN] = recorder }
