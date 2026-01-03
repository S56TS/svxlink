# SvxLink local entry point (override)
#
# SvxLink uses sourceTclWithOverrides "Logic.tcl" which will pick up this file.
# We MUST chain-load the original SvxLink Logic.tcl and then load AIORS hooks.

set base_logic "/usr/share/svxlink/events.d/Logic.tcl"
if {[file exists $base_logic]} {
  if {[catch { source $base_logic } err opts]} {
    printWarning "AIORS: FAILED to source base Logic.tcl ($base_logic): $err"
    if {[dict exists $opts -errorinfo]} {
      puts "AIORS: errorinfo: [dict get $opts -errorinfo]"
    }
  }
} else {
  printWarning "AIORS: base Logic.tcl not found at $base_logic"
}

# Load AIORS behavior (this directory)
set aiors_local [file normalize [file join [file dirname [info script]] aiors.tcl]]
if {[file exists $aiors_local]} {
  if {[catch { source $aiors_local } err opts]} {
    printWarning "AIORS: FAILED to source local/aiors.tcl ($aiors_local): $err"
    if {[dict exists $opts -errorinfo]} {
      puts "AIORS: errorinfo: [dict get $opts -errorinfo]"
    }
  }
} else {
  printWarning "AIORS: local/aiors.tcl not found at $aiors_local"
}
