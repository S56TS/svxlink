# /usr/share/svxlink/events.d/local/aiors_frn_autostart.tcl
# AIORS integration: keep the FRN module active when no other module is active.

namespace eval ::AIORS::FrnAutoStart {
  if {![info exists enabled]} { variable enabled 1 }
  if {![info exists timeout_sec]} { variable timeout_sec 60 }
  if {![info exists timer_sec]} { variable timer_sec 60 }
  if {![info exists dtmf_cmd]} { variable dtmf_cmd "2#" }
  if {![info exists dtmf_ctrl_pty]} { variable dtmf_ctrl_pty "" }
  if {![info exists registered]} { variable registered 0 }
}

proc ::AIORS::FrnAutoStart::_log {msg} {
  if {[info commands ::AIORS::_log] ne ""} {
    ::AIORS::_log $msg
  } elseif {[info commands printInfo] ne ""} {
    printInfo $msg
  } else {
    puts $msg
  }
}

proc ::AIORS::FrnAutoStart::_warn {msg} {
  if {[info commands ::AIORS::_warn] ne ""} {
    ::AIORS::_warn $msg
  } elseif {[info commands printWarning] ne ""} {
    printWarning $msg
  } else {
    puts "*** WARNING: $msg"
  }
}

proc ::AIORS::FrnAutoStart::_cfg_get {key def} {
  if {[info commands getConfigValue] eq "" || ![info exists ::logic_name]} {
    return $def
  }

  if {[catch {set value [getConfigValue ${::logic_name} $key $def]}]} {
    return $def
  }
  return $value
}

proc ::AIORS::FrnAutoStart::_is_true {value} {
  set value [string tolower [string trim $value]]
  return [expr {$value eq "1" || $value eq "true" ||
                $value eq "yes" || $value eq "on"}]
}

proc ::AIORS::FrnAutoStart::_load_config {} {
  variable enabled
  variable timeout_sec
  variable timer_sec
  variable dtmf_cmd
  variable dtmf_ctrl_pty

  set fallback_timeout 60
  if {[info exists ::frn_time_sec] &&
      [string is integer -strict $::frn_time_sec] &&
      $::frn_time_sec > 0} {
    set fallback_timeout $::frn_time_sec
  }

  set legacy_timeout [_cfg_get "FRN_TIME_SEC" $fallback_timeout]
  set raw_timeout [_cfg_get "AIORS_FRN_AUTOSTART_TIME" $legacy_timeout]

  if {![string is integer -strict $raw_timeout] || $raw_timeout < 1} {
    _warn "AIORS FRN autostart: invalid timeout '$raw_timeout', using $fallback_timeout"
    set raw_timeout $fallback_timeout
  }

  set enabled [_is_true [_cfg_get "AIORS_FRN_AUTOSTART_ENABLE" "1"]]
  set timeout_sec $raw_timeout
  set timer_sec $timeout_sec
  set dtmf_cmd [_cfg_get "AIORS_FRN_AUTOSTART_DTMF" "2#"]
  set dtmf_ctrl_pty [_cfg_get "DTMF_CTRL_PTY" ""]
}

proc ::AIORS::FrnAutoStart::_send_dtmf {digits} {
  variable dtmf_ctrl_pty

  # RepeaterLogic ignores injected DTMF while the repeater is down. Its
  # DTMF_CTRL_PTY path explicitly wakes the repeater before processing digits.
  if {$dtmf_ctrl_pty ne ""} {
    if {[catch {
      set fd [open $dtmf_ctrl_pty w]
      fconfigure $fd -buffering none -translation binary
      puts -nonewline $fd $digits
      close $fd
    } err]} {
      _warn "AIORS FRN autostart: DTMF_CTRL_PTY write failed: $err"
    } else {
      return 1
    }
  }

  if {[catch {::injectDtmf $digits} err]} {
    _warn "AIORS FRN autostart: injectDtmf failed: $err"
    return 0
  }
  return 1
}

proc ::AIORS::FrnAutoStart::tick {} {
  variable enabled
  variable timeout_sec
  variable timer_sec
  variable dtmf_cmd

  if {!$enabled} { return }

  set active ""
  if {[info exists ::active_module]} {
    set active $::active_module
  }

  if {$active eq ""} {
    if {$timer_sec > 0} {
      incr timer_sec -1
    }

    if {$timer_sec <= 0} {
      _log "AIORS FRN autostart: no active module, injecting $dtmf_cmd"
      set timer_sec $timeout_sec
      _send_dtmf $dtmf_cmd
    }
  } else {
    set timer_sec $timeout_sec
  }
}

proc ::AIORS::FrnAutoStart::install {} {
  variable enabled
  variable timeout_sec
  variable dtmf_cmd
  variable dtmf_ctrl_pty
  variable registered

  _load_config

  if {!$enabled} {
    _log "AIORS FRN autostart disabled"
    return 1
  }

  if {$registered} {
    return 1
  }

  set add_second "::${::logic_name}::Logic::addSecondTickSubscriber"
  if {[info commands $add_second] eq ""} {
    _warn "AIORS FRN autostart: could not find $add_second"
    return 0
  }

  $add_second ::AIORS::FrnAutoStart::tick
  set registered 1
  _log "AIORS FRN autostart loaded: timeout=${timeout_sec}s dtmf='$dtmf_cmd'"
  if {$dtmf_ctrl_pty ne ""} {
    _log "AIORS FRN autostart using DTMF_CTRL_PTY=$dtmf_ctrl_pty"
  }
  return 1
}

::AIORS::FrnAutoStart::install
