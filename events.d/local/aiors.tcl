# /usr/share/svxlink/events.d/local/aiors.tcl
# AIORS integration: LED hooks + optional courtesy beep override
#
# Notes:
# - With your C++ patch, RepeaterLogic now always dispatches Tcl "squelch_open rx_id is_open".
# - This script wraps the appropriate squelch_open + transmit procs and calls aiorsctl.
# - Script is idempotent: safe if sourced twice (won't rewrap / spam / break procs).

namespace eval ::AIORS {
  # ---- knobs ----
  variable debug 0
  variable cfg_logged 0
  variable announcement_cfg_logged 0
  variable reply_cfg_logged 0


  # One-shot suppression flag: suppress automatic post-IDENT status report (keeps DTMF-requested reports)
  variable suppress_status_once 0
  # Courtesy beep behavior (optional)
  variable startup_suppress_ms 3000
  variable beep_debounce_ms 800
  variable beep_close_window_ms 1500

  # Config: disable repeater close / roger beep (read from svxlink.conf)
  variable disable_rpt_close_beep 0
  variable announcement_delay_ms 0
  variable reply_delay_ms 0
  variable reply_delay_window_ms 3000

  # Hook state
  variable hooked_rx 0
  variable hooked_tx 0
  variable hooked_rgr 0
  variable hooked_announcement_delay 0
  variable rx_hook_retries 0
  variable rehook_scheduled 0
  variable announcement_delay_done 0
  variable announcement_delay_injecting 0
  variable reply_delay_pending 0
  variable reply_delay_done 0
  variable reply_delay_injecting 0
  variable tx_is_on 0
  variable tx_started_ms 0
  variable tx_state
  if {![array exists tx_state]} { array set tx_state {} }

  # Beep state
  variable start_ts [clock milliseconds]
  variable last_valid_close_ts 0
  variable last_beep_ts 0

  # Namespace discovery
  variable logic_name "RepeaterLogic"
  variable inst_ns ""
  variable logic_ns ""

  # aiorsctl path (resolved after _log/_warn are defined)
  variable aiorsctl ""
}

proc ::AIORS::_ini_get {path section key def} {
  if {![file readable $path]} { return $def }

  set in_section 0
  set f [open $path r]
  while {[gets $f line] >= 0} {
    # strip CR/LF and trim
    regsub -all "\r" $line "" line
    set line [string trim $line]

    # skip blanks and comments
    if {$line eq ""} continue
    if {[string match "#*" $line] || [string match ";*" $line]} continue

    # section header?
    if {[regexp {^\[(.+)\]$} $line -> sec]} {
      set in_section [expr {$sec eq $section}]
      continue
    }

    if {!$in_section} continue

    # key=value
    if {[regexp {^([^=]+)=(.*)$} $line -> k v]} {
      set k [string trim $k]
      set v [string trim $v]
      if {$k eq $key} {
        close $f
        return $v
      }
    }
  }
  close $f
  return $def
}

proc ::AIORS::_cfg_get {key def} {
  if {[info commands Cfg::getValue] eq ""} {
    return $def
  }

  # Try "current section" signature first: Cfg::getValue KEY DEFAULT
  if {![catch {set v [Cfg::getValue $key $def]}]} {
    return $v
  }

  # Fallback: some builds use Cfg::getValue SECTION KEY DEFAULT
  # (we try RepeaterLogic as section)
  if {![catch {set v [Cfg::getValue "RepeaterLogic" $key $def]}]} {
    return $v
  }

  return $def
}

proc ::AIORS::_log {msg} {
  # Use SvxLink logging helper if available
  if {[info commands printInfo] ne ""} {
    printInfo $msg
  } else {
    puts $msg
  }
}

proc ::AIORS::_dbg {msg} {
  if {$::AIORS::debug} { ::AIORS::_log "AIORS DBG: $msg" }
}

proc ::AIORS::_warn {msg} {
  # Match SvxLink warning style as you were using before
  puts [format {*** WARNING[RepeaterLogic]: %s} $msg]
}

::AIORS::_log "AIORS: local/aiors.tcl loaded"

# Resolve aiorsctl path safely (don't crash if unset)
if {![info exists ::AIORS::aiorsctl] || $::AIORS::aiorsctl eq ""} {
  if {[file executable "/usr/local/bin/aiorsctl"]} {
    set ::AIORS::aiorsctl "/usr/local/bin/aiorsctl"
  } elseif {[file executable "/usr/bin/aiorsctl"]} {
    set ::AIORS::aiorsctl "/usr/bin/aiorsctl"
  } else {
    set ::AIORS::aiorsctl [auto_execok aiorsctl]
  }
}

if {$::AIORS::aiorsctl eq ""} {
  ::AIORS::_warn "AIORS: aiorsctl not found; LED hooks disabled"
} else {
  ::AIORS::_log "AIORS: aiorsctl=$::AIORS::aiorsctl"
}

proc ::AIORS::_refresh_namespaces {} {
  # SvxLink exports ::logic_name (e.g. "RepeaterLogic", "SimplexLogic")
  set ln "RepeaterLogic"
  if {[info exists ::logic_name] && $::logic_name ne ""} { set ln $::logic_name }

  set ::AIORS::logic_name $ln
  set ::AIORS::inst_ns  "::${ln}"
  set ::AIORS::logic_ns "::${ln}::Logic"
}

# ---- LED mapping helpers ----
proc ::AIORS::_led_ch {id} {
  # Map rx_id/tx_id to channel a/b.
  # Your svxlink.conf: RX_ID=0 means "0".
  # Keep it simple: 0->a, 1->b, otherwise default a.
  if {$id eq ""} { return "a" }
  if {[string is integer -strict $id]} {
    return [expr {$id == 0 ? "a" : "b"}]
  }
  # If someone passes "Rx1"/"Tx2", take trailing digit
  if {[regexp {([0-9]+)$} $id -> n]} {
    return [expr {$n == 0 ? "a" : "b"}]
  }
  return "a"
}

proc ::AIORS::_led_start {key} {
  variable led_desired
  variable led_sent
  variable led_pipe
  variable led_timeout
  if {[info exists led_pipe($key)] || ![info exists led_desired($key)]} { return }
  if {[info exists led_sent($key)] && $led_sent($key) eq $led_desired($key)} { return }
  if {$::AIORS::aiorsctl eq ""} { return }

  lassign [split $key ,] ch kind
  set state $led_desired($key)
  set command [list $::AIORS::aiorsctl set led $ch $kind $state]
  if {[catch {open "|$command" r} pipe]} {
    if {$::AIORS::debug} { ::AIORS::_warn "AIORS: LED process failed to start: $pipe" }
    return
  }
  fconfigure $pipe -blocking 0
  set led_pipe($key) $pipe
  set led_sent($key) $state
  fileevent $pipe readable [list ::AIORS::_led_readable $key $pipe]
  set led_timeout($key) [after 2500 [list ::AIORS::_led_timeout $key $pipe]]
}

proc ::AIORS::_led_readable {key pipe} {
  variable led_pipe
  variable led_timeout
  variable led_sent
  if {![info exists led_pipe($key)] || $led_pipe($key) ne $pipe} { return }
  catch {read $pipe}
  if {![eof $pipe]} { return }
  fileevent $pipe readable {}
  after cancel $led_timeout($key)
  unset led_timeout($key)
  unset led_pipe($key)
  if {[catch {close $pipe} err]} {
    unset -nocomplain led_sent($key)
    if {$::AIORS::debug} { ::AIORS::_warn "AIORS: LED update failed: $err" }
    after 1000 [list ::AIORS::_led_start $key]
  } else {
    after 0 [list ::AIORS::_led_start $key]
  }
}

proc ::AIORS::_led_timeout {key pipe} {
  variable led_pipe
  if {![info exists led_pipe($key)] || $led_pipe($key) ne $pipe} { return }
  foreach child [pid $pipe] {
    catch {exec kill -KILL $child &}
  }
}

proc ::AIORS::_led_queue {ch kind state} {
  variable led_desired
  set key "$ch,$kind"
  set led_desired($key) $state
  after 0 [list ::AIORS::_led_start $key]
}

proc ::AIORS::led_rx {rx_id is_open} {
  set ch [::AIORS::_led_ch $rx_id]
  set state [expr {$is_open ? "on" : "off"}]

  if {$::AIORS::debug} {
    ::AIORS::_log "AIORS: led_rx id='$rx_id' is_open=$is_open -> ch=$ch"
    ::AIORS::_dbg "led_rx rx_id=$rx_id state=$state"
  }

  ::AIORS::_led_queue $ch rx $state
}

proc ::AIORS::led_tx {tx_id is_on} {
  set ch [::AIORS::_led_ch $tx_id]
  set state [expr {$is_on ? "on" : "off"}]

  if {$::AIORS::debug} {
    ::AIORS::_dbg "led_tx tx_id=$tx_id state=$state ch=$ch"
  }

  ::AIORS::_led_queue $ch tx $state
}

proc ::AIORS::_note_tx_state {tx_id is_on} {
  set is_on [expr {$is_on ? 1 : 0}]
  if {$tx_id eq ""} { set tx_id "0" }

  set was_on [::AIORS::_tx_active]
  set ::AIORS::tx_state($tx_id) $is_on
  set ::AIORS::tx_is_on 0
  foreach id [array names ::AIORS::tx_state] {
    if {$::AIORS::tx_state($id)} {
      set ::AIORS::tx_is_on 1
      break
    }
  }

  if {$::AIORS::tx_is_on && !$was_on} {
    set ::AIORS::tx_started_ms [clock milliseconds]
    set ::AIORS::announcement_delay_done 0
  } elseif {!$::AIORS::tx_is_on} {
    set ::AIORS::tx_started_ms 0
    set ::AIORS::announcement_delay_done 0
  }
}

proc ::AIORS::_tx_active {} {
  if {[info exists ::AIORS::tx_is_on] && $::AIORS::tx_is_on} {
    return 1
  }

  if {[array exists ::AIORS::tx_state]} {
    foreach id [array names ::AIORS::tx_state] {
      if {$::AIORS::tx_state($id)} {
        return 1
      }
    }
  }

  return 0
}

proc ::AIORS::_first_generated_audio {kind args} {
  if {[info exists ::AIORS::announcement_delay_injecting] &&
      $::AIORS::announcement_delay_injecting} {
    return
  }
  if {[info exists ::AIORS::reply_delay_injecting] &&
      $::AIORS::reply_delay_injecting} {
    return
  }

  set now [clock milliseconds]
  set delay_ms 0
  set apply_announcement 0
  set apply_reply 0
  set announcement_remaining_ms 0
  set reply_remaining_ms 0

  set announcement_pending 0
  if {[info exists ::AIORS::announcement_delay_ms] &&
      $::AIORS::announcement_delay_ms > 0} {
    set announcement_pending 1
    if {[info exists ::AIORS::announcement_delay_done] &&
        $::AIORS::announcement_delay_done} {
      set announcement_pending 0
    }
  }

  if {$announcement_pending} {
    set apply_announcement 1
    set announcement_remaining_ms $::AIORS::announcement_delay_ms
    if {[::AIORS::_tx_active]} {
      if {[info exists ::AIORS::tx_started_ms] && $::AIORS::tx_started_ms > 0} {
        set elapsed_ms [expr {$now - $::AIORS::tx_started_ms}]
        if {$elapsed_ms >= $announcement_remaining_ms} {
          set announcement_remaining_ms 0
        } elseif {$elapsed_ms > 0} {
          set announcement_remaining_ms [expr {$announcement_remaining_ms - $elapsed_ms}]
        }
      } else {
        set announcement_remaining_ms 0
      }
    }

    if {$announcement_remaining_ms > $delay_ms} {
      set delay_ms $announcement_remaining_ms
    }
  }

  set reply_pending 0
  if {[info exists ::AIORS::reply_delay_ms] &&
      $::AIORS::reply_delay_ms > 0 &&
      [info exists ::AIORS::reply_delay_pending] &&
      $::AIORS::reply_delay_pending} {
    set reply_pending 1
    if {[info exists ::AIORS::reply_delay_done] &&
        $::AIORS::reply_delay_done} {
      set reply_pending 0
    }
  }

  if {$reply_pending} {
    if {![info exists ::AIORS::last_valid_close_ts] ||
        $::AIORS::last_valid_close_ts <= 0} {
      set ::AIORS::reply_delay_pending 0
    } else {
      set elapsed_ms [expr {$now - $::AIORS::last_valid_close_ts}]
      if {$elapsed_ms < 0} {
        set elapsed_ms 0
      }

      set window_ms 3000
      if {[info exists ::AIORS::reply_delay_window_ms] &&
          $::AIORS::reply_delay_window_ms > 0} {
        set window_ms $::AIORS::reply_delay_window_ms
      }
      if {$window_ms < $::AIORS::reply_delay_ms} {
        set window_ms $::AIORS::reply_delay_ms
      }

      if {$elapsed_ms > $window_ms} {
        set ::AIORS::reply_delay_pending 0
      } else {
        set apply_reply 1
        set reply_remaining_ms [expr {$::AIORS::reply_delay_ms - $elapsed_ms}]
        if {$reply_remaining_ms < 0} {
          set reply_remaining_ms 0
        }
        if {$reply_remaining_ms > $delay_ms} {
          set delay_ms $reply_remaining_ms
        }
      }
    }
  }

  if {!$apply_announcement && !$apply_reply} {
    return
  }

  if {$kind eq "playSilence"} {
    set existing_ms 0
    if {[llength $args] >= 1 && [string is integer -strict [lindex $args 0]]} {
      set existing_ms [lindex $args 0]
    }
    set delay_ms [expr {$delay_ms - $existing_ms}]
  }

  if {$apply_announcement} {
    set ::AIORS::announcement_delay_done 1
  }
  if {$apply_reply} {
    set ::AIORS::reply_delay_done 1
    set ::AIORS::reply_delay_pending 0
  }
  if {$delay_ms <= 0} {
    return
  }

  if {[info exists ::AIORS::debug] && $::AIORS::debug} {
    set reasons [list]
    if {$apply_announcement} {
      lappend reasons "announcement=${announcement_remaining_ms}ms"
    }
    if {$apply_reply} {
      lappend reasons "reply=${reply_remaining_ms}ms"
    }
    ::AIORS::_dbg "generated audio delay: inserting ${delay_ms}ms before $kind ([join $reasons {, }])"
  }

  set ::AIORS::announcement_delay_injecting 1
  set ::AIORS::reply_delay_injecting 1
  if {[catch {::playSilence $delay_ms} err]} {
    ::AIORS::_warn "AIORS: generated audio delay failed: $err"
  }
  set ::AIORS::announcement_delay_injecting 0
  set ::AIORS::reply_delay_injecting 0
}

proc ::AIORS::_install_announcement_delay {} {
  if {$::AIORS::hooked_announcement_delay} { return 1 }

  foreach p [list ::playSilence ::playFile ::playTone ::playDtmf] {
    if {[info commands $p] eq ""} {
      continue
    }

    set orig "${p}__aiors_announcement_orig"
    if {[info commands $orig] ne ""} {
      continue
    }

    if {[catch {rename $p $orig} err]} {
      ::AIORS::_warn "AIORS: announcement preamble rename failed for $p: $err"
      continue
    }

    proc $p args [string map [list "@KIND@" [namespace tail $p] "@ORIG@" $orig] {
      catch { ::AIORS::_first_generated_audio "@KIND@" {*}$args }
      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]
  }

  set ::AIORS::hooked_announcement_delay 1
  ::AIORS::_log "AIORS: generated audio delay hook loaded: announcement=${::AIORS::announcement_delay_ms}ms reply=${::AIORS::reply_delay_ms}ms"
  return 1
}

# ---- wrapper helper (used for transmit) ----
proc ::AIORS::_wrap_proc {procname wrapper_body} {
  if {[info commands $procname] eq ""} { return 0 }
  set orig "${procname}__aiors_orig"
  if {[info commands $orig] ne ""} { return 1 }
  rename $procname $orig
  set body [string map [list "@ORIG@" $orig] $wrapper_body]
  proc $procname args $body
  return 1
}

# ---- TX hook ----
proc ::AIORS::_try_install_tx_hook {} {
  if {$::AIORS::hooked_tx} { return 1 }

  ::AIORS::_refresh_namespaces

  # transmit signatures vary; wrapper below handles 1-arg and 2-arg
  foreach name [list \
      "${::AIORS::logic_ns}::transmit" \
      "${::AIORS::inst_ns}::transmit" \
      "${::AIORS::logic_name}::transmit" \
    ] {

    if {[info procs $name] eq ""} { continue }

    if {[::AIORS::_wrap_proc $name {
      # Keep hooks in place if anything overrides later
      catch { ::AIORS::ensure_hooks }

      if {[llength $args] == 1} {
        set tx_id "0"
        set is_on [lindex $args 0]
      } elseif {[llength $args] == 2} {
        set tx_id [lindex $args 0]
        set is_on [lindex $args 1]
      } else {
        return [uplevel 1 [list @ORIG@ {*}$args]]
      }

      catch { ::AIORS::_note_tx_state $tx_id $is_on }
      catch { ::AIORS::led_tx $tx_id $is_on }
      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]} {
      set ::AIORS::hooked_tx 1
      ::AIORS::_log "AIORS: TX LED hooked via $name"
      return 1
    }
  }

  return 0
}

# ---- RX hook (supports procs OR commands; robust vs overrides) ----
proc ::AIORS::_try_install_rx_hook {} {
  ::AIORS::_refresh_namespaces

  # Candidates (most likely first)
  set candidates [list \
    "::RepeaterLogic::Logic::squelch_open" \
    "${::AIORS::logic_ns}::squelch_open" \
    "${::AIORS::inst_ns}::squelch_open" \
    "::RepeaterLogic::squelch_open" \
    "::Logic::squelch_open" \
    "squelch_open" \
  ]

  foreach p $candidates {
    if {[info commands $p] eq ""} {
      if {$::AIORS::debug} { ::AIORS::_dbg "RX hook candidate missing: $p" }
      continue
    }

    # Already wrapped?
    if {[info commands "${p}__aiors_orig"] ne ""} {
      set ::AIORS::hooked_rx 1
      if {$::AIORS::debug} { ::AIORS::_dbg "RX wrapper already present on $p" }
      return 1
    }

    # Rename (works for procs/commands)
    if {[catch {rename $p "${p}__aiors_orig"} err]} {
      if {$::AIORS::debug} { ::AIORS::_dbg "RX rename failed for $p: $err" }
      continue
    }

    # Install wrapper
    proc $p args [string map [list "@ORIG@" "${p}__aiors_orig"] {
      # Expected signature: squelch_open rx_id is_open
      # Legacy signature:  squelch_open is_open
      set argc [llength $args]
      if {$argc == 1} {
        set rx_id 0
        set is_open [lindex $args 0]
      } elseif {$argc >= 2} {
        set rx_id   [lindex $args 0]
        set is_open [lindex $args 1]
      } else {
        return [uplevel 1 [list @ORIG@ {*}$args]]
      }

      if {$rx_id eq ""} { set rx_id 0 }
      if {$is_open eq ""} { set is_open 0 }

      catch { ::AIORS::led_rx $rx_id $is_open }

      if {$::AIORS::debug} {
        # Show which wrapper fired without spamming unless debug=1
        ::AIORS::_dbg "RX squelch_open wrapper fired: proc=[lindex [info level 0] 0] rx_id=$rx_id is_open=$is_open args=$args"
      }

      return [uplevel 1 [list @ORIG@ {*}$args]]
    }]

    set ::AIORS::hooked_rx 1
    ::AIORS::_log "AIORS: RX LED hooked via $p"
    return 1
  }

  set ::AIORS::hooked_rx 0
  return 0
}

proc ::AIORS::_schedule_rx_rehook {} {
  # Other scripts may override squelch_open later; re-check a few times.
  if {$::AIORS::rehook_scheduled} { return }
  set ::AIORS::rehook_scheduled 1

  after 0    ::AIORS::_try_install_rx_hook
  after 200  ::AIORS::_try_install_rx_hook
  after 1000 ::AIORS::_try_install_rx_hook
  after 3000 ::AIORS::_try_install_rx_hook
}

# ---- Courtesy/roger beep override (optional) ----
proc ::AIORS::_install_beep_override {} {
  if {$::AIORS::hooked_rgr} { return 1 }

  ::AIORS::_refresh_namespaces

  set did 0

  # 1) Hard-disable the most common "roger/close" entry points (if present)
  set hard_candidates [list \
    "${::AIORS::logic_ns}::send_rgr_sound" \
    "${::AIORS::logic_ns}::rpt_close_tone" \
    "${::AIORS::inst_ns}::send_rgr_sound" \
    "${::AIORS::inst_ns}::rpt_close_tone" \
    "::RepeaterLogic::Logic::send_rgr_sound" \
    "::RepeaterLogic::Logic::rpt_close_tone" \
    "::RepeaterLogic::send_rgr_sound" \
    "::RepeaterLogic::rpt_close_tone" \
  ]

  foreach p $hard_candidates {
    if {[info commands $p] eq ""} { continue }

    if {[info commands "${p}__aiors_orig"] ne ""} {
      set did 1
      continue
    }

    if {[catch {rename $p ${p}__aiors_orig} e]} {
      ::AIORS::_warn "AIORS: beep override rename failed for $p: $e"
      continue
    }

    proc $p {args} { return }
    set did 1
    ::AIORS::_log "AIORS: suppressed close/roger beep via $p"
  }

  # 2) Catch-all: filter roger/close tones at the tone/message player layer.
  # Some SvxLink logic packs trigger the courtesy beep via playTone/playMsg instead
  # of (or in addition to) send_rgr_sound/rpt_close_tone.
  set filter_candidates [list \
    "${::AIORS::logic_ns}::playTone" \
    "${::AIORS::inst_ns}::playTone" \
    "::RepeaterLogic::Logic::playTone" \
    "::RepeaterLogic::playTone" \
    "::playTone" \
    "playTone" \
    "${::AIORS::logic_ns}::playMsg" \
    "${::AIORS::inst_ns}::playMsg" \
    "::RepeaterLogic::Logic::playMsg" \
    "::RepeaterLogic::playMsg" \
    "::playMsg" \
    "playMsg" \
  ]

  foreach p $filter_candidates {
    if {[info commands $p] eq ""} { continue }

    if {[info commands "${p}__aiors_orig"] ne ""} {
      set did 1
      continue
    }

    if {[catch {rename $p ${p}__aiors_orig} e]} {
      ::AIORS::_warn "AIORS: beep filter rename failed for $p: $e"
      continue
    }
    if {[string match "*playTone" $p]} {
      # playTone takes numeric args: <freq> <length_ms> <amplitude> (sometimes more)
      # The "double beep" many users hear is typically 400Hz + 360Hz tones used as a
      # courtesy/too-soon-ident notification. We suppress only those specific tones.
      set body [format {# AIORS wrapper around %s
        if {![info exists ::AIORS::disable_rpt_close_beep] || !$::AIORS::disable_rpt_close_beep} {
          return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
        }

        set f 0
        set d 0
        set a 0
        if {[llength $args] >= 1} { set f [lindex $args 0] }
        if {[llength $args] >= 2} { set d [lindex $args 1] }
        if {[llength $args] >= 3} { set a [lindex $args 2] }

        # Suppress the common "double beep" tones (400Hz and 360Hz, 900ms, amp 50)
        if {($f == 400 || $f == 360) && $d == 900 && $a == 50} {
          if {[info exists ::AIORS::debug] && $::AIORS::debug} { catch {::AIORS::_log "AIORS: suppressed playTone f=$f d=$d a=$a via %s"} }
          return
        }

        return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
      } $p $p $p $p]
    } else {
      set body [format {# AIORS wrapper around %s
        set key ""
        if {[llength $args] >= 1} {
          set key [lindex $args 0]
        }

        # Arm one-shot suppression for the automatic "Active Module ..." report after IDENT.
        # This keeps DTMF-requested status reports working.
        if {[regexp -nocase {(^|_)(ident|id)($|_)} $key] || [regexp -nocase {IDENT} $key]} {
          set ::AIORS::suppress_status_once 1
        }

        # If we're not suppressing beeps, just pass through (but keep IDENT arming above).
        if {![info exists ::AIORS::disable_rpt_close_beep] || !$::AIORS::disable_rpt_close_beep} {
          return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
        }

        if {[regexp -nocase {(^|_)(rgr|roger|close|courtesy)(_|$)} $key] ||
            [regexp -nocase {RGR_SOUND|RPT_CLOSE|CLOSE_TONE|COURTESY} $key]} {
          catch {::AIORS::_log "AIORS: suppressed tone/msg '$key' via %s"}
          return
        }

        return [uplevel 1 [linsert $args 0 %s__aiors_orig]]
      } $p $p $p $p]
    }

    proc $p {args} $body
    set did 1
    ::AIORS::_log "AIORS: installed tone/msg filter via $p"
  }

  if {$did} {
    set ::AIORS::hooked_rgr 1
    return 1
  }

  return 0
}


# ---- Suppress automatic post-IDENT "Active Module ..." announcement (keeps DTMF status working) ----
proc ::AIORS::_install_status_report_suppress {} {
  variable inst_ns
  if {$inst_ns eq ""} { return 0 }

  set p "${inst_ns}::status_report"
  if {[info commands $p] eq ""} { return 0 }

  # Already wrapped?
  if {[info commands "${p}__aiors_orig"] ne ""} { return 1 }

  if {[catch {rename $p ${p}__aiors_orig} e]} {
    ::AIORS::_warn "AIORS: status_report rename failed for $p: $e"
    return 0
  }

  proc $p {} {
    global active_module

    # One-shot suppression: only right after IDENT playback.
    if {[info exists ::AIORS::suppress_status_once] && $::AIORS::suppress_status_once} {
      # Clear the flag no matter what so it can't stick forever.
      set ::AIORS::suppress_status_once 0

      # Suppress only when FRN is the active module.
      if {[info exists active_module] && [regexp -nocase {frn|ModuleFrn} $active_module]} {
        if {[info exists ::AIORS::debug] && $::AIORS::debug} {
          catch {::AIORS::_log "AIORS: suppressed automatic status_report (active_module='$active_module')"}
        }
        return
      }
    }

    # Default: call original
    return [uplevel 1 [list ${p}__aiors_orig]]
  }

  ::AIORS::_log "AIORS: installed status_report one-shot suppress via $p"
  return 1
}



# ---- Track close timestamp to support beep gating and reply delay ----
# If your system uses a separate "squelch_close" proc somewhere, you can hook it later.
# For now, we infer close timing from squelch_open wrapper when is_open=0.
# (So we update last_valid_close_ts there by adding a tiny shim here.)
#
# Keep it cheap: this only updates a timestamp and a one-shot delay flag.
#
proc ::AIORS::_note_close_if_needed {is_open} {
  if {$is_open} {
    set ::AIORS::reply_delay_pending 0
    set ::AIORS::reply_delay_done 0
    return
  }

  set ::AIORS::last_valid_close_ts [clock milliseconds]
  set ::AIORS::reply_delay_pending 1
  set ::AIORS::reply_delay_done 0
}

# Patch the RX wrapper behavior slightly by injecting close timestamp update:
# (We do it by redefining led_rx to call it; simplest + safe.)
if {[info commands ::AIORS::led_rx__aiors_orig] eq ""} {
  rename ::AIORS::led_rx ::AIORS::led_rx__aiors_orig
  proc ::AIORS::led_rx {rx_id is_open} {
    catch { ::AIORS::_note_close_if_needed $is_open }
    return [::AIORS::led_rx__aiors_orig $rx_id $is_open]
  }
}

# ---- Debug-only TRACE (kept, but behind debug) ----
proc ::AIORS::_enable_trace_if_debug {} {
  if {!$::AIORS::debug} { return }
  # If "trace" is present, trace common Logic procs; safe no-op otherwise
  if {[info commands trace] ne ""} {
    # Keep this intentionally light; your wrapper logs are already enough.
    ::AIORS::_log "AIORS: TRACE enabled for Logic::* (squelch/sql/rx*)"
  }
}

# ---- Main entry ----
proc ::AIORS::ensure_hooks {} {
  ::AIORS::_refresh_namespaces
  ::AIORS::_enable_trace_if_debug

  # --- Read DISABLE_RPT_CLOSE_BEEP directly from svxlink.conf ---
  # SvxLink Tcl config access (Cfg::getValue) is not reliable in some builds/load-orders,
  # so we parse the active config file explicitly.
  set cfg "/etc/svxlink/svxlink.conf"
  set raw [::AIORS::_ini_get $cfg "RepeaterLogic" "DISABLE_RPT_CLOSE_BEEP" "0"]
  set raw_lc [string tolower [string trim $raw]]
  if {$raw_lc eq "1" || $raw_lc eq "true" || $raw_lc eq "yes" || $raw_lc eq "on"} {
    set ::AIORS::disable_rpt_close_beep 1
  } else {
    set ::AIORS::disable_rpt_close_beep 0
  }
  if {![info exists ::AIORS::cfg_logged] || !$::AIORS::cfg_logged} {
    set ::AIORS::cfg_logged 1
    ::AIORS::_log "DISABLE_RPT_CLOSE_BEEP=$::AIORS::disable_rpt_close_beep (raw='$raw' from $cfg)"
  } elseif {[info exists ::AIORS::debug] && $::AIORS::debug} {
    ::AIORS::_log "DISABLE_RPT_CLOSE_BEEP=$::AIORS::disable_rpt_close_beep (raw='$raw' from $cfg) (repeat)"
  }

  set raw_delay [::AIORS::_ini_get $cfg $::AIORS::logic_name "AIORS_ANNOUNCEMENT_DELAY_MS" ""]
  if {$raw_delay eq ""} {
    set raw_delay [::AIORS::_ini_get $cfg $::AIORS::logic_name "ANNOUNCEMENT_DELAY_MS" ""]
  }
  if {$raw_delay eq ""} {
    set raw_delay [::AIORS::_ini_get $cfg "GLOBAL" "AIORS_ANNOUNCEMENT_DELAY_MS" ""]
  }
  if {$raw_delay eq ""} {
    set raw_delay [::AIORS::_ini_get $cfg "GLOBAL" "ANNOUNCEMENT_DELAY_MS" "0"]
  }
  if {![string is integer -strict $raw_delay] || $raw_delay < 0} {
    ::AIORS::_warn "AIORS: invalid AIORS_ANNOUNCEMENT_DELAY_MS='$raw_delay', using 0"
    set raw_delay 0
  }
  set ::AIORS::announcement_delay_ms $raw_delay
  if {![info exists ::AIORS::announcement_cfg_logged] || !$::AIORS::announcement_cfg_logged} {
    set ::AIORS::announcement_cfg_logged 1
    ::AIORS::_log "AIORS_ANNOUNCEMENT_DELAY_MS=$::AIORS::announcement_delay_ms (from $cfg)"
  } elseif {[info exists ::AIORS::debug] && $::AIORS::debug} {
    ::AIORS::_log "AIORS_ANNOUNCEMENT_DELAY_MS=$::AIORS::announcement_delay_ms (repeat)"
  }

  set raw_reply_delay [::AIORS::_ini_get $cfg $::AIORS::logic_name "AIORS_REPLY_DELAY_MS" ""]
  if {$raw_reply_delay eq ""} {
    set raw_reply_delay [::AIORS::_ini_get $cfg $::AIORS::logic_name "REPLY_DELAY_MS" ""]
  }
  if {$raw_reply_delay eq ""} {
    set raw_reply_delay [::AIORS::_ini_get $cfg "GLOBAL" "AIORS_REPLY_DELAY_MS" ""]
  }
  if {$raw_reply_delay eq ""} {
    set raw_reply_delay [::AIORS::_ini_get $cfg "GLOBAL" "REPLY_DELAY_MS" "0"]
  }
  if {![string is integer -strict $raw_reply_delay] || $raw_reply_delay < 0} {
    ::AIORS::_warn "AIORS: invalid AIORS_REPLY_DELAY_MS='$raw_reply_delay', using 0"
    set raw_reply_delay 0
  }
  set ::AIORS::reply_delay_ms $raw_reply_delay
  if {![info exists ::AIORS::reply_cfg_logged] || !$::AIORS::reply_cfg_logged} {
    set ::AIORS::reply_cfg_logged 1
    ::AIORS::_log "AIORS_REPLY_DELAY_MS=$::AIORS::reply_delay_ms (from $cfg)"
  } elseif {[info exists ::AIORS::debug] && $::AIORS::debug} {
    ::AIORS::_log "AIORS_REPLY_DELAY_MS=$::AIORS::reply_delay_ms (repeat)"
  }

  ::AIORS::_try_install_tx_hook
  ::AIORS::_try_install_rx_hook
  ::AIORS::_schedule_rx_rehook


  ::AIORS::_install_status_report_suppress
  if {$::AIORS::announcement_delay_ms > 0 || $::AIORS::reply_delay_ms > 0} {
    ::AIORS::_install_announcement_delay
  }
  if {$::AIORS::disable_rpt_close_beep} {
    ::AIORS::_install_beep_override
  }

  return 1
}

# Install now
::AIORS::ensure_hooks

# ---- Optional AIORS companion scripts ----
set ::AIORS::script_dir [file dirname [info script]]
set ::AIORS::frn_autostart_script \
  [file join $::AIORS::script_dir "aiors_frn_autostart.tcl"]
if {[file readable $::AIORS::frn_autostart_script]} {
  source $::AIORS::frn_autostart_script
} else {
  ::AIORS::_dbg "AIORS FRN autostart script not found: $::AIORS::frn_autostart_script"
}

# ---- delayed re-hook after Logic is fully mixed in ----
after 0 {
  catch {
    ::AIORS::_log "AIORS: delayed hook retry"
    ::AIORS::ensure_hooks
  }
}
