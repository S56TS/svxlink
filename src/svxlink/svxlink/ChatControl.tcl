# Optional exact-match AI controls. Loaded after local event overrides so the
# previous handler (including its return value) can be preserved.
# SPDX-License-Identifier: GPL-2.0-or-later
namespace eval ::ChatControl {}

proc ::ChatControl::install {} {
  set target [getConfigValue $::logic_name CHAT_LOGIC ""]
  if {$target eq ""} { return }
  set commands [dict create]
  foreach {key fallback action} {
    DTMF_START 981 start DTMF_STOP 980 stop DTMF_RESET 982 reset
  } {
    set value [getConfigValue $target $key $fallback]
    if {![regexp {^[0-9A-D]{1,16}$} $value] || [dict exists $commands $value]} {
      error "ChatControl: commands must be distinct, 1-16 digits (0-9/A-D), without #"
    }
    dict set commands $value $action
  }
  set ns ::${::logic_name}::ChatControl
  namespace eval $ns {}
  set ${ns}::target $target
  set ${ns}::commands $commands
  set handler ::${::logic_name}::dtmf_cmd_received
  set previous ${ns}::previous
  if {[info commands $previous] ne ""} { return }
  rename $handler $previous
  proc $handler {cmd} [string map [list @NS@ $ns] {
    if {[dict exists $@NS@::commands $cmd]} {
      set action [dict get $@NS@::commands $cmd]
      publishStateEvent "Chat:command" "$@NS@::target $action"
      return 1
    }
    return [@NS@::previous $cmd]
  }]
}
::ChatControl::install
