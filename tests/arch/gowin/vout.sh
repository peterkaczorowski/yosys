#!/usr/bin/env bash
# Text-level checks of a netlist written for the Gowin netlist reader.
set -ex
${YOSYS:-../../../yosys} -q vout.ys
# No literal or undefined constants on cell pins (constants come from GND/VCC)
! grep -E "^\s+\.[A-Za-z0-9_]+\(.*[0-9]+'[bdh]" vout.vg
# Port wires are driven by cells directly, not through assign aliases
! grep -E "^\s+assign \\\\?(lvds_p|lvds_n|p\[|q\[)" vout.vg
