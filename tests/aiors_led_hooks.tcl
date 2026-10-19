set source_file [lindex $argv 0]
set f [open $source_file r]
set source [read $f]
close $f
set first [string first {proc ::AIORS::_led_ch} $source]
set last [string first {proc ::AIORS::_note_tx_state} $source]
if {$first < 0 || $last <= $first} { error "LED procedures not found" }
if {![info complete $source]} { error "incomplete Tcl script" }
namespace eval ::AIORS { variable debug 0 }
set received {}
proc aiorsSetLed {args} { lappend ::received $args }
# No update/vwait/event pump: callbacks must directly queue to the native worker.
proc after {args} { error "LED dispatch must not use Tcl after" }
proc exec {args} { error "LED dispatch must not spawn processes" }
eval [string range $source $first [expr {$last - 1}]]
::AIORS::led_rx 0 1
::AIORS::led_tx 1 0
::AIORS::led_rx Rx1 0
::AIORS::led_tx Tx2 1
::AIORS::led_rx unexpected 1
if {$received ne {{a rx on} {b tx off} {a rx off} {b tx on}}} { error "bad LED dispatch: $received" }
puts "Tcl LED hooks dispatch without Tcl events or subprocesses; channel mapping passed"

# Load the full integration (optional companion scripts have separate tests).
# Check the actual wrappers, and make file/config I/O fail if attempted on TX/RX.
set ::logic_name RepeaterLogic
set ::logic_type Repeater
set ::active_module {}
set ::loaded_modules {}
proc printInfo {args} {}
proc getConfigValue {args} { return [lindex $args end] }
foreach name {playFile playTone playSilence playMsg playDtmf} { proc ::$name {args} {} }
namespace eval ::RepeaterLogic::Logic {
  proc transmit {args} {}
  proc squelch_open {args} {}
  proc send_rgr_sound {args} {}
  proc playMsg {args} {}
  proc playTone {args} {}
}
set end [string first {# ---- Optional AIORS companion scripts ----} $source]
eval [string range $source 0 [expr {$end - 1}]]
proc ::AIORS::_ini_get {args} { error "configuration read during an audio event" }
set received {}
::RepeaterLogic::Logic::transmit 0 1
::RepeaterLogic::Logic::squelch_open 1 1
if {$received ne {{a tx on} {b rx on}}} { error "full event hooks failed: $received" }
puts "Full TX/RX wrappers dispatch directly without configuration reads or Tcl timers"
