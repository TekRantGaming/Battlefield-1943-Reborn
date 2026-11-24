# Storm patterns

`michael_2018.bin`: Hurricane Michael's landfall on the Florida Panhandle, 10 Oct 2018, 17:00 to 17:55 UTC, twelve
radar frames five minutes apart, eye-centred, 256 x 256 at 1.17 km a pixel (300 km across).

Source: the US National Weather Service's NEXRAD radar network, composite base reflectivity (the `n0q` product) as
archived by the Iowa Environmental Mesonet (Iowa State University), <https://mesonet.agron.iastate.edu/>. NWS data
is a work of the US government and in the public domain. The frames themselves are not in this repository; the
pattern is baked from them by `tools/storm_bake.py`:

    python tools/storm_bake.py <folder of n0q_201810101700.png ... n0q_201810101755.png> bf/data/weather/storms/michael_2018.bin --centre 29.9 -85.4

What the bake does (the eye found in each frame, the frame re-centred on it, the sectors past the radars' reach filled
from the storm turned about its eye) is in the tool's notes and in NativeXRenderer's decision D228.
