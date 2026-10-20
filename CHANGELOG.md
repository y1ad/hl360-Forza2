# Changelog

## 0.3.1

### Online multiplayer (new)

- **Race online through the game's own System Link menus.** *Multiplayer →
  System Link* now lists every race hosted on the online server; create one or
  join one as on a local network. Nobody needs to open ports on their router:
  all traffic goes through the server, which gives each player a private
  address and passes the game's discovery and race traffic between them.
- **Online card in the launcher** (*Profiles* tab): *Play online* is on by
  default and a dot shows whether the server answers (green *Server
  available*, red *Server unavailable*). The server address is not shown.
  Your gamertag is your name online. If the server cannot be reached when the
  game starts, it simply runs offline.
- **Joining works.** Joining a session used to fail with "session full or
  unavailable" before anything was sent to the host: the game's network layer
  asks for a host address with an optional field left empty, and the port
  rejected it. The game also received the same "random" numbers on every
  machine, which made players indistinguishable; they are random now.
- **Smooth remote cars.** Over the internet the game's network layer
  (DirectPlay) took the extra latency for congestion and slowed itself to
  about 6 updates per second per opponent, with half-second freezes. The port
  now uses the game's own unthrottled mode, so updates flow at the game's
  rate (about 60 per second).
- **Lost packets are recovered.** Home connections drop a few percent of game
  packets on the way up (about 3 % in our tests), and every lost one froze the
  opponent's car for about 0.2 s. Each packet now also carries copies of the
  previous two, so the receiver rebuilds anything lost: 0 of 3,600 test
  packets went missing, against about 3 % before.
- **Server software included** (`tools/fm2-relay`): a single Python 3 script
  with no dependencies, plus a systemd unit, for anyone hosting a server. It
  limits players and packet rates, expires idle players, and cannot be used to
  amplify traffic towards third parties.

Known issue: with three or more players, a player who joins may occasionally
not see one of the others (three cars instead of four) and the game can close
when that race starts. Leaving and joining again fixes it.

### Vulkan

- **The game and the launcher now run on Vulkan**, with the same runtime as
  the Forza Motorsport 4 port. Direct3D 12 is gone; existing settings are
  moved over automatically and saves are untouched.
- **Correct car bodywork.** Vulkan's default rendering path drew visible
  artifacts on the bodywork; the port uses the generic render-target path,
  which renders it correctly.
- **Fewer stutters at 120 fps.** New pipelines are compiled in the background
  instead of stopping the frame, and shaders recorded earlier are prepared
  before playing. First-time content can still cause a short pause.
- **Graphics tab reworked** to match the other Forza ports: quality presets
  (Low, Medium, High, Custom), render scale 1x-4x, anisotropic filtering off
  to 16x, output scaling (bilinear, AMD CAS, AMD FSR), a vertical sync switch
  and a 60 / 120 fps choice. The game's native 2x MSAA is always on, and the
  extra FXAA options were removed.
- Shader preparation is fully automatic; the *Prepare shaders now* button is
  gone.

### Also

- **The frame rate you see now matches the refresh rate.** The game already
  produced 60 or 120 frames per second, but the window was presented far more
  often (224 times a second at 60 Hz on a 164 Hz monitor), repeating the same
  image, so FPS counters such as NVIDIA's, Steam's or RivaTuner's showed it
  unlocked. The cause was the SDK's achievement notification layer, which
  kept the window repainting even with nothing on screen. It is turned off:
  the window is now presented once per game frame, 60 or 120 times a second.
  Achievement pop-ups are no longer shown; achievements still unlock.

## 0.3.0

- **Refresh rate: 60 or 120 Hz.** *Graphics → Refresh rate* replaces the old
  *Vertical sync* switch. Turning that switch off let the game run unpaced, at
  several hundred frames per second, with constant stutter; the option is
  gone, and settings that still have it off open at 60 Hz. 60 Hz holds a
  steady 60 fps in races; 120 Hz runs races at up to 120 fps (111–115 fps in
  our tests) and needs a fast CPU. Menus run at half the rate, as on the Xbox
  360. 144 Hz is not offered: races then waited two refreshes per frame and
  ran slower than at 60 Hz.
- **Smoother races.** The game reports about 1,400 harmless graphics errors per
  second and all of them were written to the log: the filter meant to drop
  them never switched on. It works now (races at 120 Hz went from 64–105 fps
  to 111–115 fps), and log files stay small.
- **Project Forza Plus 2:** the launcher sets the mod's `CarPhysicsUpdateHz` to
  match the refresh rate (347 at 60 Hz, 695 at 120 Hz), as the mod's author
  asks, so the AI judges corners properly.
- The cover art is no longer required to build or package.

## 0.2.0

- **American release supported.** The European and American discs hold the
  same program and the same game files; only the header of `default.xex`
  differs, and the launcher accepts both.
- **Limited Edition bonus disc:** *Install → Downloadable content → Import bonus
  disc ISO* copies the 8 premium DLC packs (car packs, Road America, Motegi);
  each profile installs them the next time it plays. The disc's Title Update #4
  is not used: it patches the executable, and the port runs the unpatched one.
- **New Forza Plus 2 tab:** installs the Project Forza Plus 2 mod from its
  `.zip` (American release only), then its update; switches the career mode and
  the harder AI; sets the field of view of each camera; uninstalls it with the
  original files restored. The mod carries the DLC cars and tracks itself.
- **Install tab in two steps:** before the game is installed it only offers
  importing an ISO or choosing the game folder; afterwards it shows the
  downloadable content, file verification and the shader cache.

## 0.1.0

- First release: the European release of Forza Motorsport 2 recompiled for
  Windows, with the launcher (profiles, graphics, controls, language, ISO
  import and verification, shader warm-up).
