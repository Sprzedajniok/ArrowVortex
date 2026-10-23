"""Runs the ArrowReplay Simply Love module against a mocked ITGmania Lua API.

usage: python test_recorder.py ../ArrowReplay.lua   (needs `pip install lupa`)
"""
import sys

from lupa import lua51

MOCKS = r'''
written = {}
local now = 0
function set_time(t) now = t end

function ToEnumShortString(v) return (tostring(v):gsub("^[^_]*_", "")) end
function ivalues(t) local i = 0 return function() i = i + 1 return t[i] end end
function Trace(s) end
lua = { ReportScriptError = function(s) error(s) end }
function Year() return 2026 end
function MonthOfYear() return 9 end
function DayOfMonth() return 22 end
function Hour() return 21 end
function Minute() return 5 end
function Second() return 7 end
Def = { Actor = function(t) return t end }

local names = { "Left", "Down", "Up", "Right" }
local style = {
	GetStyleType = function() return "StyleType_OnePlayerOneSide" end,
	ColumnsPerPlayer = function() return 4 end,
	GetColumnInfo = function(self, pn, col) return { Name = names[col] } end,
}
local song = {
	GetSongDir = function() return "/Songs/Pack/Song/" end,
	GetSongFilePath = function() return "/Songs/Pack/Song/song.ssc" end,
	GetDisplayMainTitle = function() return "Test: Song?" end,
	GetDisplaySubTitle = function() return "" end,
	GetDisplayArtist = function() return "Someone" end,
}
local steps = {
	GetStepsType = function() return "StepsType_Dance_Single" end,
	GetDifficulty = function() return "Difficulty_Challenge" end,
	GetMeter = function() return 12 end,
	GetDescription = function() return "desc" end,
	GetChartName = function() return "" end,
}
local playerstate = {
	GetSongPosition = function() return { GetMusicSeconds = function() return now end } end,
	GetPlayerController = function() return "PlayerController_Human" end,
}
GAMESTATE = {
	GetHumanPlayers = function() return { "PlayerNumber_P1" } end,
	GetCurrentSong = function() return song end,
	GetCurrentSteps = function() return steps end,
	GetCurrentStyle = function() return style end,
	GetPlayerState = function() return playerstate end,
	GetSongOptionsObject = function() return { MusicRate = function() return 1.5 end } end,
	GetCurrentGame = function() return { CountNotesSeparately = function() return false end } end,
}
local profile_dirs = { ProfileSlot_Player1 = "/Save/LocalProfiles/00000000/",
	ProfileSlot_Player2 = "/Save/LocalProfiles/00000001/" }
PROFILEMAN = { GetProfile = function() return { GetDisplayName = function() return "Player" end } end,
	GetProfileDir = function(self, slot) return profile_dirs[slot] or "" end }
FILEMAN = { DoesFileExist = function(self, path) return written[path] ~= nil end }

-- Simply Love's generic SelectMultiple OptionRow, see SL-PlayerOptions.lua.
mods = { PlayerNumber_P1 = {}, PlayerNumber_P2 = {} }
function CustomOptionRow(name)
	if name ~= "GameplayExtrasB" and name ~= "GameplayExtras" then return false end
	local row = { Name = name, SelectType = "SelectMultiple",
		Values = { "JudgmentTilt", "ColumnCues" }, Choices = { "Judgment Tilt", "Column Cues" } }
	row.LoadSelections = function(self, list, pn)
		for i, v in ipairs(row.Values) do list[i] = mods[pn][v] or false end
		return list
	end
	row.SaveSelections = function(self, list, pn)
		for i, v in ipairs(row.Values) do mods[pn][v] = list[i] end
	end
	return row
end
local prefs = { TimingWindowScale = 1, TimingWindowAdd = 0.0015, GlobalOffsetSeconds = -0.008,
	TimingWindowSecondsW1 = 0.0215, TimingWindowSecondsW2 = 0.043, TimingWindowSecondsW3 = 0.102,
	TimingWindowSecondsW4 = 0.135, TimingWindowSecondsW5 = 0.18, TimingWindowSecondsHold = 0.32,
	TimingWindowSecondsMine = 0.07, TimingWindowSecondsRoll = 0.35 }
PREFSMAN = { GetPreference = function(self, name) return prefs[name] end }
SL = { Global = { GameMode = "ITG" }, Preferences = { ["FA+"] = { TimingWindowSecondsW1 = 0.0135 } },
	P1 = { Streams = { Hash = "abc123" } } }
STATSMAN = { GetCurStageStats = function() return { GetPlayerStageStats = function()
	return { GetFailed = function() return false end } end } end }
RageFileUtil = { CreateRageFile = function()
	local f = {}
	function f:Open(path, mode) self.path = path return true end
	function f:Write(s) written[self.path] = s end
	function f:Read() return written[self.path] or "" end
	function f:Close() end
	function f:destroy() end
	return f
end }

local top_name = "ScreenGameplay"
callbacks = {}
SCREENMAN = { GetTopScreen = function() return {
	GetName = function() return top_name end,
	AddInputCallback = function(self, cb) callbacks[#callbacks + 1] = cb end,
} end }
function set_screen(name) top_name = name end

function tapnote(tnt, tns, offset, held)
	return {
		GetTapNoteType = function() return "TapNoteType_" .. tnt end,
		GetTapNoteResult = function() return {
			GetTapNoteScore = function() return "TapNoteScore_" .. tns end,
			GetTapNoteOffset = function() return offset end,
			GetHeld = function() return held or false end,
		} end,
	}
end
'''

SCENARIO = r'''
local module = dofile(MODULE_PATH)
local rec = module["ScreenGameplay"]
assert(rec, "module must register ScreenGameplay")
rec:ModuleCommand()
assert(#callbacks == 1, "input callback registered")
local input = callbacks[1]

local function press(t, button, down, ago, controller, player)
	set_time(t)
	input({ type = down and "InputEventType_FirstPress" or "InputEventType_Release",
		PlayerNumber = player or "PlayerNumber_P1", button = button,
		controller = controller or "GameController_1",
		DeviceInput = { ago = ago or 0.002 } })
end

-- A single tap on Down, judged before the input callback (engine order).
set_time(10.000)
rec:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_W1",
	TapNoteOffset = -0.004, FirstTrack = 1, EarlyTapNoteScore = "TapNoteScore_None", EarlyTapNoteOffset = 0,
	Notes = { [2] = tapnote("Tap", "W1", -0.004) } })
press(10.000, "Down", true)
press(10.080, "Down", false)
-- Repeat events and other players' presses are ignored.
input({ type = "InputEventType_Repeat", PlayerNumber = "PlayerNumber_P1", button = "Down", controller = "GameController_1" })
press(10.1, "Left", true, 0, "GameController_2", "PlayerNumber_P2")
-- A stray press, a menu button, and a jump with a mine in the row.
press(10.300, "Up", true)
press(10.320, "Start", true)
set_time(10.5)
rec:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_W2",
	TapNoteOffset = 0.03, FirstTrack = 0, EarlyTapNoteScore = "TapNoteScore_None",
	Notes = { [1] = tapnote("Tap", "W1", 0.01), [3] = tapnote("Mine", "None", 0), [4] = tapnote("HoldHead", "W2", 0.03) } })
-- Early hit, hold result, mines.
rec:EarlyHitMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_W4", Column = 2, TapNoteOffset = -0.12 })
rec:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_W2", HoldNoteScore = "HoldNoteScore_LetGo", FirstTrack = 3 })
rec:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_HitMine", FirstTrack = 2 })
rec:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_AvoidMine", FirstTrack = 2 })

set_screen("ScreenEvaluationStage")
rec:ScreenChangedMessageCommand()
-- Nothing is recorded outside of gameplay.
press(20, "Down", true)
REC = rec
'''

TOGGLE_SCENARIO = r'''
local P1_SETTINGS = "/Save/LocalProfiles/00000000/ArrowReplay.ini"
local row = CustomOptionRow("GameplayExtrasB")
assert(#row.Choices == 3 and row.Choices[3] == "Record Replay", "choice added")
assert(row.Values[3] == "ArrowReplayRecord")
assert(#CustomOptionRow("GameplayExtras").Choices == 2, "other rows are untouched")

local list = row:LoadSelections({ false, false, false }, "PlayerNumber_P1")
assert(list[3] == true, "recording is on by default")
row:SaveSelections(list, "PlayerNumber_P1")
assert(written[P1_SETTINGS] == nil, "nothing is written while the choice is unchanged")

list[1] = true
list[3] = false
row:SaveSelections(list, "PlayerNumber_P1")
assert(mods.PlayerNumber_P1.JudgmentTilt == true, "the row's own choices still work")
assert(written[P1_SETTINGS] == "Record=0\n", tostring(written[P1_SETTINGS]))

-- The choice is read back from the profile.
written["/Save/LocalProfiles/00000001/ArrowReplay.ini"] = "Record=0\n"
local p2 = CustomOptionRow("GameplayExtrasB"):LoadSelections({ false, false, false }, "PlayerNumber_P2")
assert(p2[3] == false, "saved choice is loaded")

-- With recording turned off, no replay is written.
local function replays()
	local n = 0
	for path in pairs(written) do
		if path:find("%.avreplay$") then n = n + 1 end
	end
	return n
end
for path in pairs(written) do
	if path:find("%.avreplay$") then written[path] = nil end
end
set_screen("ScreenGameplay")
REC:ModuleCommand()
set_time(30)
REC:JudgmentMessageCommand({ Player = "PlayerNumber_P1", TapNoteScore = "TapNoteScore_W1",
	TapNoteOffset = 0, FirstTrack = 0, EarlyTapNoteScore = "TapNoteScore_None",
	Notes = { [1] = tapnote("Tap", "W1", 0) } })
set_screen("ScreenEvaluationStage")
REC:ScreenChangedMessageCommand()
assert(replays() == 0, "no replay when recording is off")
'''


def main():
    lua = lua51.LuaRuntime(unpack_returned_tuples=True)
    lua.execute(MOCKS)
    lua.globals().MODULE_PATH = sys.argv[1]
    lua.execute(SCENARIO)
    written = dict(lua.globals().written.items())
    assert len(written) == 1, written
    path, text = next(iter(written.items()))
    print(path)
    print(text)

    assert path == 'Save/ArrowReplays/20261022-210507_P1_Test Song.avreplay', path
    header, body = text.split('\n---\n')
    fields = dict(line.split('\t', 1) for line in header.split('\n'))
    assert fields['Rate'] == '1.500000'
    assert fields['GlobalOffset'] == '-0.008000'
    assert fields['StepsType'] == 'StepsType_Dance_Single'
    assert fields['ChartHash'] == 'abc123'
    assert 'W0=0.015000' in fields['Windows'] and 'W5=0.181500' in fields['Windows']
    lines = body.strip().split('\n')
    expected = [
        'T\t10.000000\t2\tW1\t-0.004000\tNone\t0.000000\t0\tTap',
        'P\t9.997000\t2\t1',
        'P\t10.077000\t2\t0',
        'P\t10.297000\t3\t1',
        'T\t10.500000\t1\tW1\t0.010000\tNone\t0.000000\t0\tTap',
        'T\t10.500000\t4\tW2\t0.030000\tNone\t0.000000\t0\tHoldHead',
        'E\t10.500000\t3\tW4\t-0.120000',
        'H\t10.500000\t4\tLetGo',
        'M\t10.500000\t3\tHitMine',
        'END\t10.500000\t0\tScreenEvaluationStage',
    ]
    assert sorted(lines[4:6]) == sorted(expected[4:6]), lines
    lines[4:6] = expected[4:6]
    assert lines == expected, '\n'.join(lines)

    lua.execute(TOGGLE_SCENARIO)
    print('recorder test passed')


if __name__ == '__main__':
    main()
