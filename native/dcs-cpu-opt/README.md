# DCS CPU Opt

Native Windows component for the experimental CPU options in DCS Control. Original implementation by Rolln-dev, imported from commit cd757f46a8ea27ee478cd4301925ebaadf0a158d of Rolln-dev/dcs-cpu-opt with the author's permission in DCS Control issue #36, comment 7207568431.

DCS Control writes the configuration next to the component and loads it through its Saved Games hook. Each optimization is off by default and applies at the next DCS start. The native log is written next to the component. The upstream libdeflate copyright and MIT license are in THIRD_PARTY_NOTICES.txt and third_party/libdeflate/COPYING.

The audio, terrain and route hooks check the PE identity of the validated DCS 2.9.32.30490 Steam DLLs before checking their signatures. The AMD pacer checks the complete wait-loop signature. Decompression checks the three imports and falls back to DCS's decoder for unsupported streams. DCS Control prevents its integrated version from loading next to the standalone Saved Games hook.
