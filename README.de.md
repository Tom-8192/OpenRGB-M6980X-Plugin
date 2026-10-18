# OpenRGB-Plugin für die M6980X

[English](README.md)

Ein Plugin für [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.1**, das die Maus **Gigabyte GHOST
M6980X** (USB `E0FF:0001`, OEM Areson) mit ihren LED-Farben pro Profil hinzufügt.

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

Unter *Geräte → Gigabyte GHOST M6980X* hat die Maus einen Modus (*Profile colours*) und eine Zone pro
Hardware-Profil, *Profile 1–3*:

- Profil in der Zonenliste wählen (oder sein Kästchen in der LED-Ansicht anklicken), Farbe wählen und anwenden.
  Das geht auch bei inaktiven Profilen.
- **Alle Zonen** wendet eine Farbe auf alle drei Profile an; Quick Colors tun dasselbe. Sollen die Profile
  unterschiedliche Farben behalten (z. B. um Tastenbelegungen zu unterscheiden), die Zonen verwenden.
- Geschrieben wird, sobald die Farbe eine Sekunde unverändert blieb (Ziehen im Farbwähler). Ein Profilwechsel mit
  der Profiltaste schreibt nichts.
- Die LED-Liste neben der Zonenliste gehört zur Geräteansicht von OpenRGB und lässt sich nicht ausblenden; jede
  Zone hat nur eine LED, sie wird also nicht gebraucht.

- Der Start von OpenRGB ändert nichts an der Maus, nur das Anwenden einer Farbe. Ausnahme: Ist beim Start der
  Oberfläche ein OpenRGB-Profil aktiv (z. B. das Startprofil des Dienstes), übergibt OpenRGB 1.1 dessen
  Plugin-Daten an das Plugin, das sie wie beim Laden dieses Profils anwendet.
- **OpenRGB-Profile** speichern die drei Farben im Plugin-Abschnitt des Profils. Das Laden eines Profils wendet
  sie wie eine Änderung von Hand an; geschrieben werden nur abweichende Farben. Profile aus dem früheren
  *Static*-Modus (Plugin 1.2 und älter) funktionieren weiter: ihre Farbe geht nur ins aktive Profil. Ein Profil,
  das vor dem Anstecken der Maus geladen wurde, wird beim Verbinden angewendet.
- Der Modus heißt nicht „Direct“/„Custom“, Effekte und SDK-Clients übernehmen die Maus also nicht von selbst.
- Ab- und Anstecken wird erkannt; das Gerät verschwindet und kommt wieder.
- Das Plugin teilt sich die Sperre mit dem M6980X Configuration Tool (`Local\m6980x_configtool` unter Windows,
  `flock` auf dem hidraw-Gerät unter Linux), beide können also gleichzeitig laufen.
- Eine Statusseite liegt unter *Einstellungen → M6980X*; Meldungen landen im OpenRGB-Log mit dem Präfix
  `[M6980X]`. Läuft die Oberfläche als Client des OpenRGB-Dienstes, übernimmt sie dessen Log-Einstellungen; ihre
  Logdatei enthält die Meldungen dann nur, wenn in den Einstellungen des Dienstes das Schreiben in eine Datei
  eingeschaltet ist.
- Andere GHOST-Modelle mit derselben USB-ID (M8000X, M8600, 6980) werden abgelehnt.

## Hinweise

- Hat andere Software 0–255-Werte in die Maus geschrieben (z. B. ein älterer OpenRGB-Fork), behält und meldet die
  Maus diese. Das Plugin zeigt eine solche Farbe unverändert an und ersetzt sie beim nächsten Anwenden einer Farbe
  durch Stufen.
- Beim Abstecken der Maus oder Deaktivieren des Plugins schreibt OpenRGB (1.0 und 1.1)
  `Device thread still active in base class destructor` ins Log. Das kommt aus OpenRGBs `RGBController_Virtual`,
  dessen Destruktor `Shutdown()` nicht aufruft; die Basisklasse beendet den Thread selbst, es ist also harmlos.

## Getestet

Windows 10, offizielles OpenRGB 1.0 (Windows 64-bit, Qt 6.8.3), an der echten Maus (16.10.2026): Erkennung,
passiver Start, Farbwechsel (nur das aktive Profil wird geschrieben, bei unveränderter gerundeter Farbe gar
nichts), Profilwechsel per Profiltaste, Ab-/Anstecken, Deaktivieren des Plugins, gleichzeitiger Betrieb mit
`m6980x_configtool`; beide Modi inklusive Quick Colors, mit der OpenRGB-Oberfläche als Client des
OpenRGB-Windows-Dienstes (Plugin in der Oberfläche).

Offizielles OpenRGB 1.1 mit Plugin 1.2.0 (18.10.2026), gleicher Aufbau: die Tests oben erneut, dazu Laden von
OpenRGB-Profilen durch einen SDK-Client und die Plugin-Daten des aktiven Profils beim Start der Oberfläche.

Plugin 1.3.0 (18.10.2026), gleicher Aufbau: Start, Laden von Profilen im neuen und im früheren Static-Format
(geschrieben werden nur abweichende Profile), Profilwechsel, die neue Geräteansicht (ein Modus, Zonen
*Profile 1–3*), Quick Colors (alle drei Profile). Farben aus der Oberfläche auf eine einzelne Zone oder auf
*Alle Zonen* anzuwenden, wurde an der Maus noch nicht ausprobiert.

Linux: nur gebaut, noch nicht an der Maus ausprobiert.

## Installation

Das Plugin muss zum OpenRGB-Build passen: gleiche Qt-Hauptversion und gleicher Compiler. Das Windows-Artefakt
wird wie das offizielle OpenRGB 1.1 für Windows gebaut (Qt 6.8.3, MSVC 2022, x64). OpenRGB 1.0 hat dieselbe
Plugin-API (5); Plugin 1.1.0 ist die letzte Version, die gegen 1.0 gebaut wurde.

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
