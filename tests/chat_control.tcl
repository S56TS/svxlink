# Exercise the wrapper with imported site handlers, just as events.tcl does.
set root [file dirname [file dirname [file normalize [info script]]]]
set script [file join $root src svxlink svxlink ChatControl.tcl]
set config [dict create]
set events {}
set previous_calls {}
proc getConfigValue {section key fallback} {
  if {[dict exists $::config $section $key]} { return [dict get $::config $section $key] }
  return $fallback
}
proc publishStateEvent {event message} { lappend ::events [list $event $message] }
proc check {actual expected label} {
  if {$actual ne $expected} { error "$label: expected '$expected', got '$actual'" }
}
namespace eval ::Site {
  namespace export dtmf_cmd_received
  proc dtmf_cmd_received {cmd} { lappend ::previous_calls $cmd; return 7 }
}
namespace eval ::RepeaterLogic { namespace import ::Site::* }
set logic_name RepeaterLogic
source $script
check [RepeaterLogic::dtmf_cmd_received 981] 7 disabled
dict set config RepeaterLogic CHAT_LOGIC ChatLogic
source $script
source $script
set previous_calls {}
foreach active_module {{} Frn EchoLink} {
  foreach {cmd action} {981 start 980 stop 982 reset} {
    check [RepeaterLogic::dtmf_cmd_received $cmd] 1 consumed
    check [lindex $events end] [list Chat:command "ChatLogic $action"] event
  }
  foreach cmd {{} 0 1 1981 *981 9810 999 981#} {
    check [RepeaterLogic::dtmf_cmd_received $cmd] 7 passthrough
    check [lindex $previous_calls end] $cmd original_argument
  }
}
check [llength $events] 9 no_other_events
dict set config ChatLogic DTMF_START 871
source $script
check [RepeaterLogic::dtmf_cmd_received 981] 7 old_command_released
check [RepeaterLogic::dtmf_cmd_received 871] 1 custom_command
dict set config ChatLogic DTMF_STOP 871
check [catch {source $script}] 1 duplicate_rejected
puts "ChatControl tests passed (disabled, active modules, exact commands, site handler, reload, configuration)"
