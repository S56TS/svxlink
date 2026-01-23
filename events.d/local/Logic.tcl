# /usr/share/svxlink/events.d/local/Logic.tcl
puts "Logic: local/Logic.tcl loaded"

# Load AIORS hooks (LEDs, etc)
if {[file readable "/usr/share/svxlink/events.d/local/aiors.tcl"]} {
  source "/usr/share/svxlink/events.d/local/aiors.tcl"
} elseif {[file readable "/etc/svxlink/events.d/local/aiors.tcl"]} {
  source "/etc/svxlink/events.d/local/aiors.tcl"
} else {
  puts "Logic: WARNING: Could not find aiors.tcl"
}
