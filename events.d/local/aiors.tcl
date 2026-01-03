# AIORS local entry point (single place for AIORS behavior)
# Loaded via: events.d/local/Logic.tcl (which first loads SvxLink base Logic.tcl)
#
# FIX: RX hook may appear after startup. We now poll until it exists.
# - RX LED follows RAW squelch
# - Time-only min-open gating before forwarding OPEN to base logic
# - TX LED follows transmit
# - Single courtesy/roger beep with debounce + startup suppress
#
# Install:
#   /usr/share/svxlink/events.d/local/aiors.tcl
# and restart svxlink.

printInfo "AIORS: local/aiors.tcl loaded"

namespace eval ::AIORS {
  # ---- knobs ----
  variable min_rx_open_ms 300
  variable startup_suppress_ms 3000   ;# set 10000 for site if you want
  variable beep_debounce_ms 800
  variable beep_close_window_ms 1500
  variable debug 0

  variable start_ts [clock milliseconds]

  # State
  variable rx_raw_open_ts     ; array set rx_raw_open_ts {}
  variable rx_allowed_open    ; array set rx_allowed_open {}
  variable last_valid_close_ts 0
  variable last_beep_ts 0

  variable hooked_rx 0
  variable hooked_tx 0
  variable hooked_rgr 0

  variable logic_name "SimplexLogic"
  variable inst_ns ""
  variable logic_ns ""

  # aiorsctl path (auto)
  variable aiorsctl "/usr/bin/aiorsctl"
  if {![file executable $aiorsctl]} { set aiorsctl "/usr/local/bin/aiorsctl" }
  printInfo "AIORS: aiorsctl=$aiorsctl"
}

proc ::AIORS::_wrap {procname wrapper_body} {
  if {[info procs $procname] eq ""} { return 0 }
  set orig "${procname}__aiors_orig"
  if {[info procs $orig] ne ""} { return 1 }
  rename $procname $orig
  set body [string map [list "@ORIG@" $orig] $wrapper_body]
  proc $procname args $body
  return 1
}

proc ::AIORS::_refresh_namespaces {} {
  set ln "SimplexLogic"
  if {[info exists ::logic_name] && $::logic_name ne ""} { set ln $::logic_name }
  set ::AIORS::logic_name $ln
  set ::AIORS::inst_ns  "::${ln}"
  set ::AIORS::logic_ns "::${ln}::Logic"
}

# ---- Debug: show COS vs CTCSS on SQL transitions ----

# Called when squelch opens
proc squelch_open {rx_id} {
  # If SvxLink exports any CTCSS status variables, print them.
  # (Different versions/configs export different vars, so we log whatever exists.)

  set msg "RX$rx_id SQL OPEN"

  foreach v {
    ::Logic::sql_open_type
    ::Logic::sql_rx_id
    ::Logic::ctcss
    ::Logic::ctcss_hz
    ::Logic::ctcss_ok
    ::Logic::tone_detected
    ::Logic::subtone_detected
  } {
    if {[info exists $v]} { append msg " $v=[set $v]" }
  }

  puts $msg
}

# Called when squelch closes
proc squelch_close {rx_id} {
  set msg "RX$rx_id SQL CLOSED"
  foreach v {
    ::Logic::sql_open_type
    ::Logic::sql_rx_id
    ::Logic::ctcss
    ::Logic::ctcss_hz
    ::Logic::ctcss_ok
    ::Logic::tone_detected
    ::Logic::subtone_detected
  } {
    if {[info exists $v]} { append msg " $v=[set $v]" }
  }
  puts $msg
}

# ---- LEDs ----
proc ::AIORS::_led_ch {id} { expr {$id eq "1" ? "b" : "a"} }

proc ::AIORS::led_tx {tx_id is_on} {
  set ch [::AIORS::_led_ch $tx_id]
  set state [expr {$is_on ? "on" : "off"}]
  set bin $::AIORS::aiorsctl
  if {$::AIORS::debug} { printInfo "AIORS DBG: led_tx tx_id=$tx_id state=$state" }
  catch { exec $bin -s set led $ch tx $state & }
}

proc ::AIORS::led_rx {rx_id is_open} {
  set ch [::AIORS::_led_ch $rx_id]
  set state [expr {$is_open ? "on" : "off"}]
  set bin $::AIORS::aiorsctl
  if {$::AIORS::debug} { printInfo "AIORS DBG: led_rx rx_id=$rx_id state=$state" }
  catch { exec $bin -s set led $ch rx $state & }
}

# ---- RX gating (time only) ----
proc ::AIORS::_raw_open {rx_id} { set ::AIORS::rx_raw_open_ts($rx_id) [clock milliseconds] }
proc ::AIORS::_raw_close {rx_id} { set ::AIORS::rx_raw_open_ts($rx_id) 0 }
proc ::AIORS::_set_allowed {rx_id is_allowed} { set ::AIORS::rx_allowed_open($rx_id) [expr {$is_allowed ? 1 : 0}] }
proc ::AIORS::_is_allowed {rx_id} {
  if {![info exists ::AIORS::rx_allowed_open($rx_id)]} { return 0 }
  return [expr {$::AIORS::rx_allowed_open($rx_id) ? 1 : 0}]
}

proc ::AIORS::rx_allowed {rx_id} {
  if {![info exists ::AIORS::rx_raw_open_ts($rx_id)] || $::AIORS::rx_raw_open_ts($rx_id) == 0} { return 0 }
  set dt [expr {[clock milliseconds] - $::AIORS::rx_raw_open_ts($rx_id)}]
  if {$dt < $::AIORS::min_rx_open_ms} { return 0 }

  # Require SvxLink’s idea of valid tone (best-effort across versions)
  set ns $::AIORS::logic_ns
  foreach p [list "${ns}::ctcss_detected" "${ns}::tone_detected" "::Logic::ctcss_detected" "::Logic::tone_detected"] {
    if {[info procs $p] ne ""} {
      if {![catch { $p } ok]} {
        if {[string is integer -strict $ok]} { return [expr {$ok != 0}] }
        set s [string tolower $ok]
        if {$s eq "true"}  { return 1 }
        if {$s eq "false"} { return 0 }
      }
    }
  }
  return 0
}

proc ::AIORS::_gate_try_forward {rx_id orig_proc} {
  if {![info exists ::AIORS::rx_raw_open_ts($rx_id)] || $::AIORS::rx_raw_open_ts($rx_id) == 0} { return }
  if {[::AIORS::rx_allowed $rx_id]} {
    ::AIORS::_set_allowed $rx_id 1
    if {$::AIORS::debug} {
      set dt [expr {[clock milliseconds] - $::AIORS::rx_raw_open_ts($rx_id)}]
      printInfo "AIORS DBG: forward OPEN rx=$rx_id dt=${dt}ms"
    }
    catch { $orig_proc $rx_id 1 }
  } else {
    after 100 [list ::AIORS::_gate_try_forward $rx_id $orig_proc]
  }
}

# ---- Beep override ----
proc ::AIORS::_install_beep_override {} {
  if {$::AIORS::hooked_rgr} { return }
  set p "${::AIORS::logic_ns}::send_rgr_sound"
  if {[info procs $p] eq ""} { return }

  if {[info procs "${p}__aiors_orig"] ne ""} {
    set ::AIORS::hooked_rgr 1
    return
  }

  rename $p "${p}__aiors_orig"
  proc $p {} {
    set now [clock milliseconds]

    if {[expr {$now - $::AIORS::start_ts}] < $::AIORS::startup_suppress_ms} {
      if {$::AIORS::debug} { printInfo "AIORS DBG: send_rgr_sound suppressed (startup)" }
      return
    }

    # If RX hook is installed, require recent valid close; otherwise still beep
    if {$::AIORS::hooked_rx && $::AIORS::last_valid_close_ts != 0} {
      if {[expr {$now - $::AIORS::last_valid_close_ts}] > $::AIORS::beep_close_window_ms} {
        if {$::AIORS::debug} { printInfo "AIORS DBG: send_rgr_sound suppressed (too late)" }
        return
      }
    }

    if {$::AIORS::last_beep_ts != 0 && [expr {$now - $::AIORS::last_beep_ts}] < $::AIORS::beep_debounce_ms} {
      if {$::AIORS::debug} { printInfo "AIORS DBG: send_rgr_sound suppressed (debounce)" }
      return
    }
    set ::AIORS::last_beep_ts $now

    if {$::AIORS::debug} { printInfo "AIORS DBG: send_rgr_sound -> BEEP" }
    # Guard so the tone starts after TX key-up / TX_DELAY
    playSilence 900
    # Single courtesy beep
    playTone 440 900 120
    # Tail guard so the end is not clipped
    playSilence 300
  }

  set ::AIORS::hooked_rgr 1
  printInfo "AIORS: roger/courtesy beep override installed (single beep)"
}

# ---- Hook installers ----
proc ::AIORS::_try_install_tx_hook {} {
  if {$::AIORS::hooked_tx} { return 1 }
  foreach name [list "${::AIORS::logic_ns}::transmit" "${::AIORS::inst_ns}::transmit" "${::AIORS::logic_name}::transmit"] {
    if {[info procs $name] eq ""} { continue }
    if {[::AIORS::_wrap $name {
      catch { ::AIORS::ensure_hooks }
      if {[llength $args] == 1} {
        set tx_id "0"; set is_on [lindex $args 0]
      } elseif {[llength $args] == 2} {
        set tx_id [lindex $args 0]; set is_on [lindex $args 1]
      } else {
        return [uplevel 1 [list @ORIG@ {*}$args]]
      }
      ::AIORS::led_tx $tx_id $is_on
      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]} {
      set ::AIORS::hooked_tx 1
      printInfo "AIORS: TX LED hooked via $name"
      return 1
    }
  }
  return 0
}

proc ::AIORS::_try_install_rx_hook {} {
  if {$::AIORS::hooked_rx} { return 1 }

  # Events call instance: ::SimplexLogic::squelch_open
  foreach name [list "${::AIORS::inst_ns}::squelch_open" "${::AIORS::logic_name}::squelch_open" "${::AIORS::logic_ns}::squelch_open"] {
    if {[info procs $name] eq ""} { continue }

    if {[::AIORS::_wrap $name {
      catch { ::AIORS::ensure_hooks }
      if {[llength $args] != 2} {
        return [uplevel 1 [list @ORIG@ {*}$args]]
      }

      set rx_id   [lindex $args 0]
      set is_open [lindex $args 1]

      if {![info exists ::AIORS::rx_allowed_open($rx_id)]} { set ::AIORS::rx_allowed_open($rx_id) 0 }
      if {![info exists ::AIORS::rx_raw_open_ts($rx_id)]}  { set ::AIORS::rx_raw_open_ts($rx_id) 0 }

      # LED follows raw squelch
      ::AIORS::led_rx $rx_id $is_open

      if {$is_open} {
        ::AIORS::_raw_open $rx_id
        ::AIORS::_set_allowed $rx_id 0
        after $::AIORS::min_rx_open_ms [list ::AIORS::_gate_try_forward $rx_id @ORIG@]
        return
      } else {
        ::AIORS::_raw_close $rx_id
        if {[::AIORS::_is_allowed $rx_id]} {
          ::AIORS::_set_allowed $rx_id 0
          set ::AIORS::last_valid_close_ts [clock milliseconds]
          return [uplevel 1 [list @ORIG@ $rx_id 0]]
        } else {
          return
        }
      }
    }]} {
      set ::AIORS::hooked_rx 1
      printInfo "AIORS: RX gating hooked via $name"
      return 1
    }
  }
  return 0
}

proc ::AIORS::ensure_hooks {} {
  ::AIORS::_refresh_namespaces
  ::AIORS::_try_install_tx_hook
  ::AIORS::_try_install_rx_hook
  ::AIORS::_install_beep_override

  if {!$::AIORS::hooked_rx} {
    if {$::AIORS::debug} { printInfo "AIORS DBG: RX hook not found yet, retry..." }
    after 200 ::AIORS::ensure_hooks
  }
  return 1
}

::AIORS::ensure_hooks
