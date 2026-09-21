-- Exercise the production audio page with only its mpv property boundary stubbed.
local file = assert(io.open(assert(arg[1]), "rb"))
local source = file:read("*a"):gsub("\r\n", "\n")
file:close()
local body = assert(source:match("\n(local function add_audio%(s%).-\nend)\n"))
local compile = loadstring or load
local factory = assert(compile("return function(mp, o, append, append_property, " ..
                              "append_filters)\n" .. body ..
                              "\nreturn add_audio\nend"))()

local function audio_page(input, output)
    for _, format in pairs({input=input, output=output}) do
        format["channel-count"] = 2
        format.samplerate = 48000
    end
    local properties = {
        ["audio-params"] = input,
        ["audio-out-params"] = output,
        ["audio-delay"] = 0,
    }
    local mp = {
        get_property_native = function(name, default)
            local value = properties[name]
            if value == nil then
                return default
            end
            return value
        end,
    }
    local append = function(page, value, options)
        if options.prefix == "Output:" then
            page.output = value
        end
        return value
    end
    local noop = function() end
    local add_audio = factory(mp, {nl="\n", prefix_sep=" "}, append, noop, noop)
    local page = {}
    add_audio(page)
    return page.output
end

for _, codec in ipairs({"ac3", "eac3", "dts", "dtshd", "truehd"}) do
    local actual = audio_page(nil, {format="raw-" .. codec})
    assert(actual == "Passthrough (raw)", codec .. ": " .. tostring(actual))
end
assert(audio_page({format="float"}, {format="spdif-ac3"}) ==
       "Passthrough (IEC61937)")
assert(audio_page(nil, {format="dop"}) == "Passthrough (DoP)")
assert(audio_page(nil, {format="dsd"}) == "DSD")
assert(audio_page(nil, {format="float"}) == "PCM")
assert(audio_page({format="float"}, nil) == nil)
assert(audio_page(nil, nil) == nil)
print("Audio stats: raw passthrough, carrier, PCM and absent output passed")
