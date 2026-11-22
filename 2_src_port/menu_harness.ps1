<#
  menu_harness.ps1 - offline preview harness for menu.html

  Writes a temporary copy of menu.html with a stub chrome.webview bridge
  injected before the page script, plus a floating control panel. The page
  then runs in a plain browser: tabs, sliders, colors, toasts and the
  live-telemetry pill all work without WebView2, the overlay, the driver
  or the game.

  Usage:
    .\menu_harness.ps1                     # build %TEMP%\menu_harness.html
    .\menu_harness.ps1 -Open               # build and open in Chrome/Edge
    .\menu_harness.ps1 -Open -Hash colors  # open that page directly
    .\menu_harness.ps1 -Out .\h.html -Open

  menu.html is never modified: the file is read, the stub block is inserted
  after <body>, and the result is written to -Out.

  Host replies the stub imitates: hello -> state + telem, act:save -> toast,
  act:load -> state + toast, act:reset_colors -> defaults + toast. Positions
  for drag/dragend are logged only; the stub cannot move a real window.

  Reading theme values: many colours animate (transition .12s-.16s), so a
  computed style read immediately after changing an accent shows the old
  colour. Either wait ~350 ms or run the browser with
  --force-prefers-reduced-motion, which trips the reduce block at
  menu.html:360 and makes every read exact.
#>
[CmdletBinding()]
param(
  [string]$Out = (Join-Path $env:TEMP 'menu_harness.html'),
  [switch]$Open,
  [string]$Hash = '',
  [ValidateSet('chrome', 'edge', 'default')][string]$Browser = 'chrome'
)

$ErrorActionPreference = 'Stop'

$menu = Join-Path $PSScriptRoot 'menu.html'
if (-not (Test-Path -LiteralPath $menu)) {
  throw "menu.html not found beside this script: $menu"
}
if ($Out -and ($Out -eq $menu)) {
  throw 'Refusing to overwrite menu.html; choose a different -Out path.'
}

$source = [IO.File]::ReadAllText($menu)
if ($source.Contains('id="hx-panel"')) {
  throw "The input already contains harness markup; pass a plain menu.html. ($menu)"
}

# --- where to inject -------------------------------------------------------
# The stub must define window.chrome.webview before the page script runs, so
# it goes immediately after the opening <body> tag.
$bodyMatch = [regex]::Match($source, '<body[^>]*>')
if (-not $bodyMatch.Success) { throw 'menu.html has no <body> tag; nothing to inject into.' }
$bodyAt = $bodyMatch.Index + $bodyMatch.Length

$appScriptAt = $source.IndexOf('<script>')
if ($appScriptAt -lt 0) { throw 'menu.html has no <script> block; the harness expects the page script inline.' }
if ($appScriptAt -lt $bodyAt) { throw 'The page script runs before <body>; the stub could not install in time.' }

$before = $source.Substring(0, $bodyAt)
$middle = $source.Substring($bodyAt, $appScriptAt - $bodyAt)
$after = $source.Substring($appScriptAt)

$stub = @'
<!-- ---------------------------------------------------------------- -->
<!-- menu_harness: stub host. Not present in the real menu.html.        -->
<!-- Delete this block and the page is byte-identical again.            -->
<!-- ---------------------------------------------------------------- -->
<div id="hx-panel" class="hx-min">
  <div id="hx-head" role="button" tabindex="0" aria-expanded="false">
    <span class="hx-dot"></span><b>harness</b><span id="hx-note">stub bridge</span><span class="hx-grip">&#9776;</span>
  </div>
  <div id="hx-body" hidden>
    <button type="button" data-hx="state">State (sample)</button>
    <button type="button" data-hx="defaults">State (markup)</button>
    <button type="button" data-hx="attach">Attach</button>
    <button type="button" data-hx="detach">Detach</button>
    <button type="button" data-hx="toast">Toast</button>
    <button type="button" data-hx="badstate">Malformed</button>
    <label class="hx-row">LOS
      <select id="hx-los"><option value="0">Off</option><option value="1">Loading</option><option value="2" selected>Ready</option></select>
    </label>
    <label class="hx-row">Preview <input type="checkbox" id="hx-preview"></label>
    <label class="hx-row">Accent <input type="color" id="hx-accent" value="#b45cff"></label>
    <label class="hx-row">Second <input type="color" id="hx-second" value="#ff5cae"></label>
    <button type="button" data-hx="theme" class="hx-wide">Reset theme tokens</button>
    <label class="hx-row hx-wide hx-check">auto state on hello: <input type="checkbox" id="hx-auto" checked></label>
    <pre id="hx-log"></pre>
  </div>
</div>
<style>
  /* Harness chrome only, scoped to #hx-* so it cannot affect the menu. */
  #hx-panel{position:fixed;right:10px;bottom:10px;z-index:2147483000;width:296px;
    max-height:calc(100vh - 20px);display:flex;flex-direction:column;
    font:11px/1.4 'Inter','Segoe UI',system-ui,sans-serif;color:#d7dae2;
    background:rgba(11,13,18,.95);border:1px solid #2a2f3c;border-radius:10px;
    box-shadow:0 10px 28px rgba(0,0,0,.5);overflow:hidden;text-align:left}
  #hx-panel.hx-min{width:114px}
  #hx-panel.hx-min #hx-note{display:none}
  #hx-head{display:flex;align-items:center;gap:6px;padding:6px 9px;cursor:pointer;
    background:rgba(30,34,44,.94);user-select:none;flex:none}
  #hx-head b{font-weight:600;letter-spacing:.02em;color:#e9ecf2}
  #hx-head .hx-dot{width:6px;height:6px;border-radius:50%;background:#4fd6a0;flex:none}
  #hx-note{margin-left:auto;color:#7b8290;font-size:10px;white-space:nowrap}
  .hx-grip{margin-left:auto;color:#7b8290}
  #hx-note + .hx-grip{margin-left:8px}
  #hx-body{padding:8px;display:grid;grid-template-columns:1fr 1fr;gap:6px;
    overflow:auto;min-height:0}
  #hx-body[hidden]{display:none}
  #hx-body button{font:11px/1 inherit;color:#dfe3ea;background:#1b2029;
    border:1px solid #2c3340;border-radius:6px;padding:6px 4px;cursor:pointer}
  #hx-body button:hover{background:#232a35;border-color:#3a4454}
  .hx-wide,#hx-log{grid-column:1/3}
  .hx-row{display:flex;align-items:center;justify-content:space-between;gap:6px;
    padding:3px 4px;background:#141821;border:1px solid #232833;border-radius:6px;color:#aab1bf}
  .hx-row input[type=color]{width:34px;height:16px;padding:0;border:1px solid #2c3340;background:none}
  .hx-row input[type=checkbox]{margin:0}
  .hx-row.hx-check{justify-content:flex-start;gap:8px;color:#aab1bf}
  .hx-row.hx-check input{margin-left:auto}
  .hx-row select{font:11px/1 inherit;color:#dfe3ea;background:#1b2029;border:1px solid #2c3340;border-radius:4px;padding:2px}
  #hx-log{height:78px;margin:0;overflow:auto;padding:4px 5px;background:#080a0e;
    border:1px solid #232833;border-radius:6px;color:#9aa2b1;
    font:10px/1.35 Consolas,'Cascadia Mono',monospace;white-space:pre-wrap;word-break:break-all}
</style>
'@

# The stub script is kept separate from the panel markup so the two can be
# reviewed independently. Both are concatenated before the page script.
$stubScript = @'
<script>
"use strict";
/* ---- harness stub host: replace chrome.webview ---------------------- */
(function () {
  var listeners = [], sent = [], logEl = null;

  function stamp() {
    var d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0') + ':' + String(d.getSeconds()).padStart(2, '0');
  }
  function log(direction, text) {
    var line = stamp() + ' ' + direction + ' ' + String(text);
    if (line.length > 220) line = line.slice(0, 220) + '\u2026';
    if (!logEl) {
      window.__hxLog = window.__hxLog || [];
      window.__hxLog.push(line);
      if (window.__hxLog.length > 200) window.__hxLog.shift();
      return;
    }
    logEl.textContent += (logEl.textContent ? '\n' : '') + line;
    logEl.scrollTop = logEl.scrollHeight;
    var lines = logEl.textContent.split('\n');
    while (lines.length > 200) {
      logEl.textContent = logEl.textContent.slice(logEl.textContent.indexOf('\n') + 1);
      lines = logEl.textContent.split('\n');
    }
  }

  /* Host -> page. Mirrors WebView2: event.data is always a string. */
  function emit(text) {
    log('\u2190', text);
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i]({ data: String(text) }); }
      catch (error) { log('!!', error && error.message); }
    }
  }

  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: function (message) {
      var text = typeof message === 'string' ? message : JSON.stringify(message);
      sent.push(text);
      if (sent.length > 400) sent.shift();
      log('\u2192', text);
      var auto = document.getElementById('hx-auto');
      if (text === 'hello:' && auto && auto.checked) {
        declared();   /* snapshot markup values before any state lands */
        setTimeout(function () {
          emit('state:' + JSON.stringify(window.__harness.state()));
          emit('telem:' + JSON.stringify(window.__harness.telem()));
        }, 40);
      }
      /* The host answers Save/Load/Reset; mirror menu_webview.cpp:285-310. */
      if (auto) {
        if (text === 'act:save') {
          setTimeout(function () { emit('toast:Settings saved'); }, 40);
        } else if (text === 'act:load') {
          setTimeout(function () {
            emit('state:' + JSON.stringify(window.__harness.state()));
            emit('toast:Settings loaded');
          }, 40);
        } else if (text === 'act:reset_colors') {
          setTimeout(function () {
            emit('state:' + JSON.stringify(window.__harness.defaults()));
            emit('toast:Colors reset to defaults');
          }, 40);
        }
      }
      return undefined;
    },
    addEventListener: function (type, fn) {
      if (type === 'message' && listeners.indexOf(fn) < 0) listeners.push(fn);
    },
    removeEventListener: function (type, fn) {
      var at = listeners.indexOf(fn);
      if (at >= 0) listeners.splice(at, 1);
    },
    hostObjects: {}
  };

  /* ---- fixtures ------------------------------------------------------ */
  var PALETTE = ['#ff4655', '#ffb020', '#ff5cae', '#b45cff', '#7dd3fc', '#4fd6a0', '#e7e9ee', '#9298a5'];
  var ROLES = { cop: '#3b82f6', police: '#3b82f6', robber: '#ef4444', civilian: '#22c55e',
                clan: '#a78bfa', hubber: '#f59e0b' };

  /* Phase D slots. If menu.html declares no value for them, use the host
     defaults (menu_webview.cpp:114-115) so the preview matches the real page
     instead of painting black. */
  var UI_DEFAULTS = { accent: '#b45cff', secondary: '#ff5cae' };

  /* Declared values, captured before the first state message. */
  var DECLARED = null;
  function declared() {
    if (DECLARED) return DECLARED;
    DECLARED = {};
    Array.prototype.forEach.call(document.querySelectorAll('[data-color]'), function (input) {
      DECLARED[input.dataset.color] = input.value;
    });
    return DECLARED;
  }

  function hexToRgb(hex) {
    var body = String(hex || '').replace('#', '');
    if (body.length !== 6) return [255, 255, 255];
    return [parseInt(body.slice(0, 2), 16), parseInt(body.slice(2, 4), 16), parseInt(body.slice(4, 6), 16)];
  }
  function rgbText(hex) { return hexToRgb(hex).join(', '); }
  function rgbArray(hex) { return hexToRgb(hex); }

  /* Values exactly as the markup declares them, honouring data-scale.
     This is the round-trip check: the page must redraw what it already shows. */
  function markupState() {
    var config = {}, colors = {}, roles = {};
    Array.prototype.forEach.call(document.querySelectorAll('[data-key]'), function (input) {
      var scale = Number(input.dataset.scale || 1);
      if (input.type === 'checkbox') config[input.dataset.key] = input.checked ? 1 : 0;
      else config[input.dataset.key] = Math.round(Number(input.value) * scale);
    });
    Array.prototype.forEach.call(document.querySelectorAll('[data-color]'), function (input) {
      var key = input.dataset.color;
      var declaredValue = input.hasAttribute('value');
      colors[key] = rgbArray(!declaredValue && UI_DEFAULTS[key] ? UI_DEFAULTS[key] : input.value);
    });
    Array.prototype.forEach.call(document.querySelectorAll('[data-role]'), function (row) {
      roles[row.dataset.role] = rgbArray(ROLES[row.dataset.role] || '#e7e9ee');
    });
    config.colors = colors;
    config.role_colors = roles;
    config.aa_key = 0x12;
    return config;
  }

  /* What the host sends after act:reset_colors: palette back to the declared
     values, UI accents back to the host defaults. */
  function defaultsState() {
    var config = markupState(), map = declared(), colors = {};
    Array.prototype.forEach.call(document.querySelectorAll('[data-color]'), function (input) {
      var key = input.dataset.color;
      colors[key] = rgbArray(map[key] || UI_DEFAULTS[key] || '#ffffff');
    });
    config.colors = colors;
    return config;
  }

  /* Deliberately different values, so every control visibly moves. The two UI
     accents stay put: cycling them repaints the chrome and hides the markers. */
  function sampleState() {
    var config = markupState(), colors = {};
    config.enabled = 1;
    config.radar_on = 1;
    config.show_health = 0;
    config.show_weapon = 0;
    config.aa_on = 1;
    config.aa_los_on = 1;
    config.max_distance = 75000;   /* data-scale 100 -> 750 m */
    config.aa_fov_px = 120;
    config.aa_key = 0x10;
    Array.prototype.forEach.call(document.querySelectorAll('[data-color]'), function (input, index) {
      var key = input.dataset.color;
      colors[key] = rgbArray(UI_DEFAULTS[key] || PALETTE[index % PALETTE.length]);
    });
    config.colors = colors;
    return config;
  }

  function sampleTelem() {
    var select = document.getElementById('hx-los');
    /* cop_count / slot_total feed the World page counter (menu_webview.cpp
     * build_telem_json). Values are fixed so the readout is reviewable. */
    return { pid: 4242, targets: 3, los: select ? Number(select.value) : 2,
             cop_count: 2, slot_total: 9 };
  }

  /* Phase D will set these four properties from applyState; the harness can
     set them now so the accent/secondary palette is reviewable early. */
  function setTheme(accent, secondary) {
    var root = document.documentElement.style;
    root.setProperty('--accent', accent);
    root.setProperty('--accent-rgb', rgbText(accent));
    if (secondary) {
      root.setProperty('--secondary', secondary);
      root.setProperty('--secondary-rgb', rgbText(secondary));
    }
    var note = document.getElementById('hx-note');
    if (note) note.textContent = accent;
  }

  window.__harness = {
    sent: sent, emit: emit, state: sampleState, telem: sampleTelem,
    markupState: markupState, defaults: defaultsState, declared: declared,
    theme: setTheme, hexToRgb: hexToRgb
  };

  /* ---- panel wiring -------------------------------------------------- */
  function ready(fn) {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', fn);
    else fn();
  }

  ready(function () {
    logEl = document.getElementById('hx-log');
    var panel = document.getElementById('hx-panel');
    var head = document.getElementById('hx-head');
    var body = document.getElementById('hx-body');

    function toggle(open) {
      var show = (typeof open === 'boolean') ? open : body.hidden;
      body.hidden = !show;
      panel.classList.toggle('hx-min', !show);
      head.setAttribute('aria-expanded', String(show));
    }
    head.addEventListener('click', function () { toggle(); });
    head.addEventListener('keydown', function (event) {
      if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); toggle(); }
    });

    var actions = {
      state: function () { emit('state:' + JSON.stringify(sampleState())); },
      defaults: function () { emit('state:' + JSON.stringify(markupState())); },
      attach: function () { emit('telem:' + JSON.stringify(sampleTelem())); },
      detach: function () { emit('telem:' + JSON.stringify({ pid: 0, targets: 0, los: 0, cop_count: 0, slot_total: 0 })); },
      toast: function () { emit('toast:Settings saved'); },
      badstate: function () { emit('state:{not json'); },
      theme: function () {
        var root = document.documentElement.style;
        root.removeProperty('--accent');
        root.removeProperty('--accent-rgb');
        root.removeProperty('--secondary');
        root.removeProperty('--secondary-rgb');
        document.getElementById('hx-accent').value = '#b45cff';
        document.getElementById('hx-second').value = '#ff5cae';
        document.getElementById('hx-note').textContent = 'stub bridge';
      }
    };
    Array.prototype.forEach.call(panel.querySelectorAll('[data-hx]'), function (button) {
      button.addEventListener('click', function () { actions[button.dataset.hx](); });
    });

    document.getElementById('hx-accent').addEventListener('input', function (event) {
      setTheme(event.target.value, document.getElementById('hx-second').value);
    });
    document.getElementById('hx-second').addEventListener('input', function (event) {
      setTheme(document.getElementById('hx-accent').value, event.target.value);
    });
    document.getElementById('hx-preview').addEventListener('change', function (event) {
      if (event.target.checked) document.body.dataset.preview = '1';
      else delete document.body.dataset.preview;
    });

    /* Flush anything logged before the panel existed (hello: fires first). */
    if (window.__hxLog) {
      logEl.textContent = window.__hxLog.join('\n');
      window.__hxLog = null;
    }
    log('\u00b7', 'harness ready \u2014 open the panel from the header');
    log('\u00b7', 'global __harness: sent, emit, state(), telem(), theme(accent, secondary)');
  });
})();
</script>
'@

# --- assemble ---------------------------------------------------------------
$assembled = $before + $stub + $stubScript + $middle + $after

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
  New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
[IO.File]::WriteAllText($Out, $assembled, (New-Object Text.UTF8Encoding($false)))

# --- report ----------------------------------------------------------------
$written = Get-Item -LiteralPath $Out
$checks = [ordered]@{
  'input'            = $menu
  'output'           = $written.FullName
  'source bytes'     = (Get-Item -LiteralPath $menu).Length
  'output bytes'     = $written.Length
  'stub bytes'       = ($stub.Length + $stubScript.Length)
  'injected after'   = "<body> at char $bodyAt"
  'page script at'   = ($bodyAt + $stub.Length + $stubScript.Length)
  'stub runs first'  = ($bodyAt -lt ($bodyAt + $stub.Length + $stubScript.Length))
  'script tags'      = ([regex]::Matches($assembled, '<script').Count)
  'tab buttons'      = ([regex]::Matches($assembled, 'data-tab="').Count)
  'page sections'    = ([regex]::Matches($assembled, 'role="tabpanel"').Count)
}
Write-Host ''
Write-Host 'menu_harness.ps1' -ForegroundColor Cyan
foreach ($key in $checks.Keys) { Write-Host ("  {0,-17} {1}" -f $key, $checks[$key]) }
Write-Host ''
Write-Host '  menu.html is an input only - it was not written.' -ForegroundColor DarkGray
Write-Host ''

if ($Open) {
  $uri = ([Uri]$Out).AbsoluteUri
  if ($Hash) { $uri += '#' + $Hash.TrimStart('#') }

  $exe = $null
  if ($Browser -eq 'edge') {
    $candidates = @(
      (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe'),
      (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe')
    )
  } elseif ($Browser -eq 'chrome') {
    $candidates = @(
      (Join-Path $env:ProgramFiles 'Google\Chrome\Application\chrome.exe'),
      (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe'),
      (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe')
    )
  } else {
    $candidates = @()
  }
  foreach ($candidate in $candidates) {
    if ($candidate -and $candidate -notmatch '^\\' -and (Test-Path -LiteralPath $candidate)) { $exe = $candidate; break }
  }
  if ($exe) {
    Start-Process -FilePath $exe -ArgumentList '--new-window', $uri | Out-Null
    Write-Host "  opened with  $exe"
  } else {
    Start-Process -FilePath $uri | Out-Null
    Write-Host '  opened with  system default browser'
  }
  Write-Host ''
}


