-- SPDX-License-Identifier: GPL-3.0-or-later
-- harness-steps: scvb_session
--
-- A file RPC inside REAPER, for the sessions that drive SCVB's own windows. The session script outside REAPER writes
-- rpc/cmd-<n>.txt (tab-separated: op, args); this step runs it and writes rpc/res-<n>.txt. Only the fixed ops below
-- exist; nothing from the file is evaluated as code.

-- A track by index, or by name.
local function track(i)
    local n = tonumber(i)
    if n then
        local t = reaper.GetTrack(0, math.tointeger(n))
        if t then return t end
    else
        for k = 0, reaper.CountTracks(0) - 1 do
            local t = reaper.GetTrack(0, k)
            local _, name = reaper.GetSetMediaTrackInfo_String(t, "P_NAME", "", false)
            if name == i then return t end
        end
    end
    error("no track " .. tostring(i))
end

local function fxIndex(f) return math.tointeger(tonumber(f)) end

local function paramIndex(t, fx, name)
    for p = 0, reaper.TrackFX_GetNumParams(t, fx) - 1 do
        local _, n = reaper.TrackFX_GetParamName(t, fx, p, "")
        if n == name then return p end
    end
    error("no parameter " .. name)
end

local ops = {}

function ops.show(a) reaper.TrackFX_Show(track(a[1]), fxIndex(a[2]), 3); return "ok" end
function ops.hide(a) reaper.TrackFX_Show(track(a[1]), fxIndex(a[2]), 2); return "ok" end
function ops.play() reaper.OnPlayButton(); return "ok" end
function ops.stop() reaper.OnStopButton(); return "ok" end
function ops.seek(a) reaper.SetEditCurPos(tonumber(a[1]), true, true); return "ok" end
function ops.pos() return tostring(reaper.GetPlayPosition()) .. "\t" .. tostring(reaper.GetPlayState()) end
function ops.save(a) reaper.Main_SaveProjectEx(0, a[1], 0); return "ok" end
function ops.open(a) reaper.Main_openProject("noprompt:" .. a[1]); return "ok" end
-- Renders with the settings saved in the project, as File > Render does.
function ops.render() reaper.Main_OnCommand(42230, 0); return "ok" end
function ops.automode(a) reaper.SetTrackAutomationMode(track(a[1]), math.tointeger(tonumber(a[2]))); return "ok" end
function ops.getparam(a)
    local t, fx = track(a[1]), fxIndex(a[2])
    local p = paramIndex(t, fx, a[3])
    local v = reaper.TrackFX_GetParamNormalized(t, fx, p)
    local _, s = reaper.TrackFX_GetFormattedParamValue(t, fx, p, "")
    return string.format("%.9f", v) .. "\t" .. s
end
function ops.setparam(a)
    local t, fx = track(a[1]), fxIndex(a[2])
    reaper.TrackFX_SetParamNormalized(t, fx, paramIndex(t, fx, a[3]), tonumber(a[4]))
    return "ok"
end
function ops.setbypass(a) reaper.TrackFX_SetEnabled(track(a[1]), fxIndex(a[2]), a[3] ~= "1"); return "ok" end
function ops.setmute(a) reaper.SetMediaTrackInfo_Value(track(a[1]), "B_MUTE", tonumber(a[2])); return "ok" end
function ops.setsolo(a) reaper.SetMediaTrackInfo_Value(track(a[1]), "I_SOLO", tonumber(a[2])); return "ok" end
function ops.setvol(a) reaper.SetMediaTrackInfo_Value(track(a[1]), "D_VOL", tonumber(a[2])); return "ok" end
function ops.setpan(a) reaper.SetMediaTrackInfo_Value(track(a[1]), "D_PAN", tonumber(a[2])); return "ok" end
function ops.tomaster(a) reaper.TrackFX_CopyToTrack(track(a[1]), fxIndex(a[2]), reaper.GetMasterTrack(0), 0, true); return "ok" end
-- What File > Render's dialog sets: a number (RENDER_SRATE, RENDER_BOUNDSFLAG, RENDER_SETTINGS) or the file name pattern.
function ops.renderset(a) reaper.GetSetProjectInfo(0, a[1], tonumber(a[2]), true); return "ok" end
function ops.renderpat(a) reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", a[1], true); return "ok" end
function ops.timesel(a) reaper.GetSet_LoopTimeRange(true, false, tonumber(a[1]), tonumber(a[2]), false); return "ok" end
function ops.region(a) reaper.AddProjectMarker(0, true, tonumber(a[1]), tonumber(a[2]), a[3], -1); return "ok" end
function ops.seltrack(a) reaper.SetOnlyTrackSelected(track(a[1])); return "ok" end
-- Every FX parameter of the FX that has an envelope: its name, then each point as time:value.
function ops.envs(a)
    local t, fx = track(a[1]), fxIndex(a[2])
    local out = {}
    for p = 0, reaper.TrackFX_GetNumParams(t, fx) - 1 do
        local e = reaper.GetFXEnvelope(t, fx, p, false)
        if e then
            local _, name = reaper.TrackFX_GetParamName(t, fx, p, "")
            local points = {}
            for i = 0, reaper.CountEnvelopePoints(e) - 1 do
                local _, time, value = reaper.GetEnvelopePoint(e, i)
                points[#points + 1] = string.format("%.4f:%.6f", time, value)
            end
            out[#out + 1] = name .. "\t" .. table.concat(points, " ")
        end
    end
    return table.concat(out, "\n")
end

-- The master meter while playing, sampled by the session loop below. peakdump returns time, position, play state, L, R.
local peaks = nil
function ops.peakstart() peaks = {}; return "ok" end
function ops.peakdump()
    local out = peaks or {}
    peaks = nil
    return table.concat(out, "\n")
end
local function samplePeaks()
    if not peaks then return end
    local m = reaper.GetMasterTrack(0)
    peaks[#peaks + 1] = string.format("%.4f\t%.4f\t%d\t%.6f\t%.6f", reaper.time_precise(), reaper.GetPlayPosition(),
        reaper.GetPlayState(), reaper.Track_GetPeakInfo(m, 0), reaper.Track_GetPeakInfo(m, 1))
end

local function split(line)
    local a = {}
    for part in (line .. "\t"):gmatch("(.-)\t") do a[#a + 1] = part end
    return a
end

local function writeAtomic(path, text)
    local part = path .. ".part"
    local f = assert(io.open(part, "wb"))
    f:write(text)
    f:close()
    os.remove(path)
    assert(os.rename(part, path))
end

return {
    scvb_session = function(h)
        local rpc = h.job.rpc
        h.openAndWait(h.job.project)
        h.touch(rpc .. "/ready")
        local n = 1
        while true do
            local f = io.open(rpc .. "/cmd-" .. n .. ".txt", "r")
            if f then
                local line = f:read("*a")
                f:close()
                local a = split((line:gsub("[\r\n]+$", "")))
                local op = table.remove(a, 1)
                if op == "quit" then
                    writeAtomic(rpc .. "/res-" .. n .. ".txt", "ok\nok")
                    break
                end
                local fn = ops[op]
                local ok, res
                if fn then ok, res = pcall(fn, a) else ok, res = false, "no such op " .. tostring(op) end
                writeAtomic(rpc .. "/res-" .. n .. ".txt", (ok and "ok\n" or "error\n") .. tostring(res))
                n = n + 1
            end
            samplePeaks()
            h.wait(0.02)
        end
        h.put("commands", n - 1)
    end,
}
