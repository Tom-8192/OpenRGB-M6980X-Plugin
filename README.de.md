# OpenRGB-Plugin für die M6980X

[English](README.md)

Ein Plugin für [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.0**, das die Maus **Gigabyte GHOST
M6980X** (USB `E0FF:0001`, OEM Areson) als Gerät mit einer **statischen** Farbe hinzufügt.

Das Protokoll stammt aus dem Reverse Engineering für das M6980X Configuration Tool (`m6980x-driver`); das
Plugin nutzt davon nur den LED-Teil.

## Was die Maus kann (und warum das Plugin eingeschränkt ist)

- Drei RGB-LEDs, die immer **eine gemeinsame Farbe** haben; wie viele leuchten, zeigt die DPI-Stufe.
- **Eine Farbe pro Hardware-Profil** (3 Profile); die Maus zeigt die Farbe des aktiven Profils.
- Jeder Kanal hat **3 Stufen** (aus, halb, voll), also 27 Farben. OpenRGB-Farben werden auf die nächste Stufe
  gerundet (`0–63` aus, `64–191` halb, `192–255` voll).
- Die Maus behält LED-Farben auch ohne ihren „Speichern“-Befehl, schreibt also vermutlich jede Änderung in
  ihren Flash. Deshalb gibt es **keinen Direct-Modus** (Effekte, SDK-Animationen): Das Plugin schreibt nur, wenn
  sich die gerundete Farbe von der Farbe in der Maus unterscheidet.

## Verhalten

- Das Plugin schreibt die Farbe nur ins **aktive Profil**. Wechselst du mit der Profiltaste, schreibt es die
  OpenRGB-Farbe auch ins neue Profil (Prüfung alle 0,5 s).
- Der bloße Start von OpenRGB ändert nichts: Das Plugin bleibt passiv, bis in OpenRGB eine Farbe angewendet wird
  (von Hand oder durch Laden eines OpenRGB-Profils).
- Ab- und Anstecken wird erkannt; das Gerät verschwindet und kommt wieder.
- Das Plugin teilt sich die Sperre mit dem M6980X Configuration Tool (`Local\m6980x_configtool` unter Windows,
  `flock` auf dem hidraw-Gerät unter Linux), beide können also gleichzeitig laufen.
- Eine Statusseite liegt unter *Einstellungen → M6980X*; Meldungen landen im OpenRGB-Log mit dem Präfix
  `[M6980X]`.
- Andere GHOST-Modelle mit derselben USB-ID (M8000X, M8600, 6980) werden abgelehnt.

## Hinweise

- Hat andere Software 0–255-Werte in die Maus geschrieben (z. B. ein älterer OpenRGB-Fork), behält und meldet die
  Maus diese. Das Plugin zeigt eine solche Farbe unverändert an und ersetzt sie beim nächsten Anwenden einer Farbe
  durch Stufen.
- Beim Abstecken der Maus oder Deaktivieren des Plugins schreibt OpenRGB 1.0
  `Device thread still active in base class destructor` ins Log. Das kommt aus OpenRGBs `RGBController_Virtual`,
  dessen Destruktor `Shutdown()` nicht aufruft; die Basisklasse beendet den Thread selbst, es ist also harmlos.

## Getestet

Windows 10, offizielles OpenRGB 1.0 (Windows 64-bit, Qt 6.8.3), an der echten Maus (16.10.2026): Erkennung,
passiver Start, Farbwechsel (nur das aktive Profil wird geschrieben, bei unveränderter gerundeter Farbe gar
nichts), Profilwechsel per Profiltaste, Ab-/Anstecken, Deaktivieren des Plugins, gleichzeitiger Betrieb mit
`m6980x_configtool`. Linux: nur gebaut, noch nicht an der Maus ausprobiert.

## Installation

Das Plugin muss zum OpenRGB-Build passen: gleiche Qt-Hauptversion und gleicher Compiler. Das Windows-Artefakt
wird wie das offizielle OpenRGB 1.0 für Windows gebaut (Qt 6.8.3, MSVC 2022, x64).

- **Windows:** `OpenRGB-M6980X.dll` nach `%APPDATA%\OpenRGB\plugins\` kopieren und OpenRGB neu starten.
- **Linux:** `libOpenRGB-M6980X.so` nach `~/.config/OpenRGB/plugins/` kopieren. Gegen dasselbe Qt bauen wie dein
  OpenRGB-Paket (ein AppImage bringt sein eigenes Qt mit). Für den Zugriff auf die Maus die udev-Regel
  installieren:
  ```
  sudo cp linux/60-openrgb-m6980x.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```

## Bauen

Siehe [README.md](README.md#building).

## Lizenz

GPL-2.0-or-later, wie OpenRGB.
