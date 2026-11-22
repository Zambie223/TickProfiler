# Changelog

Changes after the first public release are listed here.

## 0.1.1

- Built for game version 1.2.5.1 (build 520140) and SML 3.12.1, which this version now requires.
- Fixed: on game 1.2.5.1, version 0.1.0 reported wrong per-player network numbers (ping, bandwidth, packets, packet loss, send backlog), because the engine's network connection data changed layout in that update. Frame timing, per-class data and admin checks were not affected. Use 0.1.1 on 1.2.5.1; 0.1.0 remains for game 1.2.5.0.
- Fixed: the description of the automatic hitch threshold (`HitchMs` = 0) in `/perf settings`, the Mods menu and the README left out that the threshold rises to twice the previous second's average frame time when that is higher. The threshold itself works as before.
